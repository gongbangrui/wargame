#include <gtest/gtest.h>

#include "RoomPersistence.h"
#include "core/Scenario.h"
#include "units/AttackUAV.h"

#include <QFile>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QDir>
#include <QStringList>
#include <QVector>

#include <optional>

#define private public
#include "GameServer.h"
#undef private

using namespace gbr;

TEST(RoomPersistenceTest, CheckpointRoundTripIsAtomicAndVersioned) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")),
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.runInitialScenario = source.scenario;
    source.runtimeUnits = QJsonArray{QJsonObject{{QStringLiteral("id"),
                                                  QStringLiteral("red_cp")}}};
    source.engineState = QJsonObject{{QStringLiteral("schema"), 3},
                                     {QStringLiteral("projectiles"), QJsonArray{}},
                                     {QStringLiteral("scanContacts"), QJsonArray{}}};
    source.phase = QStringLiteral("running");
    source.running = true;
    source.simTime = 42.5;
    source.speed = 2.0;
    source.scenarioRevision = 9;
    source.stateRevision = 44;
    source.eventSequence = 7;
    source.mapMarks = QJsonArray{QJsonObject{{QStringLiteral("side"), QStringLiteral("red")},
                                             {QStringLiteral("label"), QStringLiteral("接触")}}};
    source.intelLedger = QJsonObject{
        {QStringLiteral("red_commander"),
         QJsonObject{{QStringLiteral("contacts"),
                      QJsonArray{QJsonObject{{QStringLiteral("intelId"),
                                              QStringLiteral("intel-1")},
                                             {QStringLiteral("freshness"),
                                              QStringLiteral("live")}}}}}}};

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    EXPECT_EQ(loaded.sourceSchemaVersion, 6);
    EXPECT_EQ(loaded.phase, source.phase);
    EXPECT_DOUBLE_EQ(loaded.simTime, source.simTime);
    EXPECT_EQ(loaded.scenarioRevision, source.scenarioRevision);
    EXPECT_EQ(loaded.eventSequence, source.eventSequence);
    EXPECT_EQ(loaded.mapMarks, source.mapMarks);
    EXPECT_EQ(loaded.intelLedger, source.intelLedger);
    EXPECT_EQ(loaded.engineState, source.engineState);
    EXPECT_EQ(loaded.scenario.units.size(), source.scenario.units.size());
}

