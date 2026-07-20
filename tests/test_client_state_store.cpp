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
