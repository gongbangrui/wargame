#include "core/SimulationEngine.h"
#include "core/UnitBase.h"
#include "server/game/AiPlan.h"
#include "server/game/RulesAi.h"

#include <gtest/gtest.h>

#include <QSet>

#include <cmath>

using namespace gbr;

namespace {

AiSeatState seat(const QString& id, const QString& unitId, const QString& kind,
                 double x, double y) {
    AiSeatState value;
    value.seatId = id;
    value.unitId = unitId;
    value.kind = kind;
    value.x = x;
    value.y = y;
    return value;
}

}

TEST(RulesAiTest, BaselinePlanRoundTripPreservesSeatOwnership) {
    AiSeatState input = seat(QStringLiteral("blue_attack_1"), QStringLiteral("blue_a1"),
                             QStringLiteral("attackuav"), 100.0, 100.0);
    input.targetVisible = true;
    input.targetId = QStringLiteral("red_a1");
    AiPlanV1 plan = RulesAi::makeCommanderPlan({input}, QStringLiteral("request-1"), 2, 3,
                                               10.0, nullptr, 0.0, 1000.0, 800.0, 4);
    QString error;
    AiPlanV1 restored;
    ASSERT_TRUE(AiPlanV1::fromJson(plan.toJson(), &restored, &error)) << error.toStdString();
    ASSERT_EQ(restored.objectives.size(), 1);
    EXPECT_EQ(restored.objectives.first().seatId, QStringLiteral("blue_attack_1"));
    EXPECT_EQ(restored.objectives.first().targetId, QStringLiteral("red_a1"));
    EXPECT_EQ(restored.planningGeneration, 4U);
}

TEST(RulesAiTest, FiltersDeadImmobileAndCommandPostUnits) {
    AiSeatState dead = seat(QStringLiteral("blue_recon_1"), QStringLiteral("blue_r1"),
                            QStringLiteral("reconuav"), 100.0, 100.0);
    dead.alive = false;
    AiSeatState immobile = seat(QStringLiteral("blue_attack_2"), QStringLiteral("blue_a2"),
                                QStringLiteral("attackuav"), 100.0, 100.0);
    immobile.movable = false;
    AiSeatState commandPost = seat(QStringLiteral("blue_commander"), QStringLiteral("blue_cp"),
                                   QStringLiteral("commandpost"), 100.0, 100.0);
    AiSeatState red = seat(QStringLiteral("red_recon_1"), QStringLiteral("red_r1"),
                           QStringLiteral("reconuav"), 100.0, 100.0);
    const AiPlanV1 plan = RulesAi::makeCommanderPlan(
        {dead, immobile, commandPost, red}, QStringLiteral("request-2"), 2, 3, 10.0, nullptr,
        0.0, 1000.0, 800.0, 1);
    EXPECT_TRUE(plan.objectives.isEmpty());

    AiPlanV1 manual;
    for (const AiSeatState& state : {dead, immobile, commandPost, red}) {
        manual.objectives.append(AiObjectiveV1{QStringLiteral("defend"), 1,
                                                state.seatId, {}, {}, 10.0});
    }
    EXPECT_TRUE(RulesAi::commandsForPlan(
        manual, {dead, immobile, commandPost, red}, 0.0).isEmpty());
}

TEST(RulesAiTest, NonAttackVisibleTargetAdvancesSixtyPercent) {
    AiSeatState recon = seat(QStringLiteral("blue_recon_1"), QStringLiteral("blue_r1"),
                             QStringLiteral("reconuav"), 100.0, 100.0);
    recon.targetVisible = true;
    recon.targetId = QStringLiteral("red_r1");
    recon.targetX = 600.0;
    recon.targetY = 500.0;
    const AiPlanV1 plan = RulesAi::makeCommanderPlan(
        {recon}, QStringLiteral("request-3"), 2, 3, 10.0, nullptr, 0.0, 1000.0, 800.0, 1);
    ASSERT_EQ(plan.objectives.size(), 1);
    const QJsonObject region = plan.objectives.first().region;
    EXPECT_DOUBLE_EQ(region.value(QStringLiteral("x")).toDouble(), 400.0);
    EXPECT_DOUBLE_EQ(region.value(QStringLiteral("y")).toDouble(), 340.0);
}

