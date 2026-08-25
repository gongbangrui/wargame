#include <gtest/gtest.h>

#include "IntelLedger.h"

#include <QDateTime>
#include <QJsonObject>

using namespace gbr;

namespace {

QDateTime atSeconds(int seconds) {
    return QDateTime::fromString(
        QStringLiteral("2026-08-10T00:00:%1.000Z").arg(seconds, 2, 10, QLatin1Char('0')),
        Qt::ISODateWithMs);
}

QJsonObject position(double x, double y) {
    return QJsonObject{{QStringLiteral("x"), x}, {QStringLiteral("y"), y}};
}

} // namespace

TEST(IntelLedgerTest, StableSensorObservationsDoNotFloodHistory) {
    IntelLedger ledger;
    const QJsonObject attributes{{QStringLiteral("callsign"), QStringLiteral("contact-1")},
                                 {QStringLiteral("kind"), QStringLiteral("attackuav")}};
    const auto first = ledger.observeSensor("red_recon", "blue_1", attributes,
                                            position(100, 200), "red_recon_unit", atSeconds(0));
    ASSERT_TRUE(first.ok);
    EXPECT_TRUE(first.changed);
    EXPECT_EQ(ledger.state("red_recon").revision, 1);
    EXPECT_EQ(ledger.history("red_recon").size(), 1);

    const auto repeated = ledger.observeSensor("red_recon", "blue_1", attributes,
                                               position(100, 200), "red_recon_unit", atSeconds(1));
    ASSERT_TRUE(repeated.ok);
    EXPECT_FALSE(repeated.changed);
    EXPECT_EQ(repeated.code, QStringLiteral("UNCHANGED"));
    EXPECT_EQ(ledger.state("red_recon").revision, 1);
    EXPECT_EQ(ledger.history("red_recon").size(), 1);

    const auto refreshed = ledger.observeSensor("red_recon", "blue_1", attributes,
                                                position(110, 205), "red_recon_unit", atSeconds(2));
    ASSERT_TRUE(refreshed.ok);
    EXPECT_TRUE(refreshed.changed);
    EXPECT_EQ(ledger.history("red_recon").size(), 1);

    const auto moved = ledger.observeSensor("red_recon", "blue_1", attributes,
                                            position(120, 210), "red_recon_unit", atSeconds(6));
    ASSERT_TRUE(moved.ok);
    EXPECT_TRUE(moved.changed);
    EXPECT_EQ(ledger.history("red_recon").size(), 2);
    const auto history = ledger.historyPage(
        "red_recon", Protocol::IntelHistoryQuery{QString(), 20, QStringLiteral("blue_1")});
    ASSERT_EQ(history.entries.size(), 2);
    EXPECT_EQ(history.entries.constFirst().targetId, QStringLiteral("blue_1"));
}

TEST(IntelLedgerTest, MovingContactsKeepBoundedSampledHistory) {
    IntelLedger ledger;
    const QJsonObject attributes{{QStringLiteral("callsign"), QStringLiteral("moving")},
                                 {QStringLiteral("kind"), QStringLiteral("attackuav")}};
    ASSERT_TRUE(ledger.observeSensor("red_recon", "blue_1", attributes,
                                    position(0, 0), "red_recon_unit", atSeconds(0)).ok);

    QDateTime observed = atSeconds(0);
    for (int index = 1; index <= 4000; ++index) {
        observed = observed.addMSecs(100);
        ASSERT_TRUE(ledger.observeSensor(
            "red_recon", "blue_1", attributes, position(index, index / 2.0),
            "red_recon_unit", observed).ok);
    }

    EXPECT_LE(ledger.history("red_recon").size(), IntelLedger::HistoryLimit);
    EXPECT_LT(ledger.history("red_recon").size(), 100);
    ASSERT_EQ(ledger.state("red_recon").records.size(), 1);
    EXPECT_GE(ledger.state("red_recon").records.constFirst()
                  .lastPosition.value(QStringLiteral("x")).toDouble(),
              3980.0);
}

