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
                         {QStringLiteral("map"),
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("test")},
                                      {QStringLiteral("widthMeters"), 1000.0},
                                      {QStringLiteral("heightMeters"), 800.0}}},
                         {QStringLiteral("units"), QJsonArray{}}}},
            {QStringLiteral("units"), QJsonArray{}},
            {QStringLiteral("projectiles"), QJsonArray{}},
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

QJsonObject observerUnit(const QString& id, const QString& side, double hp) {
    return {{QStringLiteral("id"), id},
            {QStringLiteral("callsign"), id},
            {QStringLiteral("kind"), QStringLiteral("groundscout")},
            {QStringLiteral("side"), side},
            {QStringLiteral("movable"), true},
            {QStringLiteral("position"), QJsonArray{1.0, 2.0, 0.0}},
            {QStringLiteral("detectRange"), 5000.0},
            {QStringLiteral("attackRange"), 0.0},
            {QStringLiteral("commRange"), 15000.0},
            {QStringLiteral("speed"), 10.0},
            {QStringLiteral("baseSpeed"), 10.0},
            {QStringLiteral("maxCommandedSpeed"), 36.0},
            {QStringLiteral("maxHp"), 100.0},
            {QStringLiteral("attackPower"), 0.0},
            {QStringLiteral("hp"), hp},
            {QStringLiteral("alive"), hp > 0.0},
            {QStringLiteral("armor"), 0.0},
            {QStringLiteral("subsystems"),
             QJsonObject{{QStringLiteral("sensor"), 1.0},
                         {QStringLiteral("comms"), 1.0},
                         {QStringLiteral("mobility"), 1.0},
                         {QStringLiteral("weapon"), 1.0}}},
            {QStringLiteral("serviceRequested"), false},
            {QStringLiteral("serviceProgress"), 0.0}};
}

QJsonObject observerScenarioUnit(const QString& id, const QString& side) {
    return {{QStringLiteral("id"), id},
            {QStringLiteral("callsign"), id},
            {QStringLiteral("kind"), QStringLiteral("groundscout")},
            {QStringLiteral("side"), side},
            {QStringLiteral("x"), 1.0},
            {QStringLiteral("y"), 2.0},
            {QStringLiteral("alt"), 0.0},
            {QStringLiteral("detectRange"), 5000.0},
            {QStringLiteral("attackRange"), 0.0},
            {QStringLiteral("commRange"), 15000.0},
            {QStringLiteral("speed"), 10.0},
            {QStringLiteral("maxHp"), 100.0},
            {QStringLiteral("attackPower"), 0.0},
            {QStringLiteral("armor"), 0.0},
            {QStringLiteral("ammoCapacity"), 0},
            {QStringLiteral("initialAmmo"), 0},
            {QStringLiteral("cooldownSec"), 0.0},
            {QStringLiteral("fuelCapacitySec"), 1.0},
            {QStringLiteral("initialFuelSec"), 0.0}};
}

QJsonObject observerSnapshot(qint64 revision, bool observer = true) {
    const QJsonArray units{observerUnit(QStringLiteral("blue_r1"), QStringLiteral("blue"), 100.0),
                           observerUnit(QStringLiteral("red_r1"), QStringLiteral("red"), 100.0)};
    const QJsonArray scenarioUnits{
        observerScenarioUnit(QStringLiteral("blue_r1"), QStringLiteral("blue")),
        observerScenarioUnit(QStringLiteral("red_r1"), QStringLiteral("red"))};
    return {{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
            {QStringLiteral("stateRevision"), revision},
            {QStringLiteral("scenario"),
             QJsonObject{{QStringLiteral("schemaVersion"), 1},
                         {QStringLiteral("map"),
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("default")},
                                      {QStringLiteral("widthMeters"), 40000.0},
                                      {QStringLiteral("heightMeters"), 30000.0},
                                      {QStringLiteral("backgroundResource"), QStringLiteral("")}}},
                         {QStringLiteral("units"), scenarioUnits}}},
            {QStringLiteral("units"), units},
            {QStringLiteral("projectiles"), QJsonArray{}},
            {QStringLiteral("roomState"),
             QJsonObject{{QStringLiteral("observer"), observer},
                         {QStringLiteral("scenarioRevision"), 1},
                         {QStringLiteral("stateRevision"), revision},
                         {QStringLiteral("simTime"), 0.0}}}};
}

