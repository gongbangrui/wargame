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

void MessageBus::updateUnitPosition(const QString& unitId, const QPointF& pos, double commRange, const QString& side) {
    Reg r;
    r.pos = pos;
    r.commRange = commRange;
    if (!side.isEmpty()) r.side = side;
    else {
        auto it = m_units.find(unitId);
        if (it != m_units.end()) r.side = it->second.side;
    }
    m_units[unitId] = r;
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
    if (a->second.side.isEmpty() || a->second.side == b->second.side) {
        const double dx = a->second.pos.x() - b->second.pos.x();
        const double dy = a->second.pos.y() - b->second.pos.y();
        const double dist = std::sqrt(dx*dx + dy*dy);
        return dist <= std::min(a->second.commRange, b->second.commRange);
    }
    return false;
}

void MessageBus::send(const Message& msg) {
    Message m = msg;
    if (!m.timestamp.isValid()) m.timestamp = QDateTime::currentDateTimeUtc();
    if (m.id.isEmpty()) m.id = QStringLiteral("m_%1").arg(++m_seq);
    m.acked = !m.requiresAck;

    emit messagePosted(m.toJson());

    if (m.receiver == "*" || m.receiver.isEmpty()) {
        for (auto& [uid, handlers] : m_handlers) {
            if (uid == m.sender) continue;
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
    for (auto& h : it->second) h(msg);
}

} // namespace gbr

