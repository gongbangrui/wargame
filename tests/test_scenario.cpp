#include <gtest/gtest.h>
#include "core/Scenario.h"
#include "core/SimulationEngine.h"

#include <cmath>
#include <limits>
#include <QStringList>

using namespace gbr;

TEST(ScenarioTest, DefaultScenarioHasUnits) {
    Scenario s = ScenarioIo::defaultScenario();
    EXPECT_GE(s.units.size(), 8);
}

TEST(ScenarioTest, DefaultScenarioHasRedAndBlueCp) {
    Scenario s = ScenarioIo::defaultScenario();
    int redCp = 0, blueCp = 0;
    for (const auto& u : s.units) {
        if (u.kind == "commandpost") {
            if (u.side == "red") redCp++;
            else if (u.side == "blue") blueCp++;
        }
    }
    EXPECT_EQ(redCp, 1);
    EXPECT_EQ(blueCp, 1);
}

TEST(ScenarioTest, DefaultScenarioHasJammers) {
    Scenario s = ScenarioIo::defaultScenario();
    int jammerCount = 0;
    for (const auto& u : s.units) {
        if (u.kind == "jammeruav") jammerCount++;
    }
    EXPECT_GE(jammerCount, 2);
}

TEST(ScenarioTest, ToJsonThenFromJsonRoundTrip) {
    Scenario s = ScenarioIo::defaultScenario();
    QJsonObject json = ScenarioIo::toJson(s);
    EXPECT_EQ(json.value("schemaVersion").toInt(), ScenarioIo::SchemaVersion);
    Scenario s2 = ScenarioIo::fromJson(json);

    EXPECT_EQ(s.units.size(), s2.units.size());
    EXPECT_EQ(s.map.widthMeters, s2.map.widthMeters);
    EXPECT_EQ(s.map.heightMeters, s2.map.heightMeters);

    for (size_t i = 0; i < s.units.size(); i++) {
        EXPECT_EQ(s.units[i].id, s2.units[i].id);
        EXPECT_EQ(s.units[i].kind, s2.units[i].kind);
        EXPECT_EQ(s.units[i].side, s2.units[i].side);
        EXPECT_DOUBLE_EQ(s.units[i].pos.x, s2.units[i].pos.x);
        EXPECT_DOUBLE_EQ(s.units[i].pos.y, s2.units[i].pos.y);
        EXPECT_DOUBLE_EQ(s.units[i].detectRange, s2.units[i].detectRange);
    }
}

TEST(ScenarioTest, EmptyUnitsJson) {
    QJsonObject empty;
    Scenario s = ScenarioIo::fromJson(empty);
    EXPECT_TRUE(s.units.empty());
}

TEST(ScenarioTest, DefaultMapSize) {
    Scenario s = ScenarioIo::defaultScenario();
    EXPECT_DOUBLE_EQ(s.map.widthMeters, 20000);
    EXPECT_DOUBLE_EQ(s.map.heightMeters, 15000);
}

TEST(ScenarioTest, FromJsonRejectsMissingOrEmptyRequiredUnitIdentifiersBeforeEngineRegistration) {
    const QJsonObject validUnit{{QStringLiteral("id"), QStringLiteral("red_a1")},
                                {QStringLiteral("kind"), QStringLiteral("attackuav")},
                                {QStringLiteral("side"), QStringLiteral("red")}};

    for (const QString& field : {QStringLiteral("id"), QStringLiteral("kind"),
                                 QStringLiteral("side")}) {
        for (const bool empty : {false, true}) {
            QJsonObject unit = validUnit;
            if (empty) unit.insert(field, QString());
            else unit.remove(field);

            QString error;
            const Scenario parsed = ScenarioIo::fromJson(
                QJsonObject{{QStringLiteral("units"), QJsonArray{unit}}}, &error);

            EXPECT_TRUE(parsed.units.empty()) << field.toStdString();
            EXPECT_FALSE(error.isEmpty()) << field.toStdString();
        }
    }

    SimulationEngine engine;
    engine.loadDefaultScenario();
    const qsizetype unitsBefore = engine.unitIds().size();
    QString error;
    const QJsonObject malformedScheduleUnit{
        {QStringLiteral("id"), QStringLiteral("red_a1")},
        {QStringLiteral("kind"), QStringLiteral("attackuav")},
        {QStringLiteral("side"), QStringLiteral("red")},
        {QStringLiteral("schedule"), QJsonArray{QStringLiteral("not-a-schedule-point")}}};
    const Scenario rejected = ScenarioIo::fromJson(
        QJsonObject{{QStringLiteral("units"), QJsonArray{malformedScheduleUnit}}}, &error);

    EXPECT_FALSE(engine.setScenario(rejected));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(engine.unitIds().size(), unitsBefore);
    EXPECT_NE(engine.unit(QStringLiteral("red_cp")), nullptr);
}

