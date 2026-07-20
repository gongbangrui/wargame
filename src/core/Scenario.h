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
    /// @brief Damage per hit applied by this unit when attacking.
    /// @details Only AttackUAV currently uses this in stepCombat(); other
    /// units store the field for forward-compatibility (e.g., a future
    /// GroundScout anti-air variant).
    double attackPower = 100;
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
    static constexpr int SchemaVersion = 1;
    static Scenario loadFromFile(const QString& path, QString* err = nullptr);
    static bool saveToFile(const Scenario& s, const QString& path, QString* err = nullptr);
    static QJsonObject toJson(const Scenario& s);
    static Scenario fromJson(const QJsonObject& o);

    static Scenario defaultScenario();
};

} // namespace gbr
