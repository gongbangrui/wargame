#include "core/GuidedStrikeWorkflow.h"
#include "core/MapProvider.h"
#include "core/SimulationEngine.h"
#include "vmf/VmfMessageGateway.h"
#include "vmf/VmfProfile.h"

#include <gtest/gtest.h>

using namespace gbr;

namespace {

QString designPath(const QString& relative) {
    return QStringLiteral(WARGAME_SOURCE_DIR) + QLatin1Char('/') + relative;
}

void registerUnit(MessageBus& bus, const QString& id, const QString& side,
                  double x, double range = 10000.0) {
    bus.subscribe(id, [](const Message&) {});
    bus.updateUnitPosition(id, QPointF(x, 0.0), range, side);
}

TEST(VmfGatewayTest, TargetReportEncodesDesignFields) {
    QList<vmf::Diagnostic> diagnostics;
    const auto dictionaries = vmf::VmfProfile::loadDesignV1(
        designPath(QStringLiteral("design/EncoderDecoder")), &diagnostics);
    ASSERT_TRUE(dictionaries) << vmf::Codec::diagnosticsToString(diagnostics).toStdString();

    MessageBus bus;
    vmf::VmfMessageGateway gateway(&bus, dictionaries);
    Message input;
    input.id = QStringLiteral("target-report-1");
    input.type = Message::Type::TargetReport;
    input.sender = QStringLiteral("red_recon");
    input.receiver = QStringLiteral("red_cp");
    input.timestamp = QDateTime::fromString(QStringLiteral("2026-08-17T10:20:30Z"), Qt::ISODate);
    input.payload = QJsonObject{{QStringLiteral("targetType"), QStringLiteral("armored")},
                                {QStringLiteral("targetCount"), 2},
                                {QStringLiteral("friendFoe"), QStringLiteral("enemy")},
                                {QStringLiteral("latitude"), 30.5},
                                {QStringLiteral("longitude"), 120.25},
                                {QStringLiteral("status"), QStringLiteral("damaged")},
                                {QStringLiteral("headingDeg"), 90.0}};

    Message encoded;
    QString error;
    ASSERT_TRUE(gateway.prepareDomainMessage(input, &encoded, &error)) << error.toStdString();
    EXPECT_EQ(encoded.vmfMessage, QStringLiteral("Target Report"));
    EXPECT_FALSE(encoded.wireBytes.isEmpty());

    vmf::DecodedMessage decoded;
    ASSERT_TRUE(gateway.decode(encoded, &decoded, &diagnostics))
        << vmf::Codec::diagnosticsToString(diagnostics).toStdString();
    ASSERT_GE(decoded.fields.size(), 12);
    auto valueFor = [&](const QString& name) {
        for (const vmf::FieldLayout& field : decoded.fields) {
            if (field.name == name) return field.value;
        }
        return quint64{0};
    };
    EXPECT_EQ(valueFor(QStringLiteral("TargetType")), 1U);
    EXPECT_EQ(valueFor(QStringLiteral("TargetQuantity")), 2U);
    EXPECT_EQ(valueFor(QStringLiteral("IdentificationFriendOrFoe")), 0U);
    EXPECT_EQ(valueFor(QStringLiteral("TargetStatus")), 1U);
    EXPECT_GT(valueFor(QStringLiteral("Direction")), 0U);
}

TEST(VmfGatewayTest, LandRouteEncodesWaypointsAndCriticalPoints) {
    QList<vmf::Diagnostic> diagnostics;
    const auto dictionaries = vmf::VmfProfile::loadDesignV1(
        designPath(QStringLiteral("design/EncoderDecoder")), &diagnostics);
    ASSERT_TRUE(dictionaries);

    MessageBus bus;
    vmf::VmfMessageGateway gateway(&bus, dictionaries);
    Message input;
    input.id = QStringLiteral("strike-plan-1");
    input.type = Message::Type::StrikePlan;
    input.sender = QStringLiteral("red_cp");
    input.receiver = QStringLiteral("red_attack_1");
    input.payload = QJsonObject{
        {QStringLiteral("targetId"), QStringLiteral("blue_tank_1")},
        {QStringLiteral("waypoints"), QJsonArray{
            QJsonObject{{QStringLiteral("x"), 10.0}, {QStringLiteral("y"), 20.0}},
            QJsonObject{{QStringLiteral("x"), 30.0}, {QStringLiteral("y"), 40.0}}}},
        {QStringLiteral("criticalPoints"), QJsonArray{
            QJsonObject{{QStringLiteral("x"), 15.0}, {QStringLiteral("y"), 25.0}}}}};

    Message encoded;
    QString error;
    ASSERT_TRUE(gateway.prepareDomainMessage(input, &encoded, &error)) << error.toStdString();
    EXPECT_EQ(encoded.vmfMessage, QStringLiteral("Land Route"));
    EXPECT_EQ(encoded.payload.value(QStringLiteral("vmfCoordinateSource")).toString(),
              QStringLiteral("logical-grid-fallback"));
    vmf::DecodedMessage decoded;
    ASSERT_TRUE(gateway.decode(encoded, &decoded, &diagnostics))
        << vmf::Codec::diagnosticsToString(diagnostics).toStdString();
    int latitudeFields = 0;
    int longitudeFields = 0;
    for (const vmf::FieldLayout& field : decoded.fields) {
        if (field.name == QLatin1String("Latitude00051")) ++latitudeFields;
        if (field.name == QLatin1String("Longtitude00051")) ++longitudeFields;
    }
    EXPECT_EQ(latitudeFields, 3);
    EXPECT_EQ(longitudeFields, 3);
}

TEST(VmfGatewayTest, LandRouteKeepsRouteDataContainerWhenCriticalPointsAreAbsent) {
    QList<vmf::Diagnostic> diagnostics;
    const auto dictionaries = vmf::VmfProfile::loadDesignV1(
        designPath(QStringLiteral("design/EncoderDecoder")), &diagnostics);
    ASSERT_TRUE(dictionaries);

    MessageBus bus;
    vmf::VmfMessageGateway gateway(&bus, dictionaries);
    Message input;
    input.id = QStringLiteral("route-without-critical-points");
    input.type = Message::Type::StrikePlan;
    input.sender = QStringLiteral("red_cp");
    input.receiver = QStringLiteral("red_attack_1");
    input.payload = QJsonObject{
        {QStringLiteral("targetId"), QStringLiteral("blue_target")},
        {QStringLiteral("waypoints"), QJsonArray{
            QJsonObject{{QStringLiteral("x"), 10.0}, {QStringLiteral("y"), 20.0}}}}};

    Message encoded;
    QString error;
    ASSERT_TRUE(gateway.prepareDomainMessage(input, &encoded, &error)) << error.toStdString();
    vmf::DecodedMessage decoded;
    ASSERT_TRUE(gateway.decode(encoded, &decoded, &diagnostics))
        << vmf::Codec::diagnosticsToString(diagnostics).toStdString();
    EXPECT_EQ(decoded.fields.size(), 15);
}

TEST(VmfGatewayTest, InjectedMapResolverUsesRealGeoQuantization) {
    QList<vmf::Diagnostic> diagnostics;
    const auto dictionaries = vmf::VmfProfile::loadDesignV1(
        designPath(QStringLiteral("design/EncoderDecoder")), &diagnostics);
    ASSERT_TRUE(dictionaries);

    MessageBus bus;
    vmf::VmfMessageGateway gateway(&bus, dictionaries);
    gateway.setCoordinateResolver([](double x, double y) -> std::optional<GeoCoord> {
        return GeoCoord{25.4001 + y * 0.00001, 119.3002 + x * 0.00001};
    });

    Message local;
    local.id = QStringLiteral("map-resolver-report");
    local.type = Message::Type::TargetReport;
    local.sender = QStringLiteral("red_recon");
    local.receiver = QStringLiteral("red_cp");
    local.payload = QJsonObject{{QStringLiteral("targetId"), QStringLiteral("blue_tank")},
                                {QStringLiteral("targetType"), QStringLiteral("armored")},
                                {QStringLiteral("x"), 1200.0},
                                {QStringLiteral("y"), 3400.0},
                                {QStringLiteral("friendFoe"), QStringLiteral("enemy")}};
    Message mapped;
    QString error;
    ASSERT_TRUE(gateway.prepareDomainMessage(local, &mapped, &error)) << error.toStdString();
    EXPECT_EQ(mapped.payload.value(QStringLiteral("vmfCoordinateSource")).toString(),
              QStringLiteral("map-gis"));

    Message geographic = local;
    geographic.payload.remove(QStringLiteral("x"));
    geographic.payload.remove(QStringLiteral("y"));
    geographic.payload.insert(QStringLiteral("latitude"), 25.4341);
    geographic.payload.insert(QStringLiteral("longitude"), 119.3122);
    Message explicitlyMapped;
    ASSERT_TRUE(gateway.prepareDomainMessage(geographic, &explicitlyMapped, &error))
        << error.toStdString();
    EXPECT_EQ(mapped.wireBytes, explicitlyMapped.wireBytes);
    EXPECT_EQ(mapped.wireBitLength, explicitlyMapped.wireBitLength);
}

}

