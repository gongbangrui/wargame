#include "UnitBase.h"

#include "../units/CommandPost.h"
#include "../units/ReconUAV.h"
#include "../units/AttackUAV.h"
#include "../units/GroundScout.h"
#include "../units/JammerUAV.h"
#include "../units/GroundTarget.h"

#include <QDateTime>
#include <QJsonArray>
#include <QByteArray>

#include <limits>

namespace gbr {

namespace {

quint64 mix64(quint64 value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

double deterministicUnitSample(quint64 seed, const QString& id, quint64 sequence) {
    quint64 value = mix64(seed ^ sequence);
    for (const unsigned char byte : id.toUtf8()) value = mix64(value ^ byte);
    return static_cast<double>(value >> 11U) * (1.0 / 9007199254740992.0);
}

bool finiteNonNegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

} // namespace

UnitBase::UnitBase(const QString& id, UnitKind kind, Side side, MessageBus* bus,
                     QObject* parent, UnitOwner owner)
    : QObject(parent), m_id(id), m_kind(kind), m_side(side), m_owner(owner), m_bus(bus) {
    m_commandedSpeedLimitMps = commandedSpeedLimitMps(kind);
    m_params.commRange = defaultCommRangeM(kind);
    m_params.speed = defaultSpeedMps(kind);
    m_params.collisionRadius = defaultCollisionRadiusM(kind);
    m_params.collisionHalfHeight = defaultCollisionHalfHeightM(kind);
    m_hp = m_params.maxHp;
    m_lastNotifiedHp = m_hp;
    m_baseDetectRange = m_params.detectRange;
    m_baseCommRange = m_params.commRange;
    configureAbilitiesAndFuelEconomy();
    if (m_bus) {
        m_bus->subscribe(m_id, [this](const Message& m){ this->handleMessage(m); });
        m_bus->updateUnitPosition(m_id, m_params.pos.toPointF(), m_params.commRange, sideName(m_side));
    }
}

void UnitBase::setDemoSpeedProfile(bool enabled) {
    const double limit = enabled ? demoCommandedSpeedLimitMps(m_kind)
                                 : commandedSpeedLimitMps(m_kind);
    if (m_commandedSpeedLimitMps == limit && m_demoSpeedProfile == enabled) return;
    m_demoSpeedProfile = enabled;
    m_commandedSpeedLimitMps = limit;
    if (enabled) m_baseSpeed = std::max(m_baseSpeed, demoCruiseSpeedMps(m_kind));
    recomputeEffectiveParameters();
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
    case UnitKind::GroundTarget:return std::make_unique<GroundTarget>(id, side, bus, parent);
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
    if (m_demoSpeedProfile) m_baseSpeed = std::max(m_baseSpeed, demoCruiseSpeedMps(m_kind));
    m_baseAttackPower = p.attackPower;
    m_armor = std::clamp(p.armor, 0.0, 0.9);
    m_repairRate = std::max(0.0, p.repairRate);
    m_subsystemRepairRate = std::max(0.0, p.subsystemRepairRate);
    m_params.collisionRadius = std::clamp(
        std::isfinite(p.collisionRadius) && p.collisionRadius > 0.0
            ? p.collisionRadius : defaultCollisionRadiusM(m_kind),
        1.0, 1000.0);
    m_params.collisionHalfHeight = std::clamp(
        std::isfinite(p.collisionHalfHeight) && p.collisionHalfHeight > 0.0
            ? p.collisionHalfHeight : defaultCollisionHalfHeightM(m_kind),
        0.1, 500.0);
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
    if (hullDamage > 0.0 || delta.sensorLoss > 0.0 || delta.commsLoss > 0.0
        || delta.mobilityLoss > 0.0 || delta.weaponLoss > 0.0) {
        cancelService();
    }
    recomputeEffectiveParameters();
    emit damageStateChanged();
}

void UnitBase::configureAbilitiesAndFuelEconomy() {
    m_countermeasure = {};
    m_scan = {};
    switch (m_kind) {
    case UnitKind::CommandPost:
        m_countermeasure = AbilityState{2000.0, 65.0, 0.0, -1, -1};
        break;
    case UnitKind::AttackUAV:
        m_countermeasure = AbilityState{900.0, 40.0, 0.0, 3, 3};
        m_fuelIdleRate = 0.50;
        m_fuelMoveCoefficient = 3.50;
        break;
    case UnitKind::ReconUAV:
        m_countermeasure = AbilityState{1100.0, 50.0, 0.0, 2, 2};
        // Recon UAVs provide an area-wide sweep. Sensor damage and ECM still
        // reduce the effective range at execution time.
        m_scan = AbilityState{15000.0, 45.0, 0.0, -1, -1};
        m_fuelIdleRate = 0.40;
        m_fuelMoveCoefficient = 3.10;
        break;
    case UnitKind::JammerUAV:
        m_countermeasure = AbilityState{1800.0, 35.0, 0.0, 4, 4};
        m_fuelIdleRate = 0.80;
        m_fuelMoveCoefficient = 3.70;
        break;
    case UnitKind::GroundScout:
        m_countermeasure = AbilityState{650.0, 55.0, 0.0, 2, 2};
        m_fuelIdleRate = 0.0;
        m_fuelMoveCoefficient = 3.0;
        break;
    case UnitKind::GroundTarget:
        m_countermeasure = {};
        m_fuelIdleRate = 0.0;
        m_fuelMoveCoefficient = 0.0;
        break;
    }
}

void UnitBase::configureFuel(double capacity, double initialFuel,
                             double economyCruiseSpeed) {
    if (m_kind == UnitKind::CommandPost || !movable() || !std::isfinite(capacity) || capacity <= 0.0
        || !std::isfinite(initialFuel) || !std::isfinite(economyCruiseSpeed)
        || economyCruiseSpeed <= 0.0) {
        if (!movable()) {
            m_fuelCapacity = 0.0;
            m_fuelRemaining = 0.0;
            m_fuelBurnRate = 0.0;
        }
        return;
    }
    m_fuelCapacity = capacity;
    m_fuelRemaining = std::clamp(initialFuel, 0.0, capacity);
    m_economyCruiseSpeed = economyCruiseSpeed;
    const double ratio = m_fuelRemaining / m_fuelCapacity;
    m_fuelWarningStage = ratio <= 0.10 ? 2 : (ratio <= 0.20 ? 1 : 0);
    emit runtimeStateChanged();
}

void UnitBase::advanceRuntimeState(double dt, double actualSpeed) {
    if (!std::isfinite(dt) || dt <= 0.0) return;
    auto advanceCooldown = [dt](double& value) {
        value = std::max(0.0, value - dt);
    };
    advanceCooldown(m_countermeasure.cooldownRemaining);
    advanceCooldown(m_scan.cooldownRemaining);
    advanceCooldown(m_repairCooldownRemaining);

    if (movable() && m_kind != UnitKind::CommandPost && m_fuelCapacity > 0.0) {
        const double speedRatio = std::max(0.0, actualSpeed)
            / std::max(1e-6, m_economyCruiseSpeed);
        const double rawBurnRate = m_fuelIdleRate + m_fuelMoveCoefficient
            * std::min(3.0, speedRatio * speedRatio);
        // Quantize derived telemetry so replaying a serialized position cannot
        // create insignificant IEEE-754 drift in checkpoints.
        m_fuelBurnRate = std::round(rawBurnRate * 1e9) / 1e9;
        const double previousFuel = m_fuelRemaining;
        m_fuelRemaining = std::max(0.0, m_fuelRemaining - m_fuelBurnRate * dt);
        const double ratio = m_fuelRemaining / m_fuelCapacity;
        const int warningStage = ratio <= 0.10 ? 2 : (ratio <= 0.20 ? 1 : 0);
        if (warningStage > m_fuelWarningStage) {
            emit notifyEvent(QStringLiteral("燃油告警"),
                             QStringLiteral("%1 燃油低于 %2%")
                                 .arg(id()).arg(warningStage == 2 ? 10 : 20),
                             warningStage == 2 ? QStringLiteral("warn")
                                               : QStringLiteral("info"));
        }
        m_fuelWarningStage = warningStage;
        if (previousFuel > 0.0 && m_fuelRemaining <= 0.0) {
            cancelWaypointMotion();
            setStatus(QStringLiteral("燃油耗尽，停止移动"));
        }
    }
    emit runtimeStateChanged();
}

double UnitBase::estimatedFuelEndurance() const {
    if (m_kind == UnitKind::CommandPost || !movable() || m_fuelCapacity <= 0.0) return 0.0;
    if (m_fuelBurnRate <= 1e-9) return std::numeric_limits<double>::infinity();
    return m_fuelRemaining / m_fuelBurnRate;
}

bool UnitBase::activateCountermeasure() {
    if (!alive() || !m_countermeasure.available()) return false;
    if (!m_countermeasure.unlimited()) --m_countermeasure.remaining;
    m_countermeasure.cooldownRemaining = m_countermeasure.cooldownSec;
    emit runtimeStateChanged();
    return true;
}

bool UnitBase::activateScan() {
    if (!alive() || !m_scan.available()) return false;
    if (!m_scan.unlimited()) --m_scan.remaining;
    m_scan.cooldownRemaining = m_scan.cooldownSec;
    emit runtimeStateChanged();
    return true;
}

bool UnitBase::attemptFieldRepair(quint64 battleSeed) {
    if (!alive() || m_repairCooldownRemaining > 1e-9) return false;
    const quint64 sequence = m_repairAttemptSequence++;
    m_repairCooldownRemaining = m_repairCooldownSec;
    const bool success = deterministicUnitSample(battleSeed, id(), sequence) < 0.70;
    if (success) {
        double* lowest = &m_sensorHealth;
        if (m_commsHealth < *lowest) lowest = &m_commsHealth;
        if (m_mobilityHealth < *lowest) lowest = &m_mobilityHealth;
        if (m_weaponHealth < *lowest) lowest = &m_weaponHealth;
        *lowest = std::min(1.0, *lowest + 0.30);
        recomputeEffectiveParameters();
        emit damageStateChanged();
    }
    emit runtimeStateChanged();
    return success;
}

QJsonObject UnitBase::abilityStateJson() const {
    const auto encode = [](const AbilityState& state) {
        return QJsonObject{{QStringLiteral("range"), state.range},
                           {QStringLiteral("cooldownSec"), state.cooldownSec},
                           {QStringLiteral("cooldownRemaining"), state.cooldownRemaining},
                           {QStringLiteral("capacity"), state.capacity},
                           {QStringLiteral("remaining"), state.remaining},
                           {QStringLiteral("available"), state.available()}};
    };
    return {{QStringLiteral("countermeasure"), encode(m_countermeasure)},
            {QStringLiteral("scan"), encode(m_scan)},
            {QStringLiteral("fieldRepair"),
             QJsonObject{{QStringLiteral("cooldownSec"), m_repairCooldownSec},
                         {QStringLiteral("cooldownRemaining"), m_repairCooldownRemaining},
                         {QStringLiteral("attemptSequence"),
                          QString::number(m_repairAttemptSequence)},
                         {QStringLiteral("available"),
                          alive() && m_repairCooldownRemaining <= 1e-9}}}};
}

bool UnitBase::beginService(const QString& serviceCpId) {
    if (!alive() || !movable() || serviceCpId.isEmpty()) return false;
    const double hullLoss = 1.0 - hp() / std::max(1.0, maxHp());
    const double subsystemLoss = 1.0
        - (m_sensorHealth + m_commsHealth + m_mobilityHealth + m_weaponHealth) / 4.0;
    const double fuelLoss = m_fuelCapacity > 0.0
        ? 1.0 - m_fuelRemaining / m_fuelCapacity : 0.0;
    const double countermeasureLoss = m_countermeasure.capacity > 0
        ? 1.0 - static_cast<double>(m_countermeasure.remaining)
                    / m_countermeasure.capacity : 0.0;
    // A CP turnaround should be meaningful without making a damaged unit
    // disappear from the battle for most of a short online engagement.
    const double baselineDuration = 3.0 + 15.0 * hullLoss
        + 10.0 * subsystemLoss + rearmDurationContribution()
        + 8.0 * fuelLoss + 6.0 * countermeasureLoss;
    m_serviceDuration = std::clamp(baselineDuration * 0.75, 3.0, 36.0);
    m_serviceElapsed = 0.0;
    m_serviceCpId = serviceCpId;
    m_serviceRequested = true;
    setStatus(QStringLiteral("开始补充"));
    emit runtimeStateChanged();
    return true;
}

void UnitBase::requestService(bool value) {
    if (!value) {
        cancelService();
        return;
    }
    m_serviceRequested = true;
    if (m_serviceCpId.isEmpty()) m_serviceCpId = m_cpId;
    if (m_serviceDuration < 3.0) {
        m_serviceDuration = std::max(3.0, m_serviceElapsed);
    }
    emit runtimeStateChanged();
}

void UnitBase::cancelService() {
    if (!m_serviceRequested && m_serviceElapsed <= 0.0 && m_serviceDuration <= 0.0) return;
    m_serviceRequested = false;
    m_serviceCpId.clear();
    m_serviceElapsed = 0.0;
    m_serviceDuration = 0.0;
    emit runtimeStateChanged();
}

bool UnitBase::advanceService(double dt) {
    if (!m_serviceRequested || !alive() || !std::isfinite(dt) || dt <= 0.0
        || m_serviceDuration <= 0.0) return false;
    m_serviceElapsed = std::min(m_serviceDuration, m_serviceElapsed + dt);
    if (m_serviceElapsed + 1e-9 < m_serviceDuration) {
        emit runtimeStateChanged();
        return false;
    }
    completeService();
    return true;
}

void UnitBase::completeService() {
    setHp(maxHp());
    m_sensorHealth = 1.0;
    m_commsHealth = 1.0;
    m_mobilityHealth = 1.0;
    m_weaponHealth = 1.0;
    if (movable() && m_kind != UnitKind::CommandPost) m_fuelRemaining = m_fuelCapacity;
    if (!m_countermeasure.unlimited()) m_countermeasure.remaining = m_countermeasure.capacity;
    restoreServiceSpecificResources();
    m_fuelWarningStage = 0;
    m_serviceRequested = false;
    m_serviceCpId.clear();
    recomputeEffectiveParameters();
    emit damageStateChanged();
    emit runtimeStateChanged();
}

bool UnitBase::serviceTick(double dt) {
    return advanceService(dt);
}

double UnitBase::serviceProgress() const {
    if (m_serviceDuration <= 0.0) return 0.0;
    return std::clamp(m_serviceElapsed / m_serviceDuration, 0.0, 1.0);
}

QJsonObject UnitBase::runtimeStateJson() const {
    return {{QStringLiteral("schema"), 3},
            {QStringLiteral("fuelCapacity"), m_fuelCapacity},
            {QStringLiteral("fuelRemaining"), m_fuelRemaining},
            {QStringLiteral("economyCruiseSpeed"), m_economyCruiseSpeed},
            {QStringLiteral("fuelBurnRate"), m_fuelBurnRate},
            {QStringLiteral("fuelWarningStage"), m_fuelWarningStage},
            {QStringLiteral("abilities"), abilityStateJson()},
            {QStringLiteral("service"),
             QJsonObject{{QStringLiteral("active"), m_serviceRequested},
                         {QStringLiteral("cpId"), m_serviceCpId},
                         {QStringLiteral("elapsed"), m_serviceElapsed},
                         {QStringLiteral("duration"), m_serviceDuration}}}};
}

bool UnitBase::restoreRuntimeState(const QJsonObject& state, QString* error) {
    if (error) error->clear();
    if (state.isEmpty()) {
        cancelService();
        return true;
    }
    auto fail = [error, this](const QString& detail) {
        if (error) *error = QStringLiteral("检查点资源状态无效: %1 (%2)").arg(id(), detail);
        return false;
    };
    const double fuelCapacity = state.value(QStringLiteral("fuelCapacity")).toDouble(m_fuelCapacity);
    const double fuelRemaining = state.value(QStringLiteral("fuelRemaining")).toDouble(m_fuelRemaining);
    const double economySpeed = state.value(QStringLiteral("economyCruiseSpeed"))
                                    .toDouble(m_economyCruiseSpeed);
    const double burnRate = state.value(QStringLiteral("fuelBurnRate")).toDouble(0.0);
    if (!finiteNonNegative(fuelCapacity) || !finiteNonNegative(fuelRemaining)
        || fuelRemaining > fuelCapacity + 1e-9 || !std::isfinite(economySpeed)
        || economySpeed <= 0.0 || !finiteNonNegative(burnRate)) {
        return fail(QStringLiteral("燃油"));
    }

    const QJsonObject abilities = state.value(QStringLiteral("abilities")).toObject();
    const QJsonObject countermeasure = abilities.value(QStringLiteral("countermeasure")).toObject();
    const QJsonObject scan = abilities.value(QStringLiteral("scan")).toObject();
    const QJsonObject repair = abilities.value(QStringLiteral("fieldRepair")).toObject();
    const double counterCooldown = countermeasure.value(QStringLiteral("cooldownRemaining"))
                                       .toDouble(m_countermeasure.cooldownRemaining);
    const int counterRemaining = countermeasure.value(QStringLiteral("remaining"))
                                     .toInt(m_countermeasure.remaining);
    const double scanCooldown = scan.value(QStringLiteral("cooldownRemaining"))
                                    .toDouble(m_scan.cooldownRemaining);
    const double repairCooldown = repair.value(QStringLiteral("cooldownRemaining"))
                                      .toDouble(m_repairCooldownRemaining);
    bool sequenceOk = true;
    quint64 repairSequence = m_repairAttemptSequence;
    if (repair.contains(QStringLiteral("attemptSequence"))) {
        repairSequence = repair.value(QStringLiteral("attemptSequence")).toString()
                             .toULongLong(&sequenceOk);
    }
    if (!finiteNonNegative(counterCooldown)
        || counterCooldown > m_countermeasure.cooldownSec + 1e-9
        || (m_countermeasure.capacity >= 0
            && (counterRemaining < 0 || counterRemaining > m_countermeasure.capacity))
        || !finiteNonNegative(scanCooldown)
        || scanCooldown > m_scan.cooldownSec + 1e-9
        || !finiteNonNegative(repairCooldown)
        || repairCooldown > m_repairCooldownSec + 1e-9 || !sequenceOk) {
        return fail(QStringLiteral("技能"));
    }

    const QJsonObject service = state.value(QStringLiteral("service")).toObject();
    const bool serviceActive = service.value(QStringLiteral("active")).toBool(false);
    const QString serviceCpId = service.value(QStringLiteral("cpId")).toString();
    const double serviceElapsed = service.value(QStringLiteral("elapsed")).toDouble(0.0);
    const double serviceDuration = service.value(QStringLiteral("duration")).toDouble(0.0);
    if (!finiteNonNegative(serviceElapsed) || !finiteNonNegative(serviceDuration)
        || serviceElapsed > serviceDuration + 1e-9
        || (serviceActive && (serviceCpId.isEmpty() || serviceDuration < 3.0
                              || serviceDuration > 45.0))) {
        return fail(QStringLiteral("补充"));
    }

    m_fuelCapacity = fuelCapacity;
    m_fuelRemaining = fuelRemaining;
    m_economyCruiseSpeed = economySpeed;
    m_fuelBurnRate = burnRate;
    m_fuelWarningStage = state.value(QStringLiteral("fuelWarningStage")).toInt(0);
    m_countermeasure.cooldownRemaining = counterCooldown;
    m_countermeasure.remaining = counterRemaining;
    m_scan.cooldownRemaining = scanCooldown;
    m_repairCooldownRemaining = repairCooldown;
    m_repairAttemptSequence = repairSequence;
    m_serviceRequested = serviceActive;
    m_serviceCpId = serviceActive ? serviceCpId : QString();
    m_serviceElapsed = serviceElapsed;
    m_serviceDuration = serviceDuration;
    emit runtimeStateChanged();
    return true;
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
    if (m.payload.value(QStringLiteral("notificationOnly")).toBool()) return;
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
            {QStringLiteral("runtimeState"), runtimeStateJson()},
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
    // Schema-2 checkpoints did not have the atomic service model. They are
    // upgraded by cancelling any legacy in-progress service action.
    m_serviceRequested = false;
    m_serviceCpId.clear();
    m_serviceElapsed = 0.0;
    m_serviceDuration = 0.0;
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
    if (!restoreRuntimeState(state.value(QStringLiteral("runtimeState")).toObject(), error)) {
        return false;
    }
    applyJamming(restoredJamFactor);
    recomputeEffectiveParameters();
    setStatus(state.value(QStringLiteral("status")).toString());
    emit sharedKnowledgeChanged();
    emit recentPathChanged();
    emit damageStateChanged();
    emit runtimeStateChanged();
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
