#include <gtest/gtest.h>

#include "protocol/Protocol.h"
#include "protocol/dds/WargameEnvelope.h"
#include "protocol/StateDelta.h"

#include <QJsonArray>

using namespace gbr;

TEST(DdsEnvelopeTest, RoundTripsValidatedPayloadAndMetadata) {
    Protocol::Dds::WargameEnvelope source;
    source.protocolVersion = Protocol::Version;
    source.schemaVersion = Protocol::SchemaVersion;
    source.messageType = QStringLiteral("ChatMessage");
    source.messageId = QStringLiteral("dds-msg-1");
    source.sequence = 1;
    source.stateRevision = 12;
    source.scenarioRevision = 4;
    source.serverTick = 99;
    source.sentAt = QStringLiteral("2026-08-03T00:00:00.000Z");
    source.payload = QByteArrayLiteral("{\"text\":\"hello\"}");

    QString error;
    const QByteArray encoded = Protocol::Dds::encode(source, &error);
    ASSERT_FALSE(encoded.isEmpty()) << error.toStdString();
    const auto decoded = Protocol::Dds::decode(encoded);
    ASSERT_TRUE(decoded.valid) << decoded.message.toStdString();
    EXPECT_EQ(decoded.envelope.messageType, source.messageType);
    EXPECT_EQ(decoded.envelope.sequence, source.sequence);
    EXPECT_EQ(decoded.envelope.stateRevision, source.stateRevision);
    EXPECT_EQ(decoded.envelope.payload, source.payload);
}

TEST(DdsEnvelopeChunkTest, ReassemblesOutOfOrderAndRejectsOversizedInput) {
    const QByteArray source(200000, 'x');
    QString error;
    const auto chunks = Protocol::Dds::splitPayload(source, QStringLiteral("snapshot-1"), 4096,
                                                    &error);
    ASSERT_TRUE(error.isEmpty());
    ASSERT_GT(chunks.size(), 1);
    Protocol::Dds::ChunkReassembler reassembler;
    Protocol::Dds::ChunkResult result;
    for (int index = chunks.size() - 1; index >= 0; --index) {
        result = reassembler.add(chunks.at(index), 1000 + index);
    }
    ASSERT_TRUE(result.complete);
    EXPECT_EQ(result.payload, source);

    Protocol::Dds::EnvelopeChunk invalid = chunks.constFirst();
    invalid.count = Protocol::Dds::MaxEnvelopeChunks + 1;
    EXPECT_FALSE(reassembler.add(invalid, 2000).code == QLatin1String("ACCEPTED"));
}

TEST(DdsEnvelopeChunkTest, ExpiresIncompleteTransfers) {
    const auto chunks = Protocol::Dds::splitPayload(QByteArray(100, 'z'), QStringLiteral("late"),
                                                    50);
    ASSERT_EQ(chunks.size(), 2);
    Protocol::Dds::ChunkReassembler reassembler;
    EXPECT_FALSE(reassembler.add(chunks.first(), 100).complete);
    reassembler.expire(100 + Protocol::Dds::MaxReassemblyAgeMs + 1);
    EXPECT_EQ(reassembler.pendingBytes(), 0);
    EXPECT_FALSE(reassembler.add(chunks.last(), 6000).complete);
}

TEST(DdsEnvelopeTest, RejectsZeroSequenceAndOversizedPayload) {
    Protocol::Dds::WargameEnvelope source;
    source.protocolVersion = Protocol::Version;
    source.schemaVersion = Protocol::SchemaVersion;
    source.messageType = QStringLiteral("Heartbeat");
    source.messageId = QStringLiteral("dds-msg-2");
    source.sentAt = QStringLiteral("2026-08-03T00:00:00.000Z");
    source.payload = QByteArrayLiteral("{}");
    EXPECT_TRUE(Protocol::Dds::encode(source).isEmpty());

    source.sequence = 1;
    source.payload = QByteArray(Protocol::Dds::MaxEnvelopePayloadBytes + 1, 'x');
    EXPECT_TRUE(Protocol::Dds::encode(source).isEmpty());
}

