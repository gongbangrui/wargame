#include "LocalTransport.h"

namespace gbr {

LocalTransport::LocalTransport(QObject* parent)
    : QObject(parent), m_bus(std::make_unique<MessageBus>()) {
    // Forward every emitted JSON message to our sink (if any) for the engine
    // to cache as recentMessages / dispatch via messagePosted.
    connect(m_bus.get(), &MessageBus::messagePosted, this,
            [this](const QJsonObject& obj) {
                if (m_sink) m_sink(obj);
            });
}

void LocalTransport::send(const Message& msg) {
    m_bus->send(msg);
}

void LocalTransport::subscribe(const QString& unitId, MessageBus::Handler h) {
    m_bus->subscribe(unitId, std::move(h));
}

void LocalTransport::unsubscribe(const QString& unitId) {
    m_bus->unsubscribe(unitId);
}

void LocalTransport::unregisterUnit(const QString& unitId) {
    m_bus->unregisterUnit(unitId);
}

bool LocalTransport::canCommunicate(const QString& aId, const QString& bId) const {
    return m_bus->canCommunicate(aId, bId);
}

void LocalTransport::updateUnitPosition(const QString& unitId, const QPointF& pos,
                                        double commRange, const QString& side) {
    m_bus->updateUnitPosition(unitId, pos, commRange, side);
}

void LocalTransport::setUnitCommandPost(const QString& unitId, bool isCp) {
    m_bus->setUnitCommandPost(unitId, isCp);
}

void LocalTransport::updateUnitSide(const QString& unitId, const QString& side) {
    m_bus->updateUnitSide(unitId, side);
}

bool LocalTransport::isRegistered(const QString& unitId) const {
    return m_bus->isRegistered(unitId);
}

QString LocalTransport::unitSide(const QString& unitId) const {
    return m_bus->unitSide(unitId);
}

void LocalTransport::setMessageSink(Sink sink) {
    m_sink = std::move(sink);
}

} // namespace gbr