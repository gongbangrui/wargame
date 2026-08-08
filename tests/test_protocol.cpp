#include <gtest/gtest.h>

#include "protocol/Protocol.h"
#include "protocol/dds/WargameEnvelope.h"
#include "protocol/StateDelta.h"

#include <QJsonArray>
#include <QStringList>

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
                    {QStringLiteral("projectiles"), QJsonArray{}},
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

TEST(ProtocolTest, AbilityAndServiceCommandsRequireAValidUnitId) {
    const QStringList actions{
        QStringLiteral("activateCountermeasure"), QStringLiteral("activateScan"),
        QStringLiteral("attemptFieldRepair"), QStringLiteral("cancelService")};
    for (const QString& action : actions) {
        const auto valid = Protocol::makeClientEnvelope(
            QStringLiteral("command"), QStringLiteral("message-") + action,
            QJsonObject{{QStringLiteral("commandId"), QStringLiteral("command-") + action},
                        {QStringLiteral("action"), action},
                        {QStringLiteral("stateRevision"), 9},
                        {QStringLiteral("args"),
                         QJsonObject{{QStringLiteral("unitId"),
                                      QStringLiteral("red_a1")}}}});
        EXPECT_TRUE(Protocol::validateClientEnvelope(valid).valid)
            << action.toStdString();

        QJsonObject invalidPayload = valid.value(QStringLiteral("payload")).toObject();
        invalidPayload[QStringLiteral("args")] = QJsonObject{
            {QStringLiteral("unitId"), QString{}}};
        const auto invalid = Protocol::makeClientEnvelope(
            QStringLiteral("command"), QStringLiteral("invalid-") + action,
            invalidPayload);
        EXPECT_FALSE(Protocol::validateClientEnvelope(invalid).valid)
            << action.toStdString();
    }
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
                    {QStringLiteral("projectiles"), QJsonArray{}},
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
                    {QStringLiteral("projectiles"), QJsonArray{}},
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
                                {QStringLiteral("aiEngine"), QStringLiteral("ollama")},
                                {QStringLiteral("running"), false},
                                {QStringLiteral("simTime"), 12.5},
                                {QStringLiteral("speed"), 2.0},
                                {QStringLiteral("scenarioRevision"), 4},
                                {QStringLiteral("seats"), QJsonArray{seat}}};
    Protocol::SnapshotProjection snapshot;
    ASSERT_TRUE(Protocol::projectSnapshot(
        QJsonObject{{QStringLiteral("roomState"), roomState}}, &snapshot).valid);
    EXPECT_EQ(snapshot.lifecycle.roomId, QStringLiteral("main"));
    EXPECT_EQ(snapshot.lifecycle.roomMode, QStringLiteral("pvp"));
    EXPECT_EQ(snapshot.lifecycle.aiDifficulty, QStringLiteral("normal"));
    EXPECT_EQ(snapshot.lifecycle.aiEngine, QStringLiteral("ollama"));
    EXPECT_EQ(snapshot.lifecycle.configVersion, 1);
    ASSERT_EQ(snapshot.lifecycle.seats.size(), 1);
    EXPECT_EQ(snapshot.lifecycle.seats.front().seatId, QStringLiteral("red_recon_1"));
    EXPECT_TRUE(snapshot.lifecycle.seats.front().connected);
    EXPECT_TRUE(snapshot.lifecycle.seats.front().deployed);
    EXPECT_EQ(snapshot.lifecycle.seats.front().unitId, QStringLiteral("red-r1"));
    EXPECT_EQ(snapshot.lifecycle.seats.front().controllerType, QStringLiteral("human"));

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
    EXPECT_EQ(projectedSeat.value(QStringLiteral("controllerType")).toString(),
              QStringLiteral("human"));

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