void expectSameLifecycle(const Protocol::RoomLifecycleProjection& actual,
                         const Protocol::RoomLifecycleProjection& expected) {
    EXPECT_EQ(actual.phase, expected.phase);
    EXPECT_EQ(actual.roomId, expected.roomId);
    EXPECT_EQ(actual.roomName, expected.roomName);
    EXPECT_EQ(actual.roomStatus, expected.roomStatus);
    EXPECT_EQ(actual.roomMode, expected.roomMode);
    EXPECT_EQ(actual.aiDifficulty, expected.aiDifficulty);
    EXPECT_EQ(actual.configVersion, expected.configVersion);
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
        EXPECT_EQ(actualSeat.controllerType, expectedSeat.controllerType);
    }
}

TEST(ClientStateStoreTest, ProjectsPveConfigurationAndAiSeatsAcrossDelta) {
    ClientStateStore store;
    store.beginConnection();
    ASSERT_EQ(store.applyEnvelope(welcome(1)).disposition,
              ClientStateStore::Disposition::Accepted);

    QJsonObject base = snapshot(10);
    base[QStringLiteral("roomState")] =
        QJsonObject{{QStringLiteral("roomId"), QStringLiteral("main")},
                    {QStringLiteral("roomMode"), QStringLiteral("pve")},
                    {QStringLiteral("aiDifficulty"), QStringLiteral("hard")},
                    {QStringLiteral("configVersion"), 3},
                    {QStringLiteral("scenarioRevision"), 1},
                    {QStringLiteral("simTime"), 0.0},
                    {QStringLiteral("seats"), QJsonArray{QJsonObject{
                        {QStringLiteral("seatId"), QStringLiteral("blue_commander")},
                        {QStringLiteral("seatType"), QStringLiteral("commander")},
                        {QStringLiteral("side"), QStringLiteral("blue")},
                        {QStringLiteral("occupied"), true},
                        {QStringLiteral("controllerType"), QStringLiteral("ai")}}}}};
    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 2, base)).disposition,
              ClientStateStore::Disposition::SnapshotApplied);
    EXPECT_EQ(store.lifecycle().roomMode, QStringLiteral("pve"));
    EXPECT_EQ(store.lifecycle().aiDifficulty, QStringLiteral("hard"));
    EXPECT_EQ(store.lifecycle().configVersion, 3);
    ASSERT_EQ(store.lifecycle().seats.size(), 1);
    EXPECT_EQ(store.lifecycle().seats.front().controllerType, QStringLiteral("ai"));

    QJsonObject current = base;
    current[QStringLiteral("stateRevision")] = 11;
    QJsonObject currentRoomState = current.value(QStringLiteral("roomState")).toObject();
    currentRoomState[QStringLiteral("configVersion")] = 4;
    current[QStringLiteral("roomState")] = currentRoomState;
    const QJsonObject delta = StateDelta::create(base, current);
    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("delta"), 3, delta)).disposition,
              ClientStateStore::Disposition::DeltaApplied);
    EXPECT_EQ(store.lifecycle().configVersion, 4);
    EXPECT_EQ(store.lifecycle().seats.front().controllerType, QStringLiteral("ai"));
}

