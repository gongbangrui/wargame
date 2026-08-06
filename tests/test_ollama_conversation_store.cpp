#include <gtest/gtest.h>

#include "OllamaConversationStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace gbr;

namespace {

QDateTime testTime(int day, int hour = 12) {
    return QDateTime(QDate(2026, 8, day), QTime(hour, 0), QTimeZone::utc());
}

struct Clock {
    QDateTime value = testTime(6);

    QDateTime operator()() const { return value; }
};

OllamaConversationRecord sampleRecord(const QDateTime& time,
                                      const QString& status = QStringLiteral("completed")) {
    OllamaConversationRecord record;
    record.conversationId = QStringLiteral("conversation-1");
    record.requestId = QStringLiteral("request-1");
    record.roomId = QStringLiteral("room-1");
    record.generation = 4;
    record.time = time;
    record.model = QStringLiteral("qwen3.5:2b");
    record.configuredModel = QStringLiteral("auto");
    record.resolvedModel = QStringLiteral("qwen3.5:2b");
    record.status = status;
    record.failure = status == QLatin1String("completed") ? QString() : QStringLiteral("provider");
    record.latencyMs = 42;
    record.messages = QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                             {QStringLiteral("content"), QStringLiteral("move")}}};
    record.raw = QJsonObject{{QStringLiteral("message"), QStringLiteral("raw")}};
    record.parsed = QJsonObject{{QStringLiteral("action"), QStringLiteral("defend")}};
    record.final = QJsonObject{{QStringLiteral("action"), QStringLiteral("defend")}};
    record.fallback = status == QLatin1String("completed") ? QJsonValue(QJsonValue::Null)
                                                               : QJsonValue(QJsonObject{{QStringLiteral("engine"), QStringLiteral("rules")}});
    return record;
}

QStringList storeFiles(const QString& directory) {
    return QDir(directory).entryList(QStringList{QStringLiteral("ai-conversations-*.jsonl")},
                                      QDir::Files, QDir::Name);
}

qint64 totalStoreBytes(const QString& directory) {
    qint64 total = 0;
    for (const QString& name : storeFiles(directory)) {
        total += QFileInfo(QDir(directory).filePath(name)).size();
    }
    return total;
}

#ifdef Q_OS_UNIX
bool hasMode(const QString& path, mode_t permissions) {
    struct stat status {};
    return ::stat(QFile::encodeName(path).constData(), &status) == 0
        && (status.st_mode & 0777) == permissions;
}
#endif

}

TEST(OllamaConversationStoreTest, WritesFinalRecordsAndRecursivelyRedactsSensitiveData) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    Clock clock;
    OllamaConversationStore store(temporary.path(), [&clock]() { return clock(); });

    OllamaConversationRecord record = sampleRecord(clock.value);
    record.messages = QJsonArray{QJsonObject{
        {QStringLiteral("content"), QStringLiteral("normal")},
        {QStringLiteral("password"), QStringLiteral("do-not-store")},
        {QStringLiteral("nested"), QJsonObject{{QStringLiteral("apiKey"), QStringLiteral("key")},
                                                {QStringLiteral("authorization"), QStringLiteral("Bearer abc")}}}}};
    record.raw = QJsonObject{{QStringLiteral("headers"), QJsonObject{
                                  {QStringLiteral("Authorization"), QStringLiteral("Bearer token")},
                                  {QStringLiteral("Cookie"), QStringLiteral("sid=secret")}}},
                             {QStringLiteral("text"), QStringLiteral("Authorization: Bearer abc")}};
    record.parsed = QJsonObject{{QStringLiteral("credentials"), QJsonObject{{QStringLiteral("secret"), QStringLiteral("value")}}}};
    record.final = QJsonObject{{QStringLiteral("privateKey"), QStringLiteral("private")}};
    record.fallback = QJsonObject{{QStringLiteral("reason"), QStringLiteral("rules")}};

    QString error;
    ASSERT_TRUE(store.appendFinalRecord(record, &error)) << error.toStdString();
    const QVector<QJsonObject> records = store.readRecords(&error);
    ASSERT_EQ(records.size(), 1);
    const QJsonObject saved = records.front();
    for (const QString& field : {QStringLiteral("conversationId"), QStringLiteral("requestId"),
                                 QStringLiteral("roomId"), QStringLiteral("generation"),
                                 QStringLiteral("time"), QStringLiteral("model"),
                                 QStringLiteral("configuredModel"),
                                 QStringLiteral("resolvedModel"),
                                 QStringLiteral("status"), QStringLiteral("failure"),
                                 QStringLiteral("latencyMs"), QStringLiteral("messages"),
                                 QStringLiteral("raw"), QStringLiteral("parsed"),
                                 QStringLiteral("final"), QStringLiteral("fallback")}) {
        EXPECT_TRUE(saved.contains(field)) << field.toStdString();
    }
    EXPECT_EQ(saved.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
    EXPECT_EQ(saved.value(QStringLiteral("configuredModel")).toString(),
              QStringLiteral("auto"));
    EXPECT_EQ(saved.value(QStringLiteral("resolvedModel")).toString(),
              QStringLiteral("qwen3.5:2b"));
    EXPECT_EQ(saved.value(QStringLiteral("messages")).toArray().at(0).toObject()
                  .value(QStringLiteral("password")),
              QJsonValue(QStringLiteral("[REDACTED]")));
    EXPECT_EQ(saved.value(QStringLiteral("raw")).toObject().value(QStringLiteral("headers"))
                  .toObject().value(QStringLiteral("Authorization")),
              QJsonValue(QStringLiteral("[REDACTED]")));
    EXPECT_FALSE(QJsonDocument(saved).toJson().contains("Bearer token"));
    EXPECT_FALSE(QJsonDocument(saved).toJson().contains("do-not-store"));
#ifdef Q_OS_UNIX
    EXPECT_TRUE(hasMode(temporary.filePath(QStringLiteral("ai-conversations-2026-08-06.jsonl")),
                        S_IRUSR | S_IWUSR));
    EXPECT_TRUE(hasMode(temporary.path(), S_IRUSR | S_IWUSR | S_IXUSR));
#endif
}