namespace {

QJsonObject protocolProjectile(const QString& id = QStringLiteral("missile_1")) {
    return {{QStringLiteral("id"), id},
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
            {QStringLiteral("threatRadius"), 1300.0},
            {QStringLiteral("attackerId"), QStringLiteral("red_a1")},
            {QStringLiteral("targetId"), QStringLiteral("blue_a1")}};
}

QJsonObject protocolProjectileSnapshot(qint64 revision = 1) {
    const QJsonArray units{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("blue_a1")},
                    {QStringLiteral("side"), QStringLiteral("blue")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("red_a1")},
                    {QStringLiteral("side"), QStringLiteral("red")}}};
    return {{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
            {QStringLiteral("stateRevision"), revision},
            {QStringLiteral("scenario"),
             QJsonObject{{QStringLiteral("schemaVersion"), 3},
                         {QStringLiteral("map"),
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("test")},
                                      {QStringLiteral("widthMeters"), 1000.0},
                                      {QStringLiteral("heightMeters"), 800.0}}},
                         {QStringLiteral("units"), units}}},
            {QStringLiteral("units"), units},
            {QStringLiteral("projectiles"), QJsonArray{protocolProjectile()}},
            {QStringLiteral("messages"), QJsonArray{}},
            {QStringLiteral("roomState"),
             QJsonObject{{QStringLiteral("scenarioRevision"), 1},
                         {QStringLiteral("stateRevision"), revision},
                         {QStringLiteral("simTime"), 1.0}}}};
}

QJsonObject observerProtocolRuntimeUnit() {
    return {{QStringLiteral("id"), QStringLiteral("red_a1")},
            {QStringLiteral("callsign"), QStringLiteral("Red Attack 1")},
            {QStringLiteral("side"), QStringLiteral("red")},
            {QStringLiteral("kind"), QStringLiteral("attackuav")},
            {QStringLiteral("movable"), true},
            {QStringLiteral("position"), QJsonArray{1.0, 2.0, 3.0}},
            {QStringLiteral("detectRange"), 4000.0},
            {QStringLiteral("attackRange"), 2500.0},
            {QStringLiteral("commRange"), 15000.0},
            {QStringLiteral("speed"), 100.0},
            {QStringLiteral("baseSpeed"), 100.0},
            {QStringLiteral("maxCommandedSpeed"), 360.0},
            {QStringLiteral("maxHp"), 120.0},
            {QStringLiteral("attackPower"), 100.0},
            {QStringLiteral("armor"), 0.1},
            {QStringLiteral("hp"), 100.0},
            {QStringLiteral("alive"), true},
            {QStringLiteral("subsystems"),
             QJsonObject{{QStringLiteral("sensor"), 1.0},
                         {QStringLiteral("comms"), 1.0},
                         {QStringLiteral("mobility"), 1.0},
                         {QStringLiteral("weapon"), 1.0}}},
            {QStringLiteral("serviceRequested"), false},
            {QStringLiteral("serviceProgress"), 0.0},
            {QStringLiteral("ammoRemaining"), 4},
            {QStringLiteral("ammoCapacity"), 4},
            {QStringLiteral("cooldownRemaining"), 0.0},
            {QStringLiteral("cooldownSec"), 4.0},
            {QStringLiteral("activeProjectileCount"), 0},
            {QStringLiteral("fuelRemaining"), 1200.0},
            {QStringLiteral("fuelCapacity"), 1800.0},
            {QStringLiteral("turnaroundProgress"), 0.0}};
}

QJsonObject observerProtocolScenarioUnit() {
    return {{QStringLiteral("id"), QStringLiteral("red_a1")},
            {QStringLiteral("callsign"), QStringLiteral("Red Attack 1")},
            {QStringLiteral("side"), QStringLiteral("red")},
            {QStringLiteral("kind"), QStringLiteral("attackuav")},
            {QStringLiteral("x"), 1.0},
            {QStringLiteral("y"), 2.0},
            {QStringLiteral("alt"), 3.0},
            {QStringLiteral("detectRange"), 4000.0},
            {QStringLiteral("attackRange"), 2500.0},
            {QStringLiteral("commRange"), 15000.0},
            {QStringLiteral("speed"), 100.0},
            {QStringLiteral("maxHp"), 120.0},
            {QStringLiteral("attackPower"), 100.0},
            {QStringLiteral("armor"), 0.1},
            {QStringLiteral("ammoCapacity"), 4},
            {QStringLiteral("initialAmmo"), 4},
            {QStringLiteral("cooldownSec"), 4.0},
            {QStringLiteral("fuelCapacitySec"), 1800.0},
            {QStringLiteral("initialFuelSec"), 1800.0}};
}

