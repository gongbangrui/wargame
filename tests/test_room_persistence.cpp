#include <gtest/gtest.h>

#include "RoomPersistence.h"
#include "core/Scenario.h"

#include <QTemporaryDir>

using namespace gbr;

TEST(RoomPersistenceTest, CheckpointRoundTripIsAtomicAndVersioned) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    RoomPersistence persistence(temporary.filePath(QStringLiteral("checkpoint.json")),
                                temporary.filePath(QStringLiteral("events.jsonl")));
    RoomCheckpoint source;
    source.scenario = ScenarioIo::defaultScenario();
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
