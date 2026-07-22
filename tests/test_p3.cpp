#include <gtest/gtest.h>

#include "core/Scenario.h"
#include "core/SimulationEngine.h"
#include "units/AttackUAV.h"
#include "view/SimulationController.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>

using namespace gbr;

namespace {

QJsonObject unitById(const QJsonArray& units, const QString& id) {
    for (const QJsonValue& value : units) {
        const QJsonObject unit = value.toObject();
        if (unit.value(QStringLiteral("id")).toString() == id) return unit;
    }
    return {};
}

Scenario scenarioWithoutSchedules() {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) unit.schedule.clear();
    return scenario;
}

} // namespace

TEST(P3DamageTest, ArmorAndSubsystemDamageReduceEffectiveCapabilities) {
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenarioWithoutSchedules()));
    UnitBase* target = engine.unit(QStringLiteral("blue_a1"));
    ASSERT_NE(target, nullptr);

    UnitBase::Params params = target->params();
    params.armor = 0.5;
    target->setParams(params);
    const double hpBefore = target->hp();
    const double detectBefore = target->detectRange();

    const UnitBase::DamageDelta sensorHit = target->assessDamage(40.0, 0);
    EXPECT_DOUBLE_EQ(sensorHit.hullDamage, 20.0);
    EXPECT_GT(sensorHit.sensorLoss, 0.0);
    target->applyDamageDelta(sensorHit);

    EXPECT_DOUBLE_EQ(target->hp(), hpBefore - 20.0);
    EXPECT_LT(target->sensorHealth(), 1.0);
    EXPECT_LT(target->detectRange(), detectBefore);

    ASSERT_TRUE(target->restoreSubsystemState(
        QJsonObject{{QStringLiteral("sensor"), 0.5},
                    {QStringLiteral("comms"), 0.4},
                    {QStringLiteral("mobility"), 0.25},
                    {QStringLiteral("weapon"), 0.2}}));
    EXPECT_DOUBLE_EQ(target->detectRange(), target->baseDetectRange() * 0.5);
    EXPECT_DOUBLE_EQ(target->commRange(), target->baseCommRange() * 0.4);
    EXPECT_DOUBLE_EQ(target->speed(), target->baseSpeed() * 0.25);
    EXPECT_DOUBLE_EQ(target->attackRange(), target->baseAttackRange() * 0.6);
    EXPECT_DOUBLE_EQ(target->attackPower(), params.attackPower * 0.48);
}

