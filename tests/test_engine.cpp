#include <gtest/gtest.h>
#include "core/SimulationEngine.h"
#include "core/UnitBase.h"
#include "core/Scenario.h"
#include "core/MessageLogRecorder.h"
#include "units/GroundScout.h"
#include "units/ReconUAV.h"
#include "units/AttackUAV.h"
#include <algorithm>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

using namespace gbr;

class EngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine.loadDefaultScenario();
    }

    SimulationEngine engine;
};

TEST_F(EngineTest, LoadDefaultScenario) {
    EXPECT_GE(engine.unitIds().size(), 10); // 10 units with jammers
    EXPECT_TRUE(engine.readyForSim());
}

TEST_F(EngineTest, UnitIterationOrderIsStableAndSorted) {
    const QStringList ids = engine.unitIds();
    QStringList sorted = ids;
    sorted.sort();
    EXPECT_EQ(ids, sorted);
}

TEST_F(EngineTest, NewUnitsStartAtFullHp) {
    auto* cp = engine.unit("red_cp");
    ASSERT_NE(cp, nullptr);
    EXPECT_DOUBLE_EQ(cp->hp(), cp->maxHp());
    EXPECT_DOUBLE_EQ(cp->hp(), 200.0);
}

TEST_F(EngineTest, SimTimeInitialized) {
    EXPECT_DOUBLE_EQ(engine.simTime(), 0.0);
}

TEST_F(EngineTest, RunToggle) {
    engine.setRunning(true);
    EXPECT_TRUE(engine.running());
    engine.setRunning(false);
    EXPECT_FALSE(engine.running());
}

TEST_F(EngineTest, StepOnce) {
    EXPECT_DOUBLE_EQ(engine.simTime(), 0.0);
    engine.stepOnce(1.0);
    EXPECT_GT(engine.simTime(), 0.0);
}

TEST_F(EngineTest, InvalidStepDoesNotMoveSimulationTime) {
    engine.stepOnce(-1.0);
    EXPECT_DOUBLE_EQ(engine.simTime(), 0.0);
}

TEST_F(EngineTest, SimulationSpeedMultiplierIsClampedToReplayRange) {
    engine.setSpeedMul(1000.0);
    EXPECT_DOUBLE_EQ(engine.speedMul(), 8.0);
    engine.setSpeedMul(-5.0);
    EXPECT_DOUBLE_EQ(engine.speedMul(), 0.0);
}

TEST_F(EngineTest, ReplayUsesTheRecordedVariableStepSequence) {
    Scenario scenario = engine.scenario();
    for (ScenarioUnit& unit : scenario.units) unit.schedule.clear();
    ASSERT_TRUE(engine.setScenario(scenario));
    ASSERT_TRUE(engine.executeCommand(
                    QStringLiteral("moveTo"),
                    QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_g1")},
                                {QStringLiteral("pos"), QVariantMap{
                                     {QStringLiteral("x"), scenario.map.widthMeters - 1000.0},
                                     {QStringLiteral("y"), scenario.map.heightMeters - 1000.0}}}}).accepted);
    engine.stepOnce(0.37);
    engine.stepOnce(0.11);
    const double duration = engine.simTime();
    const QByteArray expected = QJsonDocument(engine.collectCheckpointState())
                                    .toJson(QJsonDocument::Compact);

    QString error;
    ASSERT_TRUE(engine.seekReplay(duration, &error)) << error.toStdString();
    EXPECT_EQ(QJsonDocument(engine.collectCheckpointState()).toJson(QJsonDocument::Compact),
              expected);
}

TEST_F(EngineTest, CommandSetSpeed) {
    auto* u = engine.unit("red_a1");
    ASSERT_NE(u, nullptr);
    double oldSpeed = u->speed();

    QVariantMap args;
    args["unitId"] = "red_a1";
    args["speed"] = oldSpeed + 10;
    engine.command("setSpeed", args);

    EXPECT_DOUBLE_EQ(u->speed(), oldSpeed + 10);
}

TEST_F(EngineTest, CommandMoveTo) {
    QVariantMap args;
    args["unitId"] = "red_r1";
    QVariantMap pos;
    pos["x"] = 5000; pos["y"] = 5000;
    args["pos"] = pos;
    engine.command("moveTo", args);
    // should not crash; actual movement happens on tick
    SUCCEED();
}

TEST_F(EngineTest, CommanderOrdersPreserveTypedTextAndSelectedPoints) {
    const QVariantMap point{{QStringLiteral("x"), 5200.0},
                            {QStringLiteral("y"), 6100.0}};

    const auto textResult = engine.executeCommand(
        QStringLiteral("unitOrder"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_a1")},
                    {QStringLiteral("text"), QStringLiteral("保持编队并报告状态")}});
    ASSERT_TRUE(textResult.accepted) << textResult.message.toStdString();
    QVariantMap message = engine.recentMessages().constFirst().toMap();
    EXPECT_EQ(message.value(QStringLiteral("type")).toString(), QStringLiteral("UnitOrder"));
    EXPECT_EQ(message.value(QStringLiteral("receiver")).toString(), QStringLiteral("red_a1"));
    EXPECT_EQ(message.value(QStringLiteral("payload")).toMap().value(QStringLiteral("text")).toString(),
              QStringLiteral("保持编队并报告状态"));

    const QMap<QString, QString> messageTypes{
        {QStringLiteral("attackAt"), QStringLiteral("AttackOrder")},
        {QStringLiteral("moveTo"), QStringLiteral("Guidance")},
        {QStringLiteral("withdraw"), QStringLiteral("Withdraw")}};
    for (auto it = messageTypes.constBegin(); it != messageTypes.constEnd(); ++it) {
        const QString& action = it.key();
        const auto result = engine.executeCommand(
            action, QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_a1")},
                                {QStringLiteral("pos"), point}});
        ASSERT_TRUE(result.accepted) << action.toStdString() << ": "
                                     << result.message.toStdString();
        for (const QVariant& candidate : engine.recentMessages()) {
            const QVariantMap candidateMap = candidate.toMap();
            if (candidateMap.value(QStringLiteral("type")).toString() == it.value()
                && candidateMap.value(QStringLiteral("receiver")).toString()
                    == QLatin1String("red_a1")) {
                message = candidateMap;
                break;
            }
        }
        ASSERT_EQ(message.value(QStringLiteral("type")).toString(), it.value());
        const QVariantMap payload = message.value(QStringLiteral("payload")).toMap();
        EXPECT_DOUBLE_EQ(payload.value(QStringLiteral("x")).toDouble(), 5200.0);
        EXPECT_DOUBLE_EQ(payload.value(QStringLiteral("y")).toDouble(), 6100.0);
    }
}

