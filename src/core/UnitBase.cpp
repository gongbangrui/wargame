#include "UnitBase.h"

#include "../units/CommandPost.h"
#include "../units/ReconUAV.h"
#include "../units/AttackUAV.h"
#include "../units/GroundScout.h"
#include "../units/JammerUAV.h"

#include <QDateTime>
#include <QJsonArray>

namespace gbr {

UnitBase::UnitBase(const QString& id, UnitKind kind, Side side, MessageBus* bus,
                     QObject* parent, UnitOwner owner)
    : QObject(parent), m_id(id), m_kind(kind), m_side(side), m_owner(owner), m_bus(bus) {
    m_hp = m_params.maxHp;
    m_lastNotifiedHp = m_hp;
    m_baseDetectRange = m_params.detectRange;
    m_baseCommRange = m_params.commRange;
    if (m_bus) {
        m_bus->subscribe(m_id, [this](const Message& m){ this->handleMessage(m); });
        m_bus->updateUnitPosition(m_id, m_params.pos.toPointF(), m_params.commRange, sideName(m_side));
    }
}

UnitBase::~UnitBase() {
    if (m_bus) m_bus->unregisterUnit(m_id);
}

std::unique_ptr<UnitBase> UnitBase::create(const QString& id, UnitKind kind, Side side, MessageBus* bus, QObject* parent) {
    switch (kind) {
    case UnitKind::CommandPost: return std::make_unique<CommandPost>(id, side, bus, parent);
    case UnitKind::ReconUAV:    return std::make_unique<ReconUAV>(id, side, bus, parent);
    case UnitKind::AttackUAV:   return std::make_unique<AttackUAV>(id, side, bus, parent);
    case UnitKind::GroundScout: return std::make_unique<GroundScout>(id, side, bus, parent);
    case UnitKind::JammerUAV:   return std::make_unique<JammerUAV>(id, side, bus, parent);
    }
    return nullptr;
}

void UnitBase::setParams(const Params& p) {
    const bool posChanged = (m_params.pos.x != p.pos.x) || (m_params.pos.y != p.pos.y) || (m_params.pos.alt != p.pos.alt);
    const double prevHp = m_hp;
    m_params = p;
    m_baseDetectRange = p.detectRange;
    m_baseCommRange = p.commRange;
    bool hpWasClamped = false;
    if (m_hp > m_params.maxHp) { m_hp = m_params.maxHp; hpWasClamped = true; }
    if (hpWasClamped || std::abs(m_hp - prevHp) >= 0.5) {
        m_lastNotifiedHp = m_hp;
        emit hpChanged();
    }
    applyJamming(m_jamFactor); // emits paramsChanged
    if (posChanged) emit positionChanged();
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

void UnitBase::applyJamming(double factor) {
    if (!std::isfinite(factor)) factor = 1.0;
    factor = std::max(0.1, std::min(1.0, factor));
    const double detectRange = m_baseDetectRange * factor;
    const double commRange = m_baseCommRange * factor;
    if (factor == m_jamFactor
        && detectRange == m_params.detectRange
        && commRange == m_params.commRange) return;
    m_jamFactor = factor;
    m_params.detectRange = detectRange;
    m_params.commRange = commRange;
    if (m_bus) {
        m_bus->updateUnitPosition(m_id, m_params.pos.toPointF(), m_params.commRange,
                                  sideName(m_side));
    }
    emit paramsChanged();
}

void UnitBase::setDetectRange(double v) {
    if (!std::isfinite(v) || v < 0.0) return;
    m_baseDetectRange = v;
    applyJamming(m_jamFactor);
}

void UnitBase::setCommRange(double v) {
    if (!std::isfinite(v) || v < 0.0) return;
    m_baseCommRange = v;
    applyJamming(m_jamFactor);
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
    // detectRange is a 2D planar radius (altitude does not count).
    return m_params.pos.distanceTo2D(pos) <= m_params.detectRange;
}

void UnitBase::handleMessage(const Message& m) {
    if (!alive()) return;
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

QJsonObject UnitBase::checkpointState() const {
    QJsonArray positionArray{m_params.pos.x, m_params.pos.y, m_params.pos.alt};
    QJsonArray scheduleArray;
    for (const SchedulePoint& point : m_schedule) {
        scheduleArray.append(QJsonObject{{QStringLiteral("time"), point.time},
                                         {QStringLiteral("x"), point.x},
                                         {QStringLiteral("y"), point.y}});
    }
    QJsonArray recentPathArray;
    for (const QPointF& point : m_recentPath) {
        recentPathArray.append(QJsonObject{{QStringLiteral("x"), point.x()},
                                           {QStringLiteral("y"), point.y()}});
    }
    return {{QStringLiteral("id"), m_id},
            {QStringLiteral("position"), positionArray},
            {QStringLiteral("hp"), m_hp},
            {QStringLiteral("status"), m_status},
            {QStringLiteral("schedule"), scheduleArray},
            {QStringLiteral("sharedKnowledge"), m_sharedKnowledge},
            {QStringLiteral("recentPath"), recentPathArray},
            {QStringLiteral("lastSampleTime"), m_lastSampleTime},
            {QStringLiteral("jamFactor"), m_jamFactor},
            {QStringLiteral("behavior"), behaviorCheckpoint()}};
}

bool UnitBase::restoreCheckpointState(const QJsonObject& state, QString* error) {
    if (error) error->clear();
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (state.value(QStringLiteral("id")).toString() != m_id) {
        return fail(QStringLiteral("检查点单元 ID 不匹配: %1").arg(m_id));
    }
    const QJsonArray positionArray = state.value(QStringLiteral("position")).toArray();
    if (positionArray.size() != 3) {
        return fail(QStringLiteral("检查点单元位置无效: %1").arg(m_id));
    }
    const GeoPos restoredPosition{positionArray.at(0).toDouble(),
                                  positionArray.at(1).toDouble(),
                                  positionArray.at(2).toDouble()};
    const double restoredHp = state.value(QStringLiteral("hp")).toDouble(-1.0);
    const double restoredJamFactor = state.value(QStringLiteral("jamFactor")).toDouble(1.0);
    if (!std::isfinite(restoredPosition.x) || !std::isfinite(restoredPosition.y)
        || !std::isfinite(restoredPosition.alt) || !std::isfinite(restoredHp)
        || restoredHp < 0.0 || !std::isfinite(restoredJamFactor)) {
        return fail(QStringLiteral("检查点单元数值无效: %1").arg(m_id));
    }

    std::vector<SchedulePoint> restoredSchedule;
    for (const QJsonValue& value : state.value(QStringLiteral("schedule")).toArray()) {
        const QJsonObject object = value.toObject();
        SchedulePoint point{object.value(QStringLiteral("time")).toDouble(),
                            object.value(QStringLiteral("x")).toDouble(),
                            object.value(QStringLiteral("y")).toDouble()};
        if (!std::isfinite(point.time) || !std::isfinite(point.x) || !std::isfinite(point.y)) {
            return fail(QStringLiteral("检查点计划点无效: %1").arg(m_id));
        }
        restoredSchedule.push_back(point);
    }

    if (!restoreBehaviorCheckpoint(state.value(QStringLiteral("behavior")).toObject(), error)) {
        return false;
    }
    setPosition(restoredPosition);
    setHp(restoredHp);
    setSchedule(restoredSchedule);
    m_sharedKnowledge = state.value(QStringLiteral("sharedKnowledge")).toObject();
    m_recentPath.clear();
    for (const QJsonValue& value : state.value(QStringLiteral("recentPath")).toArray()) {
        const QJsonObject point = value.toObject();
        const double x = point.value(QStringLiteral("x")).toDouble();
        const double y = point.value(QStringLiteral("y")).toDouble();
        if (std::isfinite(x) && std::isfinite(y)) m_recentPath.emplace_back(x, y);
    }
    while (m_recentPath.size() > 200) m_recentPath.pop_front();
    m_lastSampleTime = state.value(QStringLiteral("lastSampleTime")).toDouble(-1.0);
    applyJamming(restoredJamFactor);
    setStatus(state.value(QStringLiteral("status")).toString());
    emit sharedKnowledgeChanged();
    emit recentPathChanged();
    return true;
}

bool UnitBase::restoreBehaviorCheckpoint(const QJsonObject&, QString*) {
    return true;
}

void UnitBase::sampleRecentPath(double simTime) {
    const QPointF cur(m_params.pos.x, m_params.pos.y);
    if (m_lastSampleTime < 0 || simTime - m_lastSampleTime >= 0.2) {
        bool needPush = m_recentPath.empty();
        if (!needPush) {
            const auto& back = m_recentPath.back();
            const double dx = back.x() - cur.x();
            const double dy = back.y() - cur.y();
            if (std::sqrt(dx*dx + dy*dy) >= 1.0) needPush = true;
        }
        if (needPush) {
            if (m_recentPath.size() >= 200) m_recentPath.pop_front();
            m_recentPath.push_back(cur);
            m_lastSampleTime = simTime;
            emit recentPathChanged();
        }
    }
}

QVariantList UnitBase::recentPath() const {
    QVariantList l;
    l.reserve((int)m_recentPath.size());
    for (const auto& p : m_recentPath) {
        QVariantMap m;
        m["x"] = p.x();
        m["y"] = p.y();
        l.append(m);
    }
    return l;
}

} // namespace gbr