TEST(VmfMessageBusTest, AutomaticAckAndSimulationRetriesUseNineSecondDeadline) {
    MessageBus bus;
    registerUnit(bus, QStringLiteral("red_cp"), QStringLiteral("red"), 0.0);
    registerUnit(bus, QStringLiteral("red_a1"), QStringLiteral("red"), 10.0);
    int posted = 0;
    QString terminalReason;
    QObject::connect(&bus, &MessageBus::messagePosted,
                     [&](const QJsonObject& message) {
                         if (message.value(QStringLiteral("type")).toString()
                             == QLatin1String("GroundGuideOrder")) ++posted;
                     });
    QObject::connect(&bus, &MessageBus::ackStateChanged,
                     [&](const QString&, bool acknowledged, int, const QString& reason) {
                         if (!acknowledged) terminalReason = reason;
                     });

    Message message;
    message.type = Message::Type::GroundGuideOrder;
    message.sender = QStringLiteral("red_cp");
    message.receiver = QStringLiteral("red_a1");
    message.requiresAck = true;
    message.automaticAck = false;
    bus.send(message);
    EXPECT_EQ(bus.pendingAcks().size(), 1);
    EXPECT_EQ(posted, 1);

    bus.advanceSimulationTime(3.0);
    EXPECT_EQ(posted, 2);
    bus.advanceSimulationTime(3.0);
    EXPECT_EQ(posted, 3);
    bus.advanceSimulationTime(3.0);
    EXPECT_TRUE(bus.pendingAcks().isEmpty());
    EXPECT_EQ(terminalReason, QStringLiteral("timeout"));
}

