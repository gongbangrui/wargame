#include <gtest/gtest.h>

#include "StateProjector.h"
#include "core/Scenario.h"
#include "core/SimulationEngine.h"
#include "core/UnitBase.h"

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
    engine.loadDefaultScenario();
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

TEST(StateProjectorTest, ObserverRoleDoesNotBypassVisibilityProjection) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const QJsonObject observer = StateProjector::snapshotFor(
        engine, QStringLiteral("observer"), 1, roomState());
    EXPECT_TRUE(observer.value(QStringLiteral("units")).toArray().isEmpty());
    EXPECT_TRUE(observer.value(QStringLiteral("scenario")).toObject()
                    .value(QStringLiteral("units")).toArray().isEmpty());
    EXPECT_FALSE(StateProjector::canControlSide(QStringLiteral("observer"),
                                                QStringLiteral("red")));
    EXPECT_FALSE(StateProjector::canEditSide(QStringLiteral("observer"),
                                             QStringLiteral("red")));
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

TEST(StateProjectorTest, FactionEventRedactsNestedEnemyIdentity) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const QJsonObject event{
        {QStringLiteral("kind"), QStringLiteral("simulationEvent")},
        {QStringLiteral("details"), QJsonObject{
             {QStringLiteral("route"), QJsonArray{
                  QJsonObject{{QStringLiteral("unit"), QStringLiteral("red_a1")}},
                  QStringLiteral("blue_r1 observed red_a1")}}}}};

    const QJsonObject projected = StateProjector::projectEvent(
        engine, QStringLiteral("blue"), event);
    const QByteArray encoded = QJsonDocument(projected).toJson(QJsonDocument::Compact);
    EXPECT_FALSE(encoded.contains("red_a1"));
    EXPECT_TRUE(encoded.contains(QStringLiteral("未知单元").toUtf8()));
    EXPECT_TRUE(encoded.contains("blue_r1"));
}

TEST(StateProjectorTest, FactionEventRedactsEnemyIdentityAfterUnitRemoval) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const QString enemyId = QStringLiteral("blue_a1");
    const QString enemyCallsign = engine.unit(enemyId)->callsign();
    engine.removeUnit(enemyId);

    const QJsonObject projected = StateProjector::projectEvent(
        engine, QStringLiteral("red_commander"),
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("simulationEvent")},
                    {QStringLiteral("details"),
                     QJsonObject{{QStringLiteral("summary"),
                                  QStringLiteral("%1/%2 已撤出")
                                      .arg(enemyId, enemyCallsign)}}}},
        QStringLiteral("red_cp"));

    const QString encoded = QString::fromUtf8(QJsonDocument(projected).toJson(QJsonDocument::Compact));
    EXPECT_FALSE(encoded.contains(enemyId));
    EXPECT_FALSE(encoded.contains(enemyCallsign));
    EXPECT_TRUE(encoded.contains(QStringLiteral("未知单元")));
}

TEST(StateProjectorTest, DetectedEnemyDoesNotExposeItsPrivateMessageTraffic) {
    SimulationEngine engine;
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        unit.commRange = 20000.0;
        if (unit.id == QLatin1String("red_a1")) {
            unit.pos = GeoPos{1000.0, 1000.0, 1000.0};
            unit.detectRange = 5000.0;
        } else if (unit.id == QLatin1String("blue_a1")) {
            unit.pos = GeoPos{1200.0, 1000.0, 1000.0};
        }
    }
    ASSERT_TRUE(engine.setScenario(scenario));
    engine.stepOnce(1.0);
    ASSERT_TRUE(StateProjector::visibleUnitIds(
                    engine, QStringLiteral("red_attack_1"), QStringLiteral("red_a1"))
                    .contains(QStringLiteral("blue_a1")));
    ASSERT_TRUE(engine.executeCommand(
                    QStringLiteral("setFlightPlan"),
                    QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("blue_a1")},
                                {QStringLiteral("waypoints"), QVariantList{
                                     QVariantMap{{QStringLiteral("x"), 4000.0},
                                                 {QStringLiteral("y"), 4000.0}}}}}).accepted);
    ASSERT_TRUE(std::any_of(engine.recentMessages().cbegin(), engine.recentMessages().cend(),
                            [](const QVariant& value) {
        const QVariantMap message = value.toMap();
        return message.value(QStringLiteral("type")).toString() == QLatin1String("FlightPlan")
            && message.value(QStringLiteral("sender")).toString() == QLatin1String("blue_cp");
    }));

    const QJsonArray projected = StateProjector::filteredMessages(
        engine, QStringLiteral("red_attack_1"), QStringLiteral("red_a1"));
    EXPECT_TRUE(std::none_of(projected.cbegin(), projected.cend(), [](const QJsonValue& value) {
        const QJsonObject message = value.toObject();
        return message.value(QStringLiteral("sender")).toString().startsWith(QLatin1String("blue_"))
            || message.value(QStringLiteral("receiver")).toString().startsWith(QLatin1String("blue_"));
    }));
}