TEST(RoomPersistenceTest, AiStateRoundTripsWithCurrentCheckpointSchema) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString checkpointPath = temporary.filePath(QStringLiteral("checkpoint.json"));
    RoomPersistence persistence(checkpointPath,
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.runInitialScenario = source.scenario;

    AiCheckpointState ai;
    ai.matchGeneration = 7;
    ai.commandSequence = 12;
    ai.planningGeneration = 4;
    ai.rngState = 0xfedcba9876543210ULL;
    ai.aiDifficulty = QStringLiteral("hard");
    ai.providerMode = QStringLiteral("ollama");
    ai.providerModel = QStringLiteral("qwen3:4b");
    ai.nextDecisionAt = 43.5;
    ai.nextReplanAt = 72.0;
    ai.currentPlan = AiPlanV1{};
    ai.currentPlan->requestId = QStringLiteral("ai-plan:7:4");
    ai.currentPlan->matchGeneration = 7;
    ai.currentPlan->sourceStateRevision = 9;
    ai.currentPlan->objectives.append(AiObjectiveV1{
        QStringLiteral("defend"), 100, QStringLiteral("blue_commander"), {}, {}, 72.0});
    ai.consecutiveFailures = 2;
    ai.stickyRules = true;
    ai.effectiveEngine = QStringLiteral("rules");
    ai.lastFailureClass = QStringLiteral("timeout");
    ai.providerRequests = 5;
    ai.providerSuccesses = 2;
    ai.providerFailures = 2;
    ai.lastLatencyMs = 125;
    ai.averageLatencyMs = 90;
    source.aiState = ai;

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    QFile file(checkpointPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    EXPECT_EQ(saved.value(QStringLiteral("checkpointSchemaVersion")).toInt(), 6);
    EXPECT_EQ(saved.value(QStringLiteral("protocolVersion")).toInt(), 6);
    EXPECT_TRUE(saved.value(QStringLiteral("aiState")).isObject());

    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    ASSERT_TRUE(loaded.aiState.has_value());
    EXPECT_EQ(loaded.aiState->matchGeneration, ai.matchGeneration);
    EXPECT_EQ(loaded.aiState->planningGeneration, ai.planningGeneration);
    EXPECT_EQ(loaded.aiState->rngState, ai.rngState);
    EXPECT_EQ(loaded.aiState->aiDifficulty, ai.aiDifficulty);
    EXPECT_EQ(loaded.aiState->providerMode, ai.providerMode);
    EXPECT_EQ(loaded.aiState->providerModel, ai.providerModel);
    ASSERT_TRUE(loaded.aiState->currentPlan.has_value());
    EXPECT_EQ(loaded.aiState->currentPlan->requestId, ai.currentPlan->requestId);
    EXPECT_EQ(loaded.aiState->consecutiveFailures, ai.consecutiveFailures);
    EXPECT_EQ(loaded.aiState->stickyRules, ai.stickyRules);
    EXPECT_EQ(loaded.aiState->providerRequests, ai.providerRequests);
    EXPECT_EQ(loaded.aiState->providerSuccesses, ai.providerSuccesses);
    EXPECT_EQ(loaded.aiState->providerFailures, ai.providerFailures);
    EXPECT_EQ(loaded.aiState->effectiveEngine, ai.effectiveEngine);
}

TEST(RoomPersistenceTest, StrictVmfProfileOptsIntoSchemaSevenAndProtocolEight) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString checkpointPath = temporary.filePath(QStringLiteral("checkpoint.json"));
    RoomPersistence persistence(checkpointPath,
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.runInitialScenario = source.scenario;
    source.protocolProfile = QStringLiteral("vmf-guided-strike-v1");
    source.strictVmfTasks = QJsonObject{{QStringLiteral("schemaVersion"), 1},
                                        {QStringLiteral("tasks"), QJsonArray{}}};

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    QFile file(checkpointPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    EXPECT_EQ(saved.value(QStringLiteral("checkpointSchemaVersion")).toInt(), 7);
    EXPECT_EQ(saved.value(QStringLiteral("protocolVersion")).toInt(), 8);

    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    EXPECT_EQ(loaded.sourceSchemaVersion, 7);
    EXPECT_EQ(loaded.protocolProfile, QStringLiteral("vmf-guided-strike-v1"));
    EXPECT_EQ(loaded.strictVmfTasks, source.strictVmfTasks);
}

TEST(RoomPersistenceTest, DemoV2ProfileOptsIntoSchemaEightAndRestoresWorkflow) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString checkpointPath = temporary.filePath(QStringLiteral("checkpoint.json"));
    RoomPersistence persistence(checkpointPath,
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.runInitialScenario = source.scenario;
    source.protocolProfile = QStringLiteral("vmf-demo-v2");
    VmfDemoWorkflow workflow;
    source.demoState = workflow.toJson();

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    QFile file(checkpointPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    EXPECT_EQ(saved.value(QStringLiteral("checkpointSchemaVersion")).toInt(), 8);
    EXPECT_EQ(saved.value(QStringLiteral("protocolVersion")).toInt(), 8);

    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    EXPECT_EQ(loaded.sourceSchemaVersion, 8);
    EXPECT_EQ(loaded.protocolProfile, QStringLiteral("vmf-demo-v2"));
    EXPECT_EQ(loaded.demoState, source.demoState);

    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonObject mismatched = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QJsonObject room = mismatched.value(QStringLiteral("roomState")).toObject();
    room[QStringLiteral("protocolProfile")] = QStringLiteral("native");
    mismatched[QStringLiteral("roomState")] = room;
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GT(file.write(QJsonDocument(mismatched).toJson()), 0);
    file.close();
    EXPECT_FALSE(persistence.loadCheckpoint(&loaded, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("profile")));
}

TEST(RoomPersistenceTest, SchemaTwoCheckpointLoadsWithMigrationSourceVersion) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString checkpointPath = temporary.filePath(QStringLiteral("checkpoint.json"));
    RoomPersistence persistence(checkpointPath,
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.runInitialScenario = source.scenario;

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    QFile file(checkpointPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonObject legacy = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    legacy[QStringLiteral("checkpointSchemaVersion")] = 2;
    legacy[QStringLiteral("protocolVersion")] = 4;
    legacy.remove(QStringLiteral("engineState"));
    legacy.remove(QStringLiteral("intelLedger"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GT(file.write(QJsonDocument(legacy).toJson()), 0);
    file.close();

    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    EXPECT_EQ(loaded.sourceSchemaVersion, 2);
    EXPECT_TRUE(loaded.engineState.isEmpty());
    EXPECT_TRUE(loaded.intelLedger.isEmpty());
}

TEST(RoomPersistenceTest, SchemaFourCheckpointLoadsWithoutIntelLedger) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString checkpointPath = temporary.filePath(QStringLiteral("checkpoint.json"));
    RoomPersistence persistence(checkpointPath,
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.runInitialScenario = source.scenario;
    source.intelLedger = QJsonObject{{QStringLiteral("index"), QJsonArray{1, 2}}};

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    QFile file(checkpointPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonObject legacy = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    legacy[QStringLiteral("checkpointSchemaVersion")] = 4;
    legacy[QStringLiteral("protocolVersion")] = 4;
    legacy.remove(QStringLiteral("intelLedger"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GT(file.write(QJsonDocument(legacy).toJson()), 0);
    file.close();

    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    EXPECT_EQ(loaded.sourceSchemaVersion, 4);
    EXPECT_TRUE(loaded.intelLedger.isEmpty());
}

TEST(RoomPersistenceTest, SchemaFiveCheckpointKeepsProtocolFiveCompatibility) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString checkpointPath = temporary.filePath(QStringLiteral("checkpoint.json"));
    RoomPersistence persistence(checkpointPath,
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.runInitialScenario = source.scenario;
    source.intelLedger = QJsonObject{{QStringLiteral("index"), QJsonArray{1, 2}}};

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    QFile file(checkpointPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonObject legacy = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    legacy[QStringLiteral("checkpointSchemaVersion")] = 5;
    legacy[QStringLiteral("protocolVersion")] = 5;
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GT(file.write(QJsonDocument(legacy).toJson()), 0);
    file.close();

    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    EXPECT_EQ(loaded.sourceSchemaVersion, 5);
    EXPECT_EQ(loaded.intelLedger, source.intelLedger);
}

TEST(RoomPersistenceTest, MissingProviderModelUsesLegacyCheckpointCompatibilityValue) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString checkpointPath = temporary.filePath(QStringLiteral("checkpoint.json"));
    RoomPersistence persistence(checkpointPath,
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.runInitialScenario = source.scenario;
    source.aiState = AiCheckpointState{};

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    QFile file(checkpointPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QJsonObject ai = saved.value(QStringLiteral("aiState")).toObject();
    EXPECT_EQ(ai.value(QStringLiteral("providerModel")).toString(), QStringLiteral("auto"));
    ai.remove(QStringLiteral("providerModel"));
    saved[QStringLiteral("aiState")] = ai;

    QSaveFile legacyFile(checkpointPath);
    ASSERT_TRUE(legacyFile.open(QIODevice::WriteOnly));
    const QByteArray data = QJsonDocument(saved).toJson();
    ASSERT_EQ(legacyFile.write(data), data.size());
    ASSERT_TRUE(legacyFile.commit());

    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    ASSERT_TRUE(loaded.aiState.has_value());
    EXPECT_EQ(loaded.aiState->providerModel, QStringLiteral("qwen3:4b"));
}

TEST(RoomPersistenceTest, LegacyPvpCheckpointOmitsOptionalAiState) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString checkpointPath = temporary.filePath(QStringLiteral("checkpoint.json"));
    RoomPersistence persistence(checkpointPath,
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.runInitialScenario = source.scenario;

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    QFile file(checkpointPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    EXPECT_FALSE(saved.contains(QStringLiteral("aiState")));

    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    EXPECT_FALSE(loaded.aiState.has_value());
}

TEST(RoomPersistenceTest, RejectsMalformedAiState) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString checkpointPath = temporary.filePath(QStringLiteral("checkpoint.json"));
    RoomPersistence persistence(checkpointPath,
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.runInitialScenario = source.scenario;
    source.aiState = AiCheckpointState{};

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    QFile file(checkpointPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QJsonObject ai = saved.value(QStringLiteral("aiState")).toObject();
    ai[QStringLiteral("rngState")] = QStringLiteral("0");
    saved[QStringLiteral("aiState")] = ai;
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GT(file.write(QJsonDocument(saved).toJson()), 0);
    file.close();

    RoomCheckpoint loaded;
    EXPECT_FALSE(persistence.loadCheckpoint(&loaded, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("AI 检查点")));
}

TEST(RoomPersistenceTest, GameServerRestoresAiStateBeforeReplayWithoutProviderCall) {
    int argc = 1;
    char applicationName[] = "ai_checkpoint_restore_test";
    char* argv[] = {applicationName, nullptr};
    std::optional<QCoreApplication> application;
    if (!QCoreApplication::instance()) application.emplace(argc, argv);

    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QTcpServer providerProbe;
    ASSERT_TRUE(providerProbe.listen(QHostAddress::LocalHost, 0));
    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:1"));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());
    qputenv("AI_PROVIDER", QByteArray("ollama"));
    qputenv("OLLAMA_BASE_URL", QByteArray("http://127.0.0.1:")
                                    + QByteArray::number(providerProbe.serverPort()));
    QJsonObject savedCheckpoint;

    {
        GameServer source;
        ASSERT_TRUE(source.m_recoveryError.isEmpty()) << source.m_recoveryError.toStdString();
        ASSERT_TRUE(source.m_authoritativeRoom.setMode(QStringLiteral("pve")).ok);
        source.m_roomMode = QStringLiteral("pve");
        source.m_aiDifficulty = QStringLiteral("hard");
        source.m_matchGeneration = 9;
        source.m_aiCommandSequence = 17;
        source.m_aiPlanningGeneration = 6;
        source.m_aiRngState = 0xfedcba9876543210ULL;
        source.m_aiNextDecisionAt = 12.5;
        source.m_aiNextReplanAt = 30.0;
        source.m_aiPlan.requestId = QStringLiteral("ai-plan:9:6");
        source.m_aiPlan.matchGeneration = 9;
        source.m_aiPlan.sourceStateRevision = source.m_stateRevision;
        source.m_aiPlan.objectives.append(AiObjectiveV1{
            QStringLiteral("defend"), 100, QStringLiteral("blue_commander"), {}, {}, 30.0});
        source.m_aiConsecutiveFailures = 2;
        source.m_aiStickyRules = true;
        source.m_aiEffectiveEngine = QStringLiteral("rules");
        source.m_aiLastFailureClass = QStringLiteral("timeout");
        source.m_aiProviderRequests = 4;
        source.m_aiProviderSuccesses = 1;
        source.m_aiProviderFailures = 2;
        source.m_aiLastLatencyMs = 111;
        source.m_aiAverageLatencyMs = 88;
        QString error;
        ASSERT_TRUE(source.persistRoomState(&error)) << error.toStdString();
        QFile checkpointFile(temporary.filePath(QStringLiteral("checkpoint.json")));
        ASSERT_TRUE(checkpointFile.open(QIODevice::ReadOnly));
        savedCheckpoint = QJsonDocument::fromJson(checkpointFile.readAll()).object();
        ASSERT_TRUE(source.m_persistence.appendEvent(
            1, QStringLiteral("ready"),
            QJsonObject{{QStringLiteral("role"), QStringLiteral("red")},
                        {QStringLiteral("ready"), true}}, &error)) << error.toStdString();
    }

    {
        GameServer restored;
        ASSERT_TRUE(restored.m_recoveryError.isEmpty()) << restored.m_recoveryError.toStdString();
        EXPECT_EQ(restored.m_matchGeneration, 9);
        EXPECT_EQ(restored.m_aiDifficulty, QStringLiteral("hard"));
        EXPECT_EQ(restored.m_aiCommandSequence, 17);
        EXPECT_EQ(restored.m_aiPlanningGeneration, 6);
        EXPECT_EQ(restored.m_aiRngState, 0xfedcba9876543210ULL);
        EXPECT_TRUE(restored.m_aiPlan.requestId.isEmpty());
        EXPECT_DOUBLE_EQ(restored.m_aiNextReplanAt, 0.0);
        EXPECT_EQ(restored.m_aiConsecutiveFailures, 2);
        EXPECT_TRUE(restored.m_aiStickyRules);
        EXPECT_EQ(restored.m_aiProviderRequests, 4);
        EXPECT_EQ(restored.m_eventSequence, 1);
        EXPECT_TRUE(restored.m_redReady);
        EXPECT_FALSE(restored.m_aiPlanRequestInFlight);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        EXPECT_FALSE(providerProbe.hasPendingConnections());

        const QString evidencePath = qEnvironmentVariable("WARGAME_QA_ARTIFACT").trimmed();
        if (!evidencePath.isEmpty()) {
            ASSERT_TRUE(QDir().mkpath(QFileInfo(evidencePath).absolutePath()));
            QSaveFile evidence(evidencePath);
            ASSERT_TRUE(evidence.open(QIODevice::WriteOnly));
            const QJsonObject observed{
                {QStringLiteral("savedCheckpoint"), savedCheckpoint},
                {QStringLiteral("restoredState"),
                 QJsonObject{{QStringLiteral("matchGeneration"),
                              QString::number(restored.m_matchGeneration)},
                             {QStringLiteral("planningGeneration"),
                              QString::number(restored.m_aiPlanningGeneration)},
                             {QStringLiteral("rngState"),
                              QString::number(restored.m_aiRngState)},
                             {QStringLiteral("currentPlanRequestId"),
                              restored.m_aiPlan.requestId},
                             {QStringLiteral("stalePlanInvalidated"),
                              restored.m_aiPlan.requestId.isEmpty()},
                             {QStringLiteral("consecutiveFailures"),
                              restored.m_aiConsecutiveFailures},
                             {QStringLiteral("stickyRules"), restored.m_aiStickyRules},
                             {QStringLiteral("effectiveEngine"),
                              restored.m_aiEffectiveEngine},
                             {QStringLiteral("providerRequests"),
                              QString::number(restored.m_aiProviderRequests)},
                             {QStringLiteral("eventSequenceAfterReplay"),
                              QString::number(restored.m_eventSequence)},
                             {QStringLiteral("redReadyAfterReplay"), restored.m_redReady},
                             {QStringLiteral("providerConnectionsDuringReplay"),
                              providerProbe.hasPendingConnections() ? 1 : 0}}}};
            const QByteArray data = QJsonDocument(observed).toJson(QJsonDocument::Indented);
            ASSERT_EQ(evidence.write(data), data.size());
            ASSERT_TRUE(evidence.commit());
        }
    }

    qunsetenv("AI_PROVIDER");
    qunsetenv("OLLAMA_BASE_URL");
}

TEST(RoomPersistenceTest, PreparingCheckpointPersistsAnEmptyDeploymentRuntime) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")),
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.scenario.units.clear();
    source.runInitialScenario = ScenarioIo::defaultScenario();
    source.runtimeUnits = {};
    source.phase = QStringLiteral("preparing");
    source.running = false;

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    RoomCheckpoint loaded;

    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    EXPECT_TRUE(loaded.scenario.units.empty());
    EXPECT_TRUE(loaded.runtimeUnits.isEmpty());
    EXPECT_EQ(loaded.phase, QStringLiteral("preparing"));
}

TEST(RoomPersistenceTest, PausedCheckpointRoundTripsAsNotRunning) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")),
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.runInitialScenario = source.scenario;
    source.runtimeUnits = QJsonArray{QJsonObject{{QStringLiteral("id"),
                                                  QStringLiteral("red_cp")}}};
    source.phase = QStringLiteral("paused");
    source.running = false;
    source.simTime = 37.5;

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    EXPECT_EQ(loaded.phase, QStringLiteral("paused"));
    EXPECT_FALSE(loaded.running);
    EXPECT_DOUBLE_EQ(loaded.simTime, 37.5);
}

TEST(RoomPersistenceTest, RejectsPausedCheckpointMarkedRunning) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")),
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    source.runInitialScenario = source.scenario;
    source.runtimeUnits = QJsonArray{QJsonObject{{QStringLiteral("id"),
                                                  QStringLiteral("red_cp")}}};
    source.phase = QStringLiteral("paused");
    source.running = true;

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    RoomCheckpoint loaded;
    EXPECT_FALSE(persistence.loadCheckpoint(&loaded, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("阶段与运行状态冲突")));
}

TEST(RoomPersistenceTest, ReadsOnlyStrictlyOrderedEventsAfterCheckpoint) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")),
                                temporary.filePath(QStringLiteral("events.jsonl")));
    QString error;
    ASSERT_TRUE(persistence.appendEvent(1, QStringLiteral("command"),
                                        QJsonObject{{QStringLiteral("action"),
                                                     QStringLiteral("halt")}}, &error));
    ASSERT_TRUE(persistence.appendEvent(2, QStringLiteral("ready"),
                                        QJsonObject{{QStringLiteral("ready"), true}}, &error));
    const QJsonArray events = persistence.eventsAfter(1, &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events.at(0).toObject().value(QStringLiteral("sequence")).toInteger(), 2);
}

TEST(RoomPersistenceTest, RejectsGapAfterCheckpoint) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")),
                                temporary.filePath(QStringLiteral("events.jsonl")));
    QString error;
    ASSERT_TRUE(persistence.appendEvent(3, QStringLiteral("command"),
                                        QJsonObject{{QStringLiteral("action"),
                                                     QStringLiteral("halt")}}, &error));
    EXPECT_TRUE(persistence.eventsAfter(1, &error).isEmpty());
    EXPECT_TRUE(error.contains(QStringLiteral("不连续")));
}

TEST(RoomPersistenceTest, ReadsRotatedAndCurrentEventLogsAsOneSequence) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString eventPath = temporary.filePath(QStringLiteral("events.jsonl"));
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")), eventPath);
    QString error;
    ASSERT_TRUE(persistence.appendEvent(1, QStringLiteral("ready"),
                                        QJsonObject{{QStringLiteral("ready"), true}}, &error));
    ASSERT_TRUE(QFile::rename(eventPath, eventPath + QStringLiteral(".1")));
    ASSERT_TRUE(persistence.appendEvent(2, QStringLiteral("ready"),
                                        QJsonObject{{QStringLiteral("ready"), false}}, &error));

    const QJsonArray events = persistence.eventsAfter(0, &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(events.size(), 2);
    EXPECT_EQ(events.at(0).toObject().value(QStringLiteral("sequence")).toInteger(), 1);
    EXPECT_EQ(events.at(1).toObject().value(QStringLiteral("sequence")).toInteger(), 2);
}

TEST(RoomPersistenceTest, RejectsCheckpointProtocolMismatch) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString checkpointPath = temporary.filePath(QStringLiteral("checkpoint.json"));
    RoomPersistence persistence(checkpointPath,
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error));

    QFile file(checkpointPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    object[QStringLiteral("protocolVersion")] = 999;
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GT(file.write(QJsonDocument(object).toJson()), 0);
    file.close();

    RoomCheckpoint loaded;
    EXPECT_FALSE(persistence.loadCheckpoint(&loaded, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("协议版本")));
}

TEST(RoomPersistenceTest, RotatesThroughThreeGenerationsAndRecoversContiguousTail) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    qputenv("WARGAME_EVENT_LOG_MAX_BYTES", QByteArray("512"));
    const QString eventPath = temporary.filePath(QStringLiteral("events.jsonl"));
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")), eventPath,
                                temporary.path());
    const QString payload(180, QLatin1Char('x'));
    QString error;
    for (quint64 sequence = 1; sequence <= 8; ++sequence) {
        ASSERT_TRUE(persistence.appendEvent(sequence, QStringLiteral("command"),
                                             QJsonObject{{QStringLiteral("payload"), payload}},
                                             &error)) << error.toStdString();
    }
    RoomCheckpoint checkpoint;
    checkpoint.scenario = ScenarioIo::defaultScenario();
    checkpoint.runInitialScenario = checkpoint.scenario;
    checkpoint.eventSequence = 8;
    ASSERT_TRUE(persistence.saveCheckpoint(checkpoint, &error)) << error.toStdString();
    ASSERT_TRUE(persistence.appendEvent(9, QStringLiteral("command"),
                                        QJsonObject{{QStringLiteral("payload"), payload}},
                                        &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(eventPath));
    EXPECT_TRUE(QFileInfo::exists(eventPath + QStringLiteral(".1")));
    EXPECT_TRUE(QFileInfo::exists(eventPath + QStringLiteral(".2")));
    EXPECT_TRUE(QFileInfo::exists(eventPath + QStringLiteral(".3")));
    const QJsonArray tail = persistence.eventsAfter(8, &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(tail.size(), 1);
    EXPECT_EQ(tail.last().toObject().value(QStringLiteral("sequence")).toInteger(), 9);
    qunsetenv("WARGAME_EVENT_LOG_MAX_BYTES");
}

TEST(RoomPersistenceTest, RotationRejectsNonRegularGenerationWithoutDeletingActiveLog) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    qputenv("WARGAME_EVENT_LOG_MAX_BYTES", QByteArray("512"));
    const QString eventPath = temporary.filePath(QStringLiteral("events.jsonl"));
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")), eventPath,
                                temporary.path());
    QString error;
    ASSERT_TRUE(persistence.appendEvent(1, QStringLiteral("ready"), QJsonObject{}, &error));
    ASSERT_TRUE(QDir().mkpath(eventPath + QStringLiteral(".3")));
    EXPECT_FALSE(persistence.appendEvent(2, QStringLiteral("ready"), QJsonObject{}, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(QFileInfo::exists(eventPath));
    EXPECT_FALSE(QFileInfo(eventPath).isDir());
    EXPECT_TRUE(QDir(eventPath + QStringLiteral(".3")).removeRecursively());
    EXPECT_EQ(persistence.eventsAfter(0, &error).size(), 1);
    qunsetenv("WARGAME_EVENT_LOG_MAX_BYTES");
}

TEST(RoomPersistenceTest, MidInstallRotationFailureRestoresEveryPriorGeneration) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    qputenv("WARGAME_EVENT_LOG_MAX_BYTES", QByteArray("512"));
    const QString eventPath = temporary.filePath(QStringLiteral("events.jsonl"));
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")), eventPath,
                                temporary.path());
    const QString payload(220, QLatin1Char('x'));
    const auto eventLine = [&payload](quint64 sequence) {
        return QJsonDocument(QJsonObject{
                                  {QStringLiteral("eventSchemaVersion"), 1},
                                  {QStringLiteral("sequence"), static_cast<qint64>(sequence)},
                                  {QStringLiteral("kind"), QStringLiteral("ready")},
                                  {QStringLiteral("payload"),
                                   QJsonObject{{QStringLiteral("value"), payload}}}})
                   .toJson(QJsonDocument::Compact)
               + '\n';
    };
    const QStringList paths{eventPath, eventPath + QStringLiteral(".1"),
                            eventPath + QStringLiteral(".2"), eventPath + QStringLiteral(".3")};
    QVector<QByteArray> priorContents;
    for (int generation = 0; generation < paths.size(); ++generation) {
        QFile file(paths.at(generation));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QByteArray content;
        const quint64 firstSequence = static_cast<quint64>((paths.size() - generation - 1) * 3 + 1);
        for (quint64 offset = 0; offset < 3; ++offset) content += eventLine(firstSequence + offset);
        ASSERT_EQ(file.write(content), content.size());
        file.close();
        priorContents.append(content);
    }

    RoomCheckpoint checkpoint;
    checkpoint.scenario = ScenarioIo::defaultScenario();
    checkpoint.runInitialScenario = checkpoint.scenario;
    checkpoint.eventSequence = 3;
    QString checkpointError;
    ASSERT_TRUE(persistence.saveCheckpoint(checkpoint, &checkpointError))
        << checkpointError.toStdString();

    QString error;
    qputenv("WARGAME_FORCE_ROTATION_INSTALL_RENAME_FAILURE", QByteArray("1"));
    EXPECT_FALSE(persistence.appendEvent(13, QStringLiteral("ready"), QJsonObject{}, &error));
    qunsetenv("WARGAME_FORCE_ROTATION_INSTALL_RENAME_FAILURE");
    EXPECT_FALSE(error.isEmpty());

    for (int index = 0; index < paths.size(); ++index) {
        QFile file(paths.at(index));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(file.readAll(), priorContents.at(index));
    }

    const QJsonArray events = persistence.eventsAfter(0, &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(events.size(), 12);
    for (int index = 0; index < events.size(); ++index) {
        EXPECT_EQ(events.at(index).toObject().value(QStringLiteral("sequence")).toInteger(),
                  index + 1);
    }
    qunsetenv("WARGAME_EVENT_LOG_MAX_BYTES");
}

TEST(RoomPersistenceTest, RejectsPathsOutsideDataRootAndSymlinkEscapes) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    QString error;
    RoomPersistence traversal(temporary.filePath(QStringLiteral("../outside/checkpoint.json")),
                              temporary.filePath(QStringLiteral("events.jsonl")),
                              temporary.path());
    EXPECT_FALSE(traversal.configurationError().isEmpty());

    const QString symlink = temporary.filePath(QStringLiteral("checkpoint-link.json"));
    if (QFile::link(outside.filePath(QStringLiteral("checkpoint.json")), symlink)) {
        RoomPersistence escaped(symlink, temporary.filePath(QStringLiteral("events.jsonl")),
                                temporary.path());
        EXPECT_FALSE(escaped.configurationError().isEmpty());
        RoomCheckpoint checkpoint;
        EXPECT_FALSE(escaped.saveCheckpoint(checkpoint, &error));
        EXPECT_FALSE(QFileInfo::exists(outside.filePath(QStringLiteral("checkpoint.json"))));
    }
}

TEST(RoomPersistenceTest, StrictSeatProjectionIsRedClaimableAndAdminEditable) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:1"));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    server.m_protocolProfile = QStringLiteral("vmf-guided-strike-v1");
    QString error;
    ASSERT_TRUE(server.applyProtocolProfilePolicy(&error)) << error.toStdString();

    const QJsonObject state = server.roomState();
    EXPECT_EQ(state.value(QStringLiteral("operationMode")).toString(),
              QStringLiteral("vmf-single-side"));
    EXPECT_EQ(state.value(QStringLiteral("participantSide")).toString(),
              QStringLiteral("red"));
    EXPECT_EQ(state.value(QStringLiteral("fixedTargetSide")).toString(),
              QStringLiteral("blue"));
    EXPECT_TRUE(state.value(QStringLiteral("scenarioEditable")).toBool());
    EXPECT_FALSE(server.m_authoritativeRoom.readiness()
                     .value(QStringLiteral("ready")).toBool());

    QJsonObject redCommander;
    QJsonObject blueCommander;
    for (const QJsonValue& value : state.value(QStringLiteral("seats")).toArray()) {
        const QJsonObject seat = value.toObject();
        if (seat.value(QStringLiteral("seatId")).toString()
            == QLatin1String("red_commander")) {
            redCommander = seat;
        } else if (seat.value(QStringLiteral("seatId")).toString()
                   == QLatin1String("blue_commander")) {
            blueCommander = seat;
        }
    }
    ASSERT_FALSE(redCommander.isEmpty());
    EXPECT_FALSE(redCommander.value(QStringLiteral("occupied")).toBool());
    EXPECT_TRUE(redCommander.value(QStringLiteral("claimable")).toBool());
    ASSERT_FALSE(blueCommander.isEmpty());
    EXPECT_TRUE(blueCommander.value(QStringLiteral("occupied")).toBool());
    EXPECT_FALSE(blueCommander.value(QStringLiteral("claimable")).toBool());
    EXPECT_EQ(blueCommander.value(QStringLiteral("controlMode")).toString(),
              QStringLiteral("fixed-target"));
    EXPECT_EQ(blueCommander.value(QStringLiteral("sourceUnitId")).toString(),
              QStringLiteral("blue_cp"));

    Scenario withNewUnit = server.m_runInitialScenario;
    const auto attack = std::find_if(withNewUnit.units.cbegin(), withNewUnit.units.cend(),
                                     [](const ScenarioUnit& unit) {
                                         return unit.side == QLatin1String("red")
                                             && unit.kind == QLatin1String("attackuav");
                                     });
    ASSERT_NE(attack, withNewUnit.units.cend());
    ScenarioUnit newUnit = *attack;
    newUnit.id = QStringLiteral("red_attack_vmf_added");
    newUnit.callsign = QStringLiteral("新增攻击机");
    withNewUnit.units.push_back(newUnit);
    ASSERT_TRUE(server.replaceInitialScenario(withNewUnit, &error)) << error.toStdString();

    std::erase_if(withNewUnit.units, [](const ScenarioUnit& unit) {
        return unit.id == QLatin1String("red_attack_vmf_added");
    });
    ASSERT_TRUE(server.replaceInitialScenario(withNewUnit, &error)) << error.toStdString();

    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        1, QStringLiteral("red commander"), QStringLiteral("red_commander"),
        QStringLiteral("commandpost")).ok);
    server.syncAuthoritativeSeats();
    EXPECT_TRUE(server.roomState().value(QStringLiteral("scenarioEditable")).toBool());
    Scenario edited = server.m_runInitialScenario;
    ASSERT_FALSE(edited.units.empty());
    edited.units.front().callsign += QStringLiteral("-管理员调整");
    ASSERT_TRUE(server.replaceInitialScenario(edited, &error)) << error.toStdString();
    EXPECT_EQ(server.m_authoritativeRoom.claimSeat(
                  2, QStringLiteral("blue user"), QStringLiteral("blue_commander"),
                  QStringLiteral("commandpost")).code,
              QStringLiteral("SIDE_RESERVED_FOR_FIXED_TARGET"));
}