TEST(VmfMessageBusTest, AutomaticAckCompletesWithoutUnitSpecificHandler) {
    MessageBus bus;
    registerUnit(bus, QStringLiteral("red_cp"), QStringLiteral("red"), 0.0);
    registerUnit(bus, QStringLiteral("red_a1"), QStringLiteral("red"), 10.0);
    Message message;
    message.type = Message::Type::TargetReport;
    message.sender = QStringLiteral("red_cp");
    message.receiver = QStringLiteral("red_a1");
    message.requiresAck = true;
    message.automaticAck = true;
    bus.send(message);
    EXPECT_TRUE(bus.pendingAcks().isEmpty());
}

TEST(VmfMessageBusTest, EngineMessageCacheReflectsAutomaticAck) {
    SimulationEngine engine;
    Scenario scenario = ScenarioIo::defaultScenario();
    scenario.communicationPolicy.format = QStringLiteral("vmf-design-v1");
    scenario.communicationPolicy.vmfProfile = QStringLiteral("vmf-design-v1");
    ASSERT_TRUE(engine.setScenario(scenario));

    Message input;
    input.id = QStringLiteral("engine-ack-report");
    input.type = Message::Type::TargetReport;
    input.sender = QStringLiteral("red_cp");
    input.receiver = QStringLiteral("red_a1");
    input.requiresAck = true;
    input.automaticAck = true;
    input.payload = QJsonObject{{QStringLiteral("targetId"), QStringLiteral("blue_cp")},
                                {QStringLiteral("targetType"), QStringLiteral("commandpost")},
                                {QStringLiteral("friendFoe"), QStringLiteral("enemy")},
                                {QStringLiteral("x"), 3000.0},
                                {QStringLiteral("y"), 11000.0}};
    Message encoded;
    QString error;
    ASSERT_TRUE(engine.prepareVmfMessage(input, &encoded, &error)) << error.toStdString();
    ASSERT_TRUE(engine.postVmfMessage(encoded, &error)) << error.toStdString();

    ASSERT_FALSE(engine.recentMessages().isEmpty());
    QVariantMap posted;
    for (const QVariant& value : engine.recentMessages()) {
        const QVariantMap candidate = value.toMap();
        if (candidate.value(QStringLiteral("id")).toString() == input.id) {
            posted = candidate;
            break;
        }
    }
    ASSERT_FALSE(posted.isEmpty());
    EXPECT_TRUE(posted.value(QStringLiteral("acked")).toBool());
    EXPECT_EQ(posted.value(QStringLiteral("ackReason")).toString(), QStringLiteral("ack"));
    EXPECT_TRUE(engine.bus()->pendingAcks().isEmpty());
}