TEST(IntelLedgerTest, RestoreTrimsLegacyOversizedHistory) {
    IntelLedger source;
    for (int index = 0; index < IntelLedger::HistoryLimit + 20; ++index) {
        ASSERT_TRUE(source.createManualReport(
            "red_ground", "obstacle", QStringLiteral("report-%1").arg(index),
            position(index, index), {}, atSeconds(0).addSecs(index)).ok);
    }
    QJsonObject checkpoint = source.toJson();
    QJsonObject seats = checkpoint.value(QStringLiteral("seats")).toObject();
    QJsonObject seat = seats.value(QStringLiteral("red_ground")).toObject();
    QJsonArray oversized = seat.value(QStringLiteral("history")).toArray();
    for (int index = 0; index < 20; ++index) oversized.prepend(oversized.at(0));
    seat[QStringLiteral("history")] = oversized;
    seats[QStringLiteral("red_ground")] = seat;
    checkpoint[QStringLiteral("seats")] = seats;

    IntelLedger restored;
    QString error;
    ASSERT_TRUE(restored.restore(checkpoint, &error)) << error.toStdString();
    EXPECT_EQ(restored.history("red_ground").size(), IntelLedger::HistoryLimit);
}

TEST(IntelLedgerTest, SensorFreshnessDecaysAndArchives) {
    IntelLedger ledger;
    ledger.setConfig({1.0, 3.0});
    ASSERT_TRUE(ledger.observeSensor("red_recon", "blue_1", {}, position(1, 2),
                                    "red_recon_unit", atSeconds(0)).ok);

    EXPECT_EQ(ledger.advance(atSeconds(2)), 1);
    ASSERT_EQ(ledger.state("red_recon").records.size(), 1);
    EXPECT_EQ(ledger.state("red_recon").records.constFirst().freshness, QStringLiteral("stale"));
    EXPECT_FALSE(ledger.state("red_recon").records.constFirst().actionable);
    EXPECT_LT(ledger.state("red_recon").records.constFirst().confidence, 100.0);

    EXPECT_EQ(ledger.advance(atSeconds(4)), 1);
    EXPECT_EQ(ledger.state("red_recon").records.constFirst().freshness,
              QStringLiteral("archived"));
    EXPECT_DOUBLE_EQ(ledger.state("red_recon").records.constFirst().confidence, 0.0);
}

TEST(IntelLedgerTest, ManualReportConfidenceNeverRisesWhenItBecomesStale) {
    IntelLedger ledger;
    ledger.setConfig({1.0, 3.0});
    ASSERT_TRUE(ledger.createManualReport("red_ground", "obstacle", "bridge",
                                         position(1, 2), {}, atSeconds(0)).ok);
    ASSERT_EQ(ledger.state("red_ground").records.size(), 1);
    EXPECT_DOUBLE_EQ(ledger.state("red_ground").records.constFirst().confidence, 50.0);

    EXPECT_EQ(ledger.advance(atSeconds(2)), 1);
    const auto& stale = ledger.state("red_ground").records.constFirst();
    EXPECT_EQ(stale.freshness, QStringLiteral("stale"));
    EXPECT_DOUBLE_EQ(stale.confidence, 25.0);
    EXPECT_FALSE(stale.actionable);
}

TEST(IntelLedgerTest, ShareRequiresSameSideAndPersistsPropagation) {
    IntelLedger ledger;
    ASSERT_TRUE(ledger.observeSensor("red_recon", "blue_1", {}, position(5, 6),
                                     "red_recon_unit", atSeconds(0)).ok);
    const auto denied = ledger.share("red_recon", "blue_recon", "red", "blue", true,
                                     "sensor_red_recon_blue_1", "", atSeconds(1));
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.code, QStringLiteral("PERMISSION_DENIED"));

    const auto shared = ledger.share("red_recon", "red_attack", "red", "red", true,
                                     "sensor_red_recon_blue_1", "handoff", atSeconds(1));
    ASSERT_TRUE(shared.ok);
    ASSERT_EQ(ledger.state("red_attack").records.size(), 1);
    EXPECT_EQ(ledger.state("red_attack").records.constFirst().targetId, QStringLiteral("blue_1"));
    EXPECT_EQ(ledger.state("red_attack").records.constFirst().note, QStringLiteral("handoff"));
    EXPECT_EQ(ledger.state("red_attack").records.constFirst().propagationSources.size(), 1);
}