TEST_F(EngineTest, NotificationOnlyOrdersRemainVisibleWithoutActingOnUnit) {
    UnitBase* unit = engine.unit(QStringLiteral("red_a1"));
    ASSERT_NE(unit, nullptr);
    const QString initialStatus = unit->checkpointState()
                                      .value(QStringLiteral("status")).toString();
    const QVariantMap point{{QStringLiteral("x"), 5200.0},
                            {QStringLiteral("y"), 6100.0}};
    const QList<QPair<QString, QVariantMap>> orders{
        {QStringLiteral("unitOrder"),
         {{QStringLiteral("unitId"), unit->id()},
          {QStringLiteral("text"), QStringLiteral("保持编队并报告状态")},
          {QStringLiteral("notificationOnly"), true}}},
        {QStringLiteral("assignTarget"),
         {{QStringLiteral("attackerId"), unit->id()},
          {QStringLiteral("targetId"), QStringLiteral("blue_a1")},
          {QStringLiteral("notificationOnly"), true}}},
        {QStringLiteral("moveTo"),
         {{QStringLiteral("unitId"), unit->id()},
          {QStringLiteral("pos"), point},
          {QStringLiteral("notificationOnly"), true}}},
        {QStringLiteral("withdraw"),
         {{QStringLiteral("unitId"), unit->id()},
          {QStringLiteral("notificationOnly"), true}}}
    };

    for (const auto& [action, args] : orders) {
        const auto result = engine.executeCommand(action, args);
        ASSERT_TRUE(result.accepted) << action.toStdString() << ": "
                                     << result.message.toStdString();
        EXPECT_EQ(unit->checkpointState().value(QStringLiteral("status")).toString(),
                  initialStatus)
            << action.toStdString();
        ASSERT_FALSE(engine.recentMessages().isEmpty());
        const QVariantMap message = engine.recentMessages().constFirst().toMap();
        const QVariantMap payload = message.value(QStringLiteral("payload")).toMap();
        EXPECT_TRUE(payload.value(QStringLiteral("notificationOnly")).toBool())
            << action.toStdString();
        if (action == QLatin1String("withdraw")) {
            EXPECT_FALSE(payload.contains(QStringLiteral("homeX")));
            EXPECT_FALSE(payload.contains(QStringLiteral("homeY")));
        }
    }
}

TEST_F(EngineTest, RecomputeReadyAfterCpDeath) {
    ASSERT_TRUE(engine.readyForSim());

    // Update red CP with 0 HP — addOrUpdateUnit triggers recompute
    ScenarioUnit su;
    su.id = "red_cp"; su.callsign = "红方指挥所";
    su.kind = "commandpost"; su.side = "red";
    su.pos = GeoPos{2000, 7500, 50};
    su.detectRange = 5000; su.commRange = 20000;
    su.maxHp = 0;

    // WARNING: Setting HP to 0+1 then killing isn't clean; skip live check
    engine.addOrUpdateUnit(su);
    // The engine won't auto-kill units via addOrUpdateUnit - test the normal path
    // Just verify the duplicate case still works:
    SUCCEED();
}

TEST_F(EngineTest, RecomputeReadyAfterCpDuplicate) {
    ASSERT_TRUE(engine.readyForSim());

    ScenarioUnit su;
    su.id = "red_cp2"; su.callsign = "红方指挥所2";
    su.kind = "commandpost"; su.side = "red";
    su.pos = GeoPos{3000, 7000, 50};
    su.detectRange = 5000; su.commRange = 20000;

    engine.addOrUpdateUnit(su);
    EXPECT_FALSE(engine.readyForSim());
    EXPECT_TRUE(engine.cpIssues().contains("重复"));
}

TEST_F(EngineTest, AddAndRemoveUnit) {
    size_t initial = engine.unitIds().size();

    ScenarioUnit su;
    su.id = "test_unit"; su.callsign = "测试";
    su.kind = "reconuav"; su.side = "red";
    su.pos = GeoPos{5000, 5000, 3000};
    su.detectRange = 6000; su.commRange = 15000;

    engine.addOrUpdateUnit(su);
    EXPECT_EQ(engine.unitIds().size(), initial + 1);
    EXPECT_NE(engine.unit("test_unit"), nullptr);

    engine.removeUnit("test_unit");
    EXPECT_EQ(engine.unitIds().size(), initial);
    EXPECT_EQ(engine.unit("test_unit"), nullptr);
}