TEST(VmfMessageBusTest, ScenarioCanDisableAutomaticAck) {
    SimulationEngine engine;
    Scenario scenario = ScenarioIo::defaultScenario();
    scenario.communicationPolicy.format = QStringLiteral("vmf-design-v1");
    scenario.communicationPolicy.vmfProfile = QStringLiteral("vmf-design-v1");
    scenario.communicationPolicy.automaticAck = false;
    ASSERT_TRUE(engine.setScenario(scenario));

    Message input;
    input.id = QStringLiteral("engine-manual-ack-report");
    input.type = Message::Type::TargetReport;
    input.sender = QStringLiteral("red_cp");
    input.receiver = QStringLiteral("red_a1");
    input.payload = QJsonObject{{QStringLiteral("targetId"), QStringLiteral("blue_cp")},
                                {QStringLiteral("targetType"), QStringLiteral("commandpost")},
                                {QStringLiteral("friendFoe"), QStringLiteral("enemy")},
                                {QStringLiteral("x"), 3000.0},
                                {QStringLiteral("y"), 11000.0}};
    Message encoded;
    QString error;
    ASSERT_TRUE(engine.prepareVmfMessage(input, &encoded, &error)) << error.toStdString();
    ASSERT_FALSE(encoded.automaticAck);
    ASSERT_TRUE(engine.postVmfMessage(encoded, &error)) << error.toStdString();
    EXPECT_EQ(engine.bus()->pendingAcks().size(), 1);
}

TEST(VmfMessageBusTest, AutomaticRetryCanDeliverAfterCommunicationRecovery) {
    MessageBus bus;
    registerUnit(bus, QStringLiteral("red_cp"), QStringLiteral("red"), 0.0);
    int received = 0;
    bus.subscribe(QStringLiteral("red_a1"), [&](const Message&) { ++received; });
    bus.updateUnitPosition(QStringLiteral("red_a1"), QPointF(50000.0, 0.0), 10000.0,
                           QStringLiteral("red"));

    Message message;
    message.type = Message::Type::TargetReport;
    message.sender = QStringLiteral("red_cp");
    message.receiver = QStringLiteral("red_a1");
    message.requiresAck = true;
    message.automaticAck = true;
    ASSERT_TRUE(bus.send(message));
    EXPECT_EQ(received, 0);
    EXPECT_EQ(bus.pendingAcks().size(), 1);

    bus.updateUnitPosition(QStringLiteral("red_a1"), QPointF(10.0, 0.0), 10000.0,
                           QStringLiteral("red"));
    bus.advanceSimulationTime(3.0);
    EXPECT_EQ(received, 1);
    EXPECT_TRUE(bus.pendingAcks().isEmpty());
}