TEST(DdsEnvelopeTest, RejectsInvalidTimestampAndNonCanonicalBase64) {
    const QJsonObject base{
        {QStringLiteral("protocolVersion"), Protocol::Version},
        {QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
        {QStringLiteral("messageType"), QStringLiteral("Heartbeat")},
        {QStringLiteral("messageId"), QStringLiteral("dds-msg-3")},
        {QStringLiteral("sequence"), 1},
        {QStringLiteral("stateRevision"), 0},
        {QStringLiteral("scenarioRevision"), 0},
        {QStringLiteral("serverTick"), 0},
        {QStringLiteral("sentAt"), QStringLiteral("2026-08-03T00:00:00.000Z")},
        {QStringLiteral("payload"), QStringLiteral("e30=")}};
    QJsonObject invalidTimestamp = base;
    invalidTimestamp[QStringLiteral("sentAt")] = QStringLiteral("not-a-time");
    EXPECT_FALSE(Protocol::Dds::fromJson(invalidTimestamp).valid);

    QJsonObject invalidBase64 = base;
    invalidBase64[QStringLiteral("payload")] = QStringLiteral("e30!");
    EXPECT_FALSE(Protocol::Dds::fromJson(invalidBase64).valid);
}

TEST(ProtocolTest, ClientEnvelopeRequiresBothVersionsAndMessageId) {
    const QJsonObject valid = Protocol::makeClientEnvelope(
        QStringLiteral("ping"), QStringLiteral("message-1"), QJsonObject{});
    EXPECT_TRUE(Protocol::validateClientEnvelope(valid).valid);

    QJsonObject missingSchema = valid;
    missingSchema.remove(QStringLiteral("schemaVersion"));
    EXPECT_EQ(Protocol::validateClientEnvelope(missingSchema).code,
              QStringLiteral("SCHEMA_MISMATCH"));

    QJsonObject missingMessageId = valid;
    missingMessageId.remove(QStringLiteral("messageId"));
    EXPECT_EQ(Protocol::validateClientEnvelope(missingMessageId).code,
              QStringLiteral("INVALID_ENVELOPE"));
}

TEST(ProtocolTest, RejectsFractionalVersionsAndRevisions) {
    QJsonObject client = Protocol::makeClientEnvelope(
        QStringLiteral("ping"), QStringLiteral("fractional-client"), QJsonObject{});
    client[QStringLiteral("protocolVersion")] = 3.5;
    EXPECT_EQ(Protocol::validateClientEnvelope(client).code,
              QStringLiteral("PROTOCOL_MISMATCH"));

    client = Protocol::makeClientEnvelope(
        QStringLiteral("ping"), QStringLiteral("fractional-schema"), QJsonObject{});
    client[QStringLiteral("schemaVersion")] = 2.5;
    EXPECT_EQ(Protocol::validateClientEnvelope(client).code,
              QStringLiteral("SCHEMA_MISMATCH"));

    QJsonObject snapshot = Protocol::makeServerEnvelope(
        QStringLiteral("snapshot"), 1,
        QJsonObject{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                    {QStringLiteral("stateRevision"), 1},
                    {QStringLiteral("scenario"), QJsonObject{}},
                    {QStringLiteral("units"), QJsonArray{}},
                    {QStringLiteral("messages"), QJsonArray{}},
                    {QStringLiteral("roomState"), QJsonObject{}}});
    snapshot[QStringLiteral("schemaVersion")] = 2.5;
    EXPECT_EQ(Protocol::validateServerEnvelope(snapshot).code,
              QStringLiteral("SCHEMA_MISMATCH"));

    snapshot[QStringLiteral("schemaVersion")] = Protocol::SchemaVersion;
    QJsonObject snapshotPayload = snapshot.value(QStringLiteral("payload")).toObject();
    snapshotPayload[QStringLiteral("stateRevision")] = 1.5;
    snapshot[QStringLiteral("payload")] = snapshotPayload;
    EXPECT_EQ(Protocol::validateServerEnvelope(snapshot).code,
              QStringLiteral("INVALID_PAYLOAD"));

    const QJsonObject delta = Protocol::makeServerEnvelope(
        QStringLiteral("delta"), 2,
        QJsonObject{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                    {QStringLiteral("baseStateRevision"), 1.5},
                    {QStringLiteral("stateRevision"), 2},
                    {QStringLiteral("scenarioRevision"), 1},
                    {QStringLiteral("units"), QJsonArray{}},
                    {QStringLiteral("roomState"), QJsonObject{}}});
    EXPECT_EQ(Protocol::validateServerEnvelope(delta).code,
              QStringLiteral("INVALID_PAYLOAD"));
}

TEST(ProtocolTest, ServerEnvelopeRejectsUnknownTypeAndInvalidSequence) {
    QJsonObject envelope = Protocol::makeServerEnvelope(
        QStringLiteral("pong"), 1, QJsonObject{});
    EXPECT_TRUE(Protocol::validateServerEnvelope(envelope).valid);

    envelope[QStringLiteral("type")] = QStringLiteral("not-supported");
    EXPECT_EQ(Protocol::validateServerEnvelope(envelope).code,
              QStringLiteral("UNKNOWN_MESSAGE"));

    envelope = Protocol::makeServerEnvelope(QStringLiteral("pong"), 0, QJsonObject{});
    EXPECT_EQ(Protocol::validateServerEnvelope(envelope).code,
              QStringLiteral("INVALID_ENVELOPE"));

    envelope = Protocol::makeServerEnvelope(QStringLiteral("pong"), 1, QJsonObject{});
    envelope[QStringLiteral("sequence")] = 9007199254740992.0;
    EXPECT_EQ(Protocol::validateServerEnvelope(envelope).code,
              QStringLiteral("INVALID_ENVELOPE"));

    QJsonObject resume = Protocol::makeClientEnvelope(
        QStringLiteral("auth"), QStringLiteral("unsafe-resume"),
        QJsonObject{{QStringLiteral("token"), QString(32, QLatin1Char('t'))},
                    {QStringLiteral("resumeSequence"), 9007199254740992.0}});
    EXPECT_EQ(Protocol::validateClientEnvelope(resume).code,
              QStringLiteral("INVALID_PAYLOAD"));
}

TEST(ProtocolTest, ServerSequenceStartsAtOne) {
    QJsonObject zero = Protocol::makeServerEnvelope(
        QStringLiteral("pong"), 0, QJsonObject{});
    EXPECT_FALSE(Protocol::validateServerEnvelope(zero).valid);
    EXPECT_TRUE(Protocol::validateServerEnvelope(
                    Protocol::makeServerEnvelope(QStringLiteral("pong"), 1, QJsonObject{}))
                    .valid);
}

TEST(ProtocolTest, RejectsMalformedPayloadsBeforeDispatch) {
    QJsonObject command = Protocol::makeClientEnvelope(
        QStringLiteral("command"), QStringLiteral("message-1"),
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("command-1")},
                    {QStringLiteral("action"), QStringLiteral("moveTo")},
                    {QStringLiteral("args"), QStringLiteral("not-an-object")}});
    EXPECT_EQ(Protocol::validateClientEnvelope(command).code,
              QStringLiteral("INVALID_PAYLOAD"));

    QJsonObject chat = Protocol::makeClientEnvelope(
        QStringLiteral("chat"), QStringLiteral("message-2"),
        QJsonObject{{QStringLiteral("text"), QString(501, QLatin1Char('x'))}});
    EXPECT_EQ(Protocol::validateClientEnvelope(chat).code,
              QStringLiteral("INVALID_PAYLOAD"));

    QJsonObject result = Protocol::makeServerEnvelope(
        QStringLiteral("commandResult"), 1,
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("command-1")},
                    {QStringLiteral("accepted"), QStringLiteral("yes")},
                    {QStringLiteral("code"), QStringLiteral("OK")},
                    {QStringLiteral("message"), QStringLiteral("done")}});
    EXPECT_EQ(Protocol::validateServerEnvelope(result).code,
              QStringLiteral("INVALID_PAYLOAD"));
}

