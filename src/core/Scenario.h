#pragma once

#include "Geo.h"
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QVariantList>
#include <vector>
#include <memory>

namespace gbr {

class UnitBase;

struct SchedulePoint {
    double time = 0.0;
    double x = 0.0;
    double y = 0.0;
};

struct ScenarioUnit {
    QString id;
    QString callsign;
    QString kind;
    QString side;
    GeoPos pos;
    double detectRange = 5000;
    double attackRange = 1500;
    double commRange = 20000;
    double speed = 50;
    double maxHp = 100;
    /// Fraction of incoming hull damage absorbed, in [0, 0.9].
    double armor = 0.0;
    /// Hull HP and subsystem health restored per simulation second at a live CP.
    double repairRate = 2.0;
    double subsystemRepairRate = 0.02;
    /// @brief Damage per hit applied by this unit when attacking.
    /// @details Only AttackUAV currently uses this in stepCombat(); other
    /// units store the field for forward-compatibility (e.g., a future
    /// GroundScout anti-air variant).
    double attackPower = 100;
    int ammoCapacity = 4;
    int initialAmmo = 4;
    double hitProbability = 1.0;
    double optimalRange = 1500.0;
    double minAttackRange = 0.0;
    double cooldownSec = 1.0;
    double damageMin = 100.0;
    double damageMax = 100.0;
    /// Probability loss across the interval from optimalRange to attackRange.
    double rangeFalloff = 0.0;
    /// Attack-UAV endurance and turnaround configuration.
    double fuelCapacitySec = 1800.0;
    double initialFuelSec = 1800.0;
    double rearmDurationSec = 12.0;
    std::vector<SchedulePoint> schedule;
};

struct ScenarioMap {
    QString name = QStringLiteral("default");
    double widthMeters = 40000.0;
    double heightMeters = 30000.0;
    QString backgroundResource;
};

struct Scenario {
    ScenarioMap map;
    std::vector<ScenarioUnit> units;
    QString notes;
};

class ScenarioIo : public QObject {
    Q_OBJECT
public:
    static constexpr int SchemaVersion = 3;
    static Scenario loadFromFile(const QString& path, QString* err = nullptr);
    static bool saveToFile(const Scenario& s, const QString& path, QString* err = nullptr);
    static QJsonObject toJson(const Scenario& s);
    static Scenario fromJson(const QJsonObject& o);

    static Scenario defaultScenario();
};

} // namespace gbr
