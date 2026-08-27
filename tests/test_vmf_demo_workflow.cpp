#include "VmfDemoWorkflow.h"
#include "protocol/Protocol.h"

#include <gtest/gtest.h>

using namespace gbr;

namespace {

QJsonObject demoCommand(const VmfDemoWorkflow& workflow, const QString& action,
                        const QString& actionId) {
    const QJsonObject state = workflow.stateProjection(false);
    return QJsonObject{{QStringLiteral("requestId"), actionId},
                       {QStringLiteral("actionId"), actionId},
                       {QStringLiteral("expectedRevision"),
                        state.value(QStringLiteral("revision"))},
                       {QStringLiteral("seat"), QStringLiteral("red-seat")},
                       {QStringLiteral("action"), action},
                       {QStringLiteral("phase"), state.value(QStringLiteral("phase"))},
                       {QStringLiteral("inputMode"), QStringLiteral("template")},
                       {QStringLiteral("payload"), QJsonObject{}}};
}

QJsonObject demoTrace(const QString& actionId) {
    return QJsonObject{{QStringLiteral("traceId"), QStringLiteral("trace-%1").arg(actionId)},
                       {QStringLiteral("canonicalXml"), QStringLiteral("<Message/>")},
                       {QStringLiteral("decodedXml"), QStringLiteral("<Message/>")},
                       {QStringLiteral("fields"), QJsonArray{}},
                       {QStringLiteral("wireBitLength"), 8},
                       {QStringLiteral("requiresAck"), true}};
}

} // namespace