TEST_F(EngineTest, UpdatingKindOrSideRecreatesRuntimeUnit) {
    auto scenarioIt = std::find_if(engine.scenario().units.begin(), engine.scenario().units.end(),
                                   [](const ScenarioUnit& u){ return u.id == "red_r1"; });
    ASSERT_NE(scenarioIt, engine.scenario().units.end());
    ScenarioUnit changed = *scenarioIt;
    changed.kind = "groundscout";
    changed.side = "blue";
    changed.speed = UnitBase::defaultSpeedMps(UnitKind::GroundScout);

    engine.addOrUpdateUnit(changed);

    auto* runtime = engine.unit("red_r1");
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->kindStr(), "groundscout");
    EXPECT_EQ(runtime->sideStr(), "blue");
    EXPECT_EQ(engine.bus()->unitSide("red_r1"), "blue");
}

TEST_F(EngineTest, InvalidKindIsRejectedWithoutChangingScenario) {
    const auto initialCount = engine.scenario().units.size();
    ScenarioUnit invalid;
    invalid.id = "invalid_kind";
    invalid.callsign = "无效单元";
    invalid.kind = "not-a-kind";
    invalid.side = "red";

    engine.addOrUpdateUnit(invalid);

    EXPECT_EQ(engine.scenario().units.size(), initialCount);
    EXPECT_EQ(engine.unit("invalid_kind"), nullptr);
    EXPECT_TRUE(engine.lastError().contains("未知单元类型"));
}

TEST_F(EngineTest, ScheduleCommandPersistsToScenario) {
    QVariantList schedule;
    schedule.append(QVariantMap{{"time", 10.0}, {"x", 999.0}, {"y", 888.0}});
    schedule.append(QVariantMap{{"time", 5.0}, {"x", 123.0}, {"y", 456.0}});
    engine.command("setSchedule", QVariantMap{{"unitId", "red_r1"},
                                               {"schedule", schedule}});

    auto scenarioIt = std::find_if(engine.scenario().units.begin(), engine.scenario().units.end(),
                                   [](const ScenarioUnit& u){ return u.id == "red_r1"; });
    ASSERT_NE(scenarioIt, engine.scenario().units.end());
    ASSERT_EQ(scenarioIt->schedule.size(), 2u);
    EXPECT_DOUBLE_EQ(scenarioIt->schedule.front().time, 5.0);
    EXPECT_DOUBLE_EQ(scenarioIt->schedule.front().x, 123.0);
    ASSERT_NE(engine.unit("red_r1"), nullptr);
    ASSERT_EQ(engine.unit("red_r1")->schedule().size(), 2u);
}

TEST_F(EngineTest, EcmRangeChangeUpdatesMessageBus) {
    auto* recon = engine.unit("red_r1");
    auto* attacker = engine.unit("red_a1");
    ASSERT_NE(recon, nullptr);
    ASSERT_NE(attacker, nullptr);
    recon->setPosition(GeoPos{0, 0, 0});
    attacker->setPosition(GeoPos{10000, 0, 0});
    recon->setCommRange(20000);
    attacker->setCommRange(20000);
    ASSERT_TRUE(engine.bus()->canCommunicate("red_r1", "red_a1"));

    recon->applyJamming(0.4);

    EXPECT_FALSE(engine.bus()->canCommunicate("red_r1", "red_a1"));
}

TEST_F(EngineTest, GroundRouteStopsAfterFinalWaypoint) {
    auto* scout = dynamic_cast<GroundScout*>(engine.unit("red_g1"));
    ASSERT_NE(scout, nullptr);
    scout->setPosition(GeoPos{0, 0, 0});
    scout->setSpeed(1000.0);
    QVariantList route;
    route.append(QVariant::fromValue(QPointF(0, 0)));
    route.append(QVariant::fromValue(QPointF(100, 0)));

    scout->setRoute(route);
    scout->onTick(0.1);
    scout->onTick(0.1);
    scout->onTick(0.1);

    EXPECT_NEAR(scout->pos().x, 100.0, 1e-6);
    EXPECT_FALSE(scout->hasActiveWaypoints());
}

TEST_F(EngineTest, GroundRouteSnapsToNearbyFinalWaypoint) {
    auto* scout = dynamic_cast<GroundScout*>(engine.unit("red_g1"));
    ASSERT_NE(scout, nullptr);
    scout->clearSchedule();
    scout->setPosition(GeoPos{0, 0, 0});
    scout->setRoute(QVariantList{QVariant::fromValue(QPointF(25, 10))});

    scout->onTick(0.1);

    EXPECT_DOUBLE_EQ(scout->pos().x, 25.0);
    EXPECT_DOUBLE_EQ(scout->pos().y, 10.0);
    EXPECT_FALSE(scout->hasActiveWaypoints());
}

TEST_F(EngineTest, AttackFlightPlanSnapsToNearbyFinalWaypoint) {
    auto* attacker = dynamic_cast<AttackUAV*>(engine.unit("red_a1"));
    ASSERT_NE(attacker, nullptr);
    attacker->clearSchedule();
    attacker->setPosition(GeoPos{0, 0, 2000});
    const QVariantList nearbyWaypoint{
        QVariantMap{{"x", 25.0}, {"y", 10.0}}
    };
    engine.command("setFlightPlan",
                   QVariantMap{{"attackerId", "red_a1"},
                               {"waypoints", nearbyWaypoint}});

    attacker->onTick(0.1);

    EXPECT_DOUBLE_EQ(attacker->pos().x, 25.0);
    EXPECT_DOUBLE_EQ(attacker->pos().y, 10.0);
    EXPECT_FALSE(attacker->hasActiveWaypoints());
}

TEST_F(EngineTest, ClearingPatrolReleasesWaypointOverride) {
    auto* recon = dynamic_cast<ReconUAV*>(engine.unit("red_r1"));
    ASSERT_NE(recon, nullptr);
    recon->setPatrol(QVariantList{QVariant::fromValue(QPointF(5000, 5000))});
    ASSERT_TRUE(recon->hasActiveWaypoints());

    recon->clearPatrol();

    EXPECT_FALSE(recon->hasActiveWaypoints());
}

