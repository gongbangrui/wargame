#include <gtest/gtest.h>
#include "core/MessageBus.h"

using namespace gbr;

class MessageBusTest : public ::testing::Test {
protected:
    void SetUp() override {
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

TEST_F(MessageBusTest, CpBypassesRange) {
    // Move the recon far away; CP can still reach it
    bus.updateUnitPosition("red_r1", QPointF(100000, 100000), 15000, "red");
    EXPECT_TRUE(bus.canCommunicate("red_cp", "red_r1"));
}

TEST_F(MessageBusTest, CpFlagSurvivesPositionUpdate) {
    bus.updateUnitPosition("red_cp", QPointF(100000, 100000), 20000, "red");
    bus.updateUnitPosition("red_r1", QPointF(0, 0), 15000, "red");
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