TEST(GuidedStrikeWorkflowTest, RequiresCommanderAndGroundHumanConfirmations) {
    MessageBus bus;
    registerUnit(bus, QStringLiteral("red_recon"), QStringLiteral("red"), 0.0);
    registerUnit(bus, QStringLiteral("red_cp"), QStringLiteral("red"), 10.0);
    registerUnit(bus, QStringLiteral("red_guide"), QStringLiteral("red"), 20.0);
    registerUnit(bus, QStringLiteral("red_a1"), QStringLiteral("red"), 30.0);
    GuidedStrikeWorkflow workflow(&bus, QStringLiteral("red"), QStringLiteral("red_cp"));

    QString error;
    ASSERT_TRUE(workflow.reportTarget(
        QStringLiteral("red_recon"), QStringLiteral("blue_t"),
        QJsonObject{{QStringLiteral("x"), 100.0}, {QStringLiteral("y"), 200.0}}, &error))
        << error.toStdString();
    EXPECT_EQ(workflow.stage(), GuidedStrikeWorkflow::Stage::TargetReported);
    EXPECT_FALSE(workflow.confirmGroundAttack(QStringLiteral("red_guide"),
                                               QStringLiteral("red_a1"),
                                               QStringLiteral("blue_t"),
                                               QJsonArray{QJsonObject{{"x", 1}, {"y", 2}}},
                                               &error));
    ASSERT_TRUE(workflow.confirmDispatch(
        QStringLiteral("red_cp"), QStringLiteral("red_a1"), QStringLiteral("blue_t"),
        QJsonArray{QJsonObject{{QStringLiteral("x"), 100.0}, {QStringLiteral("y"), 200.0}}},
        &error)) << error.toStdString();
    ASSERT_TRUE(workflow.commandGroundGuidance(
        QStringLiteral("red_cp"), QStringLiteral("red_guide"), QStringLiteral("red_a1"),
        QStringLiteral("blue_t"), &error)) << error.toStdString();
    ASSERT_TRUE(workflow.confirmGroundAttack(
        QStringLiteral("red_guide"), QStringLiteral("red_a1"), QStringLiteral("blue_t"),
        QJsonArray{QJsonObject{{QStringLiteral("x"), 100.0}, {QStringLiteral("y"), 200.0}}},
        &error)) << error.toStdString();
    EXPECT_EQ(workflow.stage(), GuidedStrikeWorkflow::Stage::Engaging);
}