TEST(ProtocolTest, RejectsMalformedTypedCommandAndTransferContracts) {
    const auto missingRecipient = Protocol::makeClientEnvelope(
        QStringLiteral("chat"), QStringLiteral("message-text"),
        QJsonObject{{QStringLiteral("text"), QStringLiteral("status")},
                    {QStringLiteral("recipientSeatIds"), QJsonArray{}}});
    EXPECT_FALSE(Protocol::validateClientEnvelope(missingRecipient).valid);

    const auto emptyText = Protocol::makeClientEnvelope(
        QStringLiteral("chat"), QStringLiteral("message-empty-text"),
        QJsonObject{{QStringLiteral("text"), QStringLiteral("   ")},
                    {QStringLiteral("recipientSeatIds"),
                     QJsonArray{QStringLiteral("red_commander")}}});
    EXPECT_FALSE(Protocol::validateClientEnvelope(emptyText).valid);

    const auto malformedAttack = Protocol::makeClientEnvelope(
        QStringLiteral("command"), QStringLiteral("message-attack"),
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("command-attack")},
                    {QStringLiteral("action"), QStringLiteral("unsupportedAttack")},
                    {QStringLiteral("args"), QJsonObject{}}});
    EXPECT_FALSE(Protocol::validateClientEnvelope(malformedAttack).valid);

    const auto emptyManeuver = Protocol::makeClientEnvelope(
        QStringLiteral("command"), QStringLiteral("message-maneuver"),
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("command-maneuver")},
                    {QStringLiteral("action"), QStringLiteral("setFlightPlan")},
                    {QStringLiteral("args"),
                     QJsonObject{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                                 {QStringLiteral("waypoints"), QJsonArray{}}}}});
    EXPECT_FALSE(Protocol::validateClientEnvelope(emptyManeuver).valid);

    const auto malformedPoint = Protocol::makeClientEnvelope(
        QStringLiteral("command"), QStringLiteral("message-point"),
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("command-point")},
                    {QStringLiteral("action"), QStringLiteral("moveTo")},
                    {QStringLiteral("stateRevision"), 8},
                    {QStringLiteral("args"),
                     QJsonObject{{QStringLiteral("unitId"), QStringLiteral("red_r1")},
                                 {QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), 1.0}}}}}});
    EXPECT_FALSE(Protocol::validateClientEnvelope(malformedPoint).valid);

    const auto malformedTransfer = Protocol::makeClientEnvelope(
        QStringLiteral("claimSeat"), QStringLiteral("message-transfer"),
        QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_commander")},
                    {QStringLiteral("approveUserId"), 7}});
    EXPECT_FALSE(Protocol::validateClientEnvelope(malformedTransfer).valid);

    const auto malformedTransferEvent = Protocol::makeServerEnvelope(
        QStringLiteral("event"), 3,
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("transferCompleted")},
                    {QStringLiteral("userId"), 7},
                    {QStringLiteral("sourceSeatId"), QStringLiteral("red_attack_1")},
                    {QStringLiteral("targetSeatId"), QStringLiteral("red_recon_1")},
                    {QStringLiteral("templateId"), QStringLiteral("reconuav")}});
    EXPECT_FALSE(Protocol::validateServerEnvelope(malformedTransferEvent).valid);

    const auto pointFreeWithdraw = Protocol::makeClientEnvelope(
        QStringLiteral("command"), QStringLiteral("message-withdraw"),
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("command-withdraw")},
                    {QStringLiteral("action"), QStringLiteral("withdraw")},
                    {QStringLiteral("stateRevision"), 9},
                    {QStringLiteral("args"),
                     QJsonObject{{QStringLiteral("unitId"), QStringLiteral("red_r1")}}}});
    EXPECT_TRUE(Protocol::validateClientEnvelope(pointFreeWithdraw).valid);

    const QJsonObject point{{QStringLiteral("x"), 3200.0},
                            {QStringLiteral("y"), 4100.0}};
    for (const QString& action : {QStringLiteral("attackAt"), QStringLiteral("moveTo"),
                                  QStringLiteral("withdraw")}) {
        const auto pointOrder = Protocol::makeClientEnvelope(
            QStringLiteral("command"), QStringLiteral("message-") + action,
            QJsonObject{{QStringLiteral("commandId"), QStringLiteral("command-") + action},
                        {QStringLiteral("action"), action},
                        {QStringLiteral("stateRevision"), 9},
                        {QStringLiteral("args"),
                         QJsonObject{{QStringLiteral("unitId"), QStringLiteral("red_a1")},
                                     {QStringLiteral("pos"), point}}}});
        EXPECT_TRUE(Protocol::validateClientEnvelope(pointOrder).valid) << action.toStdString();
    }

    const auto textOrder = Protocol::makeClientEnvelope(
        QStringLiteral("command"), QStringLiteral("message-unit-order"),
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("command-unit-order")},
                    {QStringLiteral("action"), QStringLiteral("unitOrder")},
                    {QStringLiteral("stateRevision"), 9},
                    {QStringLiteral("args"),
                     QJsonObject{{QStringLiteral("unitId"), QStringLiteral("red_a1")},
                                 {QStringLiteral("text"), QStringLiteral("保持编队并报告状态")}}}});
    EXPECT_TRUE(Protocol::validateClientEnvelope(textOrder).valid);

    Protocol::TransferEventProjection transfer;
    const auto validTransferEvent = Protocol::makeServerEnvelope(
        QStringLiteral("event"), 4,
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("transferCompleted")},
                    {QStringLiteral("revision"), 12},
                    {QStringLiteral("requestRevision"), 11},
                    {QStringLiteral("userId"), 7},
                    {QStringLiteral("sourceSeatId"), QStringLiteral("red_attack_1")},
                    {QStringLiteral("targetSeatId"), QStringLiteral("red_recon_1")},
                    {QStringLiteral("templateId"), QStringLiteral("reconuav")}});
    EXPECT_TRUE(Protocol::validateServerEnvelope(validTransferEvent).valid);
    ASSERT_TRUE(Protocol::projectTransferEvent(
        validTransferEvent.value(QStringLiteral("payload")).toObject(), &transfer).valid);
    EXPECT_EQ(transfer.requestRevision, 11);
}