TEST(RoomPersistenceTest, DemoProfileUsesV2CodecIdentityAndSingleSideProjection) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:1"));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    server.m_protocolProfile = QStringLiteral("vmf-demo-v2");
    QString error;
    ASSERT_TRUE(server.applyProtocolProfilePolicy(&error)) << error.toStdString();

    const Scenario& scenario = server.m_engine.scenario();
    EXPECT_EQ(scenario.communicationPolicy.format, QStringLiteral("vmf-design-v1"));
    EXPECT_EQ(scenario.communicationPolicy.vmfProfile, QStringLiteral("vmf-demo-v2"));
    EXPECT_TRUE(scenario.communicationPolicy.automaticAck);

    const QJsonObject state = server.roomState();
    EXPECT_EQ(state.value(QStringLiteral("operationMode")).toString(),
              QStringLiteral("vmf-single-side"));
    EXPECT_EQ(state.value(QStringLiteral("participantSide")).toString(),
              QStringLiteral("red"));
    EXPECT_EQ(state.value(QStringLiteral("fixedTargetSide")).toString(),
              QStringLiteral("blue"));
    EXPECT_EQ(state.value(QStringLiteral("demoState")).toObject()
                  .value(QStringLiteral("profile")).toString(),
              QStringLiteral("vmf-demo-v2"));
}