TEST(GuidedStrikeWorkflowTest, FollowsAuthoritativeMessageStreamExactlyOnce) {
    MessageBus bus;
    registerUnit(bus, QStringLiteral("red_recon"), QStringLiteral("red"), 0.0);
    registerUnit(bus, QStringLiteral("red_cp"), QStringLiteral("red"), 10.0);
    registerUnit(bus, QStringLiteral("red_guide"), QStringLiteral("red"), 20.0);
    registerUnit(bus, QStringLiteral("red_a1"), QStringLiteral("red"), 30.0);
    registerUnit(bus, QStringLiteral("blue_t"), QStringLiteral("blue"), 40.0);
    GuidedStrikeWorkflow workflow(&bus, QStringLiteral("red"), QStringLiteral("red_cp"));
    int transitions = 0;
    QObject::connect(&workflow, &GuidedStrikeWorkflow::workflowEvent,
                     [&](const QJsonObject& event) {
                         if (!event.value(QStringLiteral("rejected")).toBool()) ++transitions;
                     });

    auto post = [&bus](const QString& id, Message::Type type, const QString& sender,
                       const QString& receiver, const QString& targetId) {
        Message message;
        message.id = id;
        message.type = type;
        message.sender = sender;
        message.receiver = receiver;
        message.payload.insert(QStringLiteral("targetId"), targetId);
        message.payload.insert(QStringLiteral("attackerId"), QStringLiteral("red_a1"));
        if (type == Message::Type::StrikePlan || type == Message::Type::FlightPlan
            || type == Message::Type::GroundAttackConfirm) {
            message.payload.insert(QStringLiteral("waypoints"), QJsonArray{
                QJsonObject{{QStringLiteral("x"), 100.0}, {QStringLiteral("y"), 200.0}}});
        }
        ASSERT_TRUE(bus.send(message));
    };

    post(QStringLiteral("m-target"), Message::Type::TargetReport,
         QStringLiteral("red_recon"), QStringLiteral("red_cp"), QStringLiteral("blue_t"));
    EXPECT_EQ(workflow.stage(), GuidedStrikeWorkflow::Stage::TargetReported);
    post(QStringLiteral("m-target"), Message::Type::TargetReport,
         QStringLiteral("red_recon"), QStringLiteral("red_cp"), QStringLiteral("blue_t"));
    EXPECT_EQ(transitions, 1);

    post(QStringLiteral("m-plan"), Message::Type::StrikePlan,
         QStringLiteral("red_cp"), QStringLiteral("red_a1"), QStringLiteral("blue_t"));
    post(QStringLiteral("m-guide"), Message::Type::GroundGuideOrder,
         QStringLiteral("red_cp"), QStringLiteral("red_guide"), QStringLiteral("blue_t"));
    post(QStringLiteral("m-confirm"), Message::Type::GroundAttackConfirm,
         QStringLiteral("red_guide"), QStringLiteral("red_a1"), QStringLiteral("blue_t"));
    EXPECT_EQ(workflow.stage(), GuidedStrikeWorkflow::Stage::Engaging);

    Message malformed;
    malformed.id = QStringLiteral("m-destroy-empty");
    malformed.type = Message::Type::TargetDestroyed;
    malformed.sender = QStringLiteral("red_a1");
    malformed.receiver = QStringLiteral("red_cp");
    ASSERT_TRUE(bus.send(malformed));
    EXPECT_EQ(workflow.stage(), GuidedStrikeWorkflow::Stage::Engaging);

    post(QStringLiteral("m-destroy"), Message::Type::TargetDestroyed,
         QStringLiteral("red_a1"), QStringLiteral("red_cp"), QStringLiteral("blue_t"));
    EXPECT_EQ(workflow.stage(), GuidedStrikeWorkflow::Stage::TargetDestroyed);
    post(QStringLiteral("m-withdraw"), Message::Type::WithdrawOrder,
         QStringLiteral("red_cp"), QStringLiteral("red_a1"), QStringLiteral("blue_t"));
    EXPECT_EQ(workflow.stage(), GuidedStrikeWorkflow::Stage::Withdrawn);

    Message wrongSide;
    wrongSide.id = QStringLiteral("m-blue");
    wrongSide.type = Message::Type::TargetReport;
    wrongSide.sender = QStringLiteral("blue_t");
    wrongSide.receiver = QStringLiteral("red_cp");
    wrongSide.payload.insert(QStringLiteral("targetId"), QStringLiteral("blue_other"));
    ASSERT_TRUE(bus.send(wrongSide));
    EXPECT_EQ(workflow.targetId(), QStringLiteral("blue_t"));
}