TEST(IntelLedgerTest, RepeatedSharingKeepsProtocolValidBoundedPropagation) {
    IntelLedger ledger;
    ASSERT_TRUE(ledger.observeSensor("red_recon", "blue_1", {}, position(5, 6),
                                     "red_recon_unit", atSeconds(0)).ok);
    for (int index = 1; index <= Protocol::MaxIntelPropagationSources + 10; ++index) {
        ASSERT_TRUE(ledger.share(
            "red_recon", "red_attack", "red", "red", true,
            "sensor_red_recon_blue_1", QStringLiteral("handoff-%1").arg(index),
            atSeconds(0).addSecs(index)).ok);
    }

    ASSERT_EQ(ledger.state("red_attack").records.size(), 1);
    EXPECT_EQ(ledger.state("red_attack").records.constFirst()
                  .propagationSources.size(),
              Protocol::MaxIntelPropagationSources);
    EXPECT_TRUE(Protocol::validateIntelState(
        ledger.state("red_attack").toJson()).valid);
}

TEST(IntelLedgerTest, ManualReportCanBeSharedWithoutBindingATarget) {
    IntelLedger ledger;
    ASSERT_TRUE(ledger.createManualReport("red_ground", "obstacle", "bridge",
                                         position(5, 6), "blocked", atSeconds(0)).ok);
    const QString intelId = ledger.state("red_ground").records.constFirst().intelId;

    const auto shared = ledger.share("red_ground", "red_commander", "red", "red", true,
                                     intelId, "handoff", atSeconds(1));

    ASSERT_TRUE(shared.ok);
    ASSERT_EQ(ledger.state("red_commander").records.size(), 1);
    const auto& received = ledger.state("red_commander").records.constFirst();
    EXPECT_EQ(received.type, QStringLiteral("manualReport"));
    EXPECT_TRUE(received.targetId.isEmpty());
    EXPECT_FALSE(received.actionable);
}

TEST(IntelLedgerTest, HistoryTimeFilterAcceptsIsoTimestampsWithoutMilliseconds) {
    IntelLedger ledger;
    ASSERT_TRUE(ledger.createManualReport("red_ground", "obstacle", "first",
                                         position(1, 1), {}, atSeconds(0)).ok);
    ASSERT_TRUE(ledger.createManualReport("red_ground", "obstacle", "second",
                                         position(2, 2), {}, atSeconds(2)).ok);
    Protocol::IntelHistoryQuery query;
    query.pageSize = 20;
    query.from = QStringLiteral("2026-08-10T00:00:01Z");
    query.to = QStringLiteral("2026-08-10T00:00:03Z");

    const Protocol::IntelHistoryPage page = ledger.historyPage("red_ground", query);

    ASSERT_EQ(page.entries.size(), 1);
    EXPECT_EQ(page.entries.constFirst().knownAttributes.value(QStringLiteral("title")),
              QStringLiteral("second"));
}

TEST(IntelLedgerTest, SharingMergesSameTargetIntoOneRecipientContact) {
    IntelLedger ledger;
    ASSERT_TRUE(ledger.observeSensor("red_attack", "blue_1", {}, position(20, 21),
                                     "red_attack_unit", atSeconds(0)).ok);
    ASSERT_TRUE(ledger.observeSensor("red_recon", "blue_1", {}, position(25, 26),
                                     "red_recon_unit", atSeconds(1)).ok);

    const auto shared = ledger.share("red_recon", "red_attack", "red", "red", true,
                                     "sensor_red_recon_blue_1", "handoff", atSeconds(2));
    ASSERT_TRUE(shared.ok);
    ASSERT_EQ(ledger.state("red_attack").records.size(), 1);
    const auto& contact = ledger.state("red_attack").records.constFirst();
    EXPECT_EQ(contact.intelId, QStringLiteral("sensor_red_attack_blue_1"));
    EXPECT_EQ(contact.targetId, QStringLiteral("blue_1"));
    EXPECT_EQ(contact.propagationSources.size(), 1);
    EXPECT_EQ(ledger.historyPage("red_attack", Protocol::IntelHistoryQuery{
        QString(), 20, QStringLiteral("blue_1")}).entries.constLast().intelId,
              QStringLiteral("sensor_red_attack_blue_1"));
}