TEST(P3LifecycleTest, ServiceAtCommandPostRepairsRefuelsAndRearms) {
    Scenario scenario = scenarioWithoutSchedules();
    GeoPos commandPostPosition;
    for (const ScenarioUnit& unit : scenario.units) {
        if (unit.id == QLatin1String("red_cp")) commandPostPosition = unit.pos;
    }
    for (ScenarioUnit& unit : scenario.units) {
        if (unit.id != QLatin1String("red_a1")) continue;
        unit.pos = commandPostPosition;
        unit.ammoCapacity = 4;
        unit.initialAmmo = 1;
        unit.fuelCapacitySec = 100.0;
        unit.initialFuelSec = 30.0;
        unit.rearmDurationSec = 2.0;
        unit.repairRate = 50.0;
        unit.subsystemRepairRate = 1.0;
    }

    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* attacker = dynamic_cast<AttackUAV*>(engine.unit(QStringLiteral("red_a1")));
    ASSERT_NE(attacker, nullptr);
    attacker->applyDamageDelta(attacker->assessDamage(20.0, 0));
    ASSERT_LT(attacker->hp(), attacker->maxHp());

    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("service"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_a1")}}).accepted);
    engine.stepOnce(0.05);
    EXPECT_TRUE(attacker->serviceRequested());
    engine.stepOnce(2.0);

    EXPECT_FALSE(attacker->serviceRequested());
    EXPECT_DOUBLE_EQ(attacker->hp(), attacker->maxHp());
    EXPECT_DOUBLE_EQ(attacker->sensorHealth(), 1.0);
    EXPECT_EQ(attacker->ammoRemaining(), attacker->ammoCapacity());
    EXPECT_DOUBLE_EQ(attacker->fuelRemaining(), attacker->fuelCapacity());
    EXPECT_DOUBLE_EQ(attacker->turnaroundProgress(), 1.0);
}

TEST(P3LifecycleTest, LowFuelAndEmptyAmmoTriggerAutomaticServiceReturn) {
    Scenario scenario = scenarioWithoutSchedules();
    for (ScenarioUnit& unit : scenario.units) {
        if (unit.id != QLatin1String("red_a1")) continue;
        unit.fuelCapacitySec = 10.0;
        unit.initialFuelSec = 2.1;
        unit.ammoCapacity = 1;
        unit.initialAmmo = 1;
        unit.hitProbability = 0.0;
        unit.cooldownSec = 0.0;
    }
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* attacker = dynamic_cast<AttackUAV*>(engine.unit(QStringLiteral("red_a1")));
    ASSERT_NE(attacker, nullptr);

    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("moveTo"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_a1")},
                    {QStringLiteral("pos"),
                     QVariantMap{{QStringLiteral("x"), 5000.0},
                                 {QStringLiteral("y"), 11000.0}}}}).accepted);
    engine.stepOnce(0.2);
    EXPECT_TRUE(attacker->serviceRequested());
    EXPECT_EQ(attacker->checkpointState().value(QStringLiteral("behavior")).toObject()
                  .value(QStringLiteral("fsmState")).toString(),
              QStringLiteral("withdrawing"));

    attacker->requestService(false);
    attacker->cancelWaypointMotion();
    attacker->restoreRuntimeWeaponState(1, 0.0, QString(), 10.0, 0.0);
    attacker->setPosition(engine.unit(QStringLiteral("blue_r1"))->pos());
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")}}).accepted);
    engine.stepOnce(0.05);
    EXPECT_EQ(attacker->ammoRemaining(), 0);
    EXPECT_TRUE(attacker->serviceRequested());
    EXPECT_TRUE(attacker->targetId().isEmpty());
}

TEST(P3LifecycleTest, RulesOfEngagementAndCancelEngagementAreEnforced) {
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenarioWithoutSchedules()));
    auto* attacker = dynamic_cast<AttackUAV*>(engine.unit(QStringLiteral("red_a1")));
    UnitBase* target = engine.unit(QStringLiteral("blue_r1"));
    ASSERT_NE(attacker, nullptr);
    ASSERT_NE(target, nullptr);
    attacker->setPosition(target->pos());

    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("setRoe"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_a1")},
                    {QStringLiteral("roe"), QStringLiteral("hold")}}).accepted);
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")}}).accepted);
    const int ammoBefore = attacker->ammoRemaining();
    engine.stepOnce(1.0);
    EXPECT_EQ(attacker->ammoRemaining(), ammoBefore);
    EXPECT_DOUBLE_EQ(target->hp(), target->maxHp());

    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("cancelEngagement"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_a1")}}).accepted);
    EXPECT_TRUE(attacker->targetId().isEmpty());
    EXPECT_FALSE(attacker->armed());
}