QJsonObject observerProtocolSnapshot() {
    const QJsonObject roomState{{QStringLiteral("phase"), QStringLiteral("running")},
                                {QStringLiteral("roomId"), QStringLiteral("main")},
                                {QStringLiteral("roomMode"), QStringLiteral("pvp")},
                                {QStringLiteral("observer"), true},
                                {QStringLiteral("running"), true},
                                {QStringLiteral("simTime"), 1.0},
                                {QStringLiteral("speed"), 1.0},
                                {QStringLiteral("scenarioRevision"), 1},
                                {QStringLiteral("stateRevision"), 1}};
    return {
        {QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
        {QStringLiteral("stateRevision"), 1},
        {QStringLiteral("scenario"),
         QJsonObject{{QStringLiteral("schemaVersion"), 1},
                     {QStringLiteral("map"),
                      QJsonObject{{QStringLiteral("name"), QStringLiteral("default")},
                                  {QStringLiteral("widthMeters"), 40000.0},
                                  {QStringLiteral("heightMeters"), 30000.0},
                                  {QStringLiteral("backgroundResource"), QStringLiteral("")}}},
                     {QStringLiteral("units"),
                      QJsonArray{observerProtocolScenarioUnit()}}}},
        {QStringLiteral("units"), QJsonArray{observerProtocolRuntimeUnit()}},
        {QStringLiteral("projectiles"), QJsonArray{}},
        {QStringLiteral("roomState"), roomState}};
}

}

TEST(ProtocolTest, ObserverFlagDefaultsFalseAndRequiresBooleanWhenPresent) {
    Protocol::RoomLifecycleProjection projection;
    ASSERT_TRUE(Protocol::projectRoomLifecycle(QJsonObject{}, &projection).valid);
    EXPECT_FALSE(projection.observer);

    ASSERT_TRUE(Protocol::projectRoomLifecycle(
                    QJsonObject{{QStringLiteral("observer"), true}}, &projection).valid);
    EXPECT_TRUE(projection.observer);

    ASSERT_TRUE(Protocol::projectRoomLifecycle(
                    QJsonObject{{QStringLiteral("observer"), false}}, &projection).valid);
    EXPECT_FALSE(projection.observer);

    EXPECT_FALSE(Protocol::projectRoomLifecycle(
                     QJsonObject{{QStringLiteral("observer"), QStringLiteral("true")}},
                     &projection).valid);
}

TEST(ProtocolTest, AiEngineIsConstrainedToPublicRuntimeValues) {
    Protocol::RoomLifecycleProjection projection;
    ASSERT_TRUE(Protocol::projectRoomLifecycle(
                    QJsonObject{{QStringLiteral("aiEngine"), QStringLiteral("rules")}},
                    &projection).valid);
    EXPECT_EQ(projection.aiEngine, QStringLiteral("rules"));

    EXPECT_FALSE(Protocol::projectRoomLifecycle(
                     QJsonObject{{QStringLiteral("aiEngine"), QStringLiteral("unknown")}},
                     &projection).valid);
}

TEST(ProtocolTest, ObserverSnapshotUsesStrictPositiveWhitelistsWithEmptyLegacyMapMarks) {
    const QJsonObject validPayload = observerProtocolSnapshot();
    const auto validate = [](const QJsonObject& payload) {
        return Protocol::validateServerEnvelope(Protocol::makeServerEnvelope(
            QStringLiteral("snapshot"), 1, payload)).valid;
    };
    ASSERT_TRUE(validate(validPayload));

    QJsonObject legacyEmptyMapMarks = validPayload;
    legacyEmptyMapMarks[QStringLiteral("mapMarks")] = QJsonArray{};
    EXPECT_TRUE(validate(legacyEmptyMapMarks));

    QJsonObject visibleMapMark = validPayload;
    visibleMapMark[QStringLiteral("mapMarks")] = QJsonArray{
        QJsonObject{{QStringLiteral("label"), QStringLiteral("private mark")}}};
    EXPECT_FALSE(validate(visibleMapMark));

    for (const QString& field : {QStringLiteral("messages"),
                                 QStringLiteral("serverMetrics")}) {
        QJsonObject sensitive = validPayload;
        sensitive[field] = field == QLatin1String("serverMetrics")
            ? QJsonValue(QJsonObject{}) : QJsonValue(QJsonArray{});
        EXPECT_FALSE(validate(sensitive)) << field.toStdString();
    }

    for (const QString& field : {QStringLiteral("sharedKnowledge"),
                                 QStringLiteral("detections"), QStringLiteral("schedule"),
                                 QStringLiteral("recentPath"), QStringLiteral("targetId"),
                                 QStringLiteral("rulesOfEngagement"),
                                 QStringLiteral("lastShotOutcome")}) {
        QJsonObject sensitive = validPayload;
        QJsonObject unit = observerProtocolRuntimeUnit();
        unit[field] = QJsonObject{};
        sensitive[QStringLiteral("units")] = QJsonArray{unit};
        EXPECT_FALSE(validate(sensitive)) << field.toStdString();
    }

    for (const QString& field : {QStringLiteral("seats"),
                                 QStringLiteral("transferRequests"),
                                 QStringLiteral("online"), QStringLiteral("serverPrivate")}) {
        QJsonObject sensitive = validPayload;
        QJsonObject room = sensitive.value(QStringLiteral("roomState")).toObject();
        room[field] = QJsonArray{};
        sensitive[QStringLiteral("roomState")] = room;
        EXPECT_FALSE(validate(sensitive)) << field.toStdString();
    }

    QJsonObject sensitiveScenario = validPayload;
    QJsonObject scenario = sensitiveScenario.value(QStringLiteral("scenario")).toObject();
    scenario[QStringLiteral("notes")] = QStringLiteral("private plan");
    sensitiveScenario[QStringLiteral("scenario")] = scenario;
    EXPECT_FALSE(validate(sensitiveScenario));
}