TEST(IntelLedgerTest, ArchivedIntelCannotBeShared) {
    IntelLedger ledger;
    ledger.setConfig({1.0, 2.0});
    ASSERT_TRUE(ledger.observeSensor("red_recon", "blue_1", {}, position(1, 2),
                                     "red_recon_unit", atSeconds(0)).ok);
    ASSERT_EQ(ledger.advance(atSeconds(3)), 1);
    const auto result = ledger.share("red_recon", "red_attack", "red", "red", true,
                                     "sensor_red_recon_blue_1", {}, atSeconds(3));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, QStringLiteral("INTEL_ARCHIVED"));
}

TEST(IntelLedgerTest, CheckpointRoundTripKeepsSeatHistory) {
    IntelLedger source;
    source.setConfig({2.0, 8.0});
    ASSERT_TRUE(source.createManualReport("red_ground", "obstacle", "bridge", position(9, 10),
                                         "blocked", atSeconds(0)).ok);
    const QJsonObject checkpoint = source.toJson();

    IntelLedger restored;
    QString error;
    ASSERT_TRUE(restored.restore(checkpoint, &error)) << error.toStdString();
    EXPECT_EQ(restored.config().staleAfterSec, 2.0);
    EXPECT_EQ(restored.config().archiveAfterSec, 8.0);
    EXPECT_EQ(restored.state("red_ground").toJson(), source.state("red_ground").toJson());
    EXPECT_EQ(restored.history("red_ground"), source.history("red_ground"));
}

TEST(IntelLedgerTest, HistoryCursorRemainsStableAfterOlderEntriesAreTrimmed) {
    IntelLedger source;
    ASSERT_TRUE(source.createManualReport("red_ground", "obstacle", "first",
                                         position(1, 1), {}, atSeconds(0)).ok);
    ASSERT_TRUE(source.createManualReport("red_ground", "obstacle", "second",
                                         position(2, 2), {}, atSeconds(1)).ok);
    ASSERT_TRUE(source.createManualReport("red_ground", "obstacle", "third",
                                         position(3, 3), {}, atSeconds(2)).ok);

    Protocol::IntelHistoryQuery firstQuery;
    firstQuery.pageSize = 1;
    const Protocol::IntelHistoryPage first = source.historyPage("red_ground", firstQuery);
    ASSERT_EQ(first.entries.size(), 1);
    EXPECT_EQ(first.entries.constFirst().historyId, QStringLiteral("ih_1"));
    EXPECT_TRUE(first.hasMore);
    EXPECT_EQ(first.nextCursor, QStringLiteral("1"));

    QJsonObject checkpoint = source.toJson();
    QJsonObject seats = checkpoint.value(QStringLiteral("seats")).toObject();
    QJsonObject seat = seats.value(QStringLiteral("red_ground")).toObject();
    QJsonArray history = seat.value(QStringLiteral("history")).toArray();
    history.removeAt(0);
    seat[QStringLiteral("history")] = history;
    seats[QStringLiteral("red_ground")] = seat;
    checkpoint[QStringLiteral("seats")] = seats;

    IntelLedger trimmed;
    QString error;
    ASSERT_TRUE(trimmed.restore(checkpoint, &error)) << error.toStdString();
    Protocol::IntelHistoryQuery nextQuery;
    nextQuery.cursor = first.nextCursor;
    nextQuery.pageSize = 2;
    const Protocol::IntelHistoryPage next = trimmed.historyPage("red_ground", nextQuery);
    ASSERT_EQ(next.entries.size(), 2);
    EXPECT_EQ(next.entries.at(0).historyId, QStringLiteral("ih_2"));
    EXPECT_EQ(next.entries.at(1).historyId, QStringLiteral("ih_3"));
    EXPECT_FALSE(next.hasMore);
    EXPECT_TRUE(next.nextCursor.isEmpty());
}
