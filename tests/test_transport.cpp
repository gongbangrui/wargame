#include <gtest/gtest.h>
#include "core/LocalTransport.h"
#include "core/MessageBus.h"
#include "core/UnitBase.h"
#include "core/SimulationEngine.h"

using namespace gbr;

namespace {

/// Helper: register a unit with the given id/side/pos on a transport + return the unit.
UnitBase* regUnit(ITransport* t, const QString& id, const QString& side,
                  const QPointF& pos = QPointF(0, 0), double commRange = 20000.0) {
    auto* bus = t->bus();
    if (!bus) return nullptr;
    bus->updateUnitPosition(id, pos, commRange, side);
    return nullptr; // This helper is just for transport-level tests; units
                    // themselves are exercised via SimulationEngine.
}

} // namespace

TEST(LocalTransportTest, IsLocalFlag) {
    LocalTransport t;
    EXPECT_TRUE(t.isLocal());
    EXPECT_NE(t.bus(), nullptr);
}

TEST(LocalTransportTest, SubscribeAndDeliver) {
    LocalTransport t;
    int delivered = 0;
    t.subscribe("red_a1", [&](const Message&){ ++delivered; });
    t.updateUnitPosition("red_cp", QPointF(0, 0), 20000, "red");
    t.updateUnitPosition("red_a1", QPointF(1000, 0), 20000, "red");

    Message m;
    m.type = Message::Type::PositionReport;
    m.sender = "red_cp";
    m.receiver = "red_a1";
    t.send(m);

    EXPECT_EQ(delivered, 1);
}

TEST(LocalTransportTest, SinkReceivesEveryEmission) {
    LocalTransport t;
    int sinkHits = 0;
    t.setMessageSink([&](const QJsonObject&){ ++sinkHits; });

    t.updateUnitPosition("red_cp", QPointF(0, 0), 20000, "red");
    t.updateUnitPosition("red_a1", QPointF(1000, 0), 20000, "red");

    Message m;
    m.type = Message::Type::PositionReport;
    m.sender = "red_cp";
    m.receiver = "red_a1";
    t.send(m);
    EXPECT_EQ(sinkHits, 1);
}

TEST(LocalTransportTest, UnregisterCleansState) {
    LocalTransport t;
    t.updateUnitPosition("red_x", QPointF(0, 0), 20000, "red");
    EXPECT_TRUE(t.isRegistered("red_x"));
    t.unregisterUnit("red_x");
    EXPECT_FALSE(t.isRegistered("red_x"));
    EXPECT_FALSE(t.canCommunicate("red_x", "red_cp"));
}

TEST(LocalTransportTest, CrossSideBlocked) {
    LocalTransport t;
    t.updateUnitPosition("red_a1", QPointF(0, 0), 20000, "red");
    t.updateUnitPosition("blue_a1", QPointF(0, 0), 20000, "blue");

    int red = 0, blue = 0;
    t.subscribe("red_a1", [&](const Message&){ ++red; });
    t.subscribe("blue_a1", [&](const Message&){ ++blue; });

    Message m;
    m.type = Message::Type::PositionReport;
    m.sender = "red_a1";
    m.receiver = "blue_a1";
    t.send(m);
    EXPECT_EQ(red, 0);
    EXPECT_EQ(blue, 0);
}

TEST(LocalTransportTest, CpBypassesRange) {
    LocalTransport t;
    t.updateUnitPosition("red_cp", QPointF(0, 0), 20000, "red");
    t.updateUnitPosition("red_a1", QPointF(50000, 50000), 1000, "red");
    t.setUnitCommandPost("red_cp", true);

    int hits = 0;
    t.subscribe("red_a1", [&](const Message&){ ++hits; });

    Message m;
    m.type = Message::Type::AttackOrder;
    m.sender = "red_cp";
    m.receiver = "red_a1";
    t.send(m);
    EXPECT_EQ(hits, 1);
}

TEST(LocalTransportTest, EngineUsesLocalTransportByDefault) {
    SimulationEngine engine;
    EXPECT_NE(engine.transport(), nullptr);
    EXPECT_TRUE(engine.transport()->isLocal());
    EXPECT_NE(engine.bus(), nullptr);
}

TEST(LocalTransportTest, EngineAcceptsExternalTransport) {
    LocalTransport t;
    SimulationEngine engine(static_cast<ITransport*>(&t));
    EXPECT_EQ(engine.transport(), static_cast<ITransport*>(&t));
    EXPECT_TRUE(engine.transport()->isLocal());
}

TEST(LocalTransportTest, ExternalTransportOwnerNotDeleted) {
    LocalTransport t;
    {
        SimulationEngine engine(static_cast<ITransport*>(&t));
        engine.loadDefaultScenario();
        // On destruction, engine must NOT delete an externally-supplied transport.
    }
    EXPECT_TRUE(t.isLocal()); // still valid

    // The engine must also detach the sink lambda that captured its `this`.
    // This send is particularly useful under ASan, where a stale sink becomes
    // an immediate use-after-free report.
    Message afterEngineDestruction;
    afterEngineDestruction.sender = "gone_sender";
    afterEngineDestruction.receiver = "gone_receiver";
    EXPECT_NO_THROW(t.send(afterEngineDestruction));
}