TEST(RulesAiTest, AttackAndMovementCommandsCoexistInEasyPlan) {
    AiSeatState attack = seat(QStringLiteral("blue_attack_1"), QStringLiteral("blue_a1"),
                              QStringLiteral("attackuav"), 100.0, 100.0);
    attack.targetVisible = true;
    attack.targetId = QStringLiteral("red_a1");
    AiSeatState recon = seat(QStringLiteral("blue_recon_1"), QStringLiteral("blue_r1"),
                             QStringLiteral("reconuav"), 100.0, 500.0);
    const AiPlanV1 plan = RulesAi::makeCommanderPlan(
        {recon, attack}, QStringLiteral("request-easy"), 2, 3, 10.0, nullptr, 0.0,
        1000.0, 800.0, 1);
    const QList<AiCommand> commands = RulesAi::commandsForPlan(plan, {recon, attack}, 0.0);
    ASSERT_EQ(commands.size(), 2);
    EXPECT_EQ(commands.at(0).action, QStringLiteral("engageTarget"));
    EXPECT_EQ(commands.at(1).action, QStringLiteral("moveTo"));
}

TEST(RulesAiTest, PatrolProgressionIsDeterministicInBoundsAndNonOrigin) {
    const AiSeatState scout = seat(QStringLiteral("blue_recon_1"), QStringLiteral("blue_r1"),
                                   QStringLiteral("reconuav"), 500.0, 400.0);
    const AiPlanV1 first = RulesAi::makeCommanderPlan(
        {scout}, QStringLiteral("request-4"), 2, 3, 10.0, nullptr, 0.0, 1000.0, 800.0, 1);
    const AiPlanV1 second = RulesAi::makeCommanderPlan(
        {scout}, QStringLiteral("request-5"), 2, 3, 10.0, nullptr, 0.0, 1000.0, 800.0, 2);
    ASSERT_EQ(first.objectives.size(), 1);
    ASSERT_EQ(second.objectives.size(), 1);
    const QJsonObject firstRegion = first.objectives.first().region;
    const QJsonObject secondRegion = second.objectives.first().region;
    EXPECT_NE(firstRegion, secondRegion);
    for (const QJsonObject& region : {firstRegion, secondRegion}) {
        const double x = region.value(QStringLiteral("x")).toDouble();
        const double y = region.value(QStringLiteral("y")).toDouble();
        EXPECT_GE(x, 0.0);
        EXPECT_LE(x, 1000.0);
        EXPECT_GE(y, 0.0);
        EXPECT_LE(y, 800.0);
        EXPECT_GE(std::hypot(x - scout.x, y - scout.y), 50.0);
    }
    const QList<AiCommand> commands = RulesAi::commandsForPlan(first, {scout}, 0.0);
    ASSERT_EQ(commands.size(), 1);
    EXPECT_EQ(commands.first().action, QStringLiteral("moveTo"));

    QSet<QString> ringPoints;
    for (quint64 generation = 1; generation <= 8; ++generation) {
        const AiPlanV1 generated = RulesAi::makeCommanderPlan(
            {scout}, QStringLiteral("ring-%1").arg(generation), 2, 3, 10.0,
            nullptr, 0.0, 1000.0, 800.0, generation);
        ASSERT_EQ(generated.objectives.size(), 1);
        const QJsonObject region = generated.objectives.first().region;
        ringPoints.insert(QStringLiteral("%1:%2")
                              .arg(region.value(QStringLiteral("x")).toDouble(), 0, 'f', 6)
                              .arg(region.value(QStringLiteral("y")).toDouble(), 0, 'f', 6));
    }
    EXPECT_EQ(ringPoints.size(), 8);
}

TEST(RulesAiTest, VisibleAdvanceClampsAndFallsBackFromMapEdge) {
    AiSeatState scout = seat(QStringLiteral("blue_recon_1"), QStringLiteral("blue_r1"),
                             QStringLiteral("reconuav"), 1000.0, 800.0);
    scout.targetVisible = true;
    scout.targetId = QStringLiteral("red_r1");
    scout.targetX = 2000.0;
    scout.targetY = 1600.0;
    const AiPlanV1 plan = RulesAi::makeCommanderPlan(
        {scout}, QStringLiteral("request-edge"), 2, 3, 10.0, nullptr, 0.0,
        1000.0, 800.0, 1);
    ASSERT_EQ(plan.objectives.size(), 1);
    const QJsonObject region = plan.objectives.first().region;
    const double x = region.value(QStringLiteral("x")).toDouble();
    const double y = region.value(QStringLiteral("y")).toDouble();
    EXPECT_GE(x, 0.0);
    EXPECT_LE(x, 1000.0);
    EXPECT_GE(y, 0.0);
    EXPECT_LE(y, 800.0);
    EXPECT_GE(std::hypot(x - scout.x, y - scout.y), 50.0);
}

