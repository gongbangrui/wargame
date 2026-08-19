#pragma once

#include "MessageBus.h"

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace gbr {

/// Explicit human-in-the-loop workflow for the design scenario.  The class
/// does not mutate unit state; it emits domain messages and records the
/// externally observable stage transitions so local and authoritative hosts
/// can apply the same approval sequence.
class GuidedStrikeWorkflow final : public QObject {
    Q_OBJECT
public:
    enum class Stage {
        Idle,
        TargetReported,
        DispatchPending,
        StrikeDispatched,
        GroundGuidancePending,
        Engaging,
        TargetDestroyed,
        WithdrawPending,
        Withdrawn,
        Failed
    };
    Q_ENUM(Stage)

    GuidedStrikeWorkflow(MessageBus* bus, const QString& side,
                          const QString& commandPostId, QObject* parent = nullptr);

    bool reportTarget(const QString& reconId, const QString& targetId,
                      const QJsonObject& report, QString* error = nullptr);
    bool confirmDispatch(const QString& commanderId, const QString& attackerId,
                         const QString& targetId, const QJsonArray& waypoints,
                         QString* error = nullptr);
    bool commandGroundGuidance(const QString& commanderId, const QString& guideId,
                               const QString& attackerId, const QString& targetId,
                               QString* error = nullptr);
    bool confirmGroundAttack(const QString& guideId, const QString& attackerId,
                             const QString& targetId, const QJsonArray& waypoints,
                             QString* error = nullptr);
    bool confirmWithdraw(const QString& commanderId, const QString& attackerId,
                         double homeX, double homeY, QString* error = nullptr);

    Stage stage() const { return m_stage; }
    QString stageName() const;
    QString reconId() const { return m_reconId; }
    QString targetId() const { return m_targetId; }
    QString attackerId() const { return m_attackerId; }
    QString guideId() const { return m_guideId; }
    QString correlationId() const { return m_correlationId; }
    /// Validate a message against the current authoritative workflow edge.
    /// Generic position/control traffic returns true; guided-strike message
    /// types must satisfy the current stage and correlation identity.
    bool validateIncomingMessage(const Message& message, QString* error = nullptr) const;
    QJsonObject snapshot() const;
    bool restoreSnapshot(const QJsonObject& snapshot, QString* error = nullptr);

signals:
    void stageChanged(gbr::GuidedStrikeWorkflow::Stage stage);
    void workflowEvent(const QJsonObject& event);

private slots:
    void onMessagePosted(const QJsonObject& message);

private:
    bool reject(const QString& message, QString* error);
    bool send(Message message, QString* error);
    void setStage(Stage stage, const QString& reason);

    MessageBus* m_bus = nullptr;
    QString m_side;
    QString m_commandPostId;
    QString m_reconId;
    QString m_targetId;
    QString m_attackerId;
    QString m_guideId;
    Stage m_stage = Stage::Idle;
    QJsonArray m_events;
    quint64 m_sequence = 0;
    double m_updatedAt = 0.0;
    double m_createdAt = 0.0;
    QString m_correlationId;
    QSet<QString> m_observedMessageIds;
};

} // namespace gbr