TEST(OllamaConversationStoreTest, AllowsMalformedPriorLinesAndAppendsCompleteJsonLines) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("ai-conversations-2026-08-06.jsonl"));
    QFile existing(path);
    ASSERT_TRUE(existing.open(QIODevice::WriteOnly));
    ASSERT_GT(existing.write("{malformed\n"), 0);
    existing.close();
    Clock clock;
    OllamaConversationStore store(temporary.path(), [&clock]() { return clock(); });

    QString error;
    ASSERT_TRUE(store.appendFinalRecord(sampleRecord(clock.value), &error)) << error.toStdString();
    QFile read(path);
    ASSERT_TRUE(read.open(QIODevice::ReadOnly));
    const QByteArray bytes = read.readAll();
    EXPECT_TRUE(bytes.endsWith('\n'));
    EXPECT_EQ(bytes.count('\n'), 2);
    EXPECT_EQ(store.readRecords(&error).size(), 1);
}

TEST(OllamaConversationStoreTest, RepairsUnterminatedMalformedPriorLineBeforeAppending) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    Clock clock;
    OllamaConversationStore store(temporary.path(), [&clock]() { return clock(); });

    const QString path = temporary.filePath(QStringLiteral("ai-conversations-2026-08-06.jsonl"));
    QFile existing(path);
    ASSERT_TRUE(existing.open(QIODevice::WriteOnly));
    ASSERT_GT(existing.write("{malformed"), 0);
    existing.close();

    QString error;
    ASSERT_TRUE(store.appendFinalRecord(sampleRecord(clock.value), &error)) << error.toStdString();
    QFile read(path);
    ASSERT_TRUE(read.open(QIODevice::ReadOnly));
    const QByteArray bytes = read.readAll();
    EXPECT_TRUE(bytes.endsWith('\n'));
    EXPECT_EQ(bytes.count('\n'), 2);
    EXPECT_EQ(store.readRecords(&error).size(), 1);
}

TEST(OllamaConversationStoreTest, IsIdempotentForRepeatedFinalRequest) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    Clock clock;
    OllamaConversationStore store(temporary.path(), [&clock]() { return clock(); });

    QString error;
    ASSERT_TRUE(store.appendFinalRecord(sampleRecord(clock.value), &error)) << error.toStdString();
    ASSERT_TRUE(store.appendFinalRecord(sampleRecord(clock.value, QStringLiteral("failed")),
                                        &error)) << error.toStdString();
    const QVector<QJsonObject> records = store.readRecords(&error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(records.size(), 1);
    EXPECT_EQ(records.first().value(QStringLiteral("status")).toString(),
              QStringLiteral("completed"));
}