TEST(ProtocolTest, ObserverSnapshotAndDeltaRejectMalformedNestedFields) {
    const auto validateSnapshot = [](const QJsonObject& payload) {
        return Protocol::validateServerEnvelope(Protocol::makeServerEnvelope(
            QStringLiteral("snapshot"), 1, payload)).valid;
    };
    QJsonObject malformed = observerProtocolSnapshot();
    QJsonObject malformedUnit = observerProtocolRuntimeUnit();
    malformedUnit[QStringLiteral("position")] = QJsonArray{QStringLiteral("bad"), 2.0};
    malformed[QStringLiteral("units")] = QJsonArray{malformedUnit};
    EXPECT_FALSE(validateSnapshot(malformed));

    malformed = observerProtocolSnapshot();
    malformedUnit = observerProtocolRuntimeUnit();
    QJsonObject subsystems = malformedUnit.value(QStringLiteral("subsystems")).toObject();
    subsystems[QStringLiteral("targetId")] = QStringLiteral("blue_a1");
    malformedUnit[QStringLiteral("subsystems")] = subsystems;
    malformed[QStringLiteral("units")] = QJsonArray{malformedUnit};
    EXPECT_FALSE(validateSnapshot(malformed));

    const QJsonObject base = observerProtocolSnapshot();
    QJsonObject current = base;
    current[QStringLiteral("stateRevision")] = 2;
    QJsonObject currentRoom = current.value(QStringLiteral("roomState")).toObject();
    currentRoom[QStringLiteral("stateRevision")] = 2;
    current[QStringLiteral("roomState")] = currentRoom;
    QJsonObject delta = StateDelta::create(base, current);
    ASSERT_FALSE(delta.isEmpty());
    EXPECT_TRUE(Protocol::validateServerEnvelope(Protocol::makeServerEnvelope(
                    QStringLiteral("delta"), 2, delta)).valid);

    delta[QStringLiteral("mapMarks")] = QJsonArray{};
    EXPECT_TRUE(Protocol::validateServerEnvelope(Protocol::makeServerEnvelope(
                    QStringLiteral("delta"), 2, delta)).valid);

    delta[QStringLiteral("mapMarks")] = QJsonArray{
        QJsonObject{{QStringLiteral("label"), QStringLiteral("private mark")}}};
    EXPECT_FALSE(Protocol::validateServerEnvelope(Protocol::makeServerEnvelope(
                    QStringLiteral("delta"), 2, delta)).valid);

    delta.remove(QStringLiteral("mapMarks"));
    delta[QStringLiteral("serverMetrics")] = QJsonObject{};
    EXPECT_FALSE(Protocol::validateServerEnvelope(Protocol::makeServerEnvelope(
                     QStringLiteral("delta"), 2, delta)).valid);
}

