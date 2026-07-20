#include "MessageBus.h"

#include <QDateTime>
#include <QJsonArray>
#include <QDebug>

namespace gbr {

MessageBus::MessageBus(QObject* parent) : QObject(parent) {}

void MessageBus::subscribe(const QString& unitId, Handler h) {
    m_handlers[unitId].push_back(std::move(h));
}

void MessageBus::unsubscribe(const QString& unitId) {
    m_handlers.erase(unitId);
}

void MessageBus::unregisterUnit(const QString& unitId) {
    m_handlers.erase(unitId);
    m_units.erase(unitId);
}

void MessageBus::updateUnitPosition(const QString& unitId, const QPointF& pos, double commRange, const QString& side) {
    auto& r = m_units[unitId];
    r.pos = pos;
    r.commRange = std::max(0.0, commRange);
    if (!side.isEmpty()) r.side = side;
}

void MessageBus::setUnitCommandPost(const QString& unitId, bool isCp) {
    auto it = m_units.find(unitId);
    if (it == m_units.end()) return;
    it->second.isCp = isCp;
}

void MessageBus::updateUnitSide(const QString& unitId, const QString& side) {
    auto it = m_units.find(unitId);
    if (it == m_units.end()) return;
    it->second.side = side;
}

bool MessageBus::isRegistered(const QString& unitId) const {
    return m_units.find(unitId) != m_units.end();
}

QString MessageBus::unitSide(const QString& unitId) const {
    auto it = m_units.find(unitId);
    return it == m_units.end() ? QString() : it->second.side;
}

bool MessageBus::canCommunicate(const QString& aId, const QString& bId) const {
    auto a = m_units.find(aId);
    auto b = m_units.find(bId);
    if (a == m_units.end() || b == m_units.end()) return false;
    if (a->second.side.isEmpty() || b->second.side.isEmpty()
        || a->second.side != b->second.side) return false;
    if (a->second.isCp || b->second.isCp) return true;
    const double dx = a->second.pos.x() - b->second.pos.x();
    const double dy = a->second.pos.y() - b->second.pos.y();
    const double dist = std::sqrt(dx*dx + dy*dy);
    return dist <= std::min(a->second.commRange, b->second.commRange);
}

void MessageBus::send(const Message& msg) {
    Message m = msg;
    if (!m.timestamp.isValid()) m.timestamp = QDateTime::currentDateTimeUtc();
    if (m.id.isEmpty()) m.id = QStringLiteral("m_%1").arg(++m_seq);
    m.acked = !m.requiresAck;

    QJsonObject posted = m.toJson();
    posted["senderSide"] = unitSide(m.sender);
    posted["receiverSide"] = unitSide(m.receiver);
    emit messagePosted(posted);

    if (m.receiver == "*" || m.receiver.isEmpty()) {
        // Handlers may synchronously unregister units. Snapshot ids so those
        // callbacks cannot invalidate the map iteration.
        std::vector<QString> recipients;
        recipients.reserve(m_handlers.size());
        for (const auto& [uid, _] : m_handlers) recipients.push_back(uid);
        for (const auto& uid : recipients) {
            if (uid == m.sender) continue;
            if (canCommunicate(m.sender, uid))
                deliver(m, uid);
        }
    } else {
        if (canCommunicate(m.sender, m.receiver)) {
            deliver(m, m.receiver);
        } else {
            qDebug() << "[bus]" << m.sender << "->" << m.receiver << "NO COMM";
        }
    }
}

void MessageBus::deliver(const Message& msg, const QString& targetId) {
    auto it = m_handlers.find(targetId);
    if (it == m_handlers.end()) return;
    // A callback is allowed to unsubscribe itself. Invoke a stable snapshot
    // and let subscription changes take effect on the next message.
    const auto handlers = it->second;
    for (const auto& h : handlers) h(msg);
}

} // namespace gbr