TEST(RoomPersistenceTest, DemoProfileEncodesAllTenActionsThroughServer) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:1"));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    server.m_protocolProfile = QStringLiteral("vmf-demo-v2");
    QString error;
    ASSERT_TRUE(server.applyProtocolProfilePolicy(&error)) << error.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        1, QStringLiteral("red commander"), QStringLiteral("red_commander"),
        QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.setReady(1, true).ok);
    ASSERT_TRUE(server.applyDeployedScenario(&error)) << error.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.start().ok);
    server.m_phase = QStringLiteral("running");
    server.m_roomStatus = QStringLiteral("running");
    server.m_engine.setRunning(true);

    const QList<VmfDemoWorkflow::ActionSpec> specs = VmfDemoWorkflow::actionSpecs();
    ASSERT_EQ(specs.size(), 10);
    const QJsonObject initialState = server.m_demoWorkflow.stateProjection(false);
    const QJsonObject invalidTargetCommand{
        {QStringLiteral("requestId"), QStringLiteral("invalid-target-request")},
        {QStringLiteral("actionId"), QStringLiteral("invalid-target-action")},
        {QStringLiteral("expectedRevision"), initialState.value(QStringLiteral("revision"))},
        {QStringLiteral("seat"), QStringLiteral("red_recon_1")},
        {QStringLiteral("action"), QStringLiteral("reportTarget")},
        {QStringLiteral("phase"), initialState.value(QStringLiteral("phase"))},
        {QStringLiteral("inputMode"), QStringLiteral("template")},
        {QStringLiteral("payload"), QJsonObject{
             {QStringLiteral("targetId"), QStringLiteral("missing-blue-target")}}}};
    QJsonObject rejectedResult;
    QJsonObject rejectedTrace;
    const quint64 initialEventSequence = server.m_eventSequence;
    EXPECT_FALSE(server.executeDemoAction(invalidTargetCommand, QStringLiteral("recon"),
                                          QStringLiteral("red_recon_1"), false,
                                          &rejectedResult, &rejectedTrace, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("有效的蓝方固定靶")));
    EXPECT_EQ(server.m_eventSequence, initialEventSequence);

    for (int index = 0; index < specs.size(); ++index) {
        const VmfDemoWorkflow::ActionSpec& spec = specs.at(index);
        const QString seatId = spec.seatType == QLatin1String("commander")
            ? QStringLiteral("red_commander")
            : QStringLiteral("red_%1_1").arg(spec.seatType);
        const QJsonObject state = server.m_demoWorkflow.stateProjection(false);
        const QString actionId = QStringLiteral("server-action-%1").arg(index);
        const QJsonObject command{
            {QStringLiteral("requestId"), actionId},
            {QStringLiteral("actionId"), actionId},
            {QStringLiteral("expectedRevision"), state.value(QStringLiteral("revision"))},
            {QStringLiteral("seat"), seatId},
            {QStringLiteral("action"), spec.action},
            {QStringLiteral("phase"), state.value(QStringLiteral("phase"))},
            {QStringLiteral("inputMode"), QStringLiteral("template")},
            {QStringLiteral("payload"), QJsonObject{}}};
        QJsonObject result;
        QJsonObject trace;
        const bool automatic = index != 0;
        ASSERT_TRUE(server.executeDemoAction(command, spec.seatType, seatId, automatic,
                                             &result, &trace, &error))
            << spec.action.toStdString() << ": " << error.toStdString();
        EXPECT_TRUE(trace.value(QStringLiteral("roundTripEqual")).toBool());
        EXPECT_FALSE(trace.value(QStringLiteral("canonicalXml")).toString().isEmpty());
        EXPECT_GT(trace.value(QStringLiteral("wireBitLength")).toInt(), 0);
        if (index == 0) {
            const quint64 eventSequence = server.m_eventSequence;
            const QJsonObject workflowBefore = server.m_demoWorkflow.toJson();
            QJsonObject duplicateCommand = command;
            duplicateCommand[QStringLiteral("requestId")] = QStringLiteral("retry-request");
            duplicateCommand[QStringLiteral("expectedRevision")] =
                server.m_demoWorkflow.stateProjection(false)
                    .value(QStringLiteral("revision"));
            QJsonObject duplicateResult;
            QJsonObject duplicateTrace;
            ASSERT_TRUE(server.executeDemoAction(
                duplicateCommand, spec.seatType, seatId, false,
                &duplicateResult, &duplicateTrace, &error)) << error.toStdString();
            EXPECT_EQ(duplicateResult.value(QStringLiteral("status")).toString(),
                      QStringLiteral("duplicate"));
            EXPECT_TRUE(duplicateTrace.isEmpty());
            EXPECT_EQ(server.m_eventSequence, eventSequence);
            EXPECT_EQ(server.m_demoWorkflow.toJson(), workflowBefore);
            EXPECT_EQ(server.m_commandResults
                          .value(QStringLiteral("demo:0:retry-request"))
                          .value(QStringLiteral("commandId")).toString(),
                      QStringLiteral("retry-request"));
        }
    }

    EXPECT_EQ(server.m_demoWorkflow.stateProjection(false)
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("completed"));
    ASSERT_TRUE(server.persistRoomState(&error)) << error.toStdString();

    QFile checkpointFile(temporary.filePath(QStringLiteral("checkpoint.json")));
    ASSERT_TRUE(checkpointFile.open(QIODevice::ReadOnly));
    QJsonObject checkpoint = QJsonDocument::fromJson(checkpointFile.readAll()).object();
    checkpointFile.close();
    QJsonArray history = checkpoint.value(QStringLiteral("commandHistory")).toArray();
    ASSERT_FALSE(history.isEmpty());
    for (QJsonValueRef value : history) {
        QJsonObject entry = value.toObject();
        QJsonObject cached = entry.value(QStringLiteral("result")).toObject();
        EXPECT_FALSE(cached.value(QStringLiteral("commandId")).toString().isEmpty());
        cached.remove(QStringLiteral("commandId"));
        entry[QStringLiteral("result")] = cached;
        value = entry;
    }
    checkpoint[QStringLiteral("commandHistory")] = history;
    ASSERT_TRUE(checkpointFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GT(checkpointFile.write(QJsonDocument(checkpoint).toJson()), 0);
    checkpointFile.close();
    ASSERT_TRUE(server.restoreRoomState(&error)) << error.toStdString();
    EXPECT_FALSE(server.m_commandResults.isEmpty());
    for (const QJsonObject& cached : std::as_const(server.m_commandResults)) {
        EXPECT_FALSE(cached.value(QStringLiteral("commandId")).toString().isEmpty());
    }
}