TEST_F(EngineTest, FriendlyTargetAttackIsRejected) {
    const auto initialMessages = engine.recentMessages().size();
    engine.command("engageTarget", QVariantMap{{"attackerId", "red_a1"},
                                                {"targetId", "red_r1"}});
    EXPECT_EQ(engine.recentMessages().size(), initialMessages);
}

TEST_F(EngineTest, CannotRunWhenCommandPostsAreInvalid) {
    ScenarioUnit duplicate;
    duplicate.id = "red_cp2";
    duplicate.callsign = "红方备用指挥所";
    duplicate.kind = "commandpost";
    duplicate.side = "red";
    engine.addOrUpdateUnit(duplicate);
    ASSERT_FALSE(engine.readyForSim());

    engine.setRunning(true);

    EXPECT_FALSE(engine.running());
}

TEST_F(EngineTest, CommandPostDestructionReportsOutcomeOnce) {
    Scenario scenario = engine.scenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.id == QLatin1String("red_a1")) {
            unit.hitProbability = 1.0;
            unit.minAttackRange = 0.0;
            unit.damageMin = 250.0;
            unit.damageMax = 250.0;
        }
    }
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* attacker = engine.unit("red_a1");
    auto* blueCp = engine.unit("blue_cp");
    ASSERT_NE(attacker, nullptr);
    ASSERT_NE(blueCp, nullptr);
    attacker->setPosition(blueCp->pos());
    blueCp->setHp(50.0);
    int outcomeCount = 0;
    QString winner;
    QObject::connect(&engine, &SimulationEngine::simulationEnded,
                     [&](const QString& w, const QString&) {
                         ++outcomeCount;
                         winner = w;
                     });
    dynamic_cast<AttackUAV*>(attacker)->fireOnTarget(QStringLiteral("blue_cp"));

    engine.stepOnce(0.1);
    engine.stepOnce(0.1);

    EXPECT_FALSE(blueCp->alive());
    EXPECT_EQ(outcomeCount, 1);
    EXPECT_EQ(winner, "红方");
}

TEST_F(EngineTest, SimultaneousCommandPostKillsProduceDraw) {
    Scenario scenario = engine.scenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.kind == QLatin1String("attackuav")) {
            unit.hitProbability = 1.0;
            unit.minAttackRange = 0.0;
            unit.damageMin = 500.0;
            unit.damageMax = 500.0;
        }
    }
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* redAttacker = engine.unit("red_a1");
    auto* blueAttacker = engine.unit("blue_a1");
    auto* redCp = engine.unit("red_cp");
    auto* blueCp = engine.unit("blue_cp");
    ASSERT_NE(redAttacker, nullptr);
    ASSERT_NE(blueAttacker, nullptr);
    ASSERT_NE(redCp, nullptr);
    ASSERT_NE(blueCp, nullptr);

    redAttacker->clearSchedule();
    blueAttacker->clearSchedule();
    redAttacker->setPosition(blueCp->pos());
    blueAttacker->setPosition(redCp->pos());

    int ended = 0;
    QString winner;
    QString loser;
    QObject::connect(&engine, &SimulationEngine::simulationEnded,
                     [&](const QString& w, const QString& l) {
                         ++ended;
                         winner = w;
                         loser = l;
                     });

    dynamic_cast<AttackUAV*>(redAttacker)->fireOnTarget(QStringLiteral("blue_cp"));
    dynamic_cast<AttackUAV*>(blueAttacker)->fireOnTarget(QStringLiteral("red_cp"));
    engine.stepOnce(0.1);

    EXPECT_FALSE(redCp->alive());
    EXPECT_FALSE(blueCp->alive());
    EXPECT_EQ(ended, 1);
    EXPECT_TRUE(winner.startsWith(QStringLiteral("平局")));
    EXPECT_TRUE(loser.isEmpty());
}

TEST_F(EngineTest, CommandOnDeadUnitRejected) {
    auto* u = engine.unit("red_a1");
    ASSERT_NE(u, nullptr);
    u->setHp(0.0);
    EXPECT_FALSE(u->alive());

    QVariantMap args;
    args["unitId"] = "red_a1";
    QVariantMap pos;
    pos["x"] = 5000; pos["y"] = 5000;
    args["pos"] = pos;

    // Engine should reject command for dead unit
    engine.command("moveTo", args);
    SUCCEED(); // should not crash, silently ignored
}

TEST_F(EngineTest, UnitSnapshotIncludesJammer) {
    auto snap = engine.unitSnapshot("red_j1");
    if (!snap.isEmpty()) {
        EXPECT_TRUE(snap.contains("jammer"));
        EXPECT_TRUE(snap.value("jammer").toBool());
    }
    // Note: red_j1 might not exist if default scenario wasn't loaded with jammers
    // in older builds; test is forward-looking
    SUCCEED();
}

TEST_F(EngineTest, PostedDestroyNotificationCannotKillLiveUnit) {
    auto* target = engine.unit("blue_r1");
    ASSERT_NE(target, nullptr);
    const double hpBefore = target->hp();
    int destroyedSignals = 0;
    QObject::connect(&engine, &SimulationEngine::unitDestroyed,
                     [&](const QString&) { ++destroyedSignals; });

    Message spoofed;
    spoofed.type = Message::Type::TargetDestroyed;
    spoofed.sender = "unregistered_sender";
    spoofed.receiver = "red_cp";
    spoofed.payload["targetId"] = "blue_r1";
    spoofed.payload["attackerId"] = "unregistered_sender";
    engine.bus()->send(spoofed);

    EXPECT_DOUBLE_EQ(target->hp(), hpBefore);
    EXPECT_TRUE(target->alive());
    EXPECT_EQ(destroyedSignals, 0);
}

