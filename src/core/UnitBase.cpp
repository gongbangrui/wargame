#include "UnitBase.h"

#include "../units/CommandPost.h"
#include "../units/ReconUAV.h"
#include "../units/AttackUAV.h"
#include "../units/GroundScout.h"

#include <QDateTime>

namespace gbr {

UnitBase::UnitBase(const QString& id, UnitKind kind, Side side, MessageBus* bus, QObject* parent)
    : QObject(parent), m_id(id), m_kind(kind), m_side(side), m_bus(bus) {
    m_hp = m_params.maxHp;
    if (m_bus) {
        m_bus->subscribe(m_id, [this](const Message& m){ this->handleMessage(m); });
        m_bus->updateUnitPosition(m_id, m_params.pos.toPointF(), m_params.commRange, sideName(m_side));
    }
}

UnitBase::~UnitBase() {
    if (m_bus) m_bus->unsubscribe(m_id);
}

std::unique_ptr<UnitBase> UnitBase::create(const QString& id, UnitKind kind, Side side, MessageBus* bus, QObject* parent) {
    switch (kind) {
    case UnitKind::CommandPost: return std::make_unique<CommandPost>(id, side, bus, parent);
    case UnitKind::ReconUAV:    return std::make_unique<ReconUAV>(id, side, bus, parent);
    case UnitKind::AttackUAV:   return std::make_unique<AttackUAV>(id, side, bus, parent);
    case UnitKind::GroundScout: return std::make_unique<GroundScout>(id, side, bus, parent);
    }
    return nullptr;
}

void UnitBase::setParams(const Params& p) {
    m_params = p;
    if (m_hp > m_params.maxHp) m_hp = m_params.maxHp;
    emit paramsChanged();
    emit positionChanged();
    if (m_bus) m_bus->updateUnitPosition(m_id, m_params.pos.toPointF(), m_params.commRange, sideName(m_side));
}

void UnitBase::setPosition(const GeoPos& pos) {
    m_params.pos = pos;
    emit positionChanged();
    if (m_bus) m_bus->updateUnitPosition(m_id, pos.toPointF(), m_params.commRange, sideName(m_side));
}

void UnitBase::setSchedule(const std::vector<SchedulePoint>& s) {
    m_schedule = s;
    std::sort(m_schedule.begin(), m_schedule.end(),
              [](const SchedulePoint& a, const SchedulePoint& b){ return a.time < b.time; });
}

void UnitBase::setCallsign(const QString& s) {
    if (m_callsign == s) return;
    m_callsign = s;
    emit callsignChanged();
}

void UnitBase::setStatus(const QString& s) {
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}

QVariantList UnitBase::position() const {
    QVariantList l;
    l << m_params.pos.x << m_params.pos.y << m_params.pos.alt;
    return l;
}

QJsonObject UnitBase::perceptionJson() const {
    QJsonObject o;
    o["ownerId"] = m_id;
    o["detections"] = QJsonArray();
    return o;
}

QJsonObject UnitBase::sharedKnowledgeJson() const {
    return m_sharedKnowledge;
}

bool UnitBase::canDetect(const GeoPos& pos) const {
    return m_params.pos.distanceTo(pos) <= m_params.detectRange;
}

void UnitBase::handleMessage(const Message& m) {
    if (m.type == Message::Type::PositionReport && !m.sender.isEmpty() && m.sender != m_id) {
        const auto senderSide = m.payload.value("side").toString();
        if (!senderSide.isEmpty() && senderSide == sideName(m_side)) {
            QJsonObject f;
            f["x"] = m.payload.value("x").toDouble();
            f["y"] = m.payload.value("y").toDouble();
            f["alt"] = m.payload.value("alt").toDouble();
            f["updated"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            rememberShared(QStringLiteral("unit:%1:last").arg(m.sender), f);
        }
    }
    onMessage(m);
}

void UnitBase::onMessage(const Message&) {}

void UnitBase::rememberShared(const QString& key, const QJsonValue& v) {
    m_sharedKnowledge.insert(key, v);
    emit sharedKnowledgeChanged();
}

} // namespace gbr