TEST(RoomPersistenceTest, StrictSeatedSnapshotRetainsOwnedUnitWhenRuntimeRosterIsEmpty) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:1"));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    server.m_protocolProfile = QStringLiteral("vmf-guided-strike-v1");
    QString error;
    ASSERT_TRUE(server.applyProtocolProfilePolicy(&error)) << error.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        11, QStringLiteral("red commander"), QStringLiteral("red_commander"),
        QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        12, QStringLiteral("red attack"), QStringLiteral("red_attack_1"),
        QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(server.applyDeployedScenario(&error)) << error.toStdString();
    server.syncAuthoritativeSeats();

    QWebSocket socket;
    auto& session = server.m_clients[&socket];
    session.authenticated = true;
    session.accountRole = QStringLiteral("player");
    session.roomId = server.m_roomId;
    session.seatId = QStringLiteral("red_attack_1");
    session.seatType = QStringLiteral("attack");
    session.side = QStringLiteral("red");

    // Simulate a recovery/deployment refresh that has not rebuilt the runtime
    // roster yet, while the authoritative seat and initial scenario remain.
    server.m_authoritativeRoom.clearDeployment();
    const QJsonObject snapshot = server.snapshotFor(session);
    const QJsonArray projected = snapshot.value(QStringLiteral("units")).toArray();
    const QString ownedId = server.m_authoritativeRoom.seat(session.seatId).unitId;
    EXPECT_FALSE(ownedId.isEmpty());
    EXPECT_TRUE(std::any_of(projected.cbegin(), projected.cend(),
                            [&ownedId](const QJsonValue& value) {
                                return value.toObject().value(QStringLiteral("id")).toString()
                                    == ownedId;
                            }));
}