TEST(ProtocolTest, RejectsExcessivelyNestedJson) {
    QJsonValue nested = QJsonObject{};
    for (int i = 0; i < Protocol::MaxJsonDepth + 2; ++i) {
        nested = QJsonObject{{QStringLiteral("child"), nested}};
    }
    const QJsonObject envelope = Protocol::makeClientEnvelope(
        QStringLiteral("ping"), QStringLiteral("message-1"), nested.toObject());
    EXPECT_EQ(Protocol::validateClientEnvelope(envelope).code,
              QStringLiteral("MESSAGE_TOO_COMPLEX"));
}

TEST(ProtocolTest, ValidatesRoomSeatAndIntelMessages) {
    const auto join = Protocol::makeClientEnvelope(
        QStringLiteral("joinRoom"), QStringLiteral("room-1"),
        QJsonObject{{QStringLiteral("roomId"), QStringLiteral("main")} });
    EXPECT_TRUE(Protocol::validateClientEnvelope(join).valid);
    const auto share = Protocol::makeClientEnvelope(
        QStringLiteral("shareIntel"), QStringLiteral("share-1"),
        QJsonObject{{QStringLiteral("targetId"), QStringLiteral("blue_r1")},
                    {QStringLiteral("recipientSeatIds"), QJsonArray{QStringLiteral("red_commander")}}});
    EXPECT_TRUE(Protocol::validateClientEnvelope(share).valid);
    const auto badSeat = Protocol::makeClientEnvelope(
        QStringLiteral("claimSeat"), QStringLiteral("seat-1"),
        QJsonObject{{QStringLiteral("seatId"), QString(Protocol::MaxIdentifierLength + 1, QLatin1Char('x'))}});
    EXPECT_FALSE(Protocol::validateClientEnvelope(badSeat).valid);
}