TEST(ProtocolTest, SchemaThreeSnapshotRequiresValidProjectiles) {
    const auto validate = [](const QJsonObject& payload) {
        return Protocol::validateServerEnvelope(Protocol::makeServerEnvelope(
            QStringLiteral("snapshot"), 1, payload));
    };

    const QJsonObject valid = protocolProjectileSnapshot();
    ASSERT_TRUE(validate(valid).valid);

    QJsonObject missing = valid;
    missing.remove(QStringLiteral("projectiles"));
    EXPECT_FALSE(validate(missing).valid);

    QJsonObject wrongType = valid;
    wrongType[QStringLiteral("projectiles")] = QJsonObject{};
    EXPECT_FALSE(validate(wrongType).valid);

    QJsonObject malformed = valid;
    QJsonObject missingSpeed = protocolProjectile();
    missingSpeed.remove(QStringLiteral("speed"));
    malformed[QStringLiteral("projectiles")] = QJsonArray{missingSpeed};
    EXPECT_FALSE(validate(malformed).valid);

    QJsonObject duplicate = valid;
    duplicate[QStringLiteral("projectiles")] =
        QJsonArray{protocolProjectile(), protocolProjectile()};
    EXPECT_FALSE(validate(duplicate).valid);

    QJsonArray tooMany;
    for (int index = 0; index <= Protocol::MaxProjectiles; ++index) {
        tooMany.append(protocolProjectile(QStringLiteral("missile_%1").arg(index)));
    }
    QJsonObject oversized = valid;
    oversized[QStringLiteral("projectiles")] = tooMany;
    EXPECT_FALSE(validate(oversized).valid);
}

TEST(ProtocolTest, ProjectileSnapshotRejectsBoundsAndReferenceSideViolations) {
    const auto validate = [](const QJsonObject& payload) {
        return Protocol::validateServerEnvelope(Protocol::makeServerEnvelope(
            QStringLiteral("snapshot"), 1, payload)).valid;
    };

    QJsonObject outOfBounds = protocolProjectileSnapshot();
    QJsonObject projectile = protocolProjectile();
    projectile[QStringLiteral("position")] = QJsonArray{1000.01, 400.0, 120.0};
    outOfBounds[QStringLiteral("projectiles")] = QJsonArray{projectile};
    EXPECT_FALSE(validate(outOfBounds));

    QJsonObject wrongAttackerSide = protocolProjectileSnapshot();
    projectile = protocolProjectile();
    projectile[QStringLiteral("attackerId")] = QStringLiteral("blue_a1");
    projectile.remove(QStringLiteral("targetId"));
    wrongAttackerSide[QStringLiteral("projectiles")] = QJsonArray{projectile};
    EXPECT_FALSE(validate(wrongAttackerSide));

    QJsonObject wrongTargetSide = protocolProjectileSnapshot();
    projectile = protocolProjectile();
    projectile.remove(QStringLiteral("attackerId"));
    projectile[QStringLiteral("targetId")] = QStringLiteral("red_a1");
    wrongTargetSide[QStringLiteral("projectiles")] = QJsonArray{projectile};
    EXPECT_FALSE(validate(wrongTargetSide));
}

TEST(ProtocolTest, AcceptsOutOfRangeTerminalProjectile) {
    const auto validate = [](const QJsonObject& payload) {
        return Protocol::validateServerEnvelope(Protocol::makeServerEnvelope(
            QStringLiteral("snapshot"), 1, payload));
    };

    QJsonObject payload = protocolProjectileSnapshot();
    QJsonObject projectile = protocolProjectile();
    projectile[QStringLiteral("active")] = false;
    projectile[QStringLiteral("terminalReason")] = QStringLiteral("out_of_range");
    projectile[QStringLiteral("resultSettled")] = true;
    payload[QStringLiteral("projectiles")] = QJsonArray{projectile};
    EXPECT_TRUE(validate(payload).valid);
}

TEST(ProtocolTest, ObserverProjectilesMustRemainAnonymous) {
    const auto validate = [](const QJsonObject& payload) {
        return Protocol::validateServerEnvelope(Protocol::makeServerEnvelope(
            QStringLiteral("snapshot"), 1, payload)).valid;
    };

    QJsonObject anonymous = protocolProjectile();
    anonymous.remove(QStringLiteral("attackerId"));
    anonymous.remove(QStringLiteral("targetId"));
    QJsonObject payload = observerProtocolSnapshot();
    payload[QStringLiteral("projectiles")] = QJsonArray{anonymous};
    ASSERT_TRUE(validate(payload));

    anonymous[QStringLiteral("attackerId")] = QStringLiteral("red_a1");
    payload[QStringLiteral("projectiles")] = QJsonArray{anonymous};
    EXPECT_FALSE(validate(payload));

    anonymous.remove(QStringLiteral("attackerId"));
    anonymous[QStringLiteral("targetId")] = QStringLiteral("red_a1");
    payload[QStringLiteral("projectiles")] = QJsonArray{anonymous};
    EXPECT_FALSE(validate(payload));
}

