#include <gtest/gtest.h>
#include "core/UnitFsm.h"

using namespace gbr;

TEST(UnitFsmTest, InitialState) {
    UnitFsm fsm;
    fsm.setInitialState("idle");
    EXPECT_EQ(fsm.currentState(), "idle");
}

TEST(UnitFsmTest, SingleStateTick) {
    UnitFsm fsm;
    bool updated = false;
    fsm.addState("idle", [&](double dt) { updated = true; });
    fsm.setInitialState("idle");
    fsm.tick(0.05);
    EXPECT_TRUE(updated);
}

TEST(UnitFsmTest, Transition) {
    UnitFsm fsm;
    fsm.addState("idle", [](double) {});
    fsm.addState("running", [](double) {});
    bool shouldRun = false;
    fsm.addTransition("idle", "running", [&shouldRun]{ return shouldRun; });
    fsm.setInitialState("idle");

    fsm.tick(0.05);
    EXPECT_EQ(fsm.currentState(), "idle");

    shouldRun = true;
    fsm.tick(0.05);
    EXPECT_EQ(fsm.currentState(), "running");
}

TEST(UnitFsmTest, EnterExitCallbacks) {
    UnitFsm fsm;
    std::vector<QString> log;

    fsm.addState("a", [](double){},
        [&]{ log.push_back("enter_a"); },
        [&]{ log.push_back("exit_a"); });
    fsm.addState("b", [](double){},
        [&]{ log.push_back("enter_b"); },
        [&]{ log.push_back("exit_b"); });
    fsm.addTransition("a", "b", []{ return true; });

    fsm.setInitialState("a"); // enter_a called here
    fsm.tick(0.05); // transition a→b: exit_a + enter_b

    ASSERT_GE(log.size(), 3);
    EXPECT_EQ(log[0].toStdString(), "enter_a");
    EXPECT_EQ(log[1].toStdString(), "exit_a");
    EXPECT_EQ(log[2].toStdString(), "enter_b");
}

TEST(UnitFsmTest, GoToForceTransition) {
    UnitFsm fsm;
    fsm.addState("idle", [](double){});
    fsm.addState("error", [](double){});

    fsm.setInitialState("idle");
    EXPECT_EQ(fsm.currentState(), "idle");

    fsm.goTo("error");
    EXPECT_EQ(fsm.currentState(), "error");
}

TEST(UnitFsmTest, GoToSameStateNoop) {
    UnitFsm fsm;
    bool exitCalled = false;
    fsm.addState("idle", [](double){}, nullptr, [&]{ exitCalled = true; });

    fsm.setInitialState("idle");
    fsm.goTo("idle");
    EXPECT_FALSE(exitCalled);
}

TEST(UnitFsmTest, UnknownTargetDoesNotCorruptCurrentState) {
    UnitFsm fsm;
    fsm.addState("idle", [](double){});
    fsm.setInitialState("idle");

    fsm.goTo("missing");

    EXPECT_EQ(fsm.currentState(), "idle");
}

TEST(UnitFsmTest, TransitionPriority) {
    UnitFsm fsm;
    fsm.addState("idle", [](double){});
    fsm.addState("a", [](double){});
    fsm.addState("b", [](double){});

    // Both transitions could fire; first registered wins
    fsm.addTransition("idle", "a", []{ return true; });
    fsm.addTransition("idle", "b", []{ return true; });

    fsm.setInitialState("idle");
    fsm.tick(0.05);
    EXPECT_EQ(fsm.currentState(), "a");
}