TEST(StateProjectorTest, RoomClosureReachesUnseatedRoomMembers) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const QJsonObject event{{QStringLiteral("kind"), QStringLiteral("roomClosed")},
                            {QStringLiteral("message"), QStringLiteral("room stopped")}};

    EXPECT_EQ(StateProjector::projectEvent(engine, QStringLiteral("player"), event), event);
}

TEST(StateProjectorTest, MatchResetReachesUnseatedRoomMembers) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const QJsonObject event{{QStringLiteral("kind"), QStringLiteral("matchReset")},
                            {QStringLiteral("message"), QStringLiteral("new round")}};

    EXPECT_EQ(StateProjector::projectEvent(engine, QStringLiteral("player"), event), event);
}

TEST(StateProjectorTest, SeatSnapshotStartsWithOwnedUnitAndExplicitIntel) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const QJsonObject base = StateProjector::snapshotFor(
        engine, QStringLiteral("red_attack_1"), 1, roomState());
    const QJsonArray baseUnits = base.value(QStringLiteral("units")).toArray();
    ASSERT_FALSE(baseUnits.isEmpty());
    EXPECT_FALSE(unitById(baseUnits, QStringLiteral("red_a1")).isEmpty());
    const QSet<QString> shared{QStringLiteral("blue_r1")};
    const QJsonObject sharedSnapshot = StateProjector::snapshotFor(
        engine, QStringLiteral("red_attack_1"), 2, roomState(), shared);
    EXPECT_FALSE(unitById(sharedSnapshot.value(QStringLiteral("units")).toArray(),
                          QStringLiteral("blue_r1")).isEmpty());
}

TEST(StateProjectorTest, AuthoritativeOwnedUnitOverridesSeatIndexInference) {
    SimulationEngine engine;
    Scenario scenario = ScenarioIo::defaultScenario();
    ScenarioUnit secondAttack = scenario.units.at(2);
    secondAttack.id = QStringLiteral("red_a2");
    secondAttack.callsign = QStringLiteral("红方攻击2");
    secondAttack.pos = GeoPos{6000.0, 11000.0, 2000.0};
    secondAttack.commRange = 0.0;
    for (ScenarioUnit& unit : scenario.units) {
        if (unit.id == QLatin1String("red_a1")) unit.commRange = 0.0;
    }
    scenario.units.push_back(secondAttack);
    ASSERT_TRUE(engine.setScenario(scenario));

    const QJsonObject snapshot = StateProjector::snapshotFor(
        engine, QStringLiteral("red_attack_1"), 1, roomState(), {}, QStringLiteral("red_a2"));
    const QJsonArray units = snapshot.value(QStringLiteral("units")).toArray();
    EXPECT_FALSE(unitById(units, QStringLiteral("red_a2")).isEmpty());
    EXPECT_TRUE(unitById(units, QStringLiteral("red_a1")).isEmpty());
}

TEST(StateProjectorTest, SeatCommunicationUsesDirectedRelayChains) {
    SimulationEngine engine;
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) {
        if (unit.side != QLatin1String("red")) continue;
        unit.commRange = 0.0;
        if (unit.id == QLatin1String("red_cp")) {
            unit.pos = GeoPos{1000.0, 1000.0, 0.0};
            unit.commRange = 1000.0;
        } else if (unit.id == QLatin1String("red_r1")) {
            unit.pos = GeoPos{1800.0, 1000.0, 0.0};
            unit.commRange = 1000.0;
        } else if (unit.id == QLatin1String("red_a1")) {
            unit.pos = GeoPos{2600.0, 1000.0, 0.0};
            unit.commRange = 100.0;
        } else {
            unit.pos = GeoPos{10000.0, 10000.0, 0.0};
        }
    }
    ASSERT_TRUE(engine.setScenario(scenario));

    EXPECT_TRUE(StateProjector::canTransmit(engine, QStringLiteral("red_cp"),
                                            QStringLiteral("red_a1")));
    EXPECT_FALSE(StateProjector::canTransmit(engine, QStringLiteral("red_a1"),
                                             QStringLiteral("red_cp")));
    const QJsonObject commander = StateProjector::snapshotFor(
        engine, QStringLiteral("red_commander"), 3, roomState());
    EXPECT_FALSE(unitById(commander.value(QStringLiteral("units")).toArray(),
                          QStringLiteral("red_a1")).isEmpty());
}