TEST(ScenarioTest, FromJsonRejectsDuplicateUnitIds) {
    const QJsonObject unit{{QStringLiteral("id"), QStringLiteral("red_a1")},
                           {QStringLiteral("kind"), QStringLiteral("attackuav")},
                           {QStringLiteral("side"), QStringLiteral("red")}};
    QString error;
    const Scenario parsed = ScenarioIo::fromJson(
        QJsonObject{{QStringLiteral("units"), QJsonArray{unit, unit}}}, &error);

    EXPECT_TRUE(parsed.units.empty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(ScenarioTest, FromJsonRejectsNonFiniteParserNumericScalars) {
    const QJsonObject validUnit{{QStringLiteral("id"), QStringLiteral("red_a1")},
                                {QStringLiteral("kind"), QStringLiteral("attackuav")},
                                {QStringLiteral("side"), QStringLiteral("red")}};
    const QStringList unitFields{
        QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("alt"),
        QStringLiteral("detectRange"), QStringLiteral("attackRange"),
        QStringLiteral("commRange"), QStringLiteral("speed"), QStringLiteral("maxHp"),
        QStringLiteral("armor"), QStringLiteral("repairRate"),
        QStringLiteral("subsystemRepairRate"), QStringLiteral("attackPower"),
        QStringLiteral("ammoCapacity"), QStringLiteral("initialAmmo"),
        QStringLiteral("hitProbability"), QStringLiteral("optimalRange"),
        QStringLiteral("minAttackRange"), QStringLiteral("cooldownSec"),
        QStringLiteral("damageMin"), QStringLiteral("damageMax"),
        QStringLiteral("rangeFalloff"), QStringLiteral("fuelCapacitySec"),
        QStringLiteral("initialFuelSec"), QStringLiteral("rearmDurationSec")};

    for (const QString& field : unitFields) {
        QJsonObject unit = validUnit;
        unit.insert(field, std::numeric_limits<double>::infinity());
        QString error;
        const Scenario parsed = ScenarioIo::fromJson(
            QJsonObject{{QStringLiteral("units"), QJsonArray{unit}}}, &error);

        EXPECT_TRUE(parsed.units.empty()) << field.toStdString();
        EXPECT_FALSE(error.isEmpty()) << field.toStdString();
    }

    for (const QString& field : {QStringLiteral("widthMeters"), QStringLiteral("heightMeters")}) {
        QString error;
        const Scenario parsed = ScenarioIo::fromJson(
            QJsonObject{{QStringLiteral("map"),
                         QJsonObject{{field, std::numeric_limits<double>::infinity()}}}},
            &error);

        EXPECT_TRUE(parsed.units.empty()) << field.toStdString();
        EXPECT_FALSE(error.isEmpty()) << field.toStdString();
    }
}

TEST(ScenarioTest, FromJsonRejectsMalformedSchedulePoints) {
    const QJsonObject validUnit{{QStringLiteral("id"), QStringLiteral("red_a1")},
                                {QStringLiteral("kind"), QStringLiteral("attackuav")},
                                {QStringLiteral("side"), QStringLiteral("red")}};
    const QJsonArray malformedSchedules{
        QJsonArray{QStringLiteral("not-an-object")},
        QJsonArray{QJsonObject{{QStringLiteral("time"), 1.0},
                               {QStringLiteral("x"), 2.0}}},
        QJsonArray{QJsonObject{{QStringLiteral("time"), 1.0},
                               {QStringLiteral("x"), QStringLiteral("two")},
                               {QStringLiteral("y"), 3.0}}},
        QJsonArray{QJsonObject{{QStringLiteral("time"), 1.0},
                               {QStringLiteral("x"), 2.0},
                               {QStringLiteral("y"), std::numeric_limits<double>::infinity()}}}};

    for (const QJsonValue& schedule : malformedSchedules) {
        QJsonObject unit = validUnit;
        unit.insert(QStringLiteral("schedule"), schedule);
        QString error;
        const Scenario parsed = ScenarioIo::fromJson(
            QJsonObject{{QStringLiteral("units"), QJsonArray{unit}}}, &error);

        EXPECT_TRUE(parsed.units.empty());
        EXPECT_FALSE(error.isEmpty());
    }
}

TEST(ScenarioTest, FromJsonPreservesLegacyOptionalWeaponDefaults) {
    const Scenario parsed = ScenarioIo::fromJson(
        QJsonObject{{QStringLiteral("schemaVersion"), 1},
                    {QStringLiteral("units"),
                     QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("red_a1")},
                                            {QStringLiteral("kind"), QStringLiteral("attackuav")},
                                            {QStringLiteral("side"), QStringLiteral("red")},
                                            {QStringLiteral("attackRange"), 2000.0},
                                            {QStringLiteral("attackPower"), 250.0}}}}});

    ASSERT_EQ(parsed.units.size(), 1u);
    const ScenarioUnit& unit = parsed.units.front();
    EXPECT_EQ(unit.ammoCapacity, 4);
    EXPECT_EQ(unit.initialAmmo, 4);
    EXPECT_DOUBLE_EQ(unit.hitProbability, 1.0);
    EXPECT_DOUBLE_EQ(unit.optimalRange, 2000.0);
    EXPECT_DOUBLE_EQ(unit.minAttackRange, 0.0);
    EXPECT_DOUBLE_EQ(unit.cooldownSec, 4.0);
    EXPECT_DOUBLE_EQ(unit.damageMin, 250.0);
    EXPECT_DOUBLE_EQ(unit.damageMax, 250.0);
    EXPECT_DOUBLE_EQ(unit.rangeFalloff, 0.0);
    EXPECT_DOUBLE_EQ(unit.fuelCapacitySec, 1800.0);
    EXPECT_DOUBLE_EQ(unit.initialFuelSec, 1800.0);
    EXPECT_DOUBLE_EQ(unit.rearmDurationSec, 12.0);
}