TEST(RoomPersistenceTest, StrictPlaceholderTakeoverPreservesRuntimeAndResetClearsTasks) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:1"));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    QWebSocket socket;
    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    QString error;
    server.m_protocolProfile = QStringLiteral("vmf-guided-strike-v1");
    ASSERT_TRUE(server.applyProtocolProfilePolicy(&error)) << error.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        1, QStringLiteral("red commander"), QStringLiteral("red_commander"),
        QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.setReady(1, true).ok);
    ASSERT_TRUE(server.applyDeployedScenario(&error)) << error.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.start().ok);
    server.m_phase = QStringLiteral("running");
    server.m_roomStatus = QStringLiteral("running");
    server.m_engine.setRunning(true);
    server.syncAuthoritativeSeats();

    const QString seatId = QStringLiteral("red_attack_1");
    const QString unitId = server.m_authoritativeRoom.seat(seatId).unitId;
    UnitBase* runtimeUnit = server.m_engine.unit(unitId);
    ASSERT_NE(runtimeUnit, nullptr);
    runtimeUnit->setHp(41.0);

    GameServer::ClientSession session;
    session.authenticated = true;
    session.userId = 3;
    session.username = QStringLiteral("attack human");
    session.displayName = session.username;
    session.roomId = server.m_roomId;
    session.accountRole = QStringLiteral("player");
    session.role = session.accountRole;
    server.m_clients.insert(&socket, session);
    server.handleClaimSeat(&socket, QJsonObject{{QStringLiteral("seatId"), seatId}});

    EXPECT_EQ(server.m_clients.value(&socket).seatId, seatId);
    EXPECT_EQ(server.m_authoritativeRoom.seat(seatId).controllerType,
              QStringLiteral("human"));
    EXPECT_EQ(server.m_authoritativeRoom.seat(seatId).unitId, unitId);
    ASSERT_NE(server.m_engine.unit(unitId), nullptr);
    EXPECT_DOUBLE_EQ(server.m_engine.unit(unitId)->hp(), 41.0);

    const AuthoritativeRoom::Seat commander =
        server.m_authoritativeRoom.seat(QStringLiteral("red_commander"));
    ASSERT_NE(server.m_engine.unit(commander.unitId), nullptr);
    server.m_engine.unit(commander.unitId)->setDetectRange(1'000'000.0);
    server.m_sharedIntel[QStringLiteral("red_commander")].insert(
        QStringLiteral("blue_cp"));
    server.m_sharedIntel[QStringLiteral("red_recon_1")].insert(
        QStringLiteral("blue_cp"));
    GameServer::ClientSession& commanderSession = server.m_clients[&socket];
    commanderSession.userId = 1;
    commanderSession.username = QStringLiteral("red commander");
    commanderSession.displayName = commanderSession.username;
    commanderSession.seatId = QStringLiteral("red_commander");
    commanderSession.seatType = QStringLiteral("commander");
    commanderSession.side = QStringLiteral("red");
    ASSERT_EQ(server.m_phase, QStringLiteral("running"));
    ASSERT_TRUE(server.m_engine.running());
    ASSERT_NE(server.m_engine.unit(QStringLiteral("blue_cp")), nullptr);
    ASSERT_EQ(server.m_engine.unit(QStringLiteral("blue_cp"))->kind(),
              UnitKind::CommandPost);
    ASSERT_TRUE(server.visibleUnitIds(commanderSession).contains(
        QStringLiteral("blue_cp")));
    for (const QString& binding : {QStringLiteral("red_commander"),
                                   QStringLiteral("red_recon_1"), seatId,
                                   QStringLiteral("red_ground_1")}) {
        ASSERT_TRUE(server.m_authoritativeRoom.hasSeat(binding));
        ASSERT_NE(server.m_engine.unit(server.m_authoritativeRoom.seat(binding).unitId),
                  nullptr);
    }
    const QString requestId = QStringLiteral("strict-create-reset");
    server.handleVmfTaskCommand(
        &socket,
        QJsonObject{{QStringLiteral("requestId"), requestId},
                    {QStringLiteral("taskId"), QStringLiteral("red-task-reset")},
                    {QStringLiteral("expectedTaskRevision"), 0},
                    {QStringLiteral("action"), QStringLiteral("createTask")},
                    {QStringLiteral("messages"), QJsonArray{}},
                    {QStringLiteral("commanderSeatId"), QStringLiteral("red_commander")},
                    {QStringLiteral("reconSeatId"), QStringLiteral("red_recon_1")},
                    {QStringLiteral("attackSeatId"), seatId},
                    {QStringLiteral("groundSeatId"), QStringLiteral("red_ground_1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_cp")},
                    {QStringLiteral("correlationId"), QStringLiteral("corr-reset")}},
        requestId);
    const StrictVmfTaskSet::Task* created =
        server.m_strictVmfTasks.task(QStringLiteral("red-task-reset"));
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(created->health, QStringLiteral("active"));
    EXPECT_EQ(created->stage, QStringLiteral("awaitingTargetReport"));
    ASSERT_EQ(created->route.size(), 1);
    const QJsonObject routePoint = created->route.first().toObject();
    const auto* attackUnit = qobject_cast<const AttackUAV*>(
        server.m_engine.unit(unitId));
    const UnitBase* routeTarget = server.m_engine.unit(QStringLiteral("blue_cp"));
    ASSERT_NE(attackUnit, nullptr);
    ASSERT_NE(routeTarget, nullptr);
    const GeoPos approach{routePoint.value(QStringLiteral("x")).toDouble(),
                          routePoint.value(QStringLiteral("y")).toDouble(),
                          attackUnit->pos().alt};
    const double approachDistance = approach.distanceTo2D(routeTarget->pos());
    EXPECT_GT(approachDistance, attackUnit->minimumAttackRange());
    EXPECT_LT(approachDistance, attackUnit->attackRange());

    UnitBase* automationActor = server.m_engine.unit(
        server.m_authoritativeRoom.seat(QStringLiteral("red_recon_1")).unitId);
    ASSERT_NE(automationActor, nullptr);
    ASSERT_EQ(server.m_authoritativeRoom.seat(QStringLiteral("red_recon_1")).controlMode,
              QStringLiteral("vmf-auto"));
    ASSERT_TRUE(server.m_engine.running());
    automationActor->setHp(0.0);
    server.runStrictVmfAutomation();
    ASSERT_EQ(server.m_vmfAutomationFailureCount.size(), 1);
    const QString failedAttempt = server.m_vmfAutomationFailureCount.constBegin().key();
    EXPECT_EQ(server.m_vmfAutomationFailureCount.value(failedAttempt), 1);
    const double retryAt = server.m_vmfAutomationRetryAfter.value(failedAttempt);
    EXPECT_GT(retryAt, server.m_engine.simTime());
    server.runStrictVmfAutomation();
    EXPECT_EQ(server.m_vmfAutomationFailureCount.value(failedAttempt), 1);

    automationActor->setHp(automationActor->maxHp());
    const double beforeStep = server.m_engine.simTime();
    server.m_engine.setRunning(false);
    server.m_engine.clock()->advance(0.6);
    server.m_engine.setRunning(true);
    EXPECT_GT(server.m_engine.simTime(), beforeStep);
    EXPECT_TRUE(server.m_engine.running());
    EXPECT_EQ(server.m_phase, QStringLiteral("running"));
    EXPECT_FALSE(server.m_replayingDurableEvents);
    server.runStrictVmfAutomation();
    created = server.m_strictVmfTasks.task(QStringLiteral("red-task-reset"));
    ASSERT_NE(created, nullptr);
    ASSERT_EQ(created->stage, QStringLiteral("targetReported"));
    const quint64 humanStageRevision = created->taskRevision;
    server.runStrictVmfAutomation();
    EXPECT_EQ(server.m_strictVmfTasks.task(QStringLiteral("red-task-reset"))
                  ->taskRevision,
              humanStageRevision);

    ASSERT_TRUE(server.m_authoritativeRoom.disconnect(1).ok);
    server.syncAuthoritativeSeats();
    EXPECT_EQ(server.m_authoritativeRoom.seat(QStringLiteral("red_commander")).controlMode,
              QStringLiteral("vmf-auto"));
    const GameServer::RoomStateBackup beforeDispatch = server.captureRoomState();
    const quint64 eventBeforeDispatch = server.m_eventSequence;
    server.runStrictVmfAutomation();
    created = server.m_strictVmfTasks.task(QStringLiteral("red-task-reset"));
    ASSERT_NE(created, nullptr);
    ASSERT_EQ(created->stage, QStringLiteral("dispatchPending"));
    ASSERT_TRUE(server.m_engine.unit(unitId)->hasActiveWaypoints());
    const GameServer::RoomStateBackup afterDispatch = server.captureRoomState();

    const QJsonArray events = server.m_persistence.eventsAfter(eventBeforeDispatch, &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(events.size(), 1);
    const QJsonObject dispatchEvent = events.first().toObject();
    EXPECT_EQ(dispatchEvent.value(QStringLiteral("kind")).toString(),
              QStringLiteral("vmfTaskCommand"));
    const QJsonObject dispatchPayload =
        dispatchEvent.value(QStringLiteral("payload")).toObject();
    EXPECT_EQ(dispatchPayload.value(QStringLiteral("generatedBy")).toString(),
              QStringLiteral("vmf-auto"));
    EXPECT_EQ(dispatchPayload.value(QStringLiteral("engineAction")).toString(),
              QStringLiteral("setFlightPlan"));

    ASSERT_TRUE(server.restoreRoomStateBackup(beforeDispatch, &error))
        << error.toStdString();
    ASSERT_FALSE(server.m_engine.unit(unitId)->hasActiveWaypoints());
    ASSERT_TRUE(server.applyDurableEvent(QStringLiteral("vmfTaskCommand"),
                                         dispatchPayload, &error))
        << error.toStdString();
    EXPECT_EQ(server.m_strictVmfTasks.task(QStringLiteral("red-task-reset"))->stage,
              QStringLiteral("dispatchPending"));
    EXPECT_TRUE(server.m_engine.unit(unitId)->hasActiveWaypoints());
    ASSERT_TRUE(server.restoreRoomStateBackup(afterDispatch, &error))
        << error.toStdString();

    ASSERT_TRUE(server.resetAuthoritativeRuntime(QStringLiteral("test-reset"), &error))
        << error.toStdString();
    EXPECT_TRUE(server.m_strictVmfTasks.tasks().isEmpty());
    EXPECT_TRUE(server.m_vmfAutomationRetryAfter.isEmpty());
    EXPECT_TRUE(server.m_vmfAutomationFailureCount.isEmpty());
    EXPECT_EQ(server.m_authoritativeRoom.seat(QStringLiteral("red_recon_1")).controllerType,
              QStringLiteral("placeholder"));
}

