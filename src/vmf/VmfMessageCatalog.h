#pragma once

#include "VmfCodec.h"

#include <QJsonObject>
#include <QStringList>
#include <memory>
#include <optional>

namespace gbr::vmf {

/// Human-auditable metadata for one domain-to-VMF mapping.  The catalog id is
/// the design-level message identifier (for example 47001); it is deliberately
/// separate from VMF DFI/DUI values, which remain owned by the XML dictionaries.
struct InformationValue {
    QString level;
    int score = 0;
    QStringList fields;

    QJsonObject toJson() const;
};

struct MessageCatalogEntry {
    QString catalogId;
    QStringList domainTypes;
    QString vmfMessage;
    QStringList senderRoles;
    QStringList receiverRoles;
    QString trigger;
    QStringList preconditions;
    QStringList nextStages;
    QString payloadCondition;
    bool requiresAck = false;
    bool automaticAck = true;
    bool repeatable = false;
    InformationValue informationValue;

    QJsonObject toJson() const;
    bool matches(const QString& domainType, const QString& selectedVmfMessage,
                 const QJsonObject& payload) const;
};

/// XML dictionaries define bit-level legality.  This catalog defines the
/// semantic contract around those dictionaries: message number, mapping,
/// roles, trigger/preconditions, workflow edge and information value.
class VmfMessageCatalog final {
public:
    static std::shared_ptr<const VmfMessageCatalog> loadDesignV1(
        const QString& rootDirectory, QList<Diagnostic>* diagnostics = nullptr);
    static std::shared_ptr<const VmfMessageCatalog> fromFile(
        const QString& path, QList<Diagnostic>* diagnostics = nullptr);

    /// Returns the repository profile when available, with a small built-in
    /// fallback so headless fixtures remain usable if source data is staged
    /// elsewhere.
    static std::shared_ptr<const VmfMessageCatalog> designV1();

    // Internal construction hook used by the strict JSON parser and the
    // source-independent fallback profile.
    static std::shared_ptr<VmfMessageCatalog> create(
        int version, QVector<MessageCatalogEntry> entries);

    bool isValid() const { return !m_entries.isEmpty(); }
    int version() const { return m_version; }
    const QVector<MessageCatalogEntry>& entries() const { return m_entries; }

    std::optional<MessageCatalogEntry> entryFor(
        const QString& domainType, const QString& vmfMessage,
        const QJsonObject& payload = {}) const;
    std::optional<MessageCatalogEntry> entryForDomain(
        const QString& domainType, const QJsonObject& payload = {}) const;

    bool validate(const QString& domainType, const QString& vmfMessage,
                  const QJsonObject& payload, const QString& senderRole = {},
                  const QString& receiverRole = {},
                  QString* error = nullptr) const;

    QJsonObject summaryFor(const QString& domainType, const QString& vmfMessage,
                           const QJsonObject& payload = {}) const;
    QJsonObject toJson() const;

private:
    int m_version = 1;
    QVector<MessageCatalogEntry> m_entries;
};

} // namespace gbr::vmf