TEST(ProtocolTest, MapMarkRejectsMalformedRecipientSeatIds) {
    const QJsonObject position{{QStringLiteral("x"), 100.0},
                               {QStringLiteral("y"), 200.0}};
    const auto notAnArray = Protocol::makeClientEnvelope(
        QStringLiteral("mapMark"), QStringLiteral("mark-invalid-list"),
        QJsonObject{{QStringLiteral("position"), position},
                    {QStringLiteral("label"), QStringLiteral("观察点")},
                    {QStringLiteral("recipientSeatIds"), QStringLiteral("red_attack_1")}});
    EXPECT_FALSE(Protocol::validateClientEnvelope(notAnArray).valid);

    const auto invalidSeat = Protocol::makeClientEnvelope(
        QStringLiteral("mapMark"), QStringLiteral("mark-invalid-seat"),
        QJsonObject{{QStringLiteral("position"), position},
                    {QStringLiteral("label"), QStringLiteral("观察点")},
                    {QStringLiteral("recipientSeatIds"), QJsonArray{QStringLiteral("bad seat")}}});
    EXPECT_FALSE(Protocol::validateClientEnvelope(invalidSeat).valid);

    const auto oversizedLabel = Protocol::makeClientEnvelope(
        QStringLiteral("mapMark"), QStringLiteral("mark-invalid-label"),
        QJsonObject{{QStringLiteral("position"), position},
                    {QStringLiteral("label"), QString(Protocol::MaxMapLabelLength + 1,
                                                       QLatin1Char('x'))}});
    EXPECT_FALSE(Protocol::validateClientEnvelope(oversizedLabel).valid);
}