TEST(RoomPersistenceTest, ReportsInjectedDurabilityFailure) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")),
                                temporary.filePath(QStringLiteral("events.jsonl")),
                                temporary.path());
    qputenv("WARGAME_FORCE_FLUSH_FAILURE", QByteArray("1"));
    QString error;
    EXPECT_FALSE(persistence.appendEvent(1, QStringLiteral("ready"), QJsonObject{}, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("同步到磁盘")));
    qunsetenv("WARGAME_FORCE_FLUSH_FAILURE");
}

TEST(RoomPersistenceTest, CheckpointWriteReportsInjectedDurabilityFailureWithoutReplacingFile) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString checkpointPath = temporary.filePath(QStringLiteral("checkpoint.json"));
    RoomPersistence persistence(checkpointPath,
                                temporary.filePath(QStringLiteral("events.jsonl")),
                                temporary.path());
    RoomCheckpoint checkpoint;
    checkpoint.scenario = ScenarioIo::defaultScenario();
    checkpoint.runInitialScenario = checkpoint.scenario;
    qputenv("WARGAME_FORCE_FLUSH_FAILURE", QByteArray("1"));
    QString error;
    EXPECT_FALSE(persistence.saveCheckpoint(checkpoint, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("同步到磁盘")));
    EXPECT_FALSE(QFileInfo::exists(checkpointPath));
    qunsetenv("WARGAME_FORCE_FLUSH_FAILURE");
}

