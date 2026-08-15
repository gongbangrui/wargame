#include <gtest/gtest.h>
#include "core/MessageBus.h"

using namespace gbr;

class MessageBusTest : public ::testing::Test {
protected:
    void SetUp() override {
        bus.subscribe("red_cp", [](const Message&) {});
        bus.subscribe("red_r1", [](const Message&) {});
        bus.subscribe("blue_cp", [](const Message&) {});
        bus.subscribe("blue_r1", [](const Message&) {});
        bus.updateUnitPosition("red_cp", QPointF(0, 0), 20000, "red");
        bus.updateUnitPosition("red_r1", QPointF(0, 0), 15000, "red");
        bus.updateUnitPosition("blue_cp", QPointF(50000, 0), 20000, "blue");
        bus.updateUnitPosition("blue_r1", QPointF(50000, 0), 15000, "blue");

        bus.setUnitCommandPost("red_cp", true);
        bus.setUnitCommandPost("blue_cp", true);
    }

    MessageBus bus;
};

TEST_F(MessageBusTest, SameSideWithinRange) {
    EXPECT_TRUE(bus.canCommunicate("red_cp", "red_r1"));
}

TEST_F(MessageBusTest, CrossSideNoComm) {
    EXPECT_FALSE(bus.canCommunicate("red_cp", "blue_cp"));
}

TEST_F(MessageBusTest, UnregisteredUnit) {
    EXPECT_FALSE(bus.canCommunicate("red_cp", "unknown_unit"));
}

TEST_F(MessageBusTest, CpRespectsRangeWhenNoRelayExists) {
    // A command post no longer has a hidden global bypass.
    bus.updateUnitPosition("red_r1", QPointF(100000, 100000), 15000, "red");
    EXPECT_FALSE(bus.canCommunicate("red_cp", "red_r1"));
}

TEST_F(MessageBusTest, CpPositionUpdateBreaksAndRestoresDirectLink) {
    bus.updateUnitPosition("red_cp", QPointF(100000, 100000), 20000, "red");
    bus.updateUnitPosition("red_r1", QPointF(0, 0), 15000, "red");
    EXPECT_FALSE(bus.canCommunicate("red_cp", "red_r1"));
    bus.updateUnitPosition("red_cp", QPointF(5000, 0), 20000, "red");
    EXPECT_TRUE(bus.canCommunicate("red_cp", "red_r1"));
}

TEST_F(MessageBusTest, FriendlyRelayRestoresCommandPostReachability) {
    bus.subscribe("red_relay", [](const Message&) {});
    bus.updateUnitPosition("red_cp", QPointF(0, 0), 2000, "red");
    bus.updateUnitPosition("red_relay", QPointF(1500, 0), 2000, "red");
    bus.updateUnitPosition("red_r1", QPointF(3000, 0), 1000, "red");
    EXPECT_TRUE(bus.canCommunicate("red_cp", "red_r1"));
}

TEST_F(MessageBusTest, InactiveUnitCannotTransmitOrRelay) {
    bus.subscribe("red_relay", [](const Message&) {});
    bus.updateUnitPosition("red_cp", QPointF(0, 0), 2000, "red");
    bus.updateUnitPosition("red_relay", QPointF(1500, 0), 2000, "red");
    bus.updateUnitPosition("red_r1", QPointF(3000, 0), 1000, "red");
    ASSERT_TRUE(bus.canCommunicate("red_cp", "red_r1"));

    bus.setUnitActive("red_relay", false);
    EXPECT_FALSE(bus.canCommunicate("red_cp", "red_r1"));
    EXPECT_FALSE(bus.canCommunicate("red_relay", "red_r1"));

    bus.setUnitActive("red_relay", true);
    EXPECT_TRUE(bus.canCommunicate("red_cp", "red_r1"));
}

TEST_F(MessageBusTest, NonCpRespectsRange) {
    bus.updateUnitPosition("red_r1", QPointF(100000, 100000), 15000, "red");
    EXPECT_FALSE(bus.canCommunicate("red_r1", "unknown_unit"));
}

TEST_F(MessageBusTest, BroadcastMessage) {
    int deliverCount = 0;
    bus.subscribe("red_r1", [&](const Message&) { deliverCount++; });
    bus.subscribe("blue_r1", [&](const Message&) { deliverCount++; });

    Message msg;
    msg.type = Message::Type::PositionReport;
    msg.sender = "red_cp";
    msg.receiver = "*";
    bus.send(msg);

    // red_r1 is same-side, should receive; blue_r1 is cross-side, should not
    EXPECT_EQ(deliverCount, 1);
}