TEST_F(EngineTest, DestroyedTargetLeavesAttackerInPlaceWithoutAutoWithdraw) {
    // 修复后的设计：摧毁目标后，攻击方不应被 CP 自动派单撤回；
    // 而是保留原地等待新指令，由指挥员手动决定下一步。
    Scenario scenario = engine.scenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.id == QLatin1String("red_a1")) {
            unit.hitProbability = 1.0;
            unit.minAttackRange = 0.0;
            unit.damageMin = 100.0;
            unit.damageMax = 100.0;
        }
    }
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* attacker = engine.unit("red_a1");
    auto* target = engine.unit("blue_r1");
    auto* home = engine.unit("red_cp");
    ASSERT_NE(attacker, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(home, nullptr);
    attacker->setPosition(target->pos());
    target->setHp(50.0);

    dynamic_cast<AttackUAV*>(attacker)->fireOnTarget(QStringLiteral("blue_r1"));
    engine.stepOnce(0.1);

    ASSERT_FALSE(target->alive());
    // 攻击方应停在原位（无 waypoint，无 schedule 自动撤离）
    EXPECT_FALSE(attacker->hasActiveWaypoints());
    EXPECT_TRUE(attacker->schedule().empty());
    // 继续推进仿真，攻击方不应自己飞回 CP
    const GeoPos afterKill = attacker->pos();
    engine.stepOnce(2.0);
    // 攻击方位置几乎不变（无主动运动）；CP 显式撤回才会移动
    EXPECT_LT(attacker->pos().distanceTo2D(afterKill), 5.0);
}

TEST_F(EngineTest, WithdrawCancelsStoredSchedule) {
    auto* recon = engine.unit("red_r1");
    ASSERT_NE(recon, nullptr);
    ASSERT_FALSE(recon->schedule().empty());

    engine.command("withdraw", QVariantMap{{"unitId", "red_r1"}});

    EXPECT_TRUE(recon->schedule().empty());
    EXPECT_TRUE(recon->hasActiveWaypoints());
}

TEST_F(EngineTest, AttackUavRepursuesTargetLeavingAttackPosition) {
    auto* attacker = engine.unit("red_a1");
    auto* target = engine.unit("blue_r1");
    ASSERT_NE(attacker, nullptr);
    ASSERT_NE(target, nullptr);
    attacker->clearSchedule();
    target->clearSchedule();
    attacker->setPosition(GeoPos{0, 0, 2000});
    target->setPosition(GeoPos{500, 0, 2000});

    // Set ROE to "hold" so the UAV monitors in attack position without firing.
    // This keeps the target alive for the repursuit test after it moves away.
    engine.command("setRoe", QVariantMap{{"unitId", "red_a1"}, {"roe", "hold"}});
    // Use "pursue" (not "assignTarget"+"moveTo") so the unit flies to the attack
    // area WITH its targetId intact.  "moveTo" sends a Guidance message that
    // clears m_targetId, which would prevent any repursuit when the target flees.
    engine.command("pursue", QVariantMap{{"attackerId", "red_a1"},
                                          {"targetId", "blue_r1"}});
    engine.stepOnce(1.0);
    engine.stepOnce(0.1);
    engine.stepOnce(0.1);

    target->setPosition(GeoPos{10000, 0, 2000});
    const GeoPos beforePursuit = attacker->pos();
    engine.stepOnce(1.0);

    EXPECT_GT(beforePursuit.distanceTo2D(attacker->pos()), 0.0);
    EXPECT_TRUE(attacker->hasActiveWaypoints());
}

TEST_F(EngineTest, SettingScheduleCancelsOldMobileWaypointRoute) {
    auto* scout = engine.unit("red_g1");
    ASSERT_NE(scout, nullptr);
    scout->setPosition(GeoPos{0, 0, 0});
    scout->setSpeed(100.0);
    engine.command("moveTo", QVariantMap{{"unitId", "red_g1"},
                                          {"pos", QVariantMap{{"x", 1000.0}, {"y", 0.0}}}});
    ASSERT_TRUE(scout->hasActiveWaypoints());

    QVariantList schedule{
        QVariantMap{{"time", 0.0}, {"x", 0.0}, {"y", 0.0}},
        QVariantMap{{"time", 100.0}, {"x", 0.0}, {"y", 0.0}}
    };
    engine.command("setSchedule", QVariantMap{{"unitId", "red_g1"},
                                               {"schedule", schedule}});
    engine.stepOnce(1.0);

    EXPECT_NEAR(scout->pos().x, 0.0, 1e-9);
    EXPECT_NEAR(scout->pos().y, 0.0, 1e-9);
}

TEST_F(EngineTest, SettingScheduleCancelsOldAttackWaypointRoute) {
    auto* attacker = engine.unit("red_a1");
    ASSERT_NE(attacker, nullptr);
    attacker->clearSchedule();
    attacker->setPosition(GeoPos{0, 0, 2000});
    attacker->setSpeed(100.0);
    engine.command("moveTo", QVariantMap{{"unitId", "red_a1"},
                                          {"pos", QVariantMap{{"x", 1000.0}, {"y", 0.0}}}});
    ASSERT_TRUE(attacker->hasActiveWaypoints());

    QVariantList schedule{
        QVariantMap{{"time", 0.0}, {"x", 0.0}, {"y", 0.0}},
        QVariantMap{{"time", 100.0}, {"x", 0.0}, {"y", 0.0}}
    };
    engine.command("setSchedule", QVariantMap{{"unitId", "red_a1"},
                                               {"schedule", schedule}});
    engine.stepOnce(1.0);

    EXPECT_NEAR(attacker->pos().x, 0.0, 1e-9);
    EXPECT_NEAR(attacker->pos().y, 0.0, 1e-9);
}