TEST(RoomPersistenceTest, RotationRequiresCheckpointBeforeDiscardingUnreplayedEvents) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    qputenv("WARGAME_EVENT_LOG_MAX_BYTES", QByteArray("512"));
    const QString eventPath = temporary.filePath(QStringLiteral("events.jsonl"));
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")), eventPath,
                                temporary.path());
    const QString payload(180, QLatin1Char('x'));
    QString error;
    quint64 sequence = 1;
    bool checkpointRequired = false;
    while (sequence <= 32 && !checkpointRequired) {
        if (persistence.appendEvent(sequence, QStringLiteral("command"),
                                    QJsonObject{{QStringLiteral("payload"), payload}},
                                    &error, &checkpointRequired)) {
            ++sequence;
        }
    }
    ASSERT_TRUE(checkpointRequired);
    EXPECT_TRUE(error.contains(QStringLiteral("更新检查点")));

    RoomCheckpoint checkpoint;
    checkpoint.scenario = ScenarioIo::defaultScenario();
    checkpoint.runInitialScenario = checkpoint.scenario;
    checkpoint.eventSequence = sequence - 1;
    ASSERT_TRUE(persistence.saveCheckpoint(checkpoint, &error)) << error.toStdString();
    EXPECT_TRUE(persistence.appendEvent(sequence, QStringLiteral("command"),
                                        QJsonObject{{QStringLiteral("payload"), payload}},
                                        &error)) << error.toStdString();
    qunsetenv("WARGAME_EVENT_LOG_MAX_BYTES");
}
