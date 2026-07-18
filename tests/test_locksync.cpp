#include <gtest/gtest.h>
#include <QObject>
#include "core/LockedStepClock.h"
#include <thread>

using namespace gbr;

TEST(LockedStepClockTest, StartsAtZero) {
    LockedStepClock c;
    EXPECT_DOUBLE_EQ(c.simTime(), 0.0);
    EXPECT_DOUBLE_EQ(c.speedMul(), 1.0);
    EXPECT_DOUBLE_EQ(c.tickInterval(), 0.0);
}

TEST(LockedStepClockTest, AdvanceAdvancesTime) {
    LockedStepClock c;
    c.advance(0.5);
    EXPECT_DOUBLE_EQ(c.simTime(), 0.5);
    c.advance(0.25);
    EXPECT_DOUBLE_EQ(c.simTime(), 0.75);
}

TEST(LockedStepClockTest, StepEmitsBarrierSignal) {
    LockedStepClock c;
    double seenTime = -1.0;
    int seenBarrier = -1;
    QObject::connect(&c, &LockedStepClock::stepped, [&](double t, int b) {
        seenTime = t;
        seenBarrier = b;
    });
    c.step(0.1, 42);
    EXPECT_DOUBLE_EQ(seenTime, 0.1);
    EXPECT_EQ(seenBarrier, 42);
}

TEST(LockedStepClockTest, SignalHandlerCanReadClockWithoutDeadlock) {
    LockedStepClock c;
    double readBack = -1.0;
    QObject::connect(&c, &LockedStepClock::stepped,
                     [&](double, int) { readBack = c.simTime(); });

    c.step(0.25, 7);

    EXPECT_DOUBLE_EQ(readBack, 0.25);
}

TEST(LockedStepClockTest, SetSpeedMulChangesValue) {
    LockedStepClock c;
    c.setSpeedMul(2.5);
    EXPECT_DOUBLE_EQ(c.speedMul(), 2.5);
    // Setting the same value is a no-op (no emit).
    c.setSpeedMul(2.5);
    EXPECT_DOUBLE_EQ(c.speedMul(), 2.5);
}

TEST(LockedStepClockTest, AdvanceIsThreadSafe) {
    LockedStepClock c;
    // Simulate concurrent advance from two threads; final time must equal
    // the sum of all advances (no lost updates, no torn reads).
    constexpr int perThread = 1000;
    constexpr double step = 0.001;
    std::thread t1([&]{
        for (int i = 0; i < perThread; ++i) c.advance(step);
    });
    std::thread t2([&]{
        for (int i = 0; i < perThread; ++i) c.advance(step);
    });
    t1.join();
    t2.join();
    EXPECT_NEAR(c.simTime(), 2.0 * perThread * step, 1e-9);
}