TEST(RulesAiTest, MissingRegionIsRejectedAndDefendHalts) {
    AiSeatState scout = seat(QStringLiteral("blue_recon_1"), QStringLiteral("blue_r1"),
                             QStringLiteral("reconuav"), 100.0, 100.0);
    AiPlanV1 missing;
    missing.objectives.append(AiObjectiveV1{QStringLiteral("patrol"), 2, scout.seatId, {}, {}, 5.0});
    EXPECT_TRUE(RulesAi::commandsForPlan(missing, {scout}, 0.0).isEmpty());

    AiPlanV1 defend;
    defend.objectives.append(AiObjectiveV1{QStringLiteral("defend"), 3, scout.seatId, {}, {}, 5.0});
    const QList<AiCommand> commands = RulesAi::commandsForPlan(defend, {scout}, 0.0);
    ASSERT_EQ(commands.size(), 1);
    EXPECT_EQ(commands.first().action, QStringLiteral("halt"));
    EXPECT_FALSE(isAiObjectiveAction(QStringLiteral("communicate")));

    AiPlanV1 invalidAttack;
    invalidAttack.objectives.append(AiObjectiveV1{QStringLiteral("attack"), 4,
        scout.seatId, QStringLiteral("red_a1"), {}, 5.0});
    EXPECT_TRUE(RulesAi::commandsForPlan(invalidAttack, {scout}, 0.0).isEmpty());
}

TEST(RulesAiTest, ParserRequiresMovementRegionAndRejectsCommunicate) {
    AiPlanV1 movement;
    movement.requestId = QStringLiteral("missing-region");
    movement.matchGeneration = 1;
    movement.sourceStateRevision = 1;
    movement.planningGeneration = 1;
    movement.objectives.append(AiObjectiveV1{QStringLiteral("patrol"), 2,
                                                 QStringLiteral("blue_recon_1"), {}, {}, 5.0});
    QString error;
    EXPECT_FALSE(AiPlanV1::fromJson(movement.toJson(), nullptr, &error));

    AiPlanV1 communicate = movement;
    communicate.requestId = QStringLiteral("communicate-action");
    communicate.objectives.first().action = QStringLiteral("communicate");
    EXPECT_FALSE(AiPlanV1::fromJson(communicate.toJson(), nullptr, &error));

    AiPlanV1 defend = movement;
    defend.requestId = QStringLiteral("defend-action");
    defend.objectives.first().action = QStringLiteral("defend");
    EXPECT_TRUE(AiPlanV1::fromJson(defend.toJson(), nullptr, &error))
        << error.toStdString();
}

TEST(RulesAiTest, EasySuboptimalRollDoesNotSuppressVisibleAttack) {
    AiSeatState attack = seat(QStringLiteral("blue_attack_1"), QStringLiteral("blue_a1"),
                              QStringLiteral("attackuav"), 100.0, 100.0);
    attack.targetVisible = true;
    attack.targetId = QStringLiteral("red_a1");
    quint64 rngState = 25;
    const AiPlanV1 plan = RulesAi::makeCommanderPlan(
        {attack}, QStringLiteral("request-easy-roll"), 2, 3, 10.0, &rngState,
        RulesAi::parameters(QStringLiteral("easy")).suboptimalRate,
        1000.0, 800.0, 1);
    ASSERT_EQ(plan.objectives.size(), 1);
    EXPECT_EQ(plan.objectives.first().action, QStringLiteral("attack"));
}

TEST(RulesAiTest, DifficultyUsesFastUnitDecisionsAndThrottledReplanning) {
    const AiDifficultyParameters easy = RulesAi::parameters(QStringLiteral("easy"));
    const AiDifficultyParameters normal = RulesAi::parameters(QStringLiteral("normal"));
    const AiDifficultyParameters hard = RulesAi::parameters(QStringLiteral("hard"));

    EXPECT_EQ(easy.unitDecisionIntervalMs, 2000);
    EXPECT_EQ(normal.unitDecisionIntervalMs, 1000);
    EXPECT_EQ(hard.unitDecisionIntervalMs, 500);
    EXPECT_EQ(easy.commanderReplanIntervalMs, 90000);
    EXPECT_EQ(normal.commanderReplanIntervalMs, 45000);
    EXPECT_EQ(hard.commanderReplanIntervalMs, 30000);
    EXPECT_LT(easy.unitDecisionIntervalMs, easy.commanderReplanIntervalMs);
    EXPECT_LT(normal.unitDecisionIntervalMs, normal.commanderReplanIntervalMs);
    EXPECT_LT(hard.unitDecisionIntervalMs, hard.commanderReplanIntervalMs);
}

