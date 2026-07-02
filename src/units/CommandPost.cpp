#include "CommandPost.h"

#include <QJsonArray>
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
        QJsonObject info = m.payload;
        info["lastReportAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        info["reportedBy"] = m.sender;
        rememberTarget(tid, info);
        m_pending.insert(tid, info);
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
        m_targets.remove(tid);
        m_pending.remove(tid);
        emit notifyEvent("目标摧毁", QStringLiteral("目标 %1 已摧毁").arg(tid), "success");
        Message w;
        w.type = Message::Type::Withdraw;
        w.sender = id();
        w.receiver = m.payload.value("attackerId").toString();
        send(w);
        break;
    }
    case Message::Type::Ack: {
        break;
    }
    default: break;
    }
}

void CommandPost::rememberTarget(const QString& tid, const QJsonObject& info) {
    m_targets.insert(tid, info);
    emit sharedKnowledgeChanged();
}

QStringList CommandPost::knownTargets() const {
    QStringList l;
    for (const auto& k : m_targets.keys()) l << k;
    return l;
}

QJsonObject CommandPost::knownTarget(const QString& id) const {
    return m_targets.value(id).toObject();
}

void CommandPost::orderStrike(const QString& attackerId, const QString& targetId) {
    if (!m_targets.contains(targetId)) return;
    Message m;
    m.type = Message::Type::AttackOrder;
    m.sender = id();
    m.receiver = attackerId;
    m.requiresAck = true;
    m.payload["targetId"] = targetId;
    send(m);
    emit notifyEvent("攻击命令", QStringLiteral("指挥所命令 %1 攻击 %2").arg(attackerId, targetId), "info");
}

void CommandPost::orderWithdraw(const QString& unitId) {
    Message m;
    m.type = Message::Type::Withdraw;
    m.sender = id();
    m.receiver = unitId;
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

} // namespace gbr


