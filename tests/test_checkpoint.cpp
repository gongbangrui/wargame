#include <gtest/gtest.h>

#include "core/SimulationEngine.h"
#include "units/AttackUAV.h"

using namespace gbr;

TEST(CheckpointTest, RestoresBehaviorAndAuthoritativeTime) {
    SimulationEngine source;
    source.loadDefaultScenario();
    auto* attacker = dynamic_cast<AttackUAV*>(source.unit(QStringLiteral("red_a1")));
    ASSERT_NE(attacker, nullptr);
    attacker->clearSchedule();
    attacker->setHp(73.0);

    ASSERT_TRUE(source.executeCommand(
        QStringLiteral("assignTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")}}).accepted);
    ASSERT_TRUE(source.executeCommand(
        QStringLiteral("setFlightPlan"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("waypoints"),
                     QVariantList{QVariantMap{{QStringLiteral("x"), 9000.0},
                                              {QStringLiteral("y"), 9000.0}}}}}).accepted);
    source.stepOnce(2.0);

    const QJsonArray checkpoint = source.collectCheckpointState();
    const double checkpointTime = source.simTime();
    const GeoPos checkpointPosition = attacker->pos();

    SimulationEngine restored;
    ASSERT_TRUE(restored.setScenario(source.scenario()));
    QString error;
    ASSERT_TRUE(restored.restoreCheckpointState(checkpoint, checkpointTime,
                                                false, 2.0, &error))
        << error.toStdString();
    auto* restoredAttacker = dynamic_cast<AttackUAV*>(
        restored.unit(QStringLiteral("red_a1")));
    ASSERT_NE(restoredAttacker, nullptr);
    EXPECT_DOUBLE_EQ(restored.simTime(), checkpointTime);
    EXPECT_DOUBLE_EQ(restored.speedMul(), 2.0);
    EXPECT_DOUBLE_EQ(restoredAttacker->hp(), 73.0);
    EXPECT_DOUBLE_EQ(restoredAttacker->pos().x, checkpointPosition.x);
    EXPECT_DOUBLE_EQ(restoredAttacker->pos().y, checkpointPosition.y);
    EXPECT_EQ(restoredAttacker->targetId(), QStringLiteral("blue_r1"));
    EXPECT_TRUE(restoredAttacker->armed());
    EXPECT_TRUE(restoredAttacker->hasActiveWaypoints());
    EXPECT_EQ(restoredAttacker->ammoRemaining(), attacker->ammoRemaining());
    EXPECT_DOUBLE_EQ(restoredAttacker->cooldownRemaining(), attacker->cooldownRemaining());
    EXPECT_EQ(restoredAttacker->shotSequence(), attacker->shotSequence());
    EXPECT_EQ(restored.combatSeed(), source.combatSeed());
}

TEST(CheckpointTest, RejectsCheckpointWithIncompleteUnitSet) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    QJsonArray checkpoint = engine.collectCheckpointState();
    checkpoint.removeLast();
    QString error;
    EXPECT_FALSE(engine.restoreCheckpointState(checkpoint, 0.0, false, 1.0, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("单元集合")));
}

TEST(CheckpointTest, RejectsCheckpointWithoutEngineRandomState) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    QJsonArray checkpoint = engine.collectCheckpointState();
    checkpoint.removeFirst();
    QString error;

    EXPECT_FALSE(engine.restoreCheckpointState(checkpoint, 0.0, false, 1.0, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("引擎随机状态")));
}

TEST(CheckpointTest, Schema2StyleServiceFlagIsCancelledOnUpgrade) {
    SimulationEngine source;
    source.loadDefaultScenario();
    QJsonArray checkpoint = source.collectCheckpointState();
    for (qsizetype index = 0; index < checkpoint.size(); ++index) {
        QJsonObject unit = checkpoint.at(index).toObject();
        if (unit.value(QStringLiteral("id")).toString() != QLatin1String("red_a1")) continue;
        unit[QStringLiteral("serviceRequested")] = true;
        QJsonObject behavior = unit.value(QStringLiteral("behavior")).toObject();
        behavior[QStringLiteral("fsmState")] = QStringLiteral("servicing");
        behavior[QStringLiteral("turnaroundElapsed")] = 1.25;
        behavior[QStringLiteral("armed")] = false;
        behavior[QStringLiteral("targetId")] = QString();
        unit[QStringLiteral("behavior")] = behavior;
        checkpoint.replace(index, unit);
        break;
    }

    SimulationEngine restored;
    ASSERT_TRUE(restored.setScenario(source.scenario()));
    QString error;
    ASSERT_TRUE(restored.restoreCheckpointState(checkpoint, 4.0, false, 1.0, &error))
        << error.toStdString();
    auto* attacker = dynamic_cast<AttackUAV*>(restored.unit(QStringLiteral("red_a1")));
    ASSERT_NE(attacker, nullptr);
    EXPECT_FALSE(attacker->serviceRequested());
    EXPECT_DOUBLE_EQ(attacker->turnaroundElapsed(), 0.0);
    EXPECT_EQ(attacker->checkpointState().value(QStringLiteral("behavior")).toObject()
                  .value(QStringLiteral("fsmState")).toString(),
              QStringLiteral("servicing"));
}