TEST(ProtocolTest, RejectsMalformedOptionalPveFields) {
    const QJsonObject malformedSeatState = Protocol::makeServerEnvelope(
        QStringLiteral("seatState"), 1,
        QJsonObject{{QStringLiteral("roomId"), QStringLiteral("main")},
                    {QStringLiteral("seats"), QJsonArray{QJsonObject{
                        {QStringLiteral("seatId"), QStringLiteral("blue_commander")},
                        {QStringLiteral("controllerType"), QStringLiteral("bot")}}}}});
    EXPECT_FALSE(Protocol::validateServerEnvelope(malformedSeatState).valid);

    const QJsonObject malformedSnapshot = Protocol::makeServerEnvelope(
        QStringLiteral("snapshot"), 2,
        QJsonObject{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                    {QStringLiteral("stateRevision"), 1},
                    {QStringLiteral("scenario"), QJsonObject{{QStringLiteral("units"), QJsonArray{}}}},
                    {QStringLiteral("units"), QJsonArray{}},
                    {QStringLiteral("projectiles"), QJsonArray{}},
                    {QStringLiteral("messages"), QJsonArray{}},
                    {QStringLiteral("roomState"),
                     QJsonObject{{QStringLiteral("roomMode"), QStringLiteral("coop")},
                                 {QStringLiteral("aiDifficulty"), QStringLiteral("normal")},
                                 {QStringLiteral("configVersion"), 1}}}});
    EXPECT_FALSE(Protocol::validateServerEnvelope(malformedSnapshot).valid);
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
                     {QStringLiteral("projectiles"), QJsonArray{}},
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

TEST(StateDeltaTest, ReplacesAndClearsProjectileCollection) {
    QJsonObject base = protocolProjectileSnapshot(10);
    QJsonObject current = base;
    current[QStringLiteral("stateRevision")] = 11;
    QJsonObject currentRoom = current.value(QStringLiteral("roomState")).toObject();
    currentRoom[QStringLiteral("stateRevision")] = 11;
    currentRoom[QStringLiteral("simTime")] = 1.1;
    current[QStringLiteral("roomState")] = currentRoom;
    QJsonObject movedProjectile = protocolProjectile();
    movedProjectile[QStringLiteral("position")] = QJsonArray{550.0, 425.0, 120.0};
    movedProjectile[QStringLiteral("age")] = 1.1;
    current[QStringLiteral("projectiles")] = QJsonArray{movedProjectile};

    const QJsonObject replacementDelta = StateDelta::create(base, current);
    ASSERT_TRUE(replacementDelta.contains(QStringLiteral("projectiles")));
    EXPECT_EQ(replacementDelta.value(QStringLiteral("projectiles")),
              current.value(QStringLiteral("projectiles")));
    QString error;
    ASSERT_TRUE(StateDelta::apply(base, replacementDelta, &error)) << error.toStdString();
    EXPECT_EQ(base, current);

    QJsonObject cleared = current;
    cleared[QStringLiteral("stateRevision")] = 12;
    QJsonObject clearedRoom = cleared.value(QStringLiteral("roomState")).toObject();
    clearedRoom[QStringLiteral("stateRevision")] = 12;
    clearedRoom[QStringLiteral("simTime")] = 1.2;
    cleared[QStringLiteral("roomState")] = clearedRoom;
    cleared[QStringLiteral("projectiles")] = QJsonArray{};

    const QJsonObject removalDelta = StateDelta::create(current, cleared);
    ASSERT_TRUE(removalDelta.contains(QStringLiteral("projectiles")));
    EXPECT_TRUE(removalDelta.value(QStringLiteral("projectiles")).toArray().isEmpty());
    ASSERT_TRUE(StateDelta::apply(base, removalDelta, &error)) << error.toStdString();
    EXPECT_EQ(base, cleared);
}

TEST(StateDeltaTest, RejectsWrongBaseRevision) {
    QJsonObject state{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                      {QStringLiteral("stateRevision"), 7},
                      {QStringLiteral("scenario"), QJsonObject{}},
                      {QStringLiteral("units"), QJsonArray{}},
                      {QStringLiteral("projectiles"), QJsonArray{}},
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
