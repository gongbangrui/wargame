#include <gtest/gtest.h>

#include "RoomPersistence.h"
#include "core/Scenario.h"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

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

    QString error;
    ASSERT_TRUE(persistence.saveCheckpoint(source, &error)) << error.toStdString();
    RoomCheckpoint loaded;
    ASSERT_TRUE(persistence.loadCheckpoint(&loaded, &error)) << error.toStdString();
    EXPECT_EQ(loaded.phase, source.phase);
    EXPECT_DOUBLE_EQ(loaded.simTime, source.simTime);
    EXPECT_EQ(loaded.scenarioRevision, source.scenarioRevision);
    EXPECT_EQ(loaded.eventSequence, source.eventSequence);
    EXPECT_EQ(loaded.scenario.units.size(), source.scenario.units.size());
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