TEST(CheckpointTest, Schema3RestoresProjectilesScansFuelAndAtomicService) {
    Scenario scenario = ScenarioIo::defaultScenario();
    GeoPos redCpPosition;
    for (const ScenarioUnit& configured : scenario.units) {
        if (configured.id == QLatin1String("red_cp")) redCpPosition = configured.pos;
    }
    for (ScenarioUnit& configured : scenario.units) {
        configured.schedule.clear();
        if (configured.id == QLatin1String("red_a1")) {
            configured.pos = GeoPos{5000.0, 5000.0, 2000.0};
            configured.attackRange = 5000.0;
            configured.optimalRange = 2500.0;
        } else if (configured.id == QLatin1String("red_r1")) {
            configured.pos = GeoPos{5000.0, 5500.0, 2000.0};
        } else if (configured.id == QLatin1String("red_g1")) {
            configured.pos = redCpPosition;
            configured.initialFuelSec = 900.0;
        } else if (configured.id == QLatin1String("blue_r1")) {
            configured.pos = GeoPos{6500.0, 5000.0, 2000.0};
        }
    }

    SimulationEngine source;
    ASSERT_TRUE(source.setScenario(scenario));
    ASSERT_TRUE(source.executeCommand(
        QStringLiteral("service"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_g1")}}).accepted);
    ASSERT_TRUE(source.executeCommand(
        QStringLiteral("activateScan"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_r1")}}).accepted);
    ASSERT_TRUE(source.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")}}).accepted);
    source.stepOnce(0.1);
    ASSERT_EQ(source.projectilesSnapshot().size(), 1);
    ASSERT_FALSE(source.activeScanContacts().isEmpty());
    ASSERT_TRUE(source.unit(QStringLiteral("red_g1"))->serviceRequested());

    const QJsonArray units = source.collectCheckpointState();
    const QJsonObject global = source.collectGlobalCheckpointState();
    SimulationEngine restored;
    ASSERT_TRUE(restored.setScenario(scenario));
    QString error;
    ASSERT_TRUE(restored.restoreCheckpointState(units, global, source.simTime(),
                                                false, source.speedMul(), &error))
        << error.toStdString();
    EXPECT_EQ(restored.projectilesSnapshot(), source.projectilesSnapshot());
    EXPECT_EQ(restored.activeScanContacts(), source.activeScanContacts());
    EXPECT_DOUBLE_EQ(restored.unit(QStringLiteral("red_g1"))->fuelRemaining(),
                     source.unit(QStringLiteral("red_g1"))->fuelRemaining());
    EXPECT_DOUBLE_EQ(restored.unit(QStringLiteral("red_g1"))->serviceElapsed(),
                     source.unit(QStringLiteral("red_g1"))->serviceElapsed());
    EXPECT_TRUE(restored.unit(QStringLiteral("red_g1"))->serviceRequested());

    source.stepOnce(0.1);
    restored.stepOnce(0.1);
    EXPECT_EQ(restored.projectilesSnapshot(), source.projectilesSnapshot());
}

TEST(CheckpointTest, RestoresOutOfRangeSettledProjectileForVisualResidual) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& configured : scenario.units) {
        configured.schedule.clear();
        if (configured.id == QLatin1String("red_a1")) {
            configured.pos = GeoPos{5000.0, 5000.0, 2000.0};
            configured.attackRange = 5000.0;
        } else if (configured.id == QLatin1String("blue_r1")) {
            configured.pos = GeoPos{6500.0, 5000.0, 2000.0};
        }
    }

    SimulationEngine source;
    ASSERT_TRUE(source.setScenario(scenario));
    ASSERT_TRUE(source.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")}}).accepted);
    source.stepOnce(0.1);
    QJsonObject global = source.collectGlobalCheckpointState();
    QJsonArray projectiles = global.value(QStringLiteral("projectiles")).toArray();
    ASSERT_EQ(projectiles.size(), 1);
    QJsonObject projectile = projectiles.first().toObject();
    projectile[QStringLiteral("active")] = false;
    projectile[QStringLiteral("terminalReason")] = QStringLiteral("out_of_range");
    projectile[QStringLiteral("resultSettled")] = true;
    projectiles[0] = projectile;
    global[QStringLiteral("projectiles")] = projectiles;

    SimulationEngine restored;
    ASSERT_TRUE(restored.setScenario(scenario));
    QString error;
    ASSERT_TRUE(restored.restoreCheckpointState(source.collectCheckpointState(), global,
                                                source.simTime(), false, 1.0, &error))
        << error.toStdString();
    ASSERT_EQ(restored.projectilesSnapshot().size(), 1);
    const QJsonObject restoredProjectile = restored.projectilesSnapshot().first().toObject();
    EXPECT_FALSE(restoredProjectile.value(QStringLiteral("active")).toBool());
    EXPECT_EQ(restoredProjectile.value(QStringLiteral("terminalReason")).toString(),
              QStringLiteral("out_of_range"));
}

TEST(CheckpointTest, MissingGlobalSchemaUpgradesWithEmptyTransientState) {
    SimulationEngine source;
    source.loadDefaultScenario();
    QJsonArray units = source.collectCheckpointState();
    for (qsizetype index = 0; index < units.size(); ++index) {
        QJsonObject state = units.at(index).toObject();
        if (state.contains(QStringLiteral("id"))) {
            state.remove(QStringLiteral("runtimeState"));
            state[QStringLiteral("serviceRequested")] = true;
            units.replace(index, state);
        }
    }

    SimulationEngine restored;
    ASSERT_TRUE(restored.setScenario(source.scenario()));
    QString error;
    ASSERT_TRUE(restored.restoreCheckpointState(units, 0.0, false, 1.0, &error))
        << error.toStdString();
    EXPECT_TRUE(restored.projectilesSnapshot().isEmpty());
    EXPECT_TRUE(restored.activeScanContacts().isEmpty());
    for (const QString& id : restored.unitIds()) {
        EXPECT_FALSE(restored.unit(id)->serviceRequested());
        if (restored.unit(id)->movable()) {
            EXPECT_DOUBLE_EQ(restored.unit(id)->fuelRemaining(),
                             restored.unit(id)->fuelCapacity());
        }
    }
}