TEST(VmfDemoWorkflowTest, EnforcesOrderedSeatActionsAndCompletes) {
    VmfDemoWorkflow workflow;
    const QList<VmfDemoWorkflow::ActionSpec> specs = VmfDemoWorkflow::actionSpecs();
    ASSERT_EQ(specs.size(), 10);

    const auto wrongSeat = workflow.applyAction(
        demoCommand(workflow, specs.first().action, QStringLiteral("wrong-seat")),
        QStringLiteral("commander"), demoTrace(QStringLiteral("wrong-seat")), 1.0);
    EXPECT_FALSE(wrongSeat.ok);
    EXPECT_EQ(wrongSeat.code, QStringLiteral("DEMO_ROLE_FORBIDDEN"));

    for (int index = 0; index < specs.size(); ++index) {
        const QString actionId = QStringLiteral("action-%1").arg(index);
        const auto result = workflow.applyAction(
            demoCommand(workflow, specs.at(index).action, actionId),
            specs.at(index).seatType, demoTrace(actionId), index + 2.0);
        ASSERT_TRUE(result.ok) << result.message.toStdString();
        EXPECT_EQ(result.revision, static_cast<quint64>(index + 2));
    }

    const QJsonObject state = workflow.stateProjection(true);
    EXPECT_EQ(state.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
    EXPECT_EQ(state.value(QStringLiteral("traceCount")).toInt(), 10);
    EXPECT_EQ(state.value(QStringLiteral("traces")).toArray().size(), 10);
}

TEST(VmfDemoWorkflowTest, DuplicateActionIsIdempotentAndStateRestores) {
    VmfDemoWorkflow workflow;
    const QJsonObject command = demoCommand(workflow, QStringLiteral("reportTarget"),
                                            QStringLiteral("same-action"));
    ASSERT_TRUE(workflow.applyAction(command, QStringLiteral("recon"),
                                     demoTrace(QStringLiteral("same-action")), 1.0).ok);
    const quint64 revision = static_cast<quint64>(
        workflow.stateProjection(false).value(QStringLiteral("revision")).toInteger());
    const auto duplicate = workflow.applyAction(command, QStringLiteral("recon"),
                                                demoTrace(QStringLiteral("same-action")), 2.0);
    EXPECT_TRUE(duplicate.ok);
    EXPECT_EQ(duplicate.status, QStringLiteral("duplicate"));
    EXPECT_EQ(duplicate.revision, revision);
    const auto wrongActor = workflow.applyAction(command, QStringLiteral("commander"),
                                                 demoTrace(QStringLiteral("same-action")), 2.0);
    EXPECT_FALSE(wrongActor.ok);
    EXPECT_EQ(wrongActor.code, QStringLiteral("ACTION_ID_CONFLICT"));
    QJsonObject conflicting = command;
    conflicting[QStringLiteral("action")] = QStringLiteral("planRoute");
    const auto wrongAction = workflow.applyAction(conflicting, QStringLiteral("recon"),
                                                  demoTrace(QStringLiteral("same-action")), 2.0);
    EXPECT_FALSE(wrongAction.ok);
    EXPECT_EQ(wrongAction.code, QStringLiteral("ACTION_ID_CONFLICT"));
    EXPECT_FALSE(workflow.stateProjection(true)
                     .value(QStringLiteral("traces")).toArray().first().toObject()
                     .value(QStringLiteral("ack")).toObject()
                     .value(QStringLiteral("automatic")).toBool());

    VmfDemoWorkflow restored;
    QString error;
    ASSERT_TRUE(restored.restore(workflow.toJson(), &error)) << error.toStdString();
    EXPECT_EQ(restored.toJson(), workflow.toJson());
}

TEST(VmfDemoWorkflowTest, DirectorControlsPauseJumpResetAndTargetScript) {
    VmfDemoWorkflow workflow;
    EXPECT_TRUE(workflow.applyControl(QStringLiteral("pause"), {}, 1.0).ok);
    EXPECT_EQ(workflow.stateProjection(false).value(QStringLiteral("status")).toString(),
              QStringLiteral("paused"));
    EXPECT_TRUE(workflow.applyControl(
        QStringLiteral("jump"),
        QJsonObject{{QStringLiteral("phase"), QStringLiteral("ground-guidance")}}, 2.0).ok);
    EXPECT_EQ(workflow.stateProjection(false).value(QStringLiteral("phase")).toString(),
              QStringLiteral("ground-guidance"));

    const QJsonObject script{
        {QStringLiteral("version"), 1},
        {QStringLiteral("targets"), QJsonArray{QJsonObject{
             {QStringLiteral("id"), QStringLiteral("blue_fixed_1")},
             {QStringLiteral("initial"), QJsonObject{
                  {QStringLiteral("position"), QJsonObject{
                       {QStringLiteral("lat"), 25.0}, {QStringLiteral("lon"), 119.0}}},
                  {QStringLiteral("visibility"), QStringLiteral("visible")},
                  {QStringLiteral("health"), 100.0},
                  {QStringLiteral("status"), QStringLiteral("active")}}}}}},
        {QStringLiteral("timeline"), QJsonArray{QJsonObject{
             {QStringLiteral("trigger"), QJsonObject{
                  {QStringLiteral("phase"), QStringLiteral("destruction-confirmation")}}},
             {QStringLiteral("targetId"), QStringLiteral("blue_fixed_1")},
             {QStringLiteral("patch"), QJsonObject{
                  {QStringLiteral("health"), 0.0},
                  {QStringLiteral("status"), QStringLiteral("destroyed")}}}}}}};
    const auto configured = workflow.applyControl(
        QStringLiteral("setTargetScript"),
        QJsonObject{{QStringLiteral("script"), script}}, 3.0);
    ASSERT_TRUE(configured.ok) << configured.message.toStdString();
    EXPECT_FALSE(workflow.stateProjection(false)
                     .value(QStringLiteral("targetScriptHash")).toString().isEmpty());
    EXPECT_TRUE(workflow.applyControl(
        QStringLiteral("jump"),
        QJsonObject{{QStringLiteral("phase"),
                     QStringLiteral("destruction-confirmation")}}, 4.0).ok);
    EXPECT_EQ(workflow.stateProjection(false)
                  .value(QStringLiteral("targetState")).toObject()
                  .value(QStringLiteral("blue_fixed_1")).toObject()
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("destroyed"));
    EXPECT_TRUE(workflow.applyControl(
        QStringLiteral("jump"),
        QJsonObject{{QStringLiteral("phase"), QStringLiteral("target-report")}}, 5.0).ok);
    const QJsonObject rewoundTarget = workflow.stateProjection(false)
                                          .value(QStringLiteral("targetState")).toObject()
                                          .value(QStringLiteral("blue_fixed_1")).toObject();
    EXPECT_EQ(rewoundTarget.value(QStringLiteral("status")).toString(),
              QStringLiteral("active"));
    EXPECT_DOUBLE_EQ(rewoundTarget.value(QStringLiteral("health")).toDouble(), 100.0);
    EXPECT_TRUE(workflow.applyControl(QStringLiteral("reset"), {}, 6.0).ok);
    EXPECT_EQ(workflow.stateProjection(false).value(QStringLiteral("phase")).toString(),
              QStringLiteral("target-report"));
}

TEST(VmfDemoWorkflowTest, RejectsInconsistentCheckpointState) {
    VmfDemoWorkflow workflow;
    QJsonObject invalid = workflow.toJson();
    invalid[QStringLiteral("status")] = QStringLiteral("completed");
    QString error;
    EXPECT_FALSE(workflow.restore(invalid, &error));

    invalid = workflow.toJson();
    invalid[QStringLiteral("seenActionIds")] = QJsonArray{QStringLiteral("missing-history")};
    EXPECT_FALSE(workflow.restore(invalid, &error));

    invalid = workflow.toJson();
    invalid[QStringLiteral("targetState")] = QJsonObject{
        {QStringLiteral("unexpected-target"), QJsonObject{{QStringLiteral("health"), 0.0}}}};
    EXPECT_FALSE(workflow.restore(invalid, &error));
}

TEST(VmfDemoProtocolTest, AcceptsV2ActionControlStateAndTrace) {
    VmfDemoWorkflow workflow;
    const QJsonObject action = demoCommand(workflow, QStringLiteral("reportTarget"),
                                           QStringLiteral("demo-request"));
    EXPECT_TRUE(Protocol::validateClientEnvelope(Protocol::makeClientEnvelope(
        QString::fromLatin1(Protocol::DemoActionMessage), QStringLiteral("demo-request"),
        action)).valid);

    const QJsonObject control{{QStringLiteral("requestId"), QStringLiteral("demo-control")},
                              {QStringLiteral("expectedRevision"), 1},
                              {QStringLiteral("action"), QStringLiteral("pause")},
                              {QStringLiteral("payload"), QJsonObject{}}};
    EXPECT_TRUE(Protocol::validateClientEnvelope(Protocol::makeClientEnvelope(
        QString::fromLatin1(Protocol::DemoControlMessage), QStringLiteral("demo-control"),
        control)).valid);

    EXPECT_TRUE(Protocol::validateServerEnvelope(Protocol::makeServerEnvelope(
        QString::fromLatin1(Protocol::DemoStateMessage), 1,
        workflow.stateProjection(false))).valid);
    EXPECT_TRUE(Protocol::validateServerEnvelope(Protocol::makeServerEnvelope(
        QString::fromLatin1(Protocol::DemoTraceMessage), 2,
        QJsonObject{{QStringLiteral("traceId"), QStringLiteral("trace-one")},
                    {QStringLiteral("actionId"), QStringLiteral("action-one")},
                    {QStringLiteral("canonicalXml"), QStringLiteral("<Message/>")},
                    {QStringLiteral("decodedXml"), QStringLiteral("<Message/>")},
                    {QStringLiteral("fields"), QJsonArray{}},
                    {QStringLiteral("wireBitLength"), 8}})).valid);
}
