#include <gtest/gtest.h>
#include "core/Scenario.h"

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
