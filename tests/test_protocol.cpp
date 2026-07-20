#include <gtest/gtest.h>

#include "protocol/Protocol.h"
#include "protocol/StateDelta.h"

#include <QJsonArray>

using namespace gbr;

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