TEST(RulesAiTest, VisibleRedCommandPostTakesAttackPriority) {
    AiSeatState ordinary = seat(QStringLiteral("blue_attack_1"), QStringLiteral("blue_a1"),
                                QStringLiteral("attackuav"), 100.0, 100.0);
    ordinary.targetVisible = true;
    ordinary.targetId = QStringLiteral("red_a1");

    AiSeatState commandPost = seat(QStringLiteral("blue_attack_2"), QStringLiteral("blue_a2"),
                                   QStringLiteral("attackuav"), 200.0, 100.0);
    commandPost.targetVisible = true;
    commandPost.targetId = QStringLiteral("red_cp");
    commandPost.targetKind = QStringLiteral("commandpost");

    const AiPlanV1 plan = RulesAi::makeCommanderPlan(
        {ordinary, commandPost}, QStringLiteral("command-post-priority"), 2, 3, 10.0,
        nullptr, 0.0, 1000.0, 800.0, 1);
    ASSERT_EQ(plan.objectives.size(), 2);
    EXPECT_EQ(plan.objectives.first().seatId, commandPost.seatId);
    EXPECT_EQ(plan.objectives.first().action, QStringLiteral("attack"));
    EXPECT_EQ(plan.objectives.first().targetId, QStringLiteral("red_cp"));
    EXPECT_GT(plan.objectives.first().priority, plan.objectives.last().priority);
}

TEST(RulesAiTest, LatestVisibleCommandPostPreemptsStaleProviderObjective) {
    AiSeatState attacker = seat(QStringLiteral("blue_attack_1"), QStringLiteral("blue_a1"),
                                QStringLiteral("attackuav"), 100.0, 100.0);
    attacker.targetVisible = true;
    attacker.targetId = QStringLiteral("red_cp");
    attacker.targetKind = QStringLiteral("commandpost");

    AiPlanV1 providerPlan;
    providerPlan.objectives.append(AiObjectiveV1{QStringLiteral("defend"), 9,
                                                  attacker.seatId, {}, {}, 10.0});
    const QList<AiCommand> commands = RulesAi::commandsForPlan(providerPlan, {attacker}, 0.0);
    ASSERT_EQ(commands.size(), 1);
    EXPECT_EQ(commands.first().action, QStringLiteral("engageTarget"));
    EXPECT_EQ(commands.first().args.value(QStringLiteral("targetId")).toString(),
              QStringLiteral("red_cp"));
}

TEST(RulesAiTest, MovementRetargetsUsingLatestPositionAndControlsSpeed) {
    AiSeatState initial = seat(QStringLiteral("blue_recon_1"), QStringLiteral("blue_r1"),
                               QStringLiteral("reconuav"), 100.0, 100.0);
    initial.targetVisible = true;
    initial.targetId = QStringLiteral("red_r1");
    initial.targetX = 600.0;
    initial.targetY = 100.0;
    const AiPlanV1 plan = RulesAi::makeCommanderPlan(
        {initial}, QStringLiteral("dynamic-movement"), 2, 3, 10.0, nullptr, 0.0,
        1000.0, 800.0, 1);
    ASSERT_EQ(plan.objectives.size(), 1);
    EXPECT_DOUBLE_EQ(plan.objectives.first().region.value(QStringLiteral("x")).toDouble(), 400.0);

    AiSeatState latest = initial;
    latest.targetX = 300.0;
    latest.speed = 80.0;
    latest.commandedSpeed = 80.0;
    latest.cruiseSpeed = 80.0;
    const QList<AiCommand> first = RulesAi::commandsForPlan(
        plan, {latest}, 0.0, 1000.0, 800.0);
    const QList<AiCommand> second = RulesAi::commandsForPlan(
        plan, {latest}, 0.0, 1000.0, 800.0);
    ASSERT_EQ(first.size(), 2);
    ASSERT_EQ(second.size(), first.size());
    EXPECT_EQ(first.at(0).action, QStringLiteral("setSpeed"));
    EXPECT_DOUBLE_EQ(first.at(0).args.value(QStringLiteral("speed")).toDouble(), 15.0);
    EXPECT_EQ(first.at(1).action, QStringLiteral("moveTo"));
    const QVariantMap position = first.at(1).args.value(QStringLiteral("pos")).toMap();
    EXPECT_DOUBLE_EQ(position.value(QStringLiteral("x")).toDouble(), 220.0);
    EXPECT_DOUBLE_EQ(position.value(QStringLiteral("y")).toDouble(), 100.0);
    EXPECT_EQ(second.at(0).args, first.at(0).args);
    EXPECT_EQ(second.at(1).args, first.at(1).args);
}