TEST(StateProjectorTest, MutuallyReachableFriendlySeatsAppearInBothSnapshots) {
    SimulationEngine engine;
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) {
        if (unit.side != QLatin1String("red")) continue;
        unit.commRange = 0.0;
        unit.pos = GeoPos{12000.0, 12000.0, 0.0};
        if (unit.id == QLatin1String("red_a1")) {
            unit.pos = GeoPos{1000.0, 1000.0, 1000.0};
            unit.commRange = 1000.0;
        } else if (unit.id == QLatin1String("red_r1")) {
            unit.pos = GeoPos{1500.0, 1000.0, 1000.0};
            unit.commRange = 1000.0;
        }
    }
    ASSERT_TRUE(engine.setScenario(scenario));
    ASSERT_TRUE(StateProjector::canTransmit(engine, QStringLiteral("red_a1"),
                                             QStringLiteral("red_r1")));
    ASSERT_TRUE(StateProjector::canTransmit(engine, QStringLiteral("red_r1"),
                                             QStringLiteral("red_a1")));

    const QJsonArray attackUnits = StateProjector::snapshotFor(
        engine, QStringLiteral("red_attack_1"), 4, roomState(), {},
        QStringLiteral("red_a1")).value(QStringLiteral("units")).toArray();
    const QJsonArray reconUnits = StateProjector::snapshotFor(
        engine, QStringLiteral("red_recon_1"), 5, roomState(), {},
        QStringLiteral("red_r1")).value(QStringLiteral("units")).toArray();
    EXPECT_FALSE(unitById(attackUnits, QStringLiteral("red_r1")).isEmpty());
    EXPECT_FALSE(unitById(reconUnits, QStringLiteral("red_a1")).isEmpty());
}

TEST(StateProjectorTest, SeatSnapshotProjectsCommanderCommunicationState) {
    const auto projectedState = [](double commanderRange, double subordinateRange,
                                   double distance) {
        SimulationEngine engine;
        Scenario scenario = ScenarioIo::defaultScenario();
        for (ScenarioUnit& unit : scenario.units) {
            if (unit.side != QLatin1String("red")) continue;
            unit.commRange = 0.0;
            unit.pos = GeoPos{12000.0, 12000.0, 0.0};
            if (unit.id == QLatin1String("red_cp")) {
                unit.pos = GeoPos{1000.0, 1000.0, 0.0};
                unit.commRange = commanderRange;
            } else if (unit.id == QLatin1String("red_a1")) {
                unit.pos = GeoPos{1000.0 + distance, 1000.0, 1000.0};
                unit.commRange = subordinateRange;
            }
        }
        EXPECT_TRUE(engine.setScenario(scenario));
        return StateProjector::snapshotFor(
            engine, QStringLiteral("red_attack_1"), 6, roomState(), {},
            QStringLiteral("red_a1"))
            .value(QStringLiteral("roomState")).toObject()
            .value(QStringLiteral("communicationState")).toString();
    };

    EXPECT_EQ(projectedState(1000.0, 1000.0, 500.0), QStringLiteral("bilateral"));
    EXPECT_EQ(projectedState(1000.0, 100.0, 500.0), QStringLiteral("receiveOnly"));
    EXPECT_EQ(projectedState(100.0, 1000.0, 500.0), QStringLiteral("disconnected"));
}

