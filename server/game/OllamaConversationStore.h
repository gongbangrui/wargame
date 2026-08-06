#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace gbr {

struct OllamaConversationRecord {
    QString conversationId;
    QString requestId;
    QString roomId;
    quint64 generation = 0;
    QDateTime time;
    QString model;
    QString configuredModel;
    QString resolvedModel;
    QString status;
    QString failure;
    qint64 latencyMs = 0;
    QJsonArray messages;
    QJsonValue raw = QJsonValue::Null;
    QJsonValue parsed = QJsonValue::Null;
    QJsonValue final = QJsonValue::Null;
    QJsonValue fallback = QJsonValue::Null;

    QJsonObject toJson() const;
};

class OllamaConversationStore final {
public:
    using Clock = std::function<QDateTime()>;

    explicit OllamaConversationStore(QString directory, Clock clock = Clock());

    bool appendFinalRecord(const OllamaConversationRecord& record, QString* error = nullptr);
    bool appendFinalRecord(const QJsonObject& record, QString* error = nullptr);
    bool cleanup(QString* error = nullptr);

    QVector<QJsonObject> readRecords(QString* error = nullptr) const;

    QString directory() const { return m_directory; }
    QString configurationError() const { return m_configurationError; }
    QString filePathForDate(const QDateTime& time) const;

    static QJsonValue redact(const QJsonValue& value);
    static QJsonObject redact(const QJsonObject& value);

private:
    bool ensureDirectory(QString* error = nullptr) const;
    bool safePath(const QString& path, bool allowMissing, QString* error = nullptr) const;
    bool appendObject(const QJsonObject& object, QString* error);
    QJsonObject normalize(const QJsonObject& object, QString* error) const;
    QString nextAppendPath(const QDateTime& time, qint64 lineBytes, QString* error) const;
    QStringList storeFilePaths(QString* error = nullptr) const;

    QString m_directory;
    Clock m_clock;
    QString m_configurationError;
    mutable QDateTime m_lastCleanup;
};

}
