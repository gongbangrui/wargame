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
    m_baseAttackRange = p.attackRange;
    m_baseSpeed = p.speed;
    m_baseAttackPower = p.attackPower;
    m_armor = std::clamp(p.armor, 0.0, 0.9);
    m_repairRate = std::max(0.0, p.repairRate);
    m_subsystemRepairRate = std::max(0.0, p.subsystemRepairRate);
    bool hpWasClamped = false;
    if (m_hp > m_params.maxHp) { m_hp = m_params.maxHp; hpWasClamped = true; }
    if (hpWasClamped || std::abs(m_hp - prevHp) >= 0.5) {
        m_lastNotifiedHp = m_hp;
        emit hpChanged();
    }
    recomputeEffectiveParameters();
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

void UnitBase::setAttackPower(double v) {
    if (!std::isfinite(v) || v < 0.0) return;
    m_baseAttackPower = v;
    recomputeEffectiveParameters();
}

void UnitBase::setAttackRange(double v) {
    if (!std::isfinite(v) || v < 0.0) return;
    m_baseAttackRange = v;
    recomputeEffectiveParameters();
}

void UnitBase::setSpeed(double v) {
    if (!std::isfinite(v) || v < 0.0) return;
    m_baseSpeed = v;
    recomputeEffectiveParameters();
}

void UnitBase::setStatus(const QString& s) {
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}

void UnitBase::applyJamming(double factor) {
    if (!std::isfinite(factor)) factor = 1.0;
    factor = std::max(0.1, std::min(1.0, factor));
    if (factor == m_jamFactor) return;
    m_jamFactor = factor;
    recomputeEffectiveParameters();
}

void UnitBase::recomputeEffectiveParameters() {
    m_params.detectRange = m_baseDetectRange * m_jamFactor * m_sensorHealth;
    m_params.commRange = m_baseCommRange * m_jamFactor * m_commsHealth;
    m_params.attackRange = m_baseAttackRange * (0.5 + 0.5 * m_weaponHealth);
    m_params.speed = m_baseSpeed * m_mobilityHealth;
    m_params.attackPower = m_baseAttackPower * (0.35 + 0.65 * m_weaponHealth);
    if (m_bus) {
        m_bus->updateUnitPosition(m_id, m_params.pos.toPointF(), m_params.commRange,
                                  sideName(m_side));
    }
    emit paramsChanged();
}

void UnitBase::setDetectRange(double v) {
    if (!std::isfinite(v) || v < 0.0) return;
    m_baseDetectRange = v;
    recomputeEffectiveParameters();
}

void UnitBase::setCommRange(double v) {
    if (!std::isfinite(v) || v < 0.0) return;
    m_baseCommRange = v;
    recomputeEffectiveParameters();
}

bool UnitBase::disabled() const {
    return alive() && m_sensorHealth <= 0.05 && m_commsHealth <= 0.05
        && m_mobilityHealth <= 0.05 && m_weaponHealth <= 0.05;
}

QJsonObject UnitBase::subsystemStateJson() const {
    return {{QStringLiteral("sensor"), m_sensorHealth},
            {QStringLiteral("comms"), m_commsHealth},
            {QStringLiteral("mobility"), m_mobilityHealth},
            {QStringLiteral("weapon"), m_weaponHealth}};
}

bool UnitBase::restoreSubsystemState(const QJsonObject& state) {
    if (state.isEmpty()) return true;
    const double sensor = state.value(QStringLiteral("sensor")).toDouble(-1.0);
    const double comms = state.value(QStringLiteral("comms")).toDouble(-1.0);
    const double mobility = state.value(QStringLiteral("mobility")).toDouble(-1.0);
    const double weapon = state.value(QStringLiteral("weapon")).toDouble(-1.0);
    auto valid = [](double value) {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    };
    if (!valid(sensor) || !valid(comms) || !valid(mobility) || !valid(weapon)) return false;
    m_sensorHealth = sensor;
    m_commsHealth = comms;
    m_mobilityHealth = mobility;
    m_weaponHealth = weapon;
    recomputeEffectiveParameters();
    emit damageStateChanged();
    return true;
}

UnitBase::DamageDelta UnitBase::assessDamage(double incomingDamage,
                                             int subsystemIndex) const {
    DamageDelta result;
    if (!alive() || !std::isfinite(incomingDamage) || incomingDamage <= 0.0) return result;
    result.hullDamage = incomingDamage * (1.0 - m_armor);
    const double subsystemLoss = std::clamp(
        result.hullDamage / std::max(1.0, maxHp()) * 0.7, 0.0, 1.0);
    switch ((subsystemIndex % 4 + 4) % 4) {
    case 0: result.sensorLoss = std::min(subsystemLoss, m_sensorHealth); break;
    case 1: result.commsLoss = std::min(subsystemLoss, m_commsHealth); break;
    case 2: result.mobilityLoss = std::min(subsystemLoss, m_mobilityHealth); break;
    case 3: result.weaponLoss = std::min(subsystemLoss, m_weaponHealth); break;
    }
    return result;
}

void UnitBase::applyDamageDelta(const DamageDelta& delta) {
    const double hullDamage = std::max(0.0, delta.hullDamage);
    setHp(hp() - hullDamage);
    m_sensorHealth = std::clamp(m_sensorHealth - std::max(0.0, delta.sensorLoss), 0.0, 1.0);
    m_commsHealth = std::clamp(m_commsHealth - std::max(0.0, delta.commsLoss), 0.0, 1.0);
    m_mobilityHealth = std::clamp(m_mobilityHealth - std::max(0.0, delta.mobilityLoss), 0.0, 1.0);
    m_weaponHealth = std::clamp(m_weaponHealth - std::max(0.0, delta.weaponLoss), 0.0, 1.0);
    recomputeEffectiveParameters();
    emit damageStateChanged();
}