TEST(ClientStateStoreTest, ObserverSnapshotDeltaAndResyncRemainContiguous) {
    ClientStateStore store;
    store.beginConnection();
    ASSERT_EQ(store.applyEnvelope(welcome(1)).disposition,
              ClientStateStore::Disposition::Accepted);

    const QJsonObject base = observerSnapshot(10);
    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 2, base)).disposition,
              ClientStateStore::Disposition::SnapshotApplied);
    EXPECT_TRUE(store.lifecycle().observer);
    EXPECT_FALSE(store.snapshot().contains(QStringLiteral("messages")));

    QJsonObject current = observerSnapshot(11);
    current[QStringLiteral("units")] = QJsonArray{
        observerUnit(QStringLiteral("blue_r1"), QStringLiteral("blue"), 75.0),
        observerUnit(QStringLiteral("red_r1"), QStringLiteral("red"), 100.0)};
    const QJsonObject delta = StateDelta::create(base, current);
    ASSERT_FALSE(delta.isEmpty());
    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("delta"), 3, delta)).disposition,
              ClientStateStore::Disposition::DeltaApplied);
    EXPECT_TRUE(store.lifecycle().observer);
    EXPECT_EQ(store.snapshot(), current);
    EXPECT_DOUBLE_EQ(store.snapshot().value(QStringLiteral("units")).toArray().at(0)
                         .toObject()
                         .value(QStringLiteral("hp"))
                         .toDouble(),
                     75.0);

    const QJsonObject futureDelta = StateDelta::create(current, observerSnapshot(12));
    EXPECT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("delta"), 5, futureDelta)).disposition,
              ClientStateStore::Disposition::ResyncRequired);
    EXPECT_TRUE(store.waitingForResync());
    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 7, observerSnapshot(20))).disposition,
              ClientStateStore::Disposition::SnapshotApplied);
    EXPECT_TRUE(store.lifecycle().observer);
    EXPECT_FALSE(store.waitingForResync());
    const QJsonArray recoveredUnits = store.snapshot().value(QStringLiteral("units")).toArray();
    ASSERT_EQ(recoveredUnits.size(), 2);
    EXPECT_EQ(recoveredUnits.at(0).toObject().value(QStringLiteral("side")).toString(),
              QStringLiteral("blue"));
    EXPECT_EQ(recoveredUnits.at(1).toObject().value(QStringLiteral("side")).toString(),
              QStringLiteral("red"));
}

TEST(ClientStateStoreTest, NormalizesEmptyLegacyObserverMapMarksAndRejectsVisibleMarkers) {
    ClientStateStore store;
    store.beginConnection();
    ASSERT_EQ(store.applyEnvelope(welcome(1)).disposition,
              ClientStateStore::Disposition::Accepted);

    QJsonObject legacySnapshot = observerSnapshot(10);
    legacySnapshot[QStringLiteral("mapMarks")] = QJsonArray{};
    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 2, legacySnapshot)).disposition,
              ClientStateStore::Disposition::SnapshotApplied);
    EXPECT_TRUE(store.lifecycle().observer);
    EXPECT_FALSE(store.snapshot().contains(QStringLiteral("mapMarks")));

    QJsonObject currentLegacySnapshot = legacySnapshot;
    currentLegacySnapshot[QStringLiteral("stateRevision")] = 11;
    QJsonObject currentRoomState = currentLegacySnapshot.value(QStringLiteral("roomState")).toObject();
    currentRoomState[QStringLiteral("stateRevision")] = 11;
    currentLegacySnapshot[QStringLiteral("roomState")] = currentRoomState;
    QJsonObject legacyDelta = StateDelta::create(legacySnapshot, currentLegacySnapshot);
    legacyDelta[QStringLiteral("mapMarks")] = QJsonArray{};
    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("delta"), 3, legacyDelta)).disposition,
              ClientStateStore::Disposition::DeltaApplied);
    EXPECT_EQ(store.stateRevision(), 11);
    EXPECT_FALSE(store.snapshot().contains(QStringLiteral("mapMarks")));

    ClientStateStore rejectingStore;
    rejectingStore.beginConnection();
    ASSERT_EQ(rejectingStore.applyEnvelope(welcome(1)).disposition,
              ClientStateStore::Disposition::Accepted);
    QJsonObject visibleMarkerSnapshot = observerSnapshot(10);
    visibleMarkerSnapshot[QStringLiteral("mapMarks")] = QJsonArray{
        QJsonObject{{QStringLiteral("label"), QStringLiteral("private mark")}}};
    const ClientStateStore::Result rejected = rejectingStore.applyEnvelope(
        Protocol::makeServerEnvelope(QStringLiteral("snapshot"), 2, visibleMarkerSnapshot));
    EXPECT_EQ(rejected.disposition, ClientStateStore::Disposition::Fatal);
    EXPECT_EQ(rejected.message, QStringLiteral("观察员快照结构无效"));
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