TEST_F(MessageBusTest, EmptyReceiverIsNotBroadcast) {
    int deliveries = 0;
    bus.subscribe("red_r1", [&](const Message&) { ++deliveries; });
    Message msg;
    msg.sender = "red_cp";
    msg.receiver.clear();
    msg.type = Message::Type::PositionReport;
    bus.send(msg);
    EXPECT_EQ(deliveries, 0);
}

TEST_F(MessageBusTest, DirectMessage) {
    bool received = false;
    bus.subscribe("red_r1", [&](const Message&) { received = true; });

    Message msg;
    msg.type = Message::Type::PositionReport;
    msg.sender = "red_cp";
    msg.receiver = "red_r1";
    bus.send(msg);

    EXPECT_TRUE(received);
}

TEST_F(MessageBusTest, PostedMessageIncludesRegisteredSides) {
    QJsonObject posted;
    QObject::connect(&bus, &MessageBus::messagePosted,
                     [&](const QJsonObject& message) { posted = message; });
    Message msg;
    msg.type = Message::Type::PositionReport;
    msg.sender = "red_cp";
    msg.receiver = "red_r1";

    bus.send(msg);

    EXPECT_EQ(posted.value("senderSide").toString(), "red");
    EXPECT_EQ(posted.value("receiverSide").toString(), "red");
}

TEST_F(MessageBusTest, DirectMessageNoComm) {
    bool received = false;
    bus.subscribe("red_r1", [&](const Message&) { received = true; });

    Message msg;
    msg.type = Message::Type::PositionReport;
    msg.sender = "blue_cp";
    msg.receiver = "red_r1";
    bus.send(msg);

    EXPECT_FALSE(received);
}

TEST_F(MessageBusTest, SubscriptionUnsubscription) {
    int count = 0;
    bus.subscribe("red_r1", [&](const Message&) { count++; });

    Message msg;
    msg.sender = "red_cp";
    msg.receiver = "red_r1";
    msg.type = Message::Type::PositionReport;
    bus.send(msg);
    EXPECT_EQ(count, 1);

    bus.unsubscribe("red_r1");
    bus.send(msg);
    EXPECT_EQ(count, 1); // should not increment
}

TEST_F(MessageBusTest, UnregisterRemovesCommunicationState) {
    ASSERT_TRUE(bus.isRegistered("red_r1"));
    bus.unregisterUnit("red_r1");
    EXPECT_FALSE(bus.isRegistered("red_r1"));
    EXPECT_FALSE(bus.canCommunicate("red_cp", "red_r1"));
}

TEST_F(MessageBusTest, UnitSide) {
    EXPECT_EQ(bus.unitSide("red_cp").toStdString(), "red");
    EXPECT_EQ(bus.unitSide("blue_r1").toStdString(), "blue");
    EXPECT_TRUE(bus.unitSide("unknown").isEmpty());
}

TEST_F(MessageBusTest, MissingSideCannotCommunicateInEitherDirection) {
    bus.updateUnitPosition("unknown_side", QPointF(0, 0), 20000);

    EXPECT_FALSE(bus.canCommunicate("unknown_side", "red_r1"));
    EXPECT_FALSE(bus.canCommunicate("red_r1", "unknown_side"));
}

TEST_F(MessageBusTest, UnknownPositionUpdateDoesNotRegisterUnit) {
    bus.updateUnitPosition("unknown_new", QPointF(0, 0), 1000, "red");
    EXPECT_FALSE(bus.isRegistered("unknown_new"));
}

TEST_F(MessageBusTest, HandlerMayUnsubscribeItselfDuringDelivery) {
    int deliveries = 0;
    bus.subscribe("red_r1", [&](const Message&) {
        ++deliveries;
        bus.unsubscribe("red_r1");
    });
    Message msg;
    msg.sender = "red_cp";
    msg.receiver = "red_r1";

    bus.send(msg);
    bus.send(msg);

    EXPECT_EQ(deliveries, 1);
}

TEST_F(MessageBusTest, BroadcastHandlerMayUnregisterItself) {
    bus.subscribe("red_r2", [](const Message&) {});
    bus.updateUnitPosition("red_r2", QPointF(0, 0), 15000, "red");
    int selfDeliveries = 0;
    int otherDeliveries = 0;
    bus.subscribe("red_r1", [&](const Message&) {
        ++selfDeliveries;
        bus.unregisterUnit("red_r1");
    });
    bus.subscribe("red_r2", [&](const Message&) { ++otherDeliveries; });
    Message msg;
    msg.sender = "red_cp";
    msg.receiver = "*";

    bus.send(msg);

    EXPECT_EQ(selfDeliveries, 1);
    EXPECT_EQ(otherDeliveries, 1);
    EXPECT_FALSE(bus.isRegistered("red_r1"));
}