bool UnitBase::serviceTick(double dt) {
    if (!std::isfinite(dt) || dt <= 0.0 || !alive()) return false;
    const double previousHp = hp();
    setHp(hp() + m_repairRate * dt);
    const double restore = m_subsystemRepairRate * dt;
    m_sensorHealth = std::min(1.0, m_sensorHealth + restore);
    m_commsHealth = std::min(1.0, m_commsHealth + restore);
    m_mobilityHealth = std::min(1.0, m_mobilityHealth + restore);
    m_weaponHealth = std::min(1.0, m_weaponHealth + restore);
    recomputeEffectiveParameters();
    if (hp() != previousHp || restore > 0.0) emit damageStateChanged();
    const bool complete = hp() >= maxHp() - 1e-6
        && m_sensorHealth >= 1.0 - 1e-6 && m_commsHealth >= 1.0 - 1e-6
        && m_mobilityHealth >= 1.0 - 1e-6 && m_weaponHealth >= 1.0 - 1e-6;
    if (complete) m_serviceRequested = false;
    return complete;
}

double UnitBase::serviceProgress() const {
    const double hull = hp() / std::max(1.0, maxHp());
    return std::clamp((hull + m_sensorHealth + m_commsHealth
                       + m_mobilityHealth + m_weaponHealth) / 5.0, 0.0, 1.0);
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
            {QStringLiteral("subsystems"), subsystemStateJson()},
            {QStringLiteral("serviceRequested"), m_serviceRequested},
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
    const QJsonObject restoredSubsystems = state.value(QStringLiteral("subsystems")).toObject();
    const double restoredSensor = restoredSubsystems.value(QStringLiteral("sensor")).toDouble(1.0);
    const double restoredComms = restoredSubsystems.value(QStringLiteral("comms")).toDouble(1.0);
    const double restoredMobility = restoredSubsystems.value(QStringLiteral("mobility")).toDouble(1.0);
    const double restoredWeapon = restoredSubsystems.value(QStringLiteral("weapon")).toDouble(1.0);
    const double restoredLastSampleTime = state.value(QStringLiteral("lastSampleTime")).toDouble(-1.0);
    if (!std::isfinite(restoredPosition.x) || !std::isfinite(restoredPosition.y)
        || !std::isfinite(restoredPosition.alt) || !std::isfinite(restoredHp)
        || restoredHp < 0.0 || restoredHp > maxHp()
        || !std::isfinite(restoredJamFactor) || restoredJamFactor < 0.1
        || restoredJamFactor > 1.0 || !std::isfinite(restoredLastSampleTime)
        || restoredLastSampleTime < -1.0
        || !std::isfinite(restoredSensor) || restoredSensor < 0.0 || restoredSensor > 1.0
        || !std::isfinite(restoredComms) || restoredComms < 0.0 || restoredComms > 1.0
        || !std::isfinite(restoredMobility) || restoredMobility < 0.0 || restoredMobility > 1.0
        || !std::isfinite(restoredWeapon) || restoredWeapon < 0.0 || restoredWeapon > 1.0) {
        return fail(QStringLiteral("检查点单元数值无效: %1").arg(m_id));
    }

    std::vector<SchedulePoint> restoredSchedule;
    const QJsonArray schedule = state.value(QStringLiteral("schedule")).toArray();
    if (schedule.size() > 512) {
        return fail(QStringLiteral("检查点计划点过多: %1").arg(m_id));
    }
    for (const QJsonValue& value : schedule) {
        const QJsonObject object = value.toObject();
        SchedulePoint point{object.value(QStringLiteral("time")).toDouble(),
                            object.value(QStringLiteral("x")).toDouble(),
                            object.value(QStringLiteral("y")).toDouble()};
        if (!std::isfinite(point.time) || point.time < 0.0
            || !std::isfinite(point.x) || !std::isfinite(point.y)) {
            return fail(QStringLiteral("检查点计划点无效: %1").arg(m_id));
        }
        restoredSchedule.push_back(point);
    }

    if (!restoreBehaviorCheckpoint(state.value(QStringLiteral("behavior")).toObject(), error)) {
        return false;
    }
    setPosition(restoredPosition);
    setHp(restoredHp);
    m_sensorHealth = restoredSensor;
    m_commsHealth = restoredComms;
    m_mobilityHealth = restoredMobility;
    m_weaponHealth = restoredWeapon;
    m_serviceRequested = state.value(QStringLiteral("serviceRequested")).toBool(false);
    setSchedule(restoredSchedule);
    m_sharedKnowledge = state.value(QStringLiteral("sharedKnowledge")).toObject();
    m_recentPath.clear();
    const QJsonArray recentPath = state.value(QStringLiteral("recentPath")).toArray();
    if (recentPath.size() > 200) {
        return fail(QStringLiteral("检查点轨迹点过多: %1").arg(m_id));
    }
    for (const QJsonValue& value : recentPath) {
        const QJsonObject point = value.toObject();
        const double x = point.value(QStringLiteral("x")).toDouble();
        const double y = point.value(QStringLiteral("y")).toDouble();
        if (!std::isfinite(x) || !std::isfinite(y)) {
            return fail(QStringLiteral("检查点轨迹点无效: %1").arg(m_id));
        }
        m_recentPath.emplace_back(x, y);
    }
    m_lastSampleTime = restoredLastSampleTime;
    applyJamming(restoredJamFactor);
    recomputeEffectiveParameters();
    setStatus(state.value(QStringLiteral("status")).toString());
    emit sharedKnowledgeChanged();
    emit recentPathChanged();
    emit damageStateChanged();
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