TEST(P3CheckpointTest, RestoresSubsystemFuelAndServiceState) {
    SimulationEngine source;
    ASSERT_TRUE(source.setScenario(scenarioWithoutSchedules()));
    auto* attacker = dynamic_cast<AttackUAV*>(source.unit(QStringLiteral("red_a1")));
    ASSERT_NE(attacker, nullptr);
    ASSERT_TRUE(attacker->restoreSubsystemState(
        QJsonObject{{QStringLiteral("sensor"), 0.7},
                    {QStringLiteral("comms"), 0.6},
                    {QStringLiteral("mobility"), 0.5},
                    {QStringLiteral("weapon"), 0.4}}));
    ASSERT_TRUE(attacker->restoreRuntimeWeaponState(2, 0.75,
                                                   QStringLiteral("miss"), 321.0, 1.25));
    attacker->requestService(true);

    SimulationEngine restored;
    ASSERT_TRUE(restored.setScenario(source.scenario()));
    QString error;
    ASSERT_TRUE(restored.restoreCheckpointState(source.collectCheckpointState(),
                                                3.0, false, 1.0, &error))
        << error.toStdString();
    auto* restoredAttacker = dynamic_cast<AttackUAV*>(
        restored.unit(QStringLiteral("red_a1")));
    ASSERT_NE(restoredAttacker, nullptr);
    EXPECT_DOUBLE_EQ(restoredAttacker->sensorHealth(), 0.7);
    EXPECT_DOUBLE_EQ(restoredAttacker->commsHealth(), 0.6);
    EXPECT_DOUBLE_EQ(restoredAttacker->mobilityHealth(), 0.5);
    EXPECT_DOUBLE_EQ(restoredAttacker->weaponHealth(), 0.4);
    EXPECT_EQ(restoredAttacker->ammoRemaining(), 2);
    EXPECT_DOUBLE_EQ(restoredAttacker->fuelRemaining(), 321.0);
    EXPECT_DOUBLE_EQ(restoredAttacker->turnaroundElapsed(), 1.25);
    EXPECT_TRUE(restoredAttacker->serviceRequested());
}