TEST(ClientStateStoreTest, ProjectileRemovalInDeltaClearsRenderedState) {
    ClientStateStore store;
    store.beginConnection();
    ASSERT_EQ(store.applyEnvelope(welcome(1)).disposition,
              ClientStateStore::Disposition::Accepted);

    QJsonObject base = snapshot(10);
    base[QStringLiteral("projectiles")] = QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("missile_1")},
        {QStringLiteral("side"), QStringLiteral("red")},
        {QStringLiteral("position"), QJsonArray{500.0, 400.0, 120.0}},
        {QStringLiteral("headingRad"), 0.25},
        {QStringLiteral("speed"), 420.0},
        {QStringLiteral("age"), 1.0},
        {QStringLiteral("lifetime"), 16.0},
        {QStringLiteral("active"), true},
        {QStringLiteral("terminalReason"), QString{}},
        {QStringLiteral("terminalAge"), 0.0},
        {QStringLiteral("resultSettled"), false},
        {QStringLiteral("threatRadius"), 1300.0}}};
    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("snapshot"), 2, base)).disposition,
              ClientStateStore::Disposition::SnapshotApplied);
    ASSERT_EQ(store.snapshot().value(QStringLiteral("projectiles")).toArray().size(), 1);

    QJsonObject current = base;
    current[QStringLiteral("stateRevision")] = 11;
    current[QStringLiteral("projectiles")] = QJsonArray{};
    current[QStringLiteral("roomState")] =
        QJsonObject{{QStringLiteral("scenarioRevision"), 1},
                    {QStringLiteral("stateRevision"), 11},
                    {QStringLiteral("simTime"), 0.1}};
    const QJsonObject delta = StateDelta::create(base, current);
    ASSERT_TRUE(delta.contains(QStringLiteral("projectiles")));
    ASSERT_TRUE(delta.value(QStringLiteral("projectiles")).toArray().isEmpty());

    ASSERT_EQ(store.applyEnvelope(Protocol::makeServerEnvelope(
                  QStringLiteral("delta"), 3, delta)).disposition,
              ClientStateStore::Disposition::DeltaApplied);
    EXPECT_TRUE(store.snapshot().value(QStringLiteral("projectiles")).toArray().isEmpty());
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

TEST(ClientStateStoreTest, InvalidMergedStateRollsBackAndBlocksFollowingDeltas) {
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
    const QJsonObject projectionInvalidDelta{
        {QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
        {QStringLiteral("baseStateRevision"), 10},
        {QStringLiteral("stateRevision"), 11},
        {QStringLiteral("scenarioRevision"), 1},
        {QStringLiteral("units"), QJsonArray{}},
        {QStringLiteral("roomState"), current.value(QStringLiteral("roomState"))}};
    const ClientStateStore::Result rejected = store.applyEnvelope(
        Protocol::makeServerEnvelope(QStringLiteral("delta"), 3, projectionInvalidDelta));
    EXPECT_EQ(rejected.disposition, ClientStateStore::Disposition::ResyncRequired);
    EXPECT_EQ(rejected.code, QStringLiteral("DELTA_REJECTED"));
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
