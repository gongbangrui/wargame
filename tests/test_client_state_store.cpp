#include <gtest/gtest.h>

#include "network/ClientStateStore.h"
#include "protocol/Protocol.h"
#include "protocol/StateDelta.h"

#include <QJsonArray>

using namespace gbr;

namespace {

QJsonObject snapshot(qint64 revision) {
    return {{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
            {QStringLiteral("stateRevision"), revision},
            {QStringLiteral("scenario"),
             QJsonObject{{QStringLiteral("schemaVersion"), 1},
                         {QStringLiteral("units"), QJsonArray{}}}},
            {QStringLiteral("units"), QJsonArray{}},
            {QStringLiteral("messages"), QJsonArray{}},
            {QStringLiteral("roomState"),
             QJsonObject{{QStringLiteral("scenarioRevision"), 1},
                         {QStringLiteral("simTime"), 0.0}}}};
}

QJsonObject welcome(quint64 sequence) {
    return Protocol::makeServerEnvelope(
        QStringLiteral("welcome"), sequence,
        QJsonObject{{QStringLiteral("username"), QStringLiteral("red-user")},
                    {QStringLiteral("displayName"), QStringLiteral("红方用户")},
                    {QStringLiteral("role"), QStringLiteral("red")}});
}

void expectSameLifecycle(const Protocol::RoomLifecycleProjection& actual,
                         const Protocol::RoomLifecycleProjection& expected) {
    EXPECT_EQ(actual.phase, expected.phase);
    EXPECT_EQ(actual.roomId, expected.roomId);
    EXPECT_EQ(actual.roomName, expected.roomName);
    EXPECT_EQ(actual.roomStatus, expected.roomStatus);
    EXPECT_EQ(actual.redReady, expected.redReady);
    EXPECT_EQ(actual.blueReady, expected.blueReady);
    EXPECT_EQ(actual.running, expected.running);
    EXPECT_EQ(actual.readyForSim, expected.readyForSim);
    EXPECT_EQ(actual.cpIssues, expected.cpIssues);
    EXPECT_DOUBLE_EQ(actual.simTime, expected.simTime);
    EXPECT_DOUBLE_EQ(actual.speed, expected.speed);
    EXPECT_EQ(actual.scenarioRevision, expected.scenarioRevision);
    EXPECT_EQ(actual.stateRevision, expected.stateRevision);
    ASSERT_EQ(actual.seats.size(), expected.seats.size());
    for (qsizetype index = 0; index < actual.seats.size(); ++index) {
        const auto& actualSeat = actual.seats.at(index);
        const auto& expectedSeat = expected.seats.at(index);
        EXPECT_EQ(actualSeat.seatId, expectedSeat.seatId);
        EXPECT_EQ(actualSeat.seatType, expectedSeat.seatType);
        EXPECT_EQ(actualSeat.side, expectedSeat.side);
        EXPECT_EQ(actualSeat.slot, expectedSeat.slot);
        EXPECT_EQ(actualSeat.capacity, expectedSeat.capacity);
        EXPECT_EQ(actualSeat.occupied, expectedSeat.occupied);
        EXPECT_EQ(actualSeat.displayName, expectedSeat.displayName);
        EXPECT_EQ(actualSeat.ready, expectedSeat.ready);
        EXPECT_EQ(actualSeat.connected, expectedSeat.connected);
        EXPECT_EQ(actualSeat.deployed, expectedSeat.deployed);
        EXPECT_EQ(actualSeat.pendingTransfer, expectedSeat.pendingTransfer);
        EXPECT_EQ(actualSeat.redeployRequested, expectedSeat.redeployRequested);
        EXPECT_EQ(actualSeat.unitId, expectedSeat.unitId);
        EXPECT_EQ(actualSeat.selectedTemplate, expectedSeat.selectedTemplate);
        EXPECT_EQ(actualSeat.unitName, expectedSeat.unitName);
    }
}

} // namespace

TEST(ClientStateStoreTest, AppliesContiguousSnapshotAndDelta) {
    ClientStateStore store;
    store.beginConnection();
    EXPECT_EQ(store.applyEnvelope(welcome(1)).disposition,
              ClientStateStore::Disposition::Accepted);
    const QJsonObject base = snapshot(10);
    EXPECT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 2, base)).disposition,
              ClientStateStore::Disposition::SnapshotApplied);
    EXPECT_EQ(store.lifecycle().scenarioRevision, 1);
    EXPECT_EQ(store.lifecycle().phase, QStringLiteral("preparing"));

    QJsonObject current = base;
    current[QStringLiteral("stateRevision")] = 11;
    current[QStringLiteral("roomState")] =
        QJsonObject{{QStringLiteral("scenarioRevision"), 1},
                    {QStringLiteral("simTime"), 0.1}};
    const QJsonObject delta = StateDelta::create(base, current);
    EXPECT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("delta"), 3, delta)).disposition,
              ClientStateStore::Disposition::DeltaApplied);
    EXPECT_EQ(store.lastSequence(), 3u);
    EXPECT_EQ(store.snapshot(), current);
}

