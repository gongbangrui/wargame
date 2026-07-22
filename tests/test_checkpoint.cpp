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
