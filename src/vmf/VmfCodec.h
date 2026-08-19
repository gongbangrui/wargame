#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

namespace gbr::vmf {

struct Diagnostic {
    enum class Severity { Warning, Error };

    Severity severity = Severity::Error;
    QString code;
    QString path;
    QString message;
};

struct XmlNode {
    QString tag;
    QHash<QString, QString> attributes;
    QString text;
    QVector<XmlNode> children;

    const XmlNode* firstChild(const QString& childTag,
                              const QString& name = QString()) const;
    QVector<const XmlNode*> childrenNamed(const QString& childTag,
                                          const QString& name = QString()) const;
    XmlNode* firstChild(const QString& childTag, const QString& name = QString());
};

struct MessageXml {
    QString message;
    QString caseId;
    XmlNode header;
    XmlNode body;

    static bool parse(const QByteArray& data, MessageXml* output,
                      QList<Diagnostic>* diagnostics = nullptr);
    QByteArray serialize(bool indent = true) const;
};

struct FieldLayout {
    QString path;
    QString tag;
    QString name;
    QString dfi;
    QString dui;
    quint64 value = 0;
    int bitOffset = 0;
    int bits = 0;
};

struct EncodedMessage {
    QString message;
    QByteArray bytes;
    int bitLength = 0;
    MessageXml canonicalXml;
    QVector<FieldLayout> fields;
};

struct DecodedMessage {
    QString message;
    MessageXml xml;
    QVector<FieldLayout> fields;
    int bitLength = 0;
};

struct ValidationReport {
    bool valid = false;
    QList<Diagnostic> diagnostics;

    QString summary() const;
};

class DictionarySet final {
public:
    struct Private;

    DictionarySet();
    ~DictionarySet();
    DictionarySet(DictionarySet&&) noexcept;
    DictionarySet& operator=(DictionarySet&&) noexcept;
    DictionarySet(const DictionarySet&) = delete;
    DictionarySet& operator=(const DictionarySet&) = delete;

    static std::shared_ptr<DictionarySet> fromFiles(const QString& messageDictionaryPath,
                                                     const QString& contentDictionaryPath,
                                                     QList<Diagnostic>* diagnostics = nullptr);
    static std::shared_ptr<DictionarySet> fromFiles(const QStringList& messageDictionaryPaths,
                                                     const QString& contentDictionaryPath,
                                                     QList<Diagnostic>* diagnostics = nullptr);
    static std::shared_ptr<DictionarySet> fromXml(const QByteArray& messageDictionary,
                                                  const QByteArray& contentDictionary,
                                                  QList<Diagnostic>* diagnostics = nullptr);
    static std::shared_ptr<DictionarySet> fromXmlSet(const QList<QByteArray>& messageDictionaries,
                                                     const QByteArray& contentDictionary,
                                                     QList<Diagnostic>* diagnostics = nullptr);

    bool isValid() const;
    QStringList messageNames() const;
    bool hasMessage(const QString& name) const;

private:
    std::unique_ptr<Private> d;

    friend class Codec;
};

class Codec final {
public:
    explicit Codec(std::shared_ptr<const DictionarySet> dictionaries);

    ValidationReport validate(const QString& messageName,
                              const MessageXml& message) const;
    ValidationReport validate(const MessageXml& message) const;

    bool encode(const QString& messageName, const MessageXml& message,
                EncodedMessage* output, QList<Diagnostic>* diagnostics = nullptr) const;
    bool decode(const QString& messageName, const QByteArray& bytes, int bitLength,
                DecodedMessage* output, QList<Diagnostic>* diagnostics = nullptr) const;

    static QString diagnosticsToString(const QList<Diagnostic>& diagnostics);

private:
    std::shared_ptr<const DictionarySet> m_dictionaries;
};

QString diagnosticsToJson(const QList<Diagnostic>& diagnostics);

} // namespace gbr::vmf
