#include <gtest/gtest.h>

#include "RoomPersistence.h"
#include "core/Scenario.h"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QDir>
#include <QStringList>
#include <QVector>

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
    EXPECT_EQ(loaded.phase, source.phase);
    EXPECT_DOUBLE_EQ(loaded.simTime, source.simTime);
    EXPECT_EQ(loaded.scenarioRevision, source.scenarioRevision);
    EXPECT_EQ(loaded.eventSequence, source.eventSequence);
    EXPECT_EQ(loaded.mapMarks, source.mapMarks);
    EXPECT_EQ(loaded.scenario.units.size(), source.scenario.units.size());
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
