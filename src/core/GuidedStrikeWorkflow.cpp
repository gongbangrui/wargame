#include "GuidedStrikeWorkflow.h"

#include <cmath>
#include <algorithm>

namespace gbr {

namespace {

bool stageFromName(const QString& name, GuidedStrikeWorkflow::Stage* stage) {
    if (!stage) return false;
    static const QList<QPair<QString, GuidedStrikeWorkflow::Stage>> values{
        {QStringLiteral("idle"), GuidedStrikeWorkflow::Stage::Idle},
        {QStringLiteral("targetReported"), GuidedStrikeWorkflow::Stage::TargetReported},
        {QStringLiteral("dispatchPending"), GuidedStrikeWorkflow::Stage::DispatchPending},
        {QStringLiteral("strikeDispatched"), GuidedStrikeWorkflow::Stage::StrikeDispatched},
        {QStringLiteral("groundGuidancePending"), GuidedStrikeWorkflow::Stage::GroundGuidancePending},
        {QStringLiteral("engaging"), GuidedStrikeWorkflow::Stage::Engaging},
        {QStringLiteral("targetDestroyed"), GuidedStrikeWorkflow::Stage::TargetDestroyed},
        {QStringLiteral("withdrawPending"), GuidedStrikeWorkflow::Stage::WithdrawPending},
        {QStringLiteral("withdrawn"), GuidedStrikeWorkflow::Stage::Withdrawn},
        {QStringLiteral("failed"), GuidedStrikeWorkflow::Stage::Failed}};
    for (const auto& value : values) {
        if (value.first == name) {
            *stage = value.second;
            return true;
        }
    }
    return false;
}

}

GuidedStrikeWorkflow::GuidedStrikeWorkflow(MessageBus* bus, const QString& side,
                                           const QString& commandPostId, QObject* parent)
    : QObject(parent), m_bus(bus), m_side(side), m_commandPostId(commandPostId) {
    if (m_bus) {
        connect(m_bus, &MessageBus::messagePosted, this,
                &GuidedStrikeWorkflow::onMessagePosted);
    }
}

QString GuidedStrikeWorkflow::stageName() const {
    switch (m_stage) {
    case Stage::Idle: return QStringLiteral("idle");
    case Stage::TargetReported: return QStringLiteral("targetReported");
    case Stage::DispatchPending: return QStringLiteral("dispatchPending");
    case Stage::StrikeDispatched: return QStringLiteral("strikeDispatched");
    case Stage::GroundGuidancePending: return QStringLiteral("groundGuidancePending");
    case Stage::Engaging: return QStringLiteral("engaging");
    case Stage::TargetDestroyed: return QStringLiteral("targetDestroyed");
    case Stage::WithdrawPending: return QStringLiteral("withdrawPending");
    case Stage::Withdrawn: return QStringLiteral("withdrawn");
    case Stage::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

bool GuidedStrikeWorkflow::reject(const QString& message, QString* error) {
    if (error) *error = message;
    // A rejected button press is not a workflow failure: the operator may
    // correct the selection and retry the same approval stage.
    QJsonObject event{{QStringLiteral("stage"), stageName()},
                      {QStringLiteral("reason"), message},
                      {QStringLiteral("rejected"), true}};
    m_events.append(event);
    while (m_events.size() > 200) m_events.removeFirst();
    emit workflowEvent(event);
    return false;
}

bool GuidedStrikeWorkflow::validateIncomingMessage(const Message& message,
                                                    QString* error) const {
    if (error) error->clear();
    const QJsonObject& payload = message.payload;
    const QString targetId = payload.value(QStringLiteral("targetId")).toString();
    const QString attackerId = payload.value(QStringLiteral("attackerId")).toString();
    const QJsonArray waypoints = payload.value(QStringLiteral("waypoints")).toArray();
    const auto fail = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    const auto sameTarget = [this, &targetId]() {
        return !targetId.isEmpty() && (m_targetId.isEmpty() || targetId == m_targetId);
    };
    const auto sameCorrelation = [this, &message]() {
        // A first message establishes the task correlation.  Once established,
        // an omitted or different correlation is an out-of-context message.
        return m_correlationId.isEmpty() || message.correlationId == m_correlationId;
    };
    if (!sameCorrelation()) return fail(QStringLiteral("VMF 消息关联 ID 与当前任务不一致"));

    switch (message.type) {
    case Message::Type::TargetReport:
    case Message::Type::TargetDetect:
        if (message.receiver != m_commandPostId || !sameTarget()
            || (m_stage != Stage::Idle && m_stage != Stage::TargetReported)) {
            return fail(QStringLiteral("当前阶段不接受该目标报告"));
        }
        return true;
    case Message::Type::StrikePlan:
    case Message::Type::FlightPlan:
    case Message::Type::AttackOrder:
        if (message.sender != m_commandPostId || message.receiver.isEmpty()
            || !sameTarget()
            || (message.type != Message::Type::AttackOrder && waypoints.isEmpty())
            || (m_attackerId.isEmpty() ? false : message.receiver != m_attackerId)
            || (m_stage != Stage::TargetReported && m_stage != Stage::DispatchPending
                && m_stage != Stage::StrikeDispatched)) {
            return fail(QStringLiteral("当前阶段不接受该打击规划或攻击命令"));
        }
        return true;
    case Message::Type::GroundGuideOrder:
        if (message.sender != m_commandPostId || message.receiver.isEmpty()
            || !sameTarget() || attackerId.isEmpty()
            || (!m_attackerId.isEmpty() && attackerId != m_attackerId)
            || (m_stage != Stage::StrikeDispatched
                && m_stage != Stage::GroundGuidancePending)) {
            return fail(QStringLiteral("当前阶段不接受地面引导命令"));
        }
        return true;
    case Message::Type::GroundAttackConfirm:
        if (message.sender.isEmpty() || message.receiver.isEmpty() || !sameTarget()
            || waypoints.isEmpty()
            || (!m_guideId.isEmpty() && message.sender != m_guideId)
            || (!m_attackerId.isEmpty() && message.receiver != m_attackerId)
            || (m_stage != Stage::GroundGuidancePending && m_stage != Stage::Engaging)) {
            return fail(QStringLiteral("当前阶段不接受地面攻击确认"));
        }
        return true;
    case Message::Type::Guidance:
        if (message.receiver != m_attackerId || !sameTarget() || waypoints.isEmpty()
            || (m_stage != Stage::GroundGuidancePending && m_stage != Stage::Engaging)) {
            return fail(QStringLiteral("当前阶段不接受攻击引导消息"));
        }
        return true;
    case Message::Type::EngagementReport:
        if (message.sender != m_attackerId || message.receiver != m_commandPostId
            || !sameTarget()
            || (m_stage != Stage::Engaging && m_stage != Stage::TargetDestroyed)) {
            return fail(QStringLiteral("当前阶段不接受交战报告"));
        }
        return true;
    case Message::Type::TargetDestroyed:
        {
            // The payload identifies the task, but it cannot grant sender
            // authority.  A recon report is accepted only from the recon
            // aircraft that registered the task; an attack confirmation is
            // accepted only from the assigned attacker itself.
            const bool attackerConfirmation = !m_attackerId.isEmpty()
                && message.sender == m_attackerId;
            const bool reconConfirmation = !m_reconId.isEmpty()
                && message.sender == m_reconId;
            if (!attackerConfirmation && !reconConfirmation) {
                return fail(QStringLiteral("目标摧毁报告必须来自攻击机或已登记侦察机"));
            }
        }
        if (message.receiver != m_commandPostId || !sameTarget()
            || (m_stage != Stage::Engaging && m_stage != Stage::TargetDestroyed)) {
            return fail(QStringLiteral("当前阶段不接受目标摧毁报告"));
        }
        return true;
    case Message::Type::Withdraw:
    case Message::Type::WithdrawOrder:
        if (message.sender != m_commandPostId || message.receiver != m_attackerId
            || (m_stage != Stage::Engaging && m_stage != Stage::TargetDestroyed
                && m_stage != Stage::WithdrawPending && m_stage != Stage::Withdrawn)) {
            return fail(QStringLiteral("当前阶段不接受撤离命令"));
        }
        return true;
    default:
        return true;
    }
}

void GuidedStrikeWorkflow::setStage(Stage stage, const QString& reason) {
    // Retries and the explicit local approval call can observe the same
    // authoritative transition twice.  A stage is an edge in the workflow,
    // so do not append a second event when the state is already there.
    if (m_stage == stage) return;
    m_stage = stage;
    emit stageChanged(stage);
    if (m_createdAt <= 0.0) m_createdAt = m_updatedAt;
    QJsonObject event{{QStringLiteral("stage"), stageName()},
                      {QStringLiteral("reason"), reason},
                      {QStringLiteral("targetId"), m_targetId},
                      {QStringLiteral("attackerId"), m_attackerId},
                      {QStringLiteral("guideId"), m_guideId},
                      {QStringLiteral("reconId"), m_reconId}};
    m_events.append(event);
    while (m_events.size() > 200) m_events.removeFirst();
    emit workflowEvent(event);
}

bool GuidedStrikeWorkflow::send(Message message, QString* error) {
    if (!m_bus) return reject(QStringLiteral("消息总线未初始化"), error);
    if (message.sender.isEmpty() || message.receiver.isEmpty()) {
        return reject(QStringLiteral("消息缺少发送方或接收方"), error);
    }
    message.traceId = QStringLiteral("guided-strike-%1").arg(++m_sequence);
    const QString correlationId = m_correlationId.isEmpty() ? message.traceId : m_correlationId;
    message.correlationId = correlationId;
    if (m_bus) {
        m_updatedAt = std::max(0.0, m_bus->simulationTime());
        if (m_createdAt <= 0.0) m_createdAt = m_updatedAt;
    }
    message.payload.insert(QStringLiteral("workflowStage"), stageName());
    if (!m_bus->send(message)) {
        return reject(QStringLiteral("VMF 消息编码失败，工作流未推进"), error);
    }
    m_correlationId = correlationId;
    return true;
}

bool GuidedStrikeWorkflow::reportTarget(const QString& reconId, const QString& targetId,
                                        const QJsonObject& report, QString* error) {
    if (m_stage != Stage::Idle && m_stage != Stage::TargetReported) {
        return reject(QStringLiteral("当前阶段不能重复报告目标"), error);
    }
    if (reconId.isEmpty() || targetId.isEmpty() || report.isEmpty()) {
        return reject(QStringLiteral("目标报告字段不完整"), error);
    }
    if (!m_reconId.isEmpty() && reconId != m_reconId) {
        return reject(QStringLiteral("当前任务已登记其他侦察机"), error);
    }
    m_reconId = reconId;
    m_targetId = targetId;
    Message message;
    message.type = Message::Type::TargetReport;
    message.sender = reconId;
    message.receiver = m_commandPostId;
    message.requiresAck = true;
    message.automaticAck = true;
    message.payload = report;
    message.payload.insert(QStringLiteral("targetId"), targetId);
    if (!send(message, error)) return false;
    setStage(Stage::TargetReported, QStringLiteral("recon report received"));
    return true;
}

bool GuidedStrikeWorkflow::confirmDispatch(const QString& commanderId,
                                           const QString& attackerId,
                                           const QString& targetId,
                                           const QJsonArray& waypoints,
                                           QString* error) {
    if (m_stage != Stage::TargetReported || commanderId != m_commandPostId
        || targetId != m_targetId || attackerId.isEmpty() || waypoints.isEmpty()) {
        return reject(QStringLiteral("指挥员派单/规划确认条件不满足"), error);
    }
    m_attackerId = attackerId;
    setStage(Stage::DispatchPending, QStringLiteral("commander approval"));
    Message plan;
    plan.type = Message::Type::StrikePlan;
    plan.sender = commanderId;
    plan.receiver = attackerId;
    plan.requiresAck = true;
    plan.automaticAck = true;
    plan.payload = QJsonObject{{QStringLiteral("targetId"), targetId},
                               {QStringLiteral("waypoints"), waypoints}};
    if (!send(plan, error)) return false;
    Message order = plan;
    order.type = Message::Type::AttackOrder;
    order.payload.insert(QStringLiteral("fireNow"), false);
    if (!send(order, error)) return false;
    setStage(Stage::StrikeDispatched, QStringLiteral("commander dispatch accepted"));
    return true;
}

bool GuidedStrikeWorkflow::commandGroundGuidance(const QString& commanderId,
                                                 const QString& guideId,
                                                 const QString& attackerId,
                                                 const QString& targetId,
                                                 QString* error) {
    if (m_stage != Stage::StrikeDispatched || commanderId != m_commandPostId
        || targetId != m_targetId || guideId.isEmpty() || attackerId != m_attackerId) {
        return reject(QStringLiteral("地面引导命令条件不满足"), error);
    }
    m_guideId = guideId;
    Message message;
    message.type = Message::Type::GroundGuideOrder;
    message.sender = commanderId;
    message.receiver = guideId;
    message.requiresAck = true;
    message.automaticAck = true;
    message.payload = QJsonObject{{QStringLiteral("attackerId"), attackerId},
                                  {QStringLiteral("targetId"), targetId}};
    if (!send(message, error)) return false;
    setStage(Stage::GroundGuidancePending, QStringLiteral("ground team confirmation requested"));
    return true;
}

bool GuidedStrikeWorkflow::confirmGroundAttack(const QString& guideId,
                                               const QString& attackerId,
                                               const QString& targetId,
                                               const QJsonArray& waypoints,
                                               QString* error) {
    if (m_stage != Stage::GroundGuidancePending || guideId != m_guideId
        || attackerId != m_attackerId || targetId != m_targetId || waypoints.isEmpty()) {
        return reject(QStringLiteral("地面分队攻击确认条件不满足"), error);
    }
    Message message;
    message.type = Message::Type::GroundAttackConfirm;
    message.sender = guideId;
    message.receiver = attackerId;
    message.requiresAck = true;
    message.automaticAck = true;
    message.payload = QJsonObject{{QStringLiteral("targetId"), targetId},
                                  {QStringLiteral("waypoints"), waypoints},
                                  {QStringLiteral("fireNow"), true}};
    if (!send(message, error)) return false;
    setStage(Stage::Engaging, QStringLiteral("ground team approved attack"));
    return true;
}

bool GuidedStrikeWorkflow::confirmWithdraw(const QString& commanderId,
                                           const QString& attackerId,
                                           double homeX, double homeY,
                                           QString* error) {
    if ((m_stage != Stage::TargetDestroyed && m_stage != Stage::Engaging)
        || commanderId != m_commandPostId || attackerId != m_attackerId
        || !std::isfinite(homeX) || !std::isfinite(homeY)) {
        return reject(QStringLiteral("指挥员撤离确认条件不满足"), error);
    }
    setStage(Stage::WithdrawPending, QStringLiteral("commander withdrawal approval"));
    Message message;
    message.type = Message::Type::WithdrawOrder;
    message.sender = commanderId;
    message.receiver = attackerId;
    message.requiresAck = true;
    message.automaticAck = true;
    message.payload = QJsonObject{{QStringLiteral("homeX"), homeX},
                                  {QStringLiteral("homeY"), homeY}};
    if (!send(message, error)) return false;
    setStage(Stage::Withdrawn, QStringLiteral("withdrawal dispatched"));
    return true;
}

QJsonObject GuidedStrikeWorkflow::snapshot() const {
    return QJsonObject{{QStringLiteral("taskId"), m_side + QLatin1String(":guided-strike")},
                       {QStringLiteral("stage"), stageName()},
                       {QStringLiteral("side"), m_side},
                       {QStringLiteral("commandPostId"), m_commandPostId},
                       {QStringLiteral("reconId"), m_reconId},
                       {QStringLiteral("targetId"), m_targetId},
                       {QStringLiteral("attackerId"), m_attackerId},
                       {QStringLiteral("guideId"), m_guideId},
                       {QStringLiteral("correlationId"), m_correlationId},
                       {QStringLiteral("sequence"), static_cast<qint64>(m_sequence)},
                       {QStringLiteral("createdAt"), m_createdAt},
                       {QStringLiteral("updatedAt"), m_updatedAt},
                       {QStringLiteral("events"), m_events}};
}

bool GuidedStrikeWorkflow::restoreSnapshot(const QJsonObject& snapshot, QString* error) {
    if (error) error->clear();
    if (snapshot.value(QStringLiteral("side")).toString() != m_side
        || snapshot.value(QStringLiteral("commandPostId")).toString() != m_commandPostId) {
        if (error) *error = QStringLiteral("引导打击工作流阵营或指挥所不一致");
        return false;
    }
    Stage restoredStage = Stage::Idle;
    if (!stageFromName(snapshot.value(QStringLiteral("stage")).toString(), &restoredStage)) {
        if (error) *error = QStringLiteral("引导打击工作流阶段无效");
        return false;
    }
    const auto validText = [](const QJsonValue& value, bool allowEmpty = true) {
        return value.isString() && value.toString().size() <= 128
            && (allowEmpty || !value.toString().trimmed().isEmpty());
    };
    if ((snapshot.contains(QStringLiteral("reconId"))
         && !validText(snapshot.value(QStringLiteral("reconId"))))
        || !validText(snapshot.value(QStringLiteral("targetId")))
        || !validText(snapshot.value(QStringLiteral("attackerId")))
        || !validText(snapshot.value(QStringLiteral("guideId")))
        || !validText(snapshot.value(QStringLiteral("correlationId")))
        || !snapshot.value(QStringLiteral("events")).isArray()
        || snapshot.value(QStringLiteral("events")).toArray().size() > 200
        || !snapshot.value(QStringLiteral("updatedAt")).isDouble()
        || snapshot.value(QStringLiteral("updatedAt")).toDouble() < 0.0
        || !snapshot.value(QStringLiteral("createdAt")).isDouble()
        || snapshot.value(QStringLiteral("createdAt")).toDouble() < 0.0
        || (snapshot.contains(QStringLiteral("sequence"))
            && (!snapshot.value(QStringLiteral("sequence")).isDouble()
                || snapshot.value(QStringLiteral("sequence")).toDouble() < 0.0
                || snapshot.value(QStringLiteral("sequence")).toDouble() > 1000000.0
                || std::floor(snapshot.value(QStringLiteral("sequence")).toDouble())
                       != snapshot.value(QStringLiteral("sequence")).toDouble()))) {
        if (error) *error = QStringLiteral("引导打击工作流快照字段无效");
        return false;
    }
    for (const QJsonValue& value : snapshot.value(QStringLiteral("events")).toArray()) {
        if (!value.isObject()) {
            if (error) *error = QStringLiteral("引导打击工作流事件必须是对象");
            return false;
        }
        const QJsonObject event = value.toObject();
        GuidedStrikeWorkflow::Stage ignored = Stage::Idle;
        if (!stageFromName(event.value(QStringLiteral("stage")).toString(), &ignored)
            || !event.value(QStringLiteral("reason")).isString()
            || event.value(QStringLiteral("reason")).toString().size() > 512) {
            if (error) *error = QStringLiteral("引导打击工作流事件字段无效");
            return false;
        }
        for (const QString& field : {QStringLiteral("reconId"), QStringLiteral("targetId"),
                                     QStringLiteral("attackerId"), QStringLiteral("guideId")}) {
            if (event.contains(field) && !validText(event.value(field))) {
                if (error) *error = QStringLiteral("引导打击工作流事件身份字段无效");
                return false;
            }
        }
        if (event.contains(QStringLiteral("rejected"))
            && !event.value(QStringLiteral("rejected")).isBool()) {
            if (error) *error = QStringLiteral("引导打击工作流事件 rejected 字段无效");
            return false;
        }
    }
    m_stage = restoredStage;
    m_reconId = snapshot.value(QStringLiteral("reconId")).toString();
    m_targetId = snapshot.value(QStringLiteral("targetId")).toString();
    m_attackerId = snapshot.value(QStringLiteral("attackerId")).toString();
    m_guideId = snapshot.value(QStringLiteral("guideId")).toString();
    m_correlationId = snapshot.value(QStringLiteral("correlationId")).toString();
    m_sequence = static_cast<quint64>(snapshot.value(QStringLiteral("sequence")).toDouble());
    m_createdAt = snapshot.value(QStringLiteral("createdAt")).toDouble();
    m_updatedAt = snapshot.value(QStringLiteral("updatedAt")).toDouble();
    m_events = snapshot.value(QStringLiteral("events")).toArray();
    m_observedMessageIds.clear();
    return true;
}

void GuidedStrikeWorkflow::onMessagePosted(const QJsonObject& message) {
    const QString messageId = message.value(QStringLiteral("id")).toString();
    if (!messageId.isEmpty()) {
        if (m_observedMessageIds.contains(messageId)) return;
        m_observedMessageIds.insert(messageId);
        while (m_observedMessageIds.size() > 256) {
            m_observedMessageIds.remove(*m_observedMessageIds.cbegin());
        }
    }
    m_updatedAt = std::max(0.0, message.value(QStringLiteral("simulationTime")).toDouble(m_updatedAt));
    const QString sender = message.value(QStringLiteral("sender")).toString();
    const QString receiver = message.value(QStringLiteral("receiver")).toString();
    const QString senderSide = message.value(QStringLiteral("senderSide")).toString();
    const QString receiverSide = message.value(QStringLiteral("receiverSide")).toString();
    if ((!senderSide.isEmpty() && senderSide != m_side)
        || (!receiverSide.isEmpty() && receiverSide != m_side)) return;
    const QString type = message.value(QStringLiteral("type")).toString();
    const QJsonObject payload = message.value(QStringLiteral("payload")).toObject();
    const QString correlationId = message.value(QStringLiteral("correlationId")).toString();
    if (m_correlationId.isEmpty() && !correlationId.isEmpty()) m_correlationId = correlationId;

    // The workflow is also a projection of the authoritative message stream.
    // This keeps server-generated commands (including QML commands in online
    // mode) checkpointable without trusting a client-supplied stage value.
    const QString targetId = payload.value(QStringLiteral("targetId")).toString();
    if (type == QLatin1String("TargetReport") || type == QLatin1String("TargetDetect")) {
        if (receiver != m_commandPostId || targetId.isEmpty()
            || (m_stage != Stage::Idle && m_stage != Stage::TargetReported)
            || (m_stage == Stage::TargetReported && !m_targetId.isEmpty()
                && targetId != m_targetId)) {
            return;
        }
        if (!m_reconId.isEmpty() && !sender.isEmpty() && sender != m_reconId) return;
        if (m_reconId.isEmpty()) m_reconId = sender;
        m_targetId = targetId;
        if (m_createdAt <= 0.0) m_createdAt = m_updatedAt;
        if (m_stage == Stage::Idle || m_stage == Stage::TargetReported) {
            setStage(Stage::TargetReported, QStringLiteral("target report observed"));
        }
        return;
    }
    if ((type == QLatin1String("StrikePlan") || type == QLatin1String("FlightPlan")
         || type == QLatin1String("AttackOrder"))
        && sender == m_commandPostId && !targetId.isEmpty() && !receiver.isEmpty()
        && (m_stage == Stage::TargetReported || m_stage == Stage::DispatchPending
            || m_stage == Stage::StrikeDispatched)
        && (m_targetId.isEmpty() || targetId == m_targetId)
        && (m_attackerId.isEmpty() || receiver == m_attackerId)) {
        m_targetId = targetId;
        m_attackerId = receiver;
        if (m_createdAt <= 0.0) m_createdAt = m_updatedAt;
        setStage(Stage::StrikeDispatched, QStringLiteral("strike route observed"));
        return;
    }
    if (type == QLatin1String("GroundGuideOrder") && sender == m_commandPostId
        && !targetId.isEmpty() && !receiver.isEmpty()
        && (m_stage == Stage::StrikeDispatched || m_stage == Stage::GroundGuidancePending)
        && (m_targetId.isEmpty() || targetId == m_targetId)
        && (m_attackerId.isEmpty()
            || payload.value(QStringLiteral("attackerId")).toString() == m_attackerId)
        && (m_guideId.isEmpty() || receiver == m_guideId)) {
        m_targetId = targetId;
        m_guideId = receiver;
        m_attackerId = payload.value(QStringLiteral("attackerId")).toString();
        setStage(Stage::GroundGuidancePending, QStringLiteral("ground guidance observed"));
        return;
    }
    if (type == QLatin1String("GroundAttackConfirm") && !targetId.isEmpty()
        && !sender.isEmpty() && !receiver.isEmpty()
        && (m_stage == Stage::GroundGuidancePending || m_stage == Stage::Engaging)
        && (m_targetId.isEmpty() || targetId == m_targetId)
        && (m_guideId.isEmpty() || sender == m_guideId)
        && (m_attackerId.isEmpty() || receiver == m_attackerId)) {
        m_targetId = targetId;
        m_guideId = sender;
        m_attackerId = receiver;
        setStage(Stage::Engaging, QStringLiteral("ground attack confirmation observed"));
        return;
    }
    if (type == QLatin1String("TargetDestroyed") && !targetId.isEmpty()
        && !m_targetId.isEmpty() && targetId == m_targetId
        && receiver == m_commandPostId
        && (m_stage == Stage::Engaging || m_stage == Stage::TargetDestroyed)
        && ((!m_attackerId.isEmpty() && sender == m_attackerId)
            || (!m_reconId.isEmpty() && sender == m_reconId))) {
        if (m_attackerId.isEmpty()) m_attackerId = payload.value(QStringLiteral("attackerId")).toString();
        setStage(Stage::TargetDestroyed, QStringLiteral("target destroyed report"));
        return;
    }
    if ((type == QLatin1String("WithdrawOrder") || type == QLatin1String("Withdraw"))
        && !sender.isEmpty() && sender == m_commandPostId
        && !receiver.isEmpty() && (receiver == m_attackerId || m_attackerId.isEmpty())
        && (m_stage == Stage::Engaging || m_stage == Stage::TargetDestroyed
            || m_stage == Stage::WithdrawPending || m_stage == Stage::Withdrawn)) {
        if (m_attackerId.isEmpty()) m_attackerId = receiver;
        setStage(Stage::Withdrawn, QStringLiteral("withdrawal observed"));
    }
}

} // namespace gbr