TEST(StateProjectorTest, ReachabilityCacheReusesRevisionAndInvalidatesTopologyChanges) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    StateProjector::resetReachabilityCacheStats();

    EXPECT_FALSE(StateProjector::canTransmit(engine, {}, QStringLiteral("red_a1")));
    EXPECT_FALSE(StateProjector::canTransmit(engine, QStringLiteral("red_cp"), {}));
    EXPECT_FALSE(StateProjector::canTransmit(engine, QStringLiteral("red_cp"),
                                             QStringLiteral("blue_cp")));
    ASSERT_TRUE(StateProjector::canTransmit(engine, QStringLiteral("red_cp"),
                                            QStringLiteral("red_a1")));
    const quint64 buildsAfterFirst = StateProjector::reachabilityGraphBuildCount();
    const quint64 traversalsAfterFirst = StateProjector::reachabilityBfsTraversalCount();
    ASSERT_EQ(buildsAfterFirst, 1U);
    ASSERT_GT(traversalsAfterFirst, 0U);

    ASSERT_TRUE(StateProjector::canTransmit(engine, QStringLiteral("red_cp"),
                                            QStringLiteral("red_a1")));
    EXPECT_EQ(StateProjector::reachabilityGraphBuildCount(), buildsAfterFirst);
    EXPECT_EQ(StateProjector::reachabilityBfsTraversalCount(), traversalsAfterFirst);

    engine.unit(QStringLiteral("red_cp"))->setPosition(GeoPos{0.0, 0.0, 0.0});
    engine.unit(QStringLiteral("red_cp"))->setCommRange(1.0);
    EXPECT_FALSE(StateProjector::canTransmit(engine, QStringLiteral("red_cp"),
                                             QStringLiteral("red_a1")));
    EXPECT_EQ(StateProjector::reachabilityGraphBuildCount(), buildsAfterFirst + 1);
    EXPECT_GT(StateProjector::reachabilityBfsTraversalCount(), traversalsAfterFirst);

    engine.unit(QStringLiteral("red_cp"))->setPosition(GeoPos{1000.0, 1000.0, 0.0});
    engine.unit(QStringLiteral("red_cp"))->setCommRange(20000.0);
    engine.unit(QStringLiteral("red_a1"))->setHp(0.0);
    EXPECT_FALSE(StateProjector::canTransmit(engine, QStringLiteral("red_cp"),
                                             QStringLiteral("red_a1")));
    EXPECT_EQ(StateProjector::reachabilityGraphBuildCount(), buildsAfterFirst + 2);
}

TEST(StateProjectorTest, ReachabilityCacheIsDeterministicForFiveHundredTwelveUnits) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    Scenario scenario = engine.scenario();
    for (int index = 0; index < 502; ++index) {
        ScenarioUnit unit = scenario.units.at(3);
        unit.id = QStringLiteral("red_cache_%1").arg(index);
        unit.callsign = unit.id;
        unit.kind = QStringLiteral("groundscout");
        unit.side = QStringLiteral("red");
        unit.detectRange = 1000.0;
        unit.attackRange = 1000.0;
        unit.commRange = 0.0;
        unit.speed = 1.0;
        unit.maxHp = 100.0;
        unit.armor = 0.0;
        unit.repairRate = 1.0;
        unit.subsystemRepairRate = 0.01;
        unit.attackPower = 10.0;
        unit.schedule.clear();
        unit.pos = GeoPos{100.0 + (index % 100) * 100.0,
                          100.0 + (index / 100) * 100.0, 0.0};
        scenario.units.push_back(unit);
    }
    ASSERT_EQ(scenario.units.size(), 512U);
    ASSERT_TRUE(engine.setScenario(scenario));

    StateProjector::resetReachabilityCacheStats();
    const QJsonObject first = StateProjector::snapshotFor(
        engine, QStringLiteral("red_commander"), 77, roomState());
    const quint64 firstBuilds = StateProjector::reachabilityGraphBuildCount();
    const quint64 firstTraversals = StateProjector::reachabilityBfsTraversalCount();
    const QJsonObject second = StateProjector::snapshotFor(
        engine, QStringLiteral("red_commander"), 77, roomState());

    EXPECT_EQ(QJsonDocument(first).toJson(QJsonDocument::Compact),
              QJsonDocument(second).toJson(QJsonDocument::Compact));
    EXPECT_EQ(firstBuilds, 1U);
    EXPECT_GT(firstTraversals, 0U);
    EXPECT_EQ(StateProjector::reachabilityGraphBuildCount(), firstBuilds);
    EXPECT_EQ(StateProjector::reachabilityBfsTraversalCount(), firstTraversals);
}
