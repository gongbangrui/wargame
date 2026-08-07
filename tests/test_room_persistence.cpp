#include <gtest/gtest.h>

#include "RoomPersistence.h"
#include "core/Scenario.h"

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

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    EXPECT_EQ(loaded.sourceSchemaVersion, 4);
    EXPECT_EQ(loaded.phase, source.phase);
    EXPECT_DOUBLE_EQ(loaded.simTime, source.simTime);
    EXPECT_EQ(loaded.scenarioRevision, source.scenarioRevision);
    EXPECT_EQ(loaded.eventSequence, source.eventSequence);
    EXPECT_EQ(loaded.mapMarks, source.mapMarks);
    EXPECT_EQ(loaded.engineState, source.engineState);
    EXPECT_EQ(loaded.scenario.units.size(), source.scenario.units.size());
}

TEST(RoomPersistenceTest, AiStateRoundTripsWithoutChangingCheckpointSchema) {
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
    EXPECT_EQ(saved.value(QStringLiteral("checkpointSchemaVersion")).toInt(), 4);
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
    legacy.remove(QStringLiteral("engineState"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GT(file.write(QJsonDocument(legacy).toJson()), 0);
    file.close();

    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    EXPECT_EQ(loaded.sourceSchemaVersion, 2);
    EXPECT_TRUE(loaded.engineState.isEmpty());
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
