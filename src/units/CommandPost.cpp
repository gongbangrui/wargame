#include "CommandPost.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <QDebug>

namespace gbr {

CommandPost::CommandPost(const QString& id, Side side, MessageBus* bus, QObject* parent)
    : UnitBase(id, UnitKind::CommandPost, side, bus, parent) {
    setStatus("待命");
}

void CommandPost::onTick(double) {
}

void CommandPost::onMessage(const Message& m) {
    switch (m.type) {
    case Message::Type::TargetDetect: {
        const QString tid = m.payload.value("targetId").toString();
        if (tid.isEmpty()) break;
        if (m_pending.contains(tid) || m_targets.contains(tid)) {
            sendAck(m);
            break;
        }
        QJsonObject info = m.payload;
        info["lastReportAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        info["reportedBy"] = m.sender;
        m_pending.insert(tid, info);
        emit sharedKnowledgeChanged();
        setStatus(QStringLiteral("发现目标 %1，等待指挥员决策").arg(tid));
        emit notifyEvent("发现目标", QStringLiteral("侦察 %1 报告目标 %2").arg(m.sender, tid), "info");
        emit strikeOrderRequested(tid);
        sendAck(m);
        break;
    }
    case Message::Type::EngagementReport: {
        const QString tid = m.payload.value("targetId").toString();
        emit notifyEvent("交战报告", QStringLiteral("攻击 %1 报告: %2").arg(m.sender, tid), "warn");
        sendAck(m);
        break;
    }
    case Message::Type::TargetDestroyed: {
        const QString tid = m.payload.value("targetId").toString();
        const QString attackerId = m.payload.value("attackerId").toString();
        m_targets.remove(tid);
        m_pending.remove(tid);
        emit notifyEvent("目标摧毁", QStringLiteral("目标 %1 已摧毁").arg(tid), "success");
        // 摧毁后不再自动派单撤离 — 攻击方目标已消灭，保留在原地等待新指令更符合战术直觉
        sendAck(m);
        Q_UNUSED(attackerId);
        break;
    }
    case Message::Type::Ack: {
        break;
    }
    default: break;
    }
}

QStringList CommandPost::knownTargets() const {
    QStringList l;
    for (const auto& k : m_pending.keys()) l << k;
    for (const auto& k : m_targets.keys()) if (!l.contains(k)) l << k;
    return l;
}

QJsonObject CommandPost::knownTarget(const QString& id) const {
    if (m_targets.contains(id)) return m_targets.value(id).toObject();
    return m_pending.value(id).toObject();
}

void CommandPost::orderStrike(const QString& attackerId, const QString& targetId) {
    if (!m_targets.contains(targetId) && !m_pending.contains(targetId)) return;
    QJsonObject info;
    if (m_pending.contains(targetId)) {
        info = m_pending.value(targetId).toObject();
        m_targets.insert(targetId, info);
        m_pending.remove(targetId);
    }
    Message m;
    m.type = Message::Type::AttackOrder;
    m.sender = id();
    m.receiver = attackerId;
    m.requiresAck = true;
    m.payload["targetId"] = targetId;
    send(m);

    if (info.contains("x") && info.contains("y")) {
        Message fp;
        fp.type = Message::Type::FlightPlan;
        fp.sender = id();
        fp.receiver = attackerId;
        QJsonArray wp;
        wp.append(QJsonObject{{"x", info.value("x").toDouble()}, {"y", info.value("y").toDouble()}, {"alt", 2000.0}});
        fp.payload["waypoints"] = wp;
        fp.payload["targetId"] = targetId;
        send(fp);
    }

    emit notifyEvent("攻击命令", QStringLiteral("指挥所命令 %1 攻击 %2").arg(attackerId, targetId), "info");
}

void CommandPost::orderWithdraw(const QString& unitId, double homeX, double homeY) {
    Message m;
    m.type = Message::Type::Withdraw;
    m.sender = id();
    m.receiver = unitId;
    m.requiresAck = true;
    m.payload["homeX"] = homeX;
    m.payload["homeY"] = homeY;
    send(m);
    emit notifyEvent("撤离命令", QStringLiteral("指挥所命令 %1 撤离").arg(unitId), "info");
}

void CommandPost::setFlightPlan(const QString& attackerId, const QVariantList& waypoints) {
    Message m;
    m.type = Message::Type::FlightPlan;
    m.sender = id();
    m.receiver = attackerId;
    QJsonArray wp;
    for (const auto& v : waypoints) {
        const auto p = v.toPointF();
        wp.append(QJsonObject{{"x", p.x()}, {"y", p.y()}, {"alt", 2000.0}});
    }
    m.payload["waypoints"] = wp;
    send(m);
}

void CommandPost::sendAck(const Message& original) {
    if (!original.requiresAck) return;
    Message ack;
    ack.type = Message::Type::Ack;
    ack.sender = id();
    ack.receiver = original.sender;
    ack.payload["inReplyTo"] = original.id;
    send(ack);
}

QJsonObject CommandPost::behaviorCheckpoint() const {
    return {{QStringLiteral("pendingTargets"), m_pending},
            {QStringLiteral("knownTargets"), m_targets}};
}

bool CommandPost::restoreBehaviorCheckpoint(const QJsonObject& state, QString*) {
    m_pending = state.value(QStringLiteral("pendingTargets")).toObject();
    m_targets = state.value(QStringLiteral("knownTargets")).toObject();
    emit sharedKnowledgeChanged();
    return true;
}

} // namespace gbr