TEST(ProtocolTest, RedeployRequiresOneTargetSeat) {
    const auto targeted = Protocol::makeClientEnvelope(
        QStringLiteral("redeploy"), QStringLiteral("redeploy-targeted"),
        QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_attack_1")}});
    EXPECT_TRUE(Protocol::validateClientEnvelope(targeted).valid);

    const auto missingTarget = Protocol::makeClientEnvelope(
        QStringLiteral("redeploy"), QStringLiteral("redeploy-missing"), QJsonObject{});
    EXPECT_FALSE(Protocol::validateClientEnvelope(missingTarget).valid);

    const auto malformedTarget = Protocol::makeClientEnvelope(
        QStringLiteral("redeploy"), QStringLiteral("redeploy-malformed"),
        QJsonObject{{QStringLiteral("seatId"), QString(Protocol::MaxIdentifierLength + 1,
                                                        QLatin1Char('x'))}});
    EXPECT_FALSE(Protocol::validateClientEnvelope(malformedTarget).valid);
}

TEST(ProtocolTest, LeaveAndReleaseSeatPayloadsAreStrict) {
    const auto leave = [](const QString& messageId, const QJsonObject& payload) {
        return Protocol::validateClientEnvelope(Protocol::makeClientEnvelope(
            QStringLiteral("leaveRoom"), messageId, payload));
    };
    const auto release = [](const QString& messageId, const QJsonObject& payload) {
        return Protocol::validateClientEnvelope(Protocol::makeClientEnvelope(
            QStringLiteral("releaseSeat"), messageId, payload));
    };

    EXPECT_TRUE(leave(QStringLiteral("leave-empty"), {}).valid);
    EXPECT_TRUE(leave(QStringLiteral("leave-transfer"),
                      QJsonObject{{QStringLiteral("successorUserId"), 42}}).valid);
    EXPECT_FALSE(leave(QStringLiteral("leave-wrong-type"),
                       QJsonObject{{QStringLiteral("successorUserId"),
                                    QStringLiteral("42")}}).valid);
    EXPECT_FALSE(leave(QStringLiteral("leave-undeclared"),
                       QJsonObject{{QStringLiteral("successor"), 42}}).valid);
    EXPECT_FALSE(leave(QStringLiteral("leave-extra"),
                       QJsonObject{{QStringLiteral("successorUserId"), 42},
                                   {QStringLiteral("force"), true}}).valid);

    EXPECT_TRUE(release(QStringLiteral("release-empty"), {}).valid);
    EXPECT_FALSE(release(QStringLiteral("release-successor"),
                         QJsonObject{{QStringLiteral("successorUserId"), 42}}).valid);
    EXPECT_FALSE(release(QStringLiteral("release-undeclared"),
                         QJsonObject{{QStringLiteral("successor"), 42}}).valid);
}

TEST(ProtocolTest, WelcomeMayOmitUnclaimedSeat) {
    const auto welcome = Protocol::makeServerEnvelope(
        QStringLiteral("welcome"), 1,
        QJsonObject{{QStringLiteral("username"), QStringLiteral("operator")},
                    {QStringLiteral("displayName"), QStringLiteral("操作员")},
                    {QStringLiteral("room"), QStringLiteral("main")} });
    EXPECT_TRUE(Protocol::validateServerEnvelope(welcome).valid);
}

TEST(ProtocolTest, RejectsMalformedLifecycleSeatDeploymentIntelAndCommandProjections) {
    const QJsonObject malformedSnapshot = Protocol::makeServerEnvelope(
        QStringLiteral("snapshot"), 1,
        QJsonObject{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                    {QStringLiteral("stateRevision"), 1},
                    {QStringLiteral("scenario"), QJsonObject{}},
                    {QStringLiteral("units"), QJsonArray{}},
                    {QStringLiteral("messages"), QJsonArray{}},
                    {QStringLiteral("roomState"),
                     QJsonObject{{QStringLiteral("phase"), 7}}}});
    EXPECT_FALSE(Protocol::validateServerEnvelope(malformedSnapshot).valid);

    const QJsonObject malformedSeats = Protocol::makeServerEnvelope(
        QStringLiteral("seatState"), 2,
        QJsonObject{{QStringLiteral("roomId"), QStringLiteral("main")},
                    {QStringLiteral("yourSeatId"), QStringLiteral("red_commander")},
                    {QStringLiteral("seats"),
                     QJsonArray{QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_commander")},
                                            {QStringLiteral("occupied"), QStringLiteral("yes")}}}}});
    EXPECT_FALSE(Protocol::validateServerEnvelope(malformedSeats).valid);

    const QJsonObject malformedDeployment = Protocol::makeServerEnvelope(
        QStringLiteral("deploymentPrompt"), 3,
        QJsonObject{{QStringLiteral("unitId"), QStringLiteral("red_r1")},
                    {QStringLiteral("seatId"), 3},
                    {QStringLiteral("message"), QStringLiteral("deploy")}});
    EXPECT_FALSE(Protocol::validateServerEnvelope(malformedDeployment).valid);

    const QJsonObject malformedIntel = Protocol::makeServerEnvelope(
        QStringLiteral("intelShare"), 4,
        QJsonObject{{QStringLiteral("senderSeatId"), QStringLiteral("red_recon_1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")},
                    {QStringLiteral("note"), 5}});
    EXPECT_FALSE(Protocol::validateServerEnvelope(malformedIntel).valid);

    const QJsonObject malformedCommand = Protocol::makeServerEnvelope(
        QStringLiteral("commandResult"), 5,
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("command-1")},
                    {QStringLiteral("accepted"), true},
                    {QStringLiteral("code"), QStringLiteral("OK")},
                    {QStringLiteral("message"), QStringLiteral("done")},
                    {QStringLiteral("serverTime"), QStringLiteral("later")}});
    EXPECT_FALSE(Protocol::validateServerEnvelope(malformedCommand).valid);
}