TEST(ClientStateStoreTest, GapDoesNotAdvanceCursorAndSnapshotRecovers) {
    ClientStateStore store;
    store.beginConnection();
    ASSERT_EQ(store.applyEnvelope(welcome(1)).disposition,
              ClientStateStore::Disposition::Accepted);
    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 2, snapshot(10))).disposition,
              ClientStateStore::Disposition::SnapshotApplied);

    const QJsonObject future = snapshot(11);
    const QJsonObject delta = StateDelta::create(snapshot(10), future);
    EXPECT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("delta"), 4, delta)).disposition,
              ClientStateStore::Disposition::ResyncRequired);
    EXPECT_EQ(store.lastSequence(), 2u);
    EXPECT_TRUE(store.waitingForResync());

    const QJsonObject recovered = snapshot(15);
    EXPECT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 6, recovered)).disposition,
              ClientStateStore::Disposition::SnapshotApplied);
    EXPECT_EQ(store.lastSequence(), 6u);
    EXPECT_FALSE(store.waitingForResync());
    EXPECT_EQ(store.snapshot(), recovered);
}

TEST(ClientStateStoreTest, NewConnectionRequiresFreshSnapshot) {
    ClientStateStore store;
    store.beginConnection();
    ASSERT_EQ(store.applyEnvelope(welcome(1)).disposition,
              ClientStateStore::Disposition::Accepted);
    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 2, snapshot(10))).disposition,
              ClientStateStore::Disposition::SnapshotApplied);

    store.beginConnection();
    ASSERT_EQ(store.applyEnvelope(welcome(1)).disposition,
              ClientStateStore::Disposition::Accepted);
    const QJsonObject delta = StateDelta::create(snapshot(10), snapshot(11));
    EXPECT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("delta"), 2, delta)).disposition,
              ClientStateStore::Disposition::ResyncRequired);
    EXPECT_TRUE(store.waitingForSnapshot());
}

TEST(ClientStateStoreTest, RejectedDeltaPreservesStateAndRequiresSnapshot) {
    ClientStateStore store;
    store.beginConnection();
    ASSERT_EQ(store.applyEnvelope(welcome(1)).disposition,
              ClientStateStore::Disposition::Accepted);

    QJsonObject base = snapshot(10);
    base[QStringLiteral("roomState")] =
        QJsonObject{{QStringLiteral("phase"), QStringLiteral("running")},
                    {QStringLiteral("roomId"), QStringLiteral("main")},
                    {QStringLiteral("roomName"), QStringLiteral("QA room")},
                    {QStringLiteral("roomStatus"), QStringLiteral("active")},
                    {QStringLiteral("redReady"), true},
                    {QStringLiteral("blueReady"), false},
                    {QStringLiteral("running"), true},
                    {QStringLiteral("readyForSim"), true},
                    {QStringLiteral("cpIssues"), QStringLiteral("none")},
                    {QStringLiteral("simTime"), 3.5},
                    {QStringLiteral("speed"), 2.0},
                    {QStringLiteral("scenarioRevision"), 1},
                    {QStringLiteral("stateRevision"), 10}};
    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 2, base)).disposition,
              ClientStateStore::Disposition::SnapshotApplied);

    const QJsonObject beforeSnapshot = store.snapshot();
    const Protocol::RoomLifecycleProjection beforeLifecycle = store.lifecycle();
    const quint64 beforeSequence = store.lastSequence();
    QJsonObject malformedDelta{
        {QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
        {QStringLiteral("baseStateRevision"), 10},
        {QStringLiteral("stateRevision"), 11},
        {QStringLiteral("scenarioRevision"), 1},
        {QStringLiteral("units"), QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("unknown-unit")},
            {QStringLiteral("hp"), 1}}}},
        {QStringLiteral("roomState"), base.value(QStringLiteral("roomState"))}};

    const ClientStateStore::Result rejected = store.applyEnvelope(
        Protocol::makeServerEnvelope(QStringLiteral("delta"), 3, malformedDelta));
    EXPECT_EQ(rejected.disposition, ClientStateStore::Disposition::ResyncRequired);
    EXPECT_EQ(rejected.code, QStringLiteral("DELTA_REJECTED"));
    EXPECT_EQ(store.snapshot(), beforeSnapshot);
    expectSameLifecycle(store.lifecycle(), beforeLifecycle);
    EXPECT_EQ(store.lastSequence(), beforeSequence);
    EXPECT_FALSE(store.waitingForSnapshot());
    EXPECT_TRUE(store.waitingForResync());

    const QJsonObject next = snapshot(11);
    const QJsonObject validDelta = StateDelta::create(beforeSnapshot, next);
    EXPECT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("delta"), 4, validDelta)).disposition,
              ClientStateStore::Disposition::Ignored);
    EXPECT_EQ(store.snapshot(), beforeSnapshot);
    expectSameLifecycle(store.lifecycle(), beforeLifecycle);
    EXPECT_EQ(store.lastSequence(), beforeSequence);

    const QJsonObject recovered = snapshot(15);
    EXPECT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 5, recovered)).disposition,
              ClientStateStore::Disposition::SnapshotApplied);
    EXPECT_FALSE(store.waitingForResync());
    EXPECT_EQ(store.snapshot(), recovered);

    QJsonObject recoveredCurrent = recovered;
    recoveredCurrent[QStringLiteral("stateRevision")] = 16;
    recoveredCurrent[QStringLiteral("roomState")] =
        QJsonObject{{QStringLiteral("scenarioRevision"), 1},
                    {QStringLiteral("simTime"), 0.1}};
    const QJsonObject recoveredDelta = StateDelta::create(recovered, recoveredCurrent);
    EXPECT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("delta"), 6, recoveredDelta)).disposition,
              ClientStateStore::Disposition::DeltaApplied);
    EXPECT_EQ(store.snapshot(), recoveredCurrent);
}