TEST(GuidedStrikeWorkflowTest, AcceptsRegisteredReconnaissanceDestructionConfirmation) {
    MessageBus bus;
    registerUnit(bus, QStringLiteral("red_recon"), QStringLiteral("red"), 0.0);
    registerUnit(bus, QStringLiteral("red_other_recon"), QStringLiteral("red"), 5.0);
    registerUnit(bus, QStringLiteral("red_cp"), QStringLiteral("red"), 10.0);
    registerUnit(bus, QStringLiteral("red_guide"), QStringLiteral("red"), 20.0);
    registerUnit(bus, QStringLiteral("red_a1"), QStringLiteral("red"), 30.0);
    GuidedStrikeWorkflow workflow(&bus, QStringLiteral("red"), QStringLiteral("red_cp"));

    auto post = [&bus](const QString& id, Message::Type type, const QString& sender,
                       const QString& receiver, const QString& targetId) {
        Message message;
        message.id = id;
        message.type = type;
        message.sender = sender;
        message.receiver = receiver;
        message.payload.insert(QStringLiteral("targetId"), targetId);
        message.payload.insert(QStringLiteral("attackerId"), QStringLiteral("red_a1"));
        if (type == Message::Type::StrikePlan || type == Message::Type::GroundAttackConfirm) {
            message.payload.insert(QStringLiteral("waypoints"), QJsonArray{
                QJsonObject{{QStringLiteral("x"), 100.0}, {QStringLiteral("y"), 200.0}}});
        }
        EXPECT_TRUE(bus.send(message));
    };

    post(QStringLiteral("recon-report"), Message::Type::TargetReport,
         QStringLiteral("red_recon"), QStringLiteral("red_cp"), QStringLiteral("blue_t"));
    post(QStringLiteral("strike-plan"), Message::Type::StrikePlan,
         QStringLiteral("red_cp"), QStringLiteral("red_a1"), QStringLiteral("blue_t"));
    post(QStringLiteral("ground-guide"), Message::Type::GroundGuideOrder,
         QStringLiteral("red_cp"), QStringLiteral("red_guide"), QStringLiteral("blue_t"));
    post(QStringLiteral("ground-confirm"), Message::Type::GroundAttackConfirm,
         QStringLiteral("red_guide"), QStringLiteral("red_a1"), QStringLiteral("blue_t"));
    ASSERT_EQ(workflow.stage(), GuidedStrikeWorkflow::Stage::Engaging);
    EXPECT_EQ(workflow.reconId(), QStringLiteral("red_recon"));

    post(QStringLiteral("unregistered-recon-confirm"), Message::Type::TargetDestroyed,
         QStringLiteral("red_other_recon"), QStringLiteral("red_cp"), QStringLiteral("blue_t"));
    EXPECT_EQ(workflow.stage(), GuidedStrikeWorkflow::Stage::Engaging);

    Message confirmation;
    confirmation.id = QStringLiteral("registered-recon-confirm");
    confirmation.type = Message::Type::TargetDestroyed;
    confirmation.sender = QStringLiteral("red_recon");
    confirmation.receiver = QStringLiteral("red_cp");
    confirmation.payload = QJsonObject{{QStringLiteral("targetId"), QStringLiteral("blue_t")},
                                       {QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                                       {QStringLiteral("status"), QStringLiteral("destroyed")}};
    ASSERT_TRUE(bus.send(confirmation));
    EXPECT_EQ(workflow.stage(), GuidedStrikeWorkflow::Stage::TargetDestroyed);
}

TEST(GuidedStrikeWorkflowTest, IgnoresOutOfOrderAuthoritativeMessages) {
    MessageBus bus;
    registerUnit(bus, QStringLiteral("red_cp"), QStringLiteral("red"), 0.0);
    registerUnit(bus, QStringLiteral("red_a1"), QStringLiteral("red"), 10.0);
    registerUnit(bus, QStringLiteral("blue_t"), QStringLiteral("blue"), 20.0);
    GuidedStrikeWorkflow workflow(&bus, QStringLiteral("red"), QStringLiteral("red_cp"));

    Message plan;
    plan.id = QStringLiteral("out-of-order-plan");
    plan.type = Message::Type::StrikePlan;
    plan.sender = QStringLiteral("red_cp");
    plan.receiver = QStringLiteral("red_a1");
    plan.payload = QJsonObject{{QStringLiteral("targetId"), QStringLiteral("blue_t")},
                               {QStringLiteral("waypoints"), QJsonArray{
                                   QJsonObject{{QStringLiteral("x"), 1.0},
                                                {QStringLiteral("y"), 2.0}}}}};
    ASSERT_TRUE(bus.send(plan));
    EXPECT_EQ(workflow.stage(), GuidedStrikeWorkflow::Stage::Idle);

    Message destroyed;
    destroyed.id = QStringLiteral("out-of-order-destroy");
    destroyed.type = Message::Type::TargetDestroyed;
    destroyed.sender = QStringLiteral("red_a1");
    destroyed.receiver = QStringLiteral("red_cp");
    destroyed.payload.insert(QStringLiteral("targetId"), QStringLiteral("blue_t"));
    ASSERT_TRUE(bus.send(destroyed));
    EXPECT_EQ(workflow.stage(), GuidedStrikeWorkflow::Stage::Idle);
}

TEST(GuidedStrikeWorkflowTest, ValidatesAuthoritativeStageAndCorrelationBeforePersistence) {
    MessageBus bus;
    registerUnit(bus, QStringLiteral("red_recon"), QStringLiteral("red"), 0.0);
    registerUnit(bus, QStringLiteral("red_cp"), QStringLiteral("red"), 10.0);
    registerUnit(bus, QStringLiteral("red_a1"), QStringLiteral("red"), 20.0);
    GuidedStrikeWorkflow workflow(&bus, QStringLiteral("red"), QStringLiteral("red_cp"));

    Message plan;
    plan.type = Message::Type::StrikePlan;
    plan.sender = QStringLiteral("red_cp");
    plan.receiver = QStringLiteral("red_a1");
    plan.correlationId = QStringLiteral("guided-correlation");
    plan.payload = QJsonObject{
        {QStringLiteral("targetId"), QStringLiteral("blue_target")},
        {QStringLiteral("waypoints"), QJsonArray{
            QJsonObject{{QStringLiteral("x"), 1.0}, {QStringLiteral("y"), 2.0}}}}};
    QString error;
    EXPECT_FALSE(workflow.validateIncomingMessage(plan, &error));
    EXPECT_FALSE(error.isEmpty());

    Message report;
    report.id = QStringLiteral("correlated-report");
    report.type = Message::Type::TargetReport;
    report.sender = QStringLiteral("red_recon");
    report.receiver = QStringLiteral("red_cp");
    report.correlationId = QStringLiteral("guided-correlation");
    report.payload = QJsonObject{{QStringLiteral("targetId"), QStringLiteral("blue_target")}};
    ASSERT_TRUE(bus.send(report));
    EXPECT_TRUE(workflow.validateIncomingMessage(plan, &error)) << error.toStdString();

    plan.correlationId = QStringLiteral("forged-correlation");
    EXPECT_FALSE(workflow.validateIncomingMessage(plan, &error));
    EXPECT_NE(error.indexOf(QStringLiteral("关联")), -1);
}

TEST(GuidedStrikeWorkflowTest, SnapshotRestoreRejectsMalformedStateAndKeepsIdentity) {
    MessageBus bus;
    registerUnit(bus, QStringLiteral("red_recon"), QStringLiteral("red"), 0.0);
    registerUnit(bus, QStringLiteral("red_cp"), QStringLiteral("red"), 10.0);
    GuidedStrikeWorkflow source(&bus, QStringLiteral("red"), QStringLiteral("red_cp"));
    QString error;
    ASSERT_TRUE(source.reportTarget(QStringLiteral("red_recon"), QStringLiteral("blue_t"),
                                    QJsonObject{{QStringLiteral("x"), 1.0}}, &error));
    const QJsonObject snapshot = source.snapshot();
    GuidedStrikeWorkflow restored(nullptr, QStringLiteral("red"), QStringLiteral("red_cp"));
    ASSERT_TRUE(restored.restoreSnapshot(snapshot, &error)) << error.toStdString();
    EXPECT_EQ(restored.stage(), source.stage());
    EXPECT_EQ(restored.targetId(), source.targetId());
    EXPECT_EQ(restored.snapshot().value(QStringLiteral("taskId")),
              snapshot.value(QStringLiteral("taskId")));

    QJsonObject wrongSide = snapshot;
    wrongSide.insert(QStringLiteral("side"), QStringLiteral("blue"));
    EXPECT_FALSE(restored.restoreSnapshot(wrongSide, &error));
    QJsonObject malformed = snapshot;
    malformed.insert(QStringLiteral("stage"), QStringLiteral("not-a-stage"));
    EXPECT_FALSE(restored.restoreSnapshot(malformed, &error));
    malformed = snapshot;
    QJsonArray tooManyEvents;
    for (int index = 0; index < 201; ++index) tooManyEvents.append(QJsonObject{});
    malformed.insert(QStringLiteral("events"), tooManyEvents);
    EXPECT_FALSE(restored.restoreSnapshot(malformed, &error));
}

TEST(MapProviderTest, MercatorRoundTripRecoversLogicalAndGeographicCoordinates) {
    MapProvider map;
    map.setOrigin(GeoCoord{25.4, 119.3});
    const GeoPos logical{1234.5, 678.25, 9.0};
    const GeoCoord coordinate = map.logicalToGeo(logical);
    const GeoPos restored = map.geoToLogical(coordinate);
    EXPECT_NEAR(restored.x, logical.x, 1e-6);
    EXPECT_NEAR(restored.y, logical.y, 1e-6);
    EXPECT_DOUBLE_EQ(restored.alt, 0.0);
}