TEST(ProtocolTest, RejectsUnknownLifecyclePhase) {
    const QJsonObject snapshot = Protocol::makeServerEnvelope(
        QStringLiteral("snapshot"), 1,
        QJsonObject{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                    {QStringLiteral("stateRevision"), 1},
                    {QStringLiteral("scenario"), QJsonObject{{QStringLiteral("units"), QJsonArray{}}}},
                    {QStringLiteral("units"), QJsonArray{}},
                    {QStringLiteral("messages"), QJsonArray{}},
                    {QStringLiteral("roomState"),
                     QJsonObject{{QStringLiteral("phase"), QStringLiteral("bogus")}}}});
    EXPECT_FALSE(Protocol::validateServerEnvelope(snapshot).valid);
}

TEST(ProtocolTest, ProjectsLifecycleSeatDeploymentIntelAndCommandPayloads) {
    const QJsonObject seat{{QStringLiteral("seatId"), QStringLiteral("red_recon_1")},
                           {QStringLiteral("seatType"), QStringLiteral("recon")},
                           {QStringLiteral("side"), QStringLiteral("red")},
                           {QStringLiteral("slot"), 1},
                           {QStringLiteral("capacity"), 2},
                           {QStringLiteral("occupied"), true},
                           {QStringLiteral("displayName"), QStringLiteral("Scout")},
                           {QStringLiteral("ready"), true},
                           {QStringLiteral("connected"), true},
                           {QStringLiteral("deployed"), true},
                           {QStringLiteral("pendingTransfer"), false},
                           {QStringLiteral("unitId"), QStringLiteral("red-r1")},
                           {QStringLiteral("selectedTemplate"), QStringLiteral("red-recon")}};
    const QJsonObject roomState{{QStringLiteral("phase"), QStringLiteral("preparing")},
                                {QStringLiteral("roomId"), QStringLiteral("main")},
                                {QStringLiteral("running"), false},
                                {QStringLiteral("simTime"), 12.5},
                                {QStringLiteral("speed"), 2.0},
                                {QStringLiteral("scenarioRevision"), 4},
                                {QStringLiteral("seats"), QJsonArray{seat}}};
    Protocol::SnapshotProjection snapshot;
    ASSERT_TRUE(Protocol::projectSnapshot(
        QJsonObject{{QStringLiteral("roomState"), roomState}}, &snapshot).valid);
    EXPECT_EQ(snapshot.lifecycle.roomId, QStringLiteral("main"));
    ASSERT_EQ(snapshot.lifecycle.seats.size(), 1);
    EXPECT_EQ(snapshot.lifecycle.seats.front().seatId, QStringLiteral("red_recon_1"));
    EXPECT_TRUE(snapshot.lifecycle.seats.front().connected);
    EXPECT_TRUE(snapshot.lifecycle.seats.front().deployed);
    EXPECT_EQ(snapshot.lifecycle.seats.front().unitId, QStringLiteral("red-r1"));

    Protocol::SeatDirectoryProjection directory;
    ASSERT_TRUE(Protocol::projectSeatDirectory(
        QJsonObject{{QStringLiteral("roomId"), QStringLiteral("main")},
                    {QStringLiteral("yourSeatId"), QStringLiteral("red_recon_1")},
                    {QStringLiteral("seats"), QJsonArray{seat}}}, &directory).valid);
    EXPECT_TRUE(directory.seats.front().ready);
    const QVariantMap projectedSeat = Protocol::seatVariants(directory.seats).front().toMap();
    EXPECT_TRUE(projectedSeat.value(QStringLiteral("deployed")).toBool());
    EXPECT_EQ(projectedSeat.value(QStringLiteral("selectedTemplate")).toString(),
              QStringLiteral("red-recon"));

    Protocol::DeploymentPromptProjection deployment;
    ASSERT_TRUE(Protocol::projectDeploymentPrompt(
        QJsonObject{{QStringLiteral("unitId"), QStringLiteral("red_r1")},
                    {QStringLiteral("seatId"), QStringLiteral("red_recon_1")},
                    {QStringLiteral("message"), QStringLiteral("deploy")}}, &deployment).valid);
    EXPECT_EQ(deployment.seatId, QStringLiteral("red_recon_1"));

    Protocol::IntelShareProjection intel;
    ASSERT_TRUE(Protocol::projectIntelShare(
        QJsonObject{{QStringLiteral("senderSeatId"), QStringLiteral("red_recon_1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")},
                    {QStringLiteral("sharedAt"), QStringLiteral("2026-07-23T12:00:00Z")},
                    {QStringLiteral("note"), QStringLiteral("contact")}}, &intel).valid);
    EXPECT_EQ(intel.note, QStringLiteral("contact"));

    Protocol::CommandResultProjection command;
    ASSERT_TRUE(Protocol::projectCommandResult(
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("command-1")},
                    {QStringLiteral("accepted"), true},
                    {QStringLiteral("code"), QStringLiteral("OK")},
                    {QStringLiteral("message"), QStringLiteral("done")},
                    {QStringLiteral("serverTime"), 13.0}}, &command).valid);
    EXPECT_TRUE(command.accepted);
    EXPECT_DOUBLE_EQ(command.serverTime, 13.0);
}