TEST(ClientStateStoreTest, ProjectionRejectedDeltaRollsBackAndBlocksFollowingDeltas) {
    ClientStateStore store;
    store.beginConnection();
    ASSERT_EQ(store.applyEnvelope(welcome(1)).disposition,
              ClientStateStore::Disposition::Accepted);

    QJsonObject base = snapshot(10);
    base[QStringLiteral("roomState")] =
        QJsonObject{{QStringLiteral("scenarioRevision"), 1},
                    {QStringLiteral("stateRevision"), 10},
                    {QStringLiteral("simTime"), 2.0}};
    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 2, base)).disposition,
              ClientStateStore::Disposition::SnapshotApplied);
    const QJsonObject beforeSnapshot = store.snapshot();
    const Protocol::RoomLifecycleProjection beforeLifecycle = store.lifecycle();

    QJsonObject current = base;
    current[QStringLiteral("stateRevision")] = 11;
    Protocol::SnapshotProjection directProjection;
    ASSERT_FALSE(Protocol::projectSnapshot(current, &directProjection).valid);
    const QJsonObject projectionInvalidDelta = StateDelta::create(base, current);
    const ClientStateStore::Result rejected = store.applyEnvelope(
        Protocol::makeServerEnvelope(QStringLiteral("delta"), 3, projectionInvalidDelta));
    EXPECT_EQ(rejected.disposition, ClientStateStore::Disposition::ResyncRequired);
    EXPECT_EQ(rejected.code, QStringLiteral("STATE_PROJECTION_REJECTED"));
    EXPECT_EQ(store.snapshot(), beforeSnapshot);
    expectSameLifecycle(store.lifecycle(), beforeLifecycle);
    EXPECT_EQ(store.lastSequence(), 2u);
    EXPECT_TRUE(store.waitingForResync());

    QJsonObject later = current;
    later[QStringLiteral("roomState")] =
        QJsonObject{{QStringLiteral("scenarioRevision"), 1},
                    {QStringLiteral("stateRevision"), 11},
                    {QStringLiteral("simTime"), 2.1}};
    const QJsonObject laterDelta = StateDelta::create(base, later);
    EXPECT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("delta"), 4, laterDelta)).disposition,
              ClientStateStore::Disposition::Ignored);
    EXPECT_EQ(store.snapshot(), beforeSnapshot);
    EXPECT_EQ(store.lastSequence(), 2u);

    const QJsonObject recovered = snapshot(20);
    EXPECT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 5, recovered)).disposition,
              ClientStateStore::Disposition::SnapshotApplied);
    EXPECT_FALSE(store.waitingForResync());
    EXPECT_EQ(store.snapshot(), recovered);
}