TEST(P3ReplayTest, SeekReproducesRecordedStateAndReportAggregatesCombat) {
    Scenario scenario = scenarioWithoutSchedules();
    for (ScenarioUnit& unit : scenario.units) {
        if (unit.id == QLatin1String("red_a1")) {
            unit.pos = GeoPos{16000.0, 11000.0, unit.pos.alt};
            unit.hitProbability = 1.0;
            unit.damageMin = 20.0;
            unit.damageMax = 20.0;
            unit.cooldownSec = 10.0;
        }
    }
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* attacker = dynamic_cast<AttackUAV*>(engine.unit(QStringLiteral("red_a1")));
    UnitBase* target = engine.unit(QStringLiteral("blue_r1"));
    ASSERT_NE(attacker, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")}}).accepted);
    for (int i = 0; i < 20; ++i) engine.stepOnce(0.05);

    const QJsonObject finalAttacker = unitById(engine.collectAllUnitsSnapshot(),
                                               QStringLiteral("red_a1"));
    const QJsonObject finalTarget = unitById(engine.collectAllUnitsSnapshot(),
                                             QStringLiteral("blue_r1"));
    ASSERT_FALSE(finalAttacker.isEmpty());
    ASSERT_FALSE(finalTarget.isEmpty());
    QString error;
    ASSERT_TRUE(engine.seekReplay(0.5, &error)) << error.toStdString();
    EXPECT_DOUBLE_EQ(engine.simTime(), 0.5);
    ASSERT_TRUE(engine.seekReplay(1.0, &error)) << error.toStdString();
    EXPECT_DOUBLE_EQ(engine.simTime(), 1.0);
    EXPECT_EQ(unitById(engine.collectAllUnitsSnapshot(), QStringLiteral("red_a1"))
                  .value(QStringLiteral("ammoRemaining")),
              finalAttacker.value(QStringLiteral("ammoRemaining")));
    EXPECT_DOUBLE_EQ(unitById(engine.collectAllUnitsSnapshot(), QStringLiteral("blue_r1"))
                         .value(QStringLiteral("hp")).toDouble(),
                     finalTarget.value(QStringLiteral("hp")).toDouble());

    const QJsonObject report = engine.battleReport();
    const QJsonObject summary = report.value(QStringLiteral("summary")).toObject();
    EXPECT_EQ(summary.value(QStringLiteral("shots")).toInt(), 1);
    EXPECT_EQ(summary.value(QStringLiteral("hits")).toInt(), 1);
    EXPECT_DOUBLE_EQ(summary.value(QStringLiteral("damage")).toDouble(), 20.0);
    EXPECT_FALSE(report.value(QStringLiteral("events")).toArray().isEmpty());
    EXPECT_FALSE(report.value(QStringLiteral("finalUnits")).toArray().isEmpty());
}

TEST(P3EditorTest, BatchTransformCopyPasteAndValidationAreAtomic) {
    SimulationController controller;
    const QJsonObject beforeRed = unitById(controller.unitsJson().value(QStringLiteral("units")).toArray(),
                                           QStringLiteral("red_r1"));
    const QJsonObject beforeBlue = unitById(controller.unitsJson().value(QStringLiteral("units")).toArray(),
                                            QStringLiteral("blue_r1"));
    ASSERT_TRUE(controller.batchUpdateUnits(
        {QStringLiteral("red_r1"), QStringLiteral("blue_r1")},
        QVariantMap{{QStringLiteral("offsetX"), 100.0},
                    {QStringLiteral("offsetY"), -50.0},
                    {QStringLiteral("armor"), 0.3}}));
    QJsonArray units = controller.unitsJson().value(QStringLiteral("units")).toArray();
    EXPECT_DOUBLE_EQ(unitById(units, QStringLiteral("red_r1")).value(QStringLiteral("x")).toDouble(),
                     beforeRed.value(QStringLiteral("x")).toDouble() + 100.0);
    EXPECT_DOUBLE_EQ(unitById(units, QStringLiteral("blue_r1")).value(QStringLiteral("y")).toDouble(),
                     beforeBlue.value(QStringLiteral("y")).toDouble() - 50.0);
    EXPECT_DOUBLE_EQ(unitById(units, QStringLiteral("red_r1")).value(QStringLiteral("armor")).toDouble(),
                     0.3);

    const double redYBeforeAlign = unitById(units, QStringLiteral("red_r1"))
        .value(QStringLiteral("y")).toDouble();
    const double blueYBeforeAlign = unitById(units, QStringLiteral("blue_r1"))
        .value(QStringLiteral("y")).toDouble();
    ASSERT_TRUE(controller.transformUnits(
        {QStringLiteral("red_r1"), QStringLiteral("blue_r1")},
        QStringLiteral("alignTop")));
    units = controller.unitsJson().value(QStringLiteral("units")).toArray();
    const double topY = std::min(redYBeforeAlign, blueYBeforeAlign);
    EXPECT_DOUBLE_EQ(unitById(units, QStringLiteral("red_r1")).value(QStringLiteral("y")).toDouble(), topY);
    EXPECT_DOUBLE_EQ(unitById(units, QStringLiteral("blue_r1")).value(QStringLiteral("y")).toDouble(), topY);

    ASSERT_TRUE(controller.batchUpdateUnits(
        {QStringLiteral("blue_r1")}, QVariantMap{{QStringLiteral("offsetY"), 1000.0}}));
    ASSERT_TRUE(controller.transformUnits(
        {QStringLiteral("red_r1"), QStringLiteral("blue_r1")},
        QStringLiteral("alignBottom")));
    units = controller.unitsJson().value(QStringLiteral("units")).toArray();
    EXPECT_DOUBLE_EQ(unitById(units, QStringLiteral("red_r1")).value(QStringLiteral("y")).toDouble(), topY + 1000.0);
    EXPECT_DOUBLE_EQ(unitById(units, QStringLiteral("blue_r1")).value(QStringLiteral("y")).toDouble(), topY + 1000.0);

    const QVariantList copied = controller.copyUnits({QStringLiteral("red_r1")});
    ASSERT_EQ(copied.size(), 1);
    const QStringList pasted = controller.pasteUnits(copied, 100.0, 100.0);
    ASSERT_EQ(pasted.size(), 1);
    EXPECT_NE(pasted.front(), QStringLiteral("red_r1"));
    EXPECT_FALSE(controller.unitAt(pasted.front()).isEmpty());
    EXPECT_TRUE(controller.scenarioValidationIssues().isEmpty());
}
