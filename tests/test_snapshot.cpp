#include <gtest/gtest.h>
#include "core/SnapshotCodec.h"
#include "core/SimulationEngine.h"
#include "core/UnitBase.h"
#include "core/Geo.h"

using namespace gbr;

TEST(SnapshotCodecTest, EncodeScenarioRoundTrip) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const Scenario& original = engine.scenario();

    const QJsonObject json = SnapshotCodec::encodeScenario(original);
    EXPECT_TRUE(json.contains("units"));
    EXPECT_TRUE(json.contains("map"));

    Scenario decoded = SnapshotCodec::decodeScenario(json);
    EXPECT_EQ(decoded.units.size(), original.units.size());
    EXPECT_DOUBLE_EQ(decoded.map.widthMeters, original.map.widthMeters);
}

TEST(SnapshotCodecTest, EncodeRuntimeContainsAllUnits) {
    SimulationEngine engine;
    engine.loadDefaultScenario();

    const QJsonArray snap = SnapshotCodec::encodeRuntimeUnits(engine);
    const QStringList expectedIds = engine.unitIds();
    EXPECT_EQ(snap.size(), expectedIds.size());

    QSet<QString> seen;
    for (const auto& v : snap) {
        const auto o = v.toObject();
        seen.insert(o.value("id").toString());
    }
    for (const auto& id : expectedIds) {
        EXPECT_TRUE(seen.contains(id)) << "missing runtime snapshot for " << id.toStdString();
    }
}

TEST(SnapshotCodecTest, DecodeRuntimePreservesHpAndPosition) {
    SimulationEngine server;
    server.loadDefaultScenario();

    // Find a mobile unit, damage it, move it; encode then decode into a fresh engine.
    auto* recon = server.unit("red_r1");
    ASSERT_NE(recon, nullptr);
    recon->setHp(60.0);
    recon->setPosition(GeoPos{12345, 6789, 100});

    const QJsonArray snap = SnapshotCodec::encodeRuntimeUnits(server);

    SimulationEngine client;
    client.loadDefaultScenario();
    SnapshotCodec::decodeRuntimeUnits(client, snap);

    auto* reconC = client.unit("red_r1");
    ASSERT_NE(reconC, nullptr);
    EXPECT_DOUBLE_EQ(reconC->hp(), 60.0);
    EXPECT_DOUBLE_EQ(reconC->pos().x, 12345);
    EXPECT_DOUBLE_EQ(reconC->pos().y, 6789);
}

TEST(SnapshotCodecTest, DecodeRuntimeMarksDeadUnits) {
    SimulationEngine server;
    server.loadDefaultScenario();
    auto* attacker = server.unit("red_a1");
    auto* target = server.unit("blue_r1");
    ASSERT_NE(attacker, nullptr);
    ASSERT_NE(target, nullptr);
    target->setHp(0.0);

    const QJsonArray snap = SnapshotCodec::encodeRuntimeUnits(server);

    SimulationEngine client;
    client.loadDefaultScenario();
    SnapshotCodec::decodeRuntimeUnits(client, snap);

    auto* targetC = client.unit("blue_r1");
    ASSERT_NE(targetC, nullptr);
    EXPECT_FALSE(targetC->alive());
    EXPECT_DOUBLE_EQ(targetC->hp(), 0.0);
}

TEST(SnapshotCodecTest, DiffEmpty) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const QJsonArray a = SnapshotCodec::encodeRuntimeUnits(engine);
    const QJsonArray b = SnapshotCodec::encodeRuntimeUnits(engine);
    EXPECT_TRUE(SnapshotCodec::diffUnitIds(a, b).isEmpty());
}

TEST(SnapshotCodecTest, DiffDetectsHpChange) {
    SimulationEngine server;
    server.loadDefaultScenario();
    auto* recon = server.unit("red_r1");
    ASSERT_NE(recon, nullptr);
    recon->setHp(50.0);
    const QJsonArray a = SnapshotCodec::encodeRuntimeUnits(server);

    // Build a parallel array that pretends HP is back at full.
    auto* recon2 = server.unit("red_r1");
    ASSERT_NE(recon2, nullptr);
    recon2->setHp(100.0);
    const QJsonArray b = SnapshotCodec::encodeRuntimeUnits(server);

    const QStringList diff = SnapshotCodec::diffUnitIds(a, b);
    EXPECT_TRUE(diff.contains("red_r1"));
}

TEST(SnapshotCodecTest, DiffDetectsUnitsOnlyPresentOnRight) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const QJsonArray left = SnapshotCodec::encodeRuntimeUnits(engine);
    QJsonArray right = left;
    right.append(QJsonObject{{"id", "new_remote_unit"},
                             {"hp", 100.0},
                             {"position", QJsonArray{1.0, 2.0, 0.0}}});

    EXPECT_TRUE(SnapshotCodec::diffUnitIds(left, right).contains("new_remote_unit"));
}