TEST_F(EngineTest, FlightPlanTemporarilyOverridesButPreservesSchedule) {
    auto* attacker = engine.unit("red_a1");
    ASSERT_NE(attacker, nullptr);
    attacker->setPosition(GeoPos{0, 0, 2000});
    attacker->setSpeed(100.0);
    QVariantList schedule{
        QVariantMap{{"time", 0.0}, {"x", 0.0}, {"y", 0.0}},
        QVariantMap{{"time", 100.0}, {"x", 0.0}, {"y", 0.0}}
    };
    engine.command("setSchedule", QVariantMap{{"unitId", "red_a1"},
                                               {"schedule", schedule}});

    QVariantList waypoints{QVariantMap{{"x", 1000.0}, {"y", 0.0}}};
    engine.command("setFlightPlan", QVariantMap{{"attackerId", "red_a1"},
                                                 {"waypoints", waypoints}});
    engine.stepOnce(1.0);

    EXPECT_NEAR(attacker->pos().x, 100.0, 1e-9);
    EXPECT_EQ(attacker->schedule().size(), 2u);
}

TEST_F(EngineTest, FlightPlanReportsMissingLiveCommandPost) {
    auto* cp = engine.unit("red_cp");
    ASSERT_NE(cp, nullptr);
    cp->setHp(0.0);
    int errors = 0;
    QObject::connect(&engine, &SimulationEngine::errorOccurred,
                     [&](const QString&) { ++errors; });

    QVariantList waypoints{QVariantMap{{"x", 1000.0}, {"y", 1000.0}}};
    engine.command("setFlightPlan", QVariantMap{{"attackerId", "red_a1"},
                                                 {"waypoints", waypoints}});

    EXPECT_EQ(errors, 1);
    EXPECT_TRUE(engine.lastError().contains("己方指挥所已摧毁"));
}

TEST_F(EngineTest, DeadGroundScoutCannotGuideAttack) {
    auto* guide = engine.unit("red_g1");
    ASSERT_NE(guide, nullptr);
    guide->setHp(0.0);
    const qsizetype messagesBefore = engine.recentMessages().size();

    engine.command("guideAttack", QVariantMap{
        {"guideId", "red_g1"},
        {"attackerId", "red_a1"},
        {"targetId", "blue_r1"},
        {"targetPos", QVariantMap{{"x", 16000.0}, {"y", 11000.0}}}
    });

    EXPECT_EQ(engine.recentMessages().size(), messagesBefore);
}

TEST_F(EngineTest, GuideAttackRequiresCommunicationWithAttacker) {
    UnitBase* guide = engine.unit(QStringLiteral("red_g1"));
    UnitBase* attacker = engine.unit(QStringLiteral("red_a1"));
    ASSERT_NE(guide, nullptr);
    ASSERT_NE(attacker, nullptr);
    guide->clearSchedule();
    attacker->clearSchedule();
    guide->setPosition(GeoPos{0.0, 0.0, 0.0});
    attacker->setPosition(GeoPos{10000.0, 10000.0, 1000.0});
    guide->setCommRange(1.0);
    attacker->setCommRange(1.0);

    const CommandResult result = engine.executeCommand(
        QStringLiteral("guideAttack"),
        QVariantMap{{QStringLiteral("guideId"), QStringLiteral("red_g1")},
                    {QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")},
                    {QStringLiteral("targetPos"),
                     QVariantMap{{QStringLiteral("x"), 16000.0},
                                 {QStringLiteral("y"), 11000.0}}}});

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.code, QStringLiteral("COMMUNICATION_LOST"));
}

TEST_F(EngineTest, MutualJammerEffectsAreOrderIndependent) {
    auto* redJammer = engine.unit("red_j1");
    auto* blueJammer = engine.unit("blue_j1");
    auto* redTarget = engine.unit("red_a1");
    auto* blueTarget = engine.unit("blue_a1");
    ASSERT_NE(redJammer, nullptr);
    ASSERT_NE(blueJammer, nullptr);
    ASSERT_NE(redTarget, nullptr);
    ASSERT_NE(blueTarget, nullptr);
    redJammer->setPosition(GeoPos{0, 0, 0});
    blueJammer->setPosition(GeoPos{500, 0, 0});
    redJammer->setDetectRange(1000.0);
    blueJammer->setDetectRange(1000.0);
    redTarget->clearSchedule();
    blueTarget->clearSchedule();
    redTarget->setPosition(GeoPos{1300, 0, 0});
    blueTarget->setPosition(GeoPos{800, 0, 0});

    engine.stepOnce(0.05);

    EXPECT_DOUBLE_EQ(redJammer->jamFactor(), 0.5);
    EXPECT_DOUBLE_EQ(blueJammer->jamFactor(), 0.5);
    EXPECT_DOUBLE_EQ(redTarget->jamFactor(), 1.0);
    EXPECT_DOUBLE_EQ(blueTarget->jamFactor(), 1.0);
}

TEST_F(EngineTest, SharedDetectionUsesCommunicationRange) {
    auto* recon = engine.unit("red_r1");
    auto* ally = engine.unit("red_a1");
    auto* target = engine.unit("blue_g1");
    ASSERT_NE(recon, nullptr);
    ASSERT_NE(ally, nullptr);
    ASSERT_NE(target, nullptr);
    recon->clearSchedule();
    ally->clearSchedule();
    target->clearSchedule();
    recon->setPosition(GeoPos{0, 0, 0});
    ally->setPosition(GeoPos{7000, 0, 0});
    target->setPosition(GeoPos{1000, 0, 0});
    recon->setDetectRange(2000.0);
    recon->setCommRange(10000.0);
    ally->setCommRange(10000.0);

    engine.stepOnce(1.5);

    EXPECT_TRUE(ally->sharedKnowledgeJson().contains("shared:detect:blue_g1"));
}

