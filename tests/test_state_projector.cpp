#include <gtest/gtest.h>

#include "StateProjector.h"
#include "core/Scenario.h"
#include "core/SimulationEngine.h"

#include <QJsonDocument>

using namespace gbr;

namespace {

QJsonObject roomState() {
    return {{QStringLiteral("scenarioRevision"), 1},
            {QStringLiteral("phase"), QStringLiteral("running")},
            {QStringLiteral("simTime"), 0.0}};
}

QJsonObject unitById(const QJsonArray& units, const QString& id) {
    for (const QJsonValue& value : units) {
        if (value.toObject().value(QStringLiteral("id")).toString() == id) {
            return value.toObject();
        }
    }
    return {};
}

} // namespace

TEST(StateProjectorTest, PermissionMatrixIsServerOwned) {
    EXPECT_TRUE(StateProjector::canControlSide(QStringLiteral("red"), QStringLiteral("red")));
    EXPECT_FALSE(StateProjector::canControlSide(QStringLiteral("red"), QStringLiteral("blue")));
    EXPECT_TRUE(StateProjector::canControlSide(QStringLiteral("director"), QStringLiteral("blue")));
    EXPECT_FALSE(StateProjector::canControlSide(QStringLiteral("editor"), QStringLiteral("red")));
    EXPECT_TRUE(StateProjector::canEditSide(QStringLiteral("editor"), QStringLiteral("blue")));
    EXPECT_TRUE(StateProjector::canEditSide(QStringLiteral("red"), QStringLiteral("red")));
    EXPECT_FALSE(StateProjector::canEditSide(QStringLiteral("red"), QStringLiteral("blue")));
}

TEST(StateProjectorTest, HiddenEnemyDoesNotAppearInRawFactionSnapshot) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const QJsonObject red = StateProjector::snapshotFor(
        engine, QStringLiteral("red"), 1, roomState());
    const QByteArray encoded = QJsonDocument(red).toJson(QJsonDocument::Compact);
    EXPECT_FALSE(encoded.contains("blue_cp"));
    EXPECT_FALSE(encoded.contains("blue_r1"));
    EXPECT_FALSE(encoded.contains("blue_a1"));
}

TEST(StateProjectorTest, ObservedEnemyExcludesPrivateBehaviorState) {
    SimulationEngine engine;
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.id == QLatin1String("blue_r1")) {
            unit.pos = GeoPos{4500.0, 4000.0, 3000.0};
            unit.schedule = {{0.0, 4500.0, 4000.0}, {10.0, 9000.0, 9000.0}};
        }
    }
    ASSERT_TRUE(engine.setScenario(scenario));
    engine.stepOnce(0.05);

    const QJsonObject red = StateProjector::snapshotFor(
        engine, QStringLiteral("red"), 2, roomState());
    const QJsonObject enemyRuntime = unitById(
        red.value(QStringLiteral("units")).toArray(), QStringLiteral("blue_r1"));
    ASSERT_FALSE(enemyRuntime.isEmpty());
    EXPECT_TRUE(enemyRuntime.contains(QStringLiteral("position")));
    EXPECT_TRUE(enemyRuntime.contains(QStringLiteral("hp")));
    EXPECT_FALSE(enemyRuntime.contains(QStringLiteral("schedule")));
    EXPECT_FALSE(enemyRuntime.contains(QStringLiteral("recentPath")));
    EXPECT_FALSE(enemyRuntime.contains(QStringLiteral("sharedKnowledge")));
    EXPECT_FALSE(enemyRuntime.contains(QStringLiteral("detections")));
    EXPECT_FALSE(enemyRuntime.contains(QStringLiteral("detectRange")));
    EXPECT_EQ(enemyRuntime.value(QStringLiteral("status")).toString(),
              QStringLiteral("已探测"));

    const QJsonObject enemyScenario = unitById(
        red.value(QStringLiteral("scenario")).toObject()
            .value(QStringLiteral("units")).toArray(), QStringLiteral("blue_r1"));
    ASSERT_FALSE(enemyScenario.isEmpty());
    EXPECT_TRUE(enemyScenario.value(QStringLiteral("schedule")).toArray().isEmpty());
    EXPECT_DOUBLE_EQ(enemyScenario.value(QStringLiteral("detectRange")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(enemyScenario.value(QStringLiteral("attackPower")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(enemyScenario.value(QStringLiteral("hitProbability")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(enemyScenario.value(QStringLiteral("damageMax")).toDouble(), 0.0);
    EXPECT_EQ(enemyScenario.value(QStringLiteral("ammoCapacity")).toInt(), 0);
    EXPECT_FALSE(enemyRuntime.contains(QStringLiteral("ammoRemaining")));
    EXPECT_FALSE(enemyRuntime.contains(QStringLiteral("cooldownRemaining")));
}

TEST(StateProjectorTest, DirectorRetainsFullRuntimeAndScenario) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const QJsonObject director = StateProjector::snapshotFor(
        engine, QStringLiteral("director"), 1, roomState());
    const QJsonObject runtime = unitById(
        director.value(QStringLiteral("units")).toArray(), QStringLiteral("blue_r1"));
    const QJsonObject scenario = unitById(
        director.value(QStringLiteral("scenario")).toObject()
            .value(QStringLiteral("units")).toArray(), QStringLiteral("blue_r1"));
    EXPECT_TRUE(runtime.contains(QStringLiteral("sharedKnowledge")));
    EXPECT_TRUE(runtime.contains(QStringLiteral("schedule")));
    EXPECT_FALSE(scenario.value(QStringLiteral("schedule")).toArray().isEmpty());
}

TEST(StateProjectorTest, FactionEventRedactsUndetectedEnemyIdentity) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const QJsonObject event{{QStringLiteral("kind"), QStringLiteral("simulationEvent")},
                            {QStringLiteral("sourceUnitId"), QStringLiteral("blue_r1")},
                            {QStringLiteral("body"),
                             QStringLiteral("蓝方单元 blue_r1 被 red_a1 摧毁")}};
    const QJsonObject projected = StateProjector::projectEvent(
        engine, QStringLiteral("blue"), event);
    ASSERT_FALSE(projected.isEmpty());
    EXPECT_TRUE(projected.value(QStringLiteral("body")).toString()
                    .contains(QStringLiteral("blue_r1")));
    EXPECT_FALSE(projected.value(QStringLiteral("body")).toString()
                     .contains(QStringLiteral("red_a1")));
    EXPECT_TRUE(projected.value(QStringLiteral("body")).toString()
                    .contains(QStringLiteral("未知单元")));
    EXPECT_EQ(StateProjector::projectEvent(engine, QStringLiteral("director"), event), event);
}
