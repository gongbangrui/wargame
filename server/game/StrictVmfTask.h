#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace gbr {

/// Authoritative task collection for protocolProfile=vmf-guided-strike-v1.
/// It deliberately contains no transport or QML code; GameServer owns the
/// communication and persistence boundary around it.
class StrictVmfTaskSet final {
public:
    static constexpr int ActiveLimitPerSide = 16;
    static constexpr int HistoryLimit = 100;

    struct Task {
        QString taskId;
        QString side;
        quint64 taskRevision = 1;
        QString stage = QStringLiteral("awaitingTargetReport");
        QString health = QStringLiteral("active");
        QString blockCode;
        QString commanderSeatId;
        QString reconSeatId;
        QString attackSeatId;
        QString groundSeatId;
        QString targetId;
        QString correlationId;
        QJsonArray route;
        QStringList pendingMessageIds;
        QJsonArray eventHistory;
        double createdAt = 0.0;
        double updatedAt = 0.0;
    };

    struct Result {
        bool ok = false;
        QString status = QStringLiteral("rejected");
        QString code;
        QString message;
        quint64 taskRevision = 0;
        QStringList messageIds;
    };

    Result createTask(const QString& taskId, const QString& side,
                      const QString& commanderSeatId, const QString& reconSeatId,
                      const QString& attackSeatId, const QString& groundSeatId,
                      const QString& targetId, const QString& correlationId,
                      bool waitingForHuman, double now);
    Result applyAction(const QJsonObject& command, const QString& actorSeatId,
                       const QSet<QString>& placeholderSeats, double now);
    Result setBlocked(const QString& taskId, const QString& code, double now);
    Result clearBlocked(const QString& taskId, double now);
    Result completeReturn(const QString& taskId, double now);

    const Task* task(const QString& taskId) const;
    Task* task(const QString& taskId);
    QList<Task> tasks(const QString& side = {}) const;
    QJsonObject toJson() const;
    bool restore(const QJsonObject& object, QString* error = nullptr);

    static bool validSide(const QString& side);

private:
    static bool validStageAction(const QString& stage, const QString& action);
    static QString nextStage(const QString& stage, const QString& action);
    static bool roleMatches(const Task& task, const QString& action,
                            const QString& actorSeatId);
    static void appendHistory(Task& task, const QString& action,
                              const QString& actorSeatId, double now,
                              const QString& code = {});
    Result failure(const QString& code, const QString& message,
                   quint64 revision = 0) const;
    Result success(Task& task, const QString& status = QStringLiteral("accepted"));

    QHash<QString, Task> m_tasks;
    QStringList m_historyOrder;
};

} // namespace gbr
