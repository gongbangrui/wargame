#pragma once

#include "Geo.h"
#include "MessageBus.h"
#include "Scenario.h"
#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QPointF>
#include <deque>
#include <vector>
#include <memory>
#include <functional>
#include <cmath>

namespace gbr {

/// @brief Faction side enumeration.
enum class Side { Red, Blue };

/// @brief Convert Side enum to string identifier ("red"/"blue").
inline QString sideName(Side s) {
    return s == Side::Red ? QStringLiteral("red") : QStringLiteral("blue");
}

/// @brief Parse side string to enum, defaulting to Blue.
inline Side sideFromName(const QString& n) {
    return n == QStringLiteral("red") ? Side::Red : Side::Blue;
}

/// @brief Unit type enumeration for factory creation and filtering.
enum class UnitKind {
    CommandPost,
    ReconUAV,
    AttackUAV,
    GroundScout,
    JammerUAV,
};

/// @brief Ownership of a unit's runtime state.
/// @details In single-process mode every unit is `Local`. When networking is
/// added, units received from a remote peer will be `Remote` and the local
/// process will not tick / re-emit on their behalf.
enum class UnitOwner { Local, Remote };

/// @brief Convert UnitKind to JSON string key.
inline QString kindName(UnitKind k) {
    switch (k) {
    case UnitKind::CommandPost: return QStringLiteral("commandpost");
    case UnitKind::ReconUAV: return QStringLiteral("reconuav");
    case UnitKind::AttackUAV: return QStringLiteral("attackuav");
    case UnitKind::GroundScout: return QStringLiteral("groundscout");
    case UnitKind::JammerUAV: return QStringLiteral("jammeruav");
    }
    return QStringLiteral("unknown");
}

/// @brief Parse kind string to enum, defaulting to CommandPost.
inline UnitKind kindFromName(const QString& n) {
    if (n == QStringLiteral("commandpost")) return UnitKind::CommandPost;
    if (n == QStringLiteral("reconuav")) return UnitKind::ReconUAV;
    if (n == QStringLiteral("attackuav")) return UnitKind::AttackUAV;
    if (n == QStringLiteral("groundscout")) return UnitKind::GroundScout;
    if (n == QStringLiteral("jammeruav")) return UnitKind::JammerUAV;
    return UnitKind::CommandPost;
}

/// @brief Abstract base class for all simulation units.
/// @details Owns common properties (position, HP, ranges), handles message
/// subscription, and provides the virtual tick/message interface for subclasses.
class UnitBase : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(QString callsign READ callsign WRITE setCallsign NOTIFY callsignChanged)
    Q_PROPERTY(QString side READ sideStr NOTIFY sideChanged)
    Q_PROPERTY(QString kind READ kindStr CONSTANT)
    Q_PROPERTY(QVariantList position READ position NOTIFY positionChanged)
    Q_PROPERTY(QJsonObject perception READ perceptionJson NOTIFY perceptionChanged)
    Q_PROPERTY(QJsonObject sharedKnowledge READ sharedKnowledgeJson NOTIFY sharedKnowledgeChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(double detectRange READ detectRange WRITE setDetectRange NOTIFY paramsChanged)
    Q_PROPERTY(double attackRange READ attackRange WRITE setAttackRange NOTIFY paramsChanged)
    Q_PROPERTY(double commRange READ commRange WRITE setCommRange NOTIFY paramsChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY paramsChanged)
    Q_PROPERTY(double maxHp READ maxHp WRITE setMaxHp NOTIFY paramsChanged)
    Q_PROPERTY(double attackPower READ attackPower WRITE setAttackPower NOTIFY paramsChanged)
    Q_PROPERTY(double armor READ armor NOTIFY damageStateChanged)
    Q_PROPERTY(double hp READ hp NOTIFY hpChanged)
    Q_PROPERTY(bool alive READ alive NOTIFY hpChanged)
    Q_PROPERTY(bool disabled READ disabled NOTIFY damageStateChanged)
    Q_PROPERTY(QJsonObject subsystems READ subsystemStateJson NOTIFY damageStateChanged)
    Q_PROPERTY(bool movable READ movable CONSTANT)
    Q_PROPERTY(QVariantList recentPath READ recentPath NOTIFY recentPathChanged)
public:
    /// @brief Unit construction parameters with sensible defaults.
    struct Params {
        double detectRange = 5000.0;
        double attackRange = 1500.0;
        double commRange = 20000.0;
        double speed = 50.0;
        double maxHp = 100.0;
        double attackPower = 100.0;
        double armor = 0.0;
        double repairRate = 2.0;
        double subsystemRepairRate = 0.02;
        GeoPos pos;
    };

    struct DamageDelta {
        double hullDamage = 0.0;
        double sensorLoss = 0.0;
        double commsLoss = 0.0;
        double mobilityLoss = 0.0;
        double weaponLoss = 0.0;
    };

    /// @brief Construct unit and subscribe to message bus.
    /// @details owner defaults to Local (single-process). Pass Remote when
    /// constructing a unit whose runtime state is owned by a peer; the local
    /// process will not tick such units.
    UnitBase(const QString& id, UnitKind kind, Side side, MessageBus* bus,
            QObject* parent = nullptr,
            UnitOwner owner = UnitOwner::Local);
    virtual ~UnitBase();

    /// @brief Apply a full parameter set, clamping HP to new maxHp.
    virtual void setParams(const Params& p);
    /// @brief Set position and notify bus of the change.
    void setPosition(const GeoPos& pos);
    /// @brief Per-tick simulation update, called by the engine.
    virtual void onTick(double simSeconds) = 0;

    QString id() const { return m_id; }
    QString callsign() const { return m_callsign; }
    void setCallsign(const QString& s);
    QString sideStr() const { return sideName(m_side); }
    QString kindStr() const { return kindName(m_kind); }
    UnitKind kind() const { return m_kind; }
    Side side() const { return m_side; }
    bool movable() const { return m_kind != UnitKind::CommandPost; }

    QVariantList position() const;
    QJsonObject perceptionJson() const;
    QJsonObject sharedKnowledgeJson() const;
    QVariantList recentPath() const;
    /// @brief Sample current position into recent path buffer, throttled to ~5Hz.
    void sampleRecentPath(double simTime);
    QString statusText() const { return m_status; }
    void setStatus(const QString& s);

    double detectRange() const { return m_params.detectRange; }
    double attackRange() const { return m_params.attackRange; }
    double commRange() const { return m_params.commRange; }
    double baseDetectRange() const { return m_baseDetectRange; }
    double baseCommRange() const { return m_baseCommRange; }
    double baseAttackRange() const { return m_baseAttackRange; }
    double baseSpeed() const { return m_baseSpeed; }
    double speed() const { return m_params.speed; }
    double maxHp() const { return m_params.maxHp; }
    double attackPower() const { return m_params.attackPower; }
    double armor() const { return m_armor; }
    double sensorHealth() const { return m_sensorHealth; }
    double commsHealth() const { return m_commsHealth; }
    double mobilityHealth() const { return m_mobilityHealth; }
    double weaponHealth() const { return m_weaponHealth; }
    double weaponEffectiveness() const { return m_weaponHealth; }
    bool disabled() const;
    QJsonObject subsystemStateJson() const;
    bool restoreSubsystemState(const QJsonObject& state);
    virtual void setAttackPower(double v);
    double hp() const { return m_hp; }
    bool alive() const { return m_hp > 0.0; }

    void setDetectRange(double v);
    void setAttackRange(double v);
    void setCommRange(double v);
    void setSpeed(double v);
    void setMaxHp(double v) {
        if (!std::isfinite(v) || v <= 0.0) return;
        m_params.maxHp = v;
        if (m_hp > v) {
            m_hp = v;
            m_lastNotifiedHp = m_hp;
            emit hpChanged();
        }
        emit paramsChanged();
    }
    void setHp(double v) {
        if (!std::isfinite(v)) return;
        double c = std::max(0.0, std::min(v, m_params.maxHp));
        if (m_hp != c) {
            const bool wasAlive = alive();
            m_hp = c;
            // Coalesce tiny updates, but compare against the last value that
            // observers actually saw so repeated small damage is not hidden.
            if (wasAlive != alive() || std::abs(c - m_lastNotifiedHp) >= 0.5) {
                m_lastNotifiedHp = c;
                emit hpChanged();
            }
        }
    }

    /// @brief Get current jamming factor (1.0 = no jamming, lower = more jamming).
    double jamFactor() const { return m_jamFactor; }

    /// @brief Apply ECM jamming factor (0.0 = full jamming, 1.0 = no effect).
    /// Reduces effective comm/detect ranges by the given factor.
    virtual void applyJamming(double factor);

    /// Compute and apply deterministic damage in two separate phases.
    DamageDelta assessDamage(double incomingDamage, int subsystemIndex) const;
    void applyDamageDelta(const DamageDelta& delta);
    /// Restore hull and subsystems while the unit is at a live command post.
    virtual bool serviceTick(double dt);
    void requestService(bool value) { m_serviceRequested = value; }
    bool serviceRequested() const { return m_serviceRequested; }
    double serviceProgress() const;

    void handleMessage(const Message& m);
    bool hasActiveWaypoints() const { return m_hasActiveWaypoints; }
    void setHasActiveWaypoints(bool v) { m_hasActiveWaypoints = v; }
    /// @brief Cancel imperative waypoint motion before a schedule takes ownership.
    virtual void cancelWaypointMotion() { setHasActiveWaypoints(false); }

    bool canDetect(const GeoPos& pos) const;
    GeoPos pos() const { return m_params.pos; }
    const Params& params() const { return m_params; }

    const std::vector<SchedulePoint>& schedule() const { return m_schedule; }
    void setSchedule(const std::vector<SchedulePoint>& s);
    void clearSchedule() { m_schedule.clear(); }

    void setCpId(const QString& cpId) { m_cpId = cpId; }
    QString cpId() const { return m_cpId; }

    void setUnitLookup(std::function<UnitBase*(const QString&)> fn) { m_lookup = std::move(fn); }
    UnitBase* findUnit(const QString& id) const { return m_lookup ? m_lookup(id) : nullptr; }

    /// @brief Owner of this unit's runtime state (default Local).
    UnitOwner owner() const { return m_owner; }
    void setOwner(UnitOwner o) { m_owner = o; }

    /// Server-only durable runtime state. This is intentionally separate from
    /// role-filtered network snapshots because it contains private behavior data.
    QJsonObject checkpointState() const;
    bool restoreCheckpointState(const QJsonObject& state, QString* error = nullptr);

    /// @brief Factory method to create the correct subclass for a given UnitKind.
    static std::unique_ptr<UnitBase> create(const QString& id, UnitKind kind, Side side, MessageBus* bus, QObject* parent = nullptr);

signals:
    void callsignChanged();
    void sideChanged();
    void positionChanged();
    void perceptionChanged();
    void sharedKnowledgeChanged();
    void statusChanged();
    void paramsChanged();
    void hpChanged();
    void damageStateChanged();
    void recentPathChanged();
    /// @brief UI event notification (e.g. target detected, unit destroyed).
    void notifyEvent(const QString& title, const QString& body, const QString& level);
    void strikeOrderRequested(const QString& targetId);
    void routeChangeRequested();
    void attackProgress(double distanceToTarget, double attackRange);

protected:
    virtual void onMessage(const Message& m);
    virtual QJsonObject behaviorCheckpoint() const { return {}; }
    virtual bool restoreBehaviorCheckpoint(const QJsonObject& state, QString* error);

    void send(const Message& m) { if (m_bus) m_bus->send(m); }
    void rememberShared(const QString& key, const QJsonValue& v);

    QString m_id;
    QString m_callsign;
    UnitKind m_kind;
    Side m_side;
    UnitOwner m_owner = UnitOwner::Local;
    Params m_params;
    double m_hp = 100.0;
    double m_lastNotifiedHp = 100.0;
    QString m_status;
    QJsonObject m_sharedKnowledge;
    MessageBus* m_bus = nullptr;
    std::vector<SchedulePoint> m_schedule;
    bool m_hasActiveWaypoints = false;
    std::deque<QPointF> m_recentPath;
    double m_lastSampleTime = -1.0;
    double m_jamFactor = 1.0;
    double m_baseDetectRange = 5000.0;
    double m_baseCommRange = 20000.0;
    double m_baseAttackRange = 1500.0;
    double m_baseSpeed = 50.0;
    double m_baseAttackPower = 100.0;
    double m_armor = 0.0;
    double m_repairRate = 2.0;
    double m_subsystemRepairRate = 0.02;
    double m_sensorHealth = 1.0;
    double m_commsHealth = 1.0;
    double m_mobilityHealth = 1.0;
    double m_weaponHealth = 1.0;
    bool m_serviceRequested = false;
    QString m_cpId;
    std::function<UnitBase*(const QString&)> m_lookup;

    void recomputeEffectiveParameters();
};

} // namespace gbr