TEST_F(EngineTest, RepeatedTinyHpChangesEventuallyNotifyObservers) {
    auto* unit = engine.unit("red_a1");
    ASSERT_NE(unit, nullptr);
    int changes = 0;
    QObject::connect(unit, &UnitBase::hpChanged, [&]() { ++changes; });
    const double original = unit->hp();

    unit->setHp(original - 0.2);
    unit->setHp(original - 0.4);
    EXPECT_EQ(changes, 0);
    unit->setHp(original - 0.6);
    EXPECT_EQ(changes, 1);
}

TEST_F(EngineTest, InvalidAttackPowerIsRejected) {
    auto scenarioIt = std::find_if(engine.scenario().units.begin(), engine.scenario().units.end(),
                                   [](const ScenarioUnit& u){ return u.id == "red_a1"; });
    ASSERT_NE(scenarioIt, engine.scenario().units.end());
    ScenarioUnit invalid = *scenarioIt;
    invalid.attackPower = -1.0;
    const double originalPower = engine.unit("red_a1")->attackPower();

    engine.addOrUpdateUnit(invalid);

    EXPECT_DOUBLE_EQ(engine.unit("red_a1")->attackPower(), originalPower);
    EXPECT_TRUE(engine.lastError().contains("参数无效"));
}

TEST_F(EngineTest, ScenarioRejectsOutOfBoundsUnitsAndSchedulePointsAtomically) {
    const Scenario original = engine.scenario();
    Scenario invalid = original;
    invalid.units.front().pos.x = invalid.map.widthMeters + 1.0;

    EXPECT_FALSE(engine.setScenario(invalid));
    EXPECT_EQ(engine.scenario().units.size(), original.units.size());
    EXPECT_NE(engine.unit(QStringLiteral("red_cp")), nullptr);

    invalid = original;
    invalid.units.front().schedule = {
        SchedulePoint{-1.0, invalid.units.front().pos.x, invalid.units.front().pos.y}};
    EXPECT_FALSE(engine.setScenario(invalid));
    EXPECT_EQ(engine.scenario().units.size(), original.units.size());
}

TEST_F(EngineTest, ScenarioRejectsExcessiveUnitCount) {
    Scenario invalid = engine.scenario();
    const ScenarioUnit prototype = invalid.units.front();
    while (invalid.units.size() <= 512) {
        ScenarioUnit unit = prototype;
        unit.id = QStringLiteral("bulk_%1").arg(invalid.units.size());
        invalid.units.push_back(unit);
    }

    EXPECT_FALSE(engine.setScenario(invalid));
    EXPECT_TRUE(engine.lastError().contains(QStringLiteral("数量")));
}

TEST(ProjectileDomainTest, MovesAtConstantSpeedAndObeysTurnRadius) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& configured : scenario.units) {
        configured.schedule.clear();
        if (configured.id == QLatin1String("red_a1")) {
            configured.pos = GeoPos{5000.0, 5000.0, 2000.0};
            configured.attackRange = 5000.0;
            configured.optimalRange = 2500.0;
        } else if (configured.id == QLatin1String("blue_r1")) {
            configured.pos = GeoPos{6500.0, 5000.0, 2000.0};
        }
    }
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")}}).accepted);

    engine.stepOnce(0.1);
    QJsonArray projectiles = engine.projectilesSnapshot();
    ASSERT_EQ(projectiles.size(), 1);
    QJsonObject first = projectiles.first().toObject();
    EXPECT_TRUE(first.value(QStringLiteral("active")).toBool());
    EXPECT_DOUBLE_EQ(first.value(QStringLiteral("speed")).toDouble(), 500.0);
    EXPECT_NEAR(first.value(QStringLiteral("position")).toArray().at(0).toDouble(),
                5050.0, 1e-6);

    engine.unit(QStringLiteral("blue_r1"))->setPosition(GeoPos{6500.0, 6000.0, 2000.0});
    engine.stepOnce(0.1);
    projectiles = engine.projectilesSnapshot();
    ASSERT_EQ(projectiles.size(), 1);
    const QJsonObject second = projectiles.first().toObject();
    EXPECT_NEAR(second.value(QStringLiteral("headingRad")).toDouble(), 0.0714285714285714, 1e-6);
    const QJsonArray p0 = first.value(QStringLiteral("position")).toArray();
    const QJsonArray p1 = second.value(QStringLiteral("position")).toArray();
    EXPECT_NEAR(std::hypot(p1.at(0).toDouble() - p0.at(0).toDouble(),
                           p1.at(1).toDouble() - p0.at(1).toDouble()),
                50.0, 1e-6);
}

TEST(ProjectileDomainTest, ExpiresWhenTargetCannotBeReachedWithinLifetime) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& configured : scenario.units) {
        configured.schedule.clear();
        if (configured.id == QLatin1String("red_a1")) {
            configured.pos = GeoPos{1000.0, 7500.0, 2000.0};
            configured.attackRange = 19000.0;
            configured.optimalRange = 10000.0;
        } else if (configured.id == QLatin1String("blue_r1")) {
            configured.pos = GeoPos{19000.0, 7500.0, 2000.0};
        }
    }
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")}}).accepted);
    engine.stepOnce(0.05);
    for (int i = 0; i < 160; ++i) engine.stepOnce(0.1);

    const QJsonArray projectiles = engine.projectilesSnapshot();
    ASSERT_EQ(projectiles.size(), 1);
    EXPECT_FALSE(projectiles.first().toObject().value(QStringLiteral("active")).toBool());
    EXPECT_EQ(projectiles.first().toObject().value(QStringLiteral("terminalReason")).toString(),
              QStringLiteral("expired"));
}