TEST(RulesAiTest, SpeedCommandsRespectTheAuthoritativeMaximum) {
    AiSeatState scout = seat(QStringLiteral("blue_recon_1"), QStringLiteral("blue_r1"),
                             QStringLiteral("reconuav"), 100.0, 100.0);
    scout.targetVisible = true;
    scout.targetId = QStringLiteral("red_r1");
    scout.targetX = 10000.0;
    scout.targetY = 100.0;
    scout.speed = 500.0;
    scout.commandedSpeed = 500.0;
    scout.cruiseSpeed = 500.0;
    AiPlanV1 plan;
    plan.objectives.append(AiObjectiveV1{QStringLiteral("search"), 1, scout.seatId, {},
                                          QJsonObject{{QStringLiteral("x"), 6000.0},
                                                      {QStringLiteral("y"), 100.0}},
                                          10.0});

    const QList<AiCommand> commands = RulesAi::commandsForPlan(
        plan, {scout}, 0.0, 20000.0, 15000.0);
    ASSERT_EQ(commands.size(), 2);
    EXPECT_EQ(commands.first().action, QStringLiteral("setSpeed"));
    EXPECT_DOUBLE_EQ(commands.first().args.value(QStringLiteral("speed")).toDouble(),
                     SimulationEngine::kMaximumCommandedUnitSpeedMps);
    EXPECT_EQ(commands.last().action, QStringLiteral("moveTo"));
}

TEST(RulesAiTest, CommandsUseStablePriorityAndSeatOrder) {
    AiSeatState first = seat(QStringLiteral("blue_attack_2"), QStringLiteral("blue_a2"),
                             QStringLiteral("attackuav"), 100.0, 100.0);
    AiSeatState second = seat(QStringLiteral("blue_attack_1"), QStringLiteral("blue_a1"),
                              QStringLiteral("attackuav"), 100.0, 100.0);
    AiPlanV1 plan;
    plan.objectives.append(AiObjectiveV1{QStringLiteral("defend"), 5, first.seatId, {}, {}, 5.0});
    plan.objectives.append(AiObjectiveV1{QStringLiteral("defend"), 5, second.seatId, {}, {}, 5.0});
    AiSeatState higher = seat(QStringLiteral("blue_recon_1"), QStringLiteral("blue_r1"),
                              QStringLiteral("reconuav"), 100.0, 100.0);
    plan.objectives.append(AiObjectiveV1{QStringLiteral("defend"), 6, higher.seatId, {}, {}, 5.0});
    const QList<AiCommand> commands = RulesAi::commandsForPlan(plan, {first, second, higher}, 0.0);
    ASSERT_EQ(commands.size(), 3);
    EXPECT_EQ(commands.at(0).seatId, QStringLiteral("blue_recon_1"));
    EXPECT_EQ(commands.at(1).seatId, QStringLiteral("blue_attack_1"));
    EXPECT_EQ(commands.at(2).seatId, QStringLiteral("blue_attack_2"));
}

TEST(RulesAiTest, RealEngineMovesBlueUnitFromGeneratedObjective) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    auto* unit = engine.unit(QStringLiteral("blue_r1"));
    ASSERT_NE(unit, nullptr);
    const GeoPos initial = unit->pos();
    AiSeatState scout = seat(QStringLiteral("blue_recon_1"), unit->id(), unit->kindStr(),
                             initial.x, initial.y);
    const QJsonObject map = engine.mapInfo();
    const AiPlanV1 plan = RulesAi::makeCommanderPlan(
        {scout}, QStringLiteral("request-6"), 2, 3, 10.0, nullptr, 0.0,
        map.value(QStringLiteral("widthMeters")).toDouble(),
        map.value(QStringLiteral("heightMeters")).toDouble(), 1);
    const QList<AiCommand> commands = RulesAi::commandsForPlan(plan, {scout}, 0.0);
    ASSERT_EQ(commands.size(), 1);
    ASSERT_TRUE(engine.executeCommand(commands.first().action, commands.first().args).accepted);
    engine.stepOnce(1.0);
    ::testing::Test::RecordProperty("initialX", QString::number(initial.x).toStdString());
    ::testing::Test::RecordProperty("initialY", QString::number(initial.y).toStdString());
    ::testing::Test::RecordProperty("finalX", QString::number(unit->pos().x).toStdString());
    ::testing::Test::RecordProperty("finalY", QString::number(unit->pos().y).toStdString());
    EXPECT_GT(std::hypot(unit->pos().x - initial.x, unit->pos().y - initial.y), 0.0);
}
