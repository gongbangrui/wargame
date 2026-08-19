#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace gbr::vmf {

/// Persistable VMF control-plane state.  Trace summaries never contain raw
/// XML/wire/payload data.  A pending ACK may retain its canonical VMF wire and
/// bounded payload so a retry after restart uses the exact same envelope.
struct RuntimeState final {
    static constexpr int SchemaVersion = 1;
    static constexpr const char* ProfileId = "vmf-design-v1";
    static constexpr qsizetype MaxActiveTasks = 256;
    static constexpr qsizetype MaxPendingAcks = 1024;
    static constexpr qsizetype MaxSeenMessageIds = 4096;
    static constexpr qsizetype MaxTraceSummaries = 200;

    QJsonArray activeTasks;
    QJsonArray pendingAcks;
    QJsonArray seenMessageIds;
    QJsonArray traceSummaries;
    QString profileId = QString::fromLatin1(ProfileId);

    QJsonObject toJson() const;
    bool validate(QString* error = nullptr) const;
    static bool fromJson(const QJsonObject& object, RuntimeState* output,
                         QString* error = nullptr);

    /// Returns false when the bounded task/ACK state cannot accept the item.
    bool upsertTask(const QJsonObject& task, QString* error = nullptr);
    bool removeTask(const QString& taskId);
    bool upsertPendingAck(const QJsonObject& ack, QString* error = nullptr);
    bool removePendingAck(const QString& messageId);
    void rememberMessageId(const QString& messageId);
    void appendTraceSummary(const QJsonObject& trace);
};

} // namespace gbr::vmf