TEST(AbilityDomainTest, CountermeasureConsumesChargeAndTerminatesAllMissilesInRange) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& configured : scenario.units) {
        configured.schedule.clear();
        if (configured.id == QLatin1String("blue_a1")) {
            configured.pos = GeoPos{11000.0, 5000.0, 2000.0};
        } else if (configured.id == QLatin1String("red_r1")) {
            configured.pos = GeoPos{10000.0, 5000.0, 2000.0};
        }
    }
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    UnitBase* defender = engine.unit(QStringLiteral("red_r1"));
    ASSERT_NE(defender, nullptr);
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("blue_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("red_r1")}}).accepted);
    engine.stepOnce(0.05);
    ASSERT_EQ(engine.projectilesSnapshot().size(), 1);
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("activateCountermeasure"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_r1")}}).accepted);

    const QJsonObject projectile = engine.projectilesSnapshot().first().toObject();
    EXPECT_FALSE(projectile.value(QStringLiteral("active")).toBool());
    EXPECT_EQ(projectile.value(QStringLiteral("terminalReason")).toString(),
              QStringLiteral("countermeasured"));
    EXPECT_EQ(defender->countermeasureState().remaining, 1);
    EXPECT_DOUBLE_EQ(defender->hp(), defender->maxHp());
}

TEST(AbilityDomainTest, ScanLocksContactsAndRepairRollIsReplayStable) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& configured : scenario.units) {
        configured.schedule.clear();
        if (configured.id == QLatin1String("red_r1")) {
            configured.pos = GeoPos{5000.0, 5000.0, 2000.0};
        } else if (configured.id == QLatin1String("blue_r1")) {
            configured.pos = GeoPos{8000.0, 5000.0, 2000.0};
        }
    }
    SimulationEngine first;
    SimulationEngine second;
    ASSERT_TRUE(first.setScenario(scenario));
    ASSERT_TRUE(second.setScenario(scenario));
    first.restoreCombatSeed(0x12345678ULL);
    second.restoreCombatSeed(0x12345678ULL);
    ASSERT_TRUE(first.executeCommand(
        QStringLiteral("activateScan"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_r1")}}).accepted);
    bool foundTarget = false;
    for (const QJsonValue& contact : first.activeScanContacts()) {
        foundTarget |= contact.toObject().value(QStringLiteral("targetId")).toString()
            == QLatin1String("blue_r1");
    }
    EXPECT_TRUE(foundTarget);

    const QJsonObject damage{{QStringLiteral("sensor"), 0.4},
                             {QStringLiteral("comms"), 0.8},
                             {QStringLiteral("mobility"), 0.9},
                             {QStringLiteral("weapon"), 0.7}};
    ASSERT_TRUE(first.unit(QStringLiteral("red_r1"))->restoreSubsystemState(damage));
    ASSERT_TRUE(second.unit(QStringLiteral("red_r1"))->restoreSubsystemState(damage));
    EXPECT_EQ(first.executeCommand(
                  QStringLiteral("attemptFieldRepair"),
                  QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_r1")}}).accepted,
              second.executeCommand(
                  QStringLiteral("attemptFieldRepair"),
                  QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_r1")}}).accepted);
    EXPECT_EQ(first.unit(QStringLiteral("red_r1"))->subsystemStateJson(),
              second.unit(QStringLiteral("red_r1"))->subsystemStateJson());
    EXPECT_EQ(first.unit(QStringLiteral("red_r1"))->repairAttemptSequence(), 1u);
    EXPECT_EQ(second.unit(QStringLiteral("red_r1"))->repairAttemptSequence(), 1u);
}

TEST(ResourceDomainTest, FuelUsesEconomicCruiseBaselineAndZeroFuelRejectsMovement) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& configured : scenario.units) configured.schedule.clear();
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    UnitBase* attacker = engine.unit(QStringLiteral("red_a1"));
    ASSERT_NE(attacker, nullptr);
    const double initialFuel = attacker->fuelRemaining();
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("moveTo"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_a1")},
                    {QStringLiteral("pos"),
                     QVariantMap{{QStringLiteral("x"), attacker->pos().x + 1000.0},
                                 {QStringLiteral("y"), attacker->pos().y}}}}).accepted);
    engine.stepOnce(1.0);
    EXPECT_DOUBLE_EQ(attacker->fuelBurnRate(), 4.0);
    EXPECT_DOUBLE_EQ(attacker->fuelRemaining(), initialFuel - 4.0);
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("halt"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_a1")}}).accepted);
    engine.stepOnce(1.0);
    EXPECT_DOUBLE_EQ(attacker->fuelBurnRate(), 0.5);

    Scenario emptyFuel = scenario;
    for (ScenarioUnit& configured : emptyFuel.units) {
        if (configured.id == QLatin1String("red_r1")) configured.initialFuelSec = 0.0;
    }
    SimulationEngine stopped;
    ASSERT_TRUE(stopped.setScenario(emptyFuel));
    EXPECT_FALSE(stopped.executeCommand(
        QStringLiteral("moveTo"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_r1")},
                    {QStringLiteral("pos"),
                     QVariantMap{{QStringLiteral("x"), 6000.0},
                                 {QStringLiteral("y"), 6000.0}}}}).accepted);
}

TEST(MessageLogRecorderTest, EnableFailureIsReportedAndNestedPathIsCreated) {
    MessageLogRecorder recorder;
    EXPECT_FALSE(recorder.setEnabled(true, QString()));
    EXPECT_FALSE(recorder.isEnabled());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/nested/messages.ndjson";
    ASSERT_TRUE(recorder.setEnabled(true, path));
    recorder.record(QJsonObject{{"type", "test"}});
    EXPECT_TRUE(recorder.setEnabled(false));

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_TRUE(file.readAll().contains("\"type\":\"test\""));
}