TEST(StateDeltaTest, AppliesChangedUnitsAndRoomState) {
    const QJsonObject scenario{{QStringLiteral("schemaVersion"), 1},
                               {QStringLiteral("units"), QJsonArray{}}};
    QJsonObject base{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                     {QStringLiteral("stateRevision"), 10},
                     {QStringLiteral("scenario"), scenario},
                     {QStringLiteral("units"),
                      QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("red_a1")},
                                             {QStringLiteral("hp"), 100}}}},
                     {QStringLiteral("messages"), QJsonArray{}},
                     {QStringLiteral("mapMarks"), QJsonArray{}},
                     {QStringLiteral("roomState"),
                      QJsonObject{{QStringLiteral("scenarioRevision"), 3},
                                  {QStringLiteral("simTime"), 1.0}}}};
    QJsonObject current = base;
    current[QStringLiteral("stateRevision")] = 11;
    current[QStringLiteral("units")] = QJsonArray{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("red_a1")},
                    {QStringLiteral("hp"), 75}}};
    current[QStringLiteral("roomState")] =
        QJsonObject{{QStringLiteral("scenarioRevision"), 3},
                    {QStringLiteral("simTime"), 1.1}};
    current[QStringLiteral("mapMarks")] = QJsonArray{
        QJsonObject{{QStringLiteral("side"), QStringLiteral("red")},
                    {QStringLiteral("label"), QStringLiteral("接触")}}};

    ASSERT_TRUE(StateDelta::canCreate(base, current));
    const QJsonObject delta = StateDelta::create(base, current);
    ASSERT_EQ(delta.value(QStringLiteral("units")).toArray().size(), 1);
    QString error;
    ASSERT_TRUE(StateDelta::apply(base, delta, &error)) << error.toStdString();
    EXPECT_EQ(base, current);
}

TEST(StateDeltaTest, RejectsWrongBaseRevision) {
    QJsonObject state{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                      {QStringLiteral("stateRevision"), 7},
                      {QStringLiteral("scenario"), QJsonObject{}},
                      {QStringLiteral("units"), QJsonArray{}},
                      {QStringLiteral("roomState"),
                       QJsonObject{{QStringLiteral("scenarioRevision"), 1}}}};
    const QJsonObject delta{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                            {QStringLiteral("baseStateRevision"), 6},
                            {QStringLiteral("stateRevision"), 8},
                            {QStringLiteral("scenarioRevision"), 1},
                            {QStringLiteral("units"), QJsonArray{}},
                            {QStringLiteral("roomState"),
                             QJsonObject{{QStringLiteral("scenarioRevision"), 1}}}};
    QString error;
    EXPECT_FALSE(StateDelta::apply(state, delta, &error));
    EXPECT_FALSE(error.isEmpty());
}