TEST(OllamaConversationStoreTest, UsesUtcDailyFilesAndRolloverWithoutGrowingOversizedFile) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    Clock clock{QDateTime(QDate(2026, 8, 6), QTime(23, 59), QTimeZone::utc())};
    OllamaConversationStore store(temporary.path(), [&clock]() { return clock(); });
    QString error;
    OllamaConversationRecord first = sampleRecord(clock.value);
    ASSERT_TRUE(store.appendFinalRecord(first, &error)) << error.toStdString();
    clock.value = QDateTime(QDate(2026, 8, 7), QTime(0, 1), QTimeZone::utc());
    OllamaConversationRecord second = sampleRecord(clock.value);
    second.conversationId = QStringLiteral("conversation-2");
    second.requestId = QStringLiteral("request-2");
    ASSERT_TRUE(store.appendFinalRecord(second, &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(temporary.filePath(QStringLiteral("ai-conversations-2026-08-06.jsonl"))));
    EXPECT_TRUE(QFileInfo::exists(temporary.filePath(QStringLiteral("ai-conversations-2026-08-07.jsonl"))));

    const QString current = temporary.filePath(QStringLiteral("ai-conversations-2026-08-07.jsonl"));
    QFile oversized(current);
    ASSERT_TRUE(oversized.open(QIODevice::Append));
    ASSERT_GT(oversized.write(QByteArray(1024 * 1024, 'x')), 0);
    oversized.close();
    OllamaConversationRecord third = second;
    third.conversationId = QStringLiteral("conversation-3");
    third.requestId = QStringLiteral("request-3");
    ASSERT_TRUE(store.appendFinalRecord(third, &error)) << error.toStdString();
    EXPECT_LT(QFileInfo(current).size(), 1024 * 1024 + 512);
}

TEST(OllamaConversationStoreTest, RetainsRecentRecordsAndBoundsTotalSize) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    Clock clock;
    OllamaConversationStore store(temporary.path(), [&clock]() { return clock(); });
    QString error;
    ASSERT_TRUE(store.appendFinalRecord(sampleRecord(clock.value), &error)) << error.toStdString();

    clock.value = clock.value.addDays(8);
    ASSERT_TRUE(store.cleanup(&error));
    EXPECT_TRUE(store.readRecords(&error).isEmpty());

    for (int index = 0; index < 12; ++index) {
        const QString name = QStringLiteral("ai-conversations-2026-08-%1.jsonl")
                                 .arg(index + 10, 2, 10, QLatin1Char('0'));
        QFile file(temporary.filePath(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write(QByteArray(1024 * 1024, 'x')), 0);
        file.close();
    }
    ASSERT_TRUE(store.cleanup(&error));
    EXPECT_LE(totalStoreBytes(temporary.path()), 10 * 1024 * 1024);
    EXPECT_LT(storeFiles(temporary.path()).size(), 12);
}

TEST(OllamaConversationStoreTest, RejectsSymlinkAndNonRegularDailyPaths) {
    QTemporaryDir temporary;
    QTemporaryDir outside;
    ASSERT_TRUE(temporary.isValid());
    ASSERT_TRUE(outside.isValid());
    Clock clock;
    const QString daily = temporary.filePath(QStringLiteral("ai-conversations-2026-08-06.jsonl"));
    ASSERT_TRUE(QFile::link(outside.filePath(QStringLiteral("outside.jsonl")), daily));
    OllamaConversationStore symlinkStore(temporary.path(), [&clock]() { return clock(); });
    QString error;
    EXPECT_FALSE(symlinkStore.appendFinalRecord(sampleRecord(clock.value), &error));
    EXPECT_FALSE(QFileInfo::exists(outside.filePath(QStringLiteral("outside.jsonl"))));

    ASSERT_TRUE(QFile::remove(daily));
    ASSERT_TRUE(QDir().mkpath(daily));
    OllamaConversationStore directoryStore(temporary.path(), [&clock]() { return clock(); });
    EXPECT_FALSE(directoryStore.appendFinalRecord(sampleRecord(clock.value), &error));
}

TEST(OllamaConversationStoreTest, ReportsWriteAndFsyncFailuresWithoutClaimingSuccess) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    Clock clock;
    OllamaConversationStore store(temporary.path(), [&clock]() { return clock(); });
    QString error;
    qputenv("WARGAME_FORCE_CONVERSATION_STORE_WRITE_FAILURE", QByteArray("1"));
    EXPECT_FALSE(store.appendFinalRecord(sampleRecord(clock.value), &error));
    qunsetenv("WARGAME_FORCE_CONVERSATION_STORE_WRITE_FAILURE");
    qputenv("WARGAME_FORCE_CONVERSATION_STORE_FLUSH_FAILURE", QByteArray("1"));
    EXPECT_FALSE(store.appendFinalRecord(sampleRecord(clock.value), &error));
    qunsetenv("WARGAME_FORCE_CONVERSATION_STORE_FLUSH_FAILURE");
}
