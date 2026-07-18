#include <gtest/gtest.h>
#include "core/SimulationEngine.h"
#include "core/UnitBase.h"
#include "core/Scenario.h"
#include "units/AttackUAV.h"

using namespace gbr;

TEST(AttackPower, ScenarioRoundTrip) {
    ScenarioUnit su;
    su.id = "t1"; su.callsign = "test"; su.kind = "attackuav"; su.side = "red";
    su.pos = GeoPos{0, 0, 2000};
    su.detectRange = 4000; su.attackRange = 2000; su.commRange = 20000;
    su.speed = 100; su.maxHp = 100; su.attackPower = 250;

    Scenario s;
    s.units.push_back(su);
    QJsonObject j = ScenarioIo::toJson(s);
    auto parsed = ScenarioIo::fromJson(j);
    ASSERT_EQ(parsed.units.size(), 1u);
    EXPECT_DOUBLE_EQ(parsed.units[0].attackPower, 250.0);
}

TEST(AttackPower, DefaultIs100) {
    ScenarioUnit su;
    su.id = "t1"; su.kind = "attackuav"; su.side = "red";
    su.pos = GeoPos{0, 0, 0};
    // Leave attackPower at default
    EXPECT_DOUBLE_EQ(su.attackPower, 100.0);
}

TEST(AttackPower, ParseMissingDefaultsTo100) {
    QJsonObject j;
    j["map"] = QJsonObject();
    QJsonArray units;
    QJsonObject u;
    u["id"] = "t1"; u["callsign"] = "x"; u["kind"] = "attackuav"; u["side"] = "red";
    u["x"] = 0; u["y"] = 0; u["alt"] = 0;
    u["maxHp"] = 100;
    // no attackPower field
    units.append(u);
    j["units"] = units;
    auto parsed = ScenarioIo::fromJson(j);
    ASSERT_EQ(parsed.units.size(), 1u);
    EXPECT_DOUBLE_EQ(parsed.units[0].attackPower, 100.0);
}

TEST(AttackPower, AttackUavAppliesConfiguredDamage) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    auto* atk = dynamic_cast<AttackUAV*>(engine.unit("red_a1"));
    auto* tgt = engine.unit("blue_r1");
    ASSERT_NE(atk, nullptr);
    ASSERT_NE(tgt, nullptr);

    // Set attackPower directly on the unit (simulating QML upsert)
    UnitBase::Params p = atk->params();
    p.attackPower = 250;
    atk->setParams(p);

    double hpBefore = tgt->hp();
    // Move attacker onto target so stepCombat fires
    atk->setPosition(tgt->pos());
    engine.command("engageTarget", QVariantMap{{"attackerId", "red_a1"},
                                                {"targetId", "blue_r1"}});

    // Force one tick so attackRange check + combat fire happens
    engine.stepOnce(2.0);
    EXPECT_LT(tgt->hp(), hpBefore);
}

TEST(AttackPower, AttackUavZeroPowerDoesNoDamage) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    auto* atk = dynamic_cast<AttackUAV*>(engine.unit("red_a1"));
    auto* tgt = engine.unit("blue_r1");
    ASSERT_NE(atk, nullptr);
    ASSERT_NE(tgt, nullptr);

    UnitBase::Params p = atk->params();
    p.attackPower = 0;
    atk->setParams(p);

    double hpBefore = tgt->hp();
    atk->setPosition(tgt->pos());
    engine.command("engageTarget", QVariantMap{{"attackerId", "red_a1"},
                                                {"targetId", "blue_r1"}});
    engine.stepOnce(2.0);
    EXPECT_DOUBLE_EQ(tgt->hp(), hpBefore);
}
