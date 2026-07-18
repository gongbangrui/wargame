#include <gtest/gtest.h>

#include "client/ClientStateStore.h"
#include "client/RemoteSessionAdapter.h"
#include "core/SnapshotCodec.h"
#include "core/UnitBase.h"
#include "protocol/Protocol.h"
#include "server/AuthPolicy.h"
#include "server/PersistenceStore.h"
#include "server/ServerConfig.h"
#include "server/ServerMetrics.h"
#include "server/SessionGateway.h"
#include "server/SimulationRoom.h"
#include "server/VisibleStateProjector.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QThread>
#include <QWebSocket>
#include <QWebSocketServer>
#include <algorithm>
#include <functional>

using namespace gbr;

namespace {

const QString kRedToken = QStringLiteral("red-token-0123456789-0123456789-abcdef");
const QByteArray kPepper = QByteArrayLiteral("test-pepper-0123456789abcdef-0123456789abcdef");

SessionIdentity identity(const QString& role, const QString& side = {}) {
    return {QStringLiteral("user-%1").arg(role), role, side,
            QStringLiteral("main"), QDateTime::currentDateTimeUtc().addDays(1)};
}

QJsonObject tokenConfig(const QString& token, const SessionIdentity& who) {
    return {{QStringLiteral("tokenHash"), QString::fromLatin1(AuthPolicy::hashToken(token, kPepper))},
            {QStringLiteral("userId"), who.userId},
            {QStringLiteral("role"), who.role},
            {QStringLiteral("side"), who.side},
            {QStringLiteral("roomId"), who.roomId},
            {QStringLiteral("expiresAt"), who.expiresAt.toString(Qt::ISODate)}};
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return predicate();
}

struct ServerFixture {
    QTemporaryDir directory;
    ServerConfig config;
    AuthPolicy auth;
    PersistenceStore persistence;
    std::unique_ptr<SimulationRoom> room;
    ServerMetrics metrics;
    std::unique_ptr<SessionGateway> gateway;
    QString lastError;

    bool start(int maxConnections = 8, int commandRate = 20, int commandBurst = 40) {
        if (!directory.isValid()) return false;
        config.listenAddress = QHostAddress::LocalHost;
        QTcpServer portProbe;
        if (!portProbe.listen(QHostAddress::LocalHost, 0)) {
            lastError = portProbe.errorString();
            return false;
        }
        config.port = portProbe.serverPort();
        portProbe.close();
        config.roomId = QStringLiteral("main");
        config.databasePath = directory.filePath(QStringLiteral("test.sqlite3"));
        config.snapshotIntervalMs = 100;
        config.checkpointIntervalMs = 60000;
        config.maxConnections = maxConnections;
        config.commandRatePerSecond = commandRate;
        config.commandBurst = commandBurst;
        config.tokenPepper = kPepper;
        config.tokens = QJsonArray{tokenConfig(kRedToken, identity(QStringLiteral("red"), QStringLiteral("red")))};
        if (!auth.load(config.tokens, kPepper, &lastError)
            || !persistence.open(config.databasePath, &lastError)) return false;
        room = std::make_unique<SimulationRoom>(config, &persistence);
        if (!room->initialize(&lastError)) return false;
        metrics.startedAtMs = QDateTime::currentMSecsSinceEpoch();
        gateway = std::make_unique<SessionGateway>(config, &auth, &persistence,
                                                   room.get(), &metrics);
        return gateway->listen(&lastError);
    }
};

} // namespace

TEST(ProtocolTest, RejectsMalformedVersionUnknownFieldAndOversize) {
    EXPECT_EQ(ProtocolCodec::parse(QByteArrayLiteral("{" )).code, QStringLiteral("MALFORMED_JSON"));
    QJsonObject valid = ProtocolCodec::envelope(QString::fromLatin1(ProtocolType::Ping), {},
                                                 QStringLiteral("message-1"));
    EXPECT_TRUE(ProtocolCodec::parse(ProtocolCodec::encode(valid)).accepted);
    valid.insert(QStringLiteral("protocolVersion"), 999);
    EXPECT_EQ(ProtocolCodec::parse(ProtocolCodec::encode(valid)).code,
              QStringLiteral("UNSUPPORTED_VERSION"));
    valid.insert(QStringLiteral("protocolVersion"), ProtocolLimits::Version);
    valid.insert(QStringLiteral("unexpected"), true);
    EXPECT_EQ(ProtocolCodec::parse(ProtocolCodec::encode(valid)).code,
              QStringLiteral("UNKNOWN_FIELD"));
    EXPECT_EQ(ProtocolCodec::parse(QByteArray(2048, 'x'), 1024).code,
              QStringLiteral("PACKET_TOO_LARGE"));
}

TEST(AuthPolicyTest, HashesTokensAndEnforcesRoleSideExpiryAndRevocation) {
    AuthPolicy policy;
    const SessionIdentity red = identity(QStringLiteral("red"), QStringLiteral("red"));
    QString error;
    ASSERT_TRUE(policy.load(QJsonArray{tokenConfig(kRedToken, red)}, kPepper, &error)) << error.toStdString();
    EXPECT_EQ(policy.authenticate(kRedToken).userId, red.userId);
    EXPECT_FALSE(policy.authenticate(QStringLiteral("wrong-token-012345678901234567890123")).isValid());

    QJsonObject revoked = tokenConfig(kRedToken, red);
    revoked.insert(QStringLiteral("revoked"), true);
    ASSERT_TRUE(policy.load(QJsonArray{revoked}, kPepper, &error));
    EXPECT_FALSE(policy.authenticate(kRedToken).isValid());

    EXPECT_FALSE(policy.load(QJsonArray{tokenConfig(kRedToken, red)},
                             QByteArray(31, 'x'), &error));
    QJsonObject invalidExpiry = tokenConfig(kRedToken, red);
    invalidExpiry.insert(QStringLiteral("expiresAt"), QStringLiteral("not-a-date"));
    EXPECT_FALSE(policy.load(QJsonArray{invalidExpiry}, kPepper, &error));
    const QJsonObject duplicate = tokenConfig(kRedToken, red);
    EXPECT_FALSE(policy.load(QJsonArray{duplicate, duplicate}, kPepper, &error));
}

TEST(AuthPolicyTest, AuthenticatesPasswordAccountsAndRejectsInvalidState) {
    AuthPolicy policy;
    QString error;
    const SessionIdentity red = identity(QStringLiteral("red"), QStringLiteral("red"));
    const PasswordAccount account = AuthPolicy::makePasswordAccount(
        QStringLiteral("red.player"), QStringLiteral("correct horse battery"), red);
    ASSERT_TRUE(policy.setPasswordAccounts({account}, &error)) << error.toStdString();
    EXPECT_EQ(policy.authenticatePassword(QStringLiteral("red.player"),
                                          QStringLiteral("correct horse battery")).userId,
              red.userId);
    EXPECT_FALSE(policy.authenticatePassword(QStringLiteral("red.player"),
                                             QStringLiteral("wrong password")).isValid());
    EXPECT_FALSE(policy.authenticatePassword(QStringLiteral("bad name!"),
                                             QStringLiteral("correct horse battery")).isValid());

    PasswordAccount disabled = account;
    disabled.disabled = true;
    ASSERT_TRUE(policy.setPasswordAccounts({disabled}, &error)) << error.toStdString();
    EXPECT_FALSE(policy.authenticatePassword(QStringLiteral("red.player"),
                                             QStringLiteral("correct horse battery")).isValid());

    PasswordAccount mismatched = account;
    mismatched.identity.side = QStringLiteral("blue");
    EXPECT_FALSE(policy.setPasswordAccounts({mismatched}, &error));
}

TEST(ServerConfigTest, PublicListenRequiresExplicitAdminExposure) {
    ServerConfig config;
    config.listenAddress = QHostAddress::LocalHost;
    config.healthAddress = QHostAddress::LocalHost;
    config.adminAddress = QHostAddress::Any;
    config.tokenPepper = kPepper;
    config.tokens = QJsonArray{tokenConfig(kRedToken, identity(QStringLiteral("red"), QStringLiteral("red")))};
    QString error;
    EXPECT_FALSE(config.validate(&error));
    EXPECT_FALSE(error.isEmpty());

    config.allowPublicListen = true;
    EXPECT_TRUE(config.validate(&error)) << error.toStdString();
}

TEST(VisibleStateProjectorTest, RedPayloadNeverContainsUndetectedEnemyState) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const SessionIdentity red = identity(QStringLiteral("red"), QStringLiteral("red"));
    const QJsonObject projected = VisibleStateProjector::project(engine, red, 1, 0, 1);
    const QByteArray raw = QJsonDocument(projected).toJson(QJsonDocument::Compact);
    EXPECT_FALSE(raw.contains("blue_cp"));
    EXPECT_FALSE(raw.contains("blue_a1"));

    engine.unit(QStringLiteral("blue_r1"))->setPosition(
        engine.unit(QStringLiteral("red_r1"))->pos());
    const QJsonObject detected = VisibleStateProjector::project(engine, red, 1, 0, 2);
    QJsonObject blueRecon;
    for (const auto& value : detected.value(QStringLiteral("units")).toArray()) {
        const QJsonObject unit = value.toObject();
        if (unit.value(QStringLiteral("id")).toString() == QLatin1String("blue_r1")) blueRecon = unit;
    }
    ASSERT_FALSE(blueRecon.isEmpty());
    EXPECT_TRUE(blueRecon.value(QStringLiteral("detected")).toBool());
    EXPECT_FALSE(blueRecon.contains(QStringLiteral("hp")));
    EXPECT_FALSE(blueRecon.contains(QStringLiteral("schedule")));
    EXPECT_FALSE(blueRecon.contains(QStringLiteral("sharedKnowledge")));
}

TEST(VisibleStateProjectorTest, FiveHundredUnitDirectorSnapshotStaysBelowTwoMiB) {
    Scenario scenario;
    ScenarioUnit redCp;
    redCp.id = QStringLiteral("red_cp");
    redCp.callsign = QStringLiteral("红方指挥所");
    redCp.kind = QStringLiteral("commandpost");
    redCp.side = QStringLiteral("red");
    redCp.pos = {500.0, 500.0, 0.0};
    scenario.units.push_back(redCp);
    ScenarioUnit blueCp = redCp;
    blueCp.id = QStringLiteral("blue_cp");
    blueCp.callsign = QStringLiteral("蓝方指挥所");
    blueCp.side = QStringLiteral("blue");
    blueCp.pos = {39500.0, 29500.0, 0.0};
    scenario.units.push_back(blueCp);
    for (int i = 0; i < 498; ++i) {
        ScenarioUnit unit;
        unit.id = QStringLiteral("unit_%1").arg(i, 3, 10, QLatin1Char('0'));
        unit.callsign = QStringLiteral("测试单元%1").arg(i);
        unit.kind = QStringLiteral("reconuav");
        unit.side = (i % 2 == 0) ? QStringLiteral("red") : QStringLiteral("blue");
        unit.pos = {100.0 + (i % 25) * 1500.0,
                    100.0 + (i / 25) * 1400.0, 1000.0};
        scenario.units.push_back(unit);
    }
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario)) << engine.lastError().toStdString();
    const QJsonObject snapshot = VisibleStateProjector::project(
        engine, identity(QStringLiteral("director")), 1, 0, 1);
    EXPECT_EQ(snapshot.value(QStringLiteral("units")).toArray().size(), 500);
    EXPECT_LT(QJsonDocument(snapshot).toJson(QJsonDocument::Compact).size(), 2 * 1024 * 1024);
}

TEST(PersistenceTest, CheckpointCommandResultAndBackupRoundTrip) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    PersistenceStore store;
    QString error;
    ASSERT_TRUE(store.open(directory.filePath(QStringLiteral("data.sqlite3")), &error)) << error.toStdString();
    const QJsonObject checkpoint{{QStringLiteral("checkpointVersion"), 1},
                                 {QStringLiteral("value"), QStringLiteral("saved")}};
    ASSERT_TRUE(store.saveCheckpoint(QStringLiteral("main"), 3, 42, checkpoint, &error));
    qint64 revision = 0;
    qint64 tick = 0;
    EXPECT_EQ(store.loadLatestCheckpoint(QStringLiteral("main"), &revision, &tick, &error), checkpoint);
    EXPECT_EQ(revision, 3);
    EXPECT_EQ(tick, 42);
    const QJsonObject result{{QStringLiteral("accepted"), true}};
    ASSERT_TRUE(store.storeCommandResult(QStringLiteral("main"), QStringLiteral("user"),
                                         QStringLiteral("cmd"), result, &error));
    EXPECT_EQ(store.commandResult(QStringLiteral("main"), QStringLiteral("user"),
                                  QStringLiteral("cmd")), result);
    ASSERT_TRUE(store.backupTo(directory.filePath(QStringLiteral("backup.sqlite3")), &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(directory.filePath(QStringLiteral("backup.sqlite3"))));
}

TEST(PersistenceTest, StoresPrivilegedTokenWithEmptySide) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    PersistenceStore store;
    QString error;
    ASSERT_TRUE(store.open(directory.filePath(QStringLiteral("identities.sqlite3")), &error))
        << error.toStdString();
    AuthPolicy policy;
    const SessionIdentity director = identity(QStringLiteral("director"));
    ASSERT_TRUE(policy.load(QJsonArray{tokenConfig(kRedToken, director)}, kPepper, &error))
        << error.toStdString();
    EXPECT_TRUE(store.syncTokenRecords(policy.records(), &error)) << error.toStdString();
}

TEST(PersistenceTest, CorruptNewestCheckpointFallsBackToPreviousValidRecord) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("fallback.sqlite3"));
    PersistenceStore store;
    QString error;
    ASSERT_TRUE(store.open(databasePath, &error)) << error.toStdString();
    const QJsonObject previous{{QStringLiteral("checkpointVersion"), 1},
                               {QStringLiteral("value"), QStringLiteral("previous")}};
    const QJsonObject newest{{QStringLiteral("checkpointVersion"), 1},
                             {QStringLiteral("value"), QStringLiteral("newest")}};
    ASSERT_TRUE(store.saveCheckpoint(QStringLiteral("main"), 1, 10, previous, &error));
    ASSERT_TRUE(store.saveCheckpoint(QStringLiteral("main"), 2, 20, newest, &error));

    const QString connectionName = QStringLiteral("checkpoint_corruption_test");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        query.prepare(QStringLiteral(
            "UPDATE checkpoints SET payload=? WHERE id=(SELECT MAX(id) FROM checkpoints)"));
        query.addBindValue(qCompress(QByteArrayLiteral("not-json"), 9));
        ASSERT_TRUE(query.exec()) << query.lastError().text().toStdString();
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    qint64 revision = 0;
    qint64 tick = 0;
    EXPECT_EQ(store.loadLatestCheckpoint(QStringLiteral("main"), &revision, &tick, &error),
              previous);
    EXPECT_TRUE(error.isEmpty());
    EXPECT_EQ(revision, 1);
    EXPECT_EQ(tick, 10);
}

TEST(SimulationRoomTest, EnforcesRevisionRoleOwnershipAndPausedEditing) {
    QTemporaryDir directory;
    ServerConfig config;
    config.roomId = QStringLiteral("main");
    config.databasePath = directory.filePath(QStringLiteral("room.sqlite3"));
    config.checkpointIntervalMs = 60000;
    PersistenceStore store;
    QString error;
    ASSERT_TRUE(store.open(config.databasePath, &error));
    SimulationRoom room(config, &store);
    ASSERT_TRUE(room.initialize(&error));
    const SessionIdentity red = identity(QStringLiteral("red"), QStringLiteral("red"));
    const auto otherSide = room.execute(red, QString::fromLatin1(CommandAction::SetSpeed),
                                        {{QStringLiteral("unitId"), QStringLiteral("blue_r1")},
                                         {QStringLiteral("speed"), 80.0}}, 1);
    EXPECT_EQ(otherSide.code, QString::fromLatin1(CommandCode::PermissionDenied));
    const auto stale = room.execute(red, QString::fromLatin1(CommandAction::SetSpeed),
                                    {{QStringLiteral("unitId"), QStringLiteral("red_r1")},
                                     {QStringLiteral("speed"), 80.0}}, 99);
    EXPECT_EQ(stale.code, QString::fromLatin1(CommandCode::RevisionMismatch));
    const auto deniedRun = room.execute(red, QStringLiteral("setRunning"),
                                        {{QStringLiteral("running"), true}}, 1);
    EXPECT_EQ(deniedRun.code, QString::fromLatin1(CommandCode::PermissionDenied));
    const auto directorRun = room.execute(identity(QStringLiteral("director")),
                                           QStringLiteral("setRunning"),
                                           {{QStringLiteral("running"), true}}, 1);
    EXPECT_EQ(directorRun.code, QString::fromLatin1(CommandCode::SimulationStateInvalid));
    EXPECT_TRUE(room.execute(red, QStringLiteral("setSideReady"),
                             {{QStringLiteral("ready"), true}}, 1).accepted);
    EXPECT_TRUE(room.execute(identity(QStringLiteral("blue"), QStringLiteral("blue")),
                             QStringLiteral("setSideReady"),
                             {{QStringLiteral("ready"), true}}, 1).accepted);
    EXPECT_TRUE(room.execute(identity(QStringLiteral("director")), QStringLiteral("setRunning"),
                             {{QStringLiteral("running"), true}}, 1).accepted);
    room.engine()->setRunning(false);
    const GeoPos initialPosition = room.engine()->unit(QStringLiteral("red_r1"))->pos();
    room.engine()->unit(QStringLiteral("red_r1"))->setPosition({9999.0, 8888.0, 777.0});
    room.engine()->unit(QStringLiteral("red_r1"))->setHp(1.0);
    const auto reset = room.execute(identity(QStringLiteral("director")),
                                    QStringLiteral("resetSimulation"), {}, 1);
    EXPECT_TRUE(reset.accepted);
    EXPECT_EQ(room.scenarioRevision(), 2);
    EXPECT_EQ(room.serverTick(), 0);
    EXPECT_FALSE(room.engine()->running());
    EXPECT_DOUBLE_EQ(room.engine()->unit(QStringLiteral("red_r1"))->pos().x, initialPosition.x);
    EXPECT_DOUBLE_EQ(room.engine()->unit(QStringLiteral("red_r1"))->hp(),
                     room.engine()->unit(QStringLiteral("red_r1"))->maxHp());
    const QJsonObject resetLobby = room.projectedState(identity(QStringLiteral("director")), 1)
        .value(QStringLiteral("lobby")).toObject();
    EXPECT_FALSE(resetLobby.value(QStringLiteral("redReady")).toBool());
    EXPECT_FALSE(resetLobby.value(QStringLiteral("blueReady")).toBool());
}

TEST(SimulationRoomTest, SidePreparationMergesOnlyOwnUnitsAndBroadcastsLobbyChat) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ServerConfig config;
    config.roomId = QStringLiteral("main");
    config.databasePath = directory.filePath(QStringLiteral("preparation.sqlite3"));
    config.checkpointIntervalMs = 60000;
    PersistenceStore store;
    QString error;
    ASSERT_TRUE(store.open(config.databasePath, &error)) << error.toStdString();
    SimulationRoom room(config, &store);
    ASSERT_TRUE(room.initialize(&error)) << error.toStdString();

    const SessionIdentity red = identity(QStringLiteral("red"), QStringLiteral("red"));
    const SessionIdentity blue = identity(QStringLiteral("blue"), QStringLiteral("blue"));
    const SessionIdentity director = identity(QStringLiteral("director"));
    QJsonObject redScenario = room.projectedState(red, 1).value(QStringLiteral("scenario")).toObject();
    QJsonArray units = redScenario.value(QStringLiteral("units")).toArray();
    ASSERT_FALSE(units.isEmpty());
    for (qsizetype i = 0; i < units.size(); ++i) {
        QJsonObject unit = units.at(i).toObject();
        ASSERT_EQ(unit.value(QStringLiteral("side")).toString(), QStringLiteral("red"));
        if (unit.value(QStringLiteral("id")).toString() == QLatin1String("red_r1")) {
            unit.insert(QStringLiteral("speed"), 123.0);
            units.replace(i, unit);
        }
    }
    redScenario.insert(QStringLiteral("units"), units);
    const CommandResult edit = room.execute(
        red, QStringLiteral("replaceScenario"),
        {{QStringLiteral("scenario"), redScenario.toVariantMap()}}, 1);
    ASSERT_TRUE(edit.accepted) << edit.message.toStdString();
    EXPECT_DOUBLE_EQ(room.engine()->unit(QStringLiteral("red_r1"))->speed(), 123.0);
    EXPECT_NE(room.engine()->unit(QStringLiteral("blue_r1")), nullptr);

    const qint64 revision = room.scenarioRevision();
    EXPECT_TRUE(room.execute(red, QStringLiteral("setSideReady"),
                             {{QStringLiteral("ready"), true}}, revision).accepted);
    EXPECT_TRUE(room.execute(blue, QStringLiteral("setSideReady"),
                             {{QStringLiteral("ready"), true}}, revision).accepted);
    const QJsonObject lobby = room.projectedState(director, 1).value(QStringLiteral("lobby")).toObject();
    EXPECT_TRUE(lobby.value(QStringLiteral("redReady")).toBool());
    EXPECT_TRUE(lobby.value(QStringLiteral("blueReady")).toBool());
    EXPECT_TRUE(lobby.value(QStringLiteral("bothReady")).toBool());

    const CommandResult chat = room.execute(red, QStringLiteral("sendChat"),
                                            {{QStringLiteral("text"), QStringLiteral("红方已完成部署")}},
                                            revision);
    ASSERT_TRUE(chat.accepted) << chat.message.toStdString();
    EXPECT_EQ(room.projectedState(red, 1).value(QStringLiteral("chatMessages")).toArray().size(), 1);
    EXPECT_EQ(room.projectedState(blue, 1).value(QStringLiteral("chatMessages")).toArray().size(), 1);
    EXPECT_EQ(room.projectedState(director, 1).value(QStringLiteral("chatMessages")).toArray().size(), 1);
    EXPECT_TRUE(room.projectedState(identity(QStringLiteral("observer")), 1)
                    .value(QStringLiteral("chatMessages")).toArray().isEmpty());
}

TEST(SimulationRoomTest, RestartRestoresTickHpPositionAndRevision) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ServerConfig config;
    config.roomId = QStringLiteral("main");
    config.databasePath = directory.filePath(QStringLiteral("restore.sqlite3"));
    config.checkpointIntervalMs = 60000;
    PersistenceStore store;
    QString error;
    ASSERT_TRUE(store.open(config.databasePath, &error)) << error.toStdString();
    {
        SimulationRoom room(config, &store);
        ASSERT_TRUE(room.initialize(&error)) << error.toStdString();
        room.engine()->stepOnce(0.05);
        room.engine()->unit(QStringLiteral("red_r1"))->setPosition({1234.0, 5678.0, 900.0});
        room.engine()->unit(QStringLiteral("red_r1"))->setHp(42.0);
        ASSERT_TRUE(room.checkpointNow(&error)) << error.toStdString();
    }

    SimulationRoom restored(config, &store);
    ASSERT_TRUE(restored.initialize(&error)) << error.toStdString();
    EXPECT_EQ(restored.scenarioRevision(), 1);
    EXPECT_EQ(restored.serverTick(), 1);
    const UnitBase* unit = restored.engine()->unit(QStringLiteral("red_r1"));
    ASSERT_NE(unit, nullptr);
    EXPECT_DOUBLE_EQ(unit->hp(), 42.0);
    EXPECT_DOUBLE_EQ(unit->pos().x, 1234.0);
    EXPECT_DOUBLE_EQ(unit->pos().y, 5678.0);
    EXPECT_DOUBLE_EQ(unit->pos().alt, 900.0);
}

TEST(SimulationRoomTest, InvalidRuntimeSnapshotIsRejectedBeforeMutation) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const double originalHp = engine.unit(QStringLiteral("red_r1"))->hp();
    QJsonObject runtime = SnapshotCodec::encodeRuntime(engine, 0, 1);
    QJsonArray units = runtime.value(QStringLiteral("units")).toArray();
    units.removeLast();
    runtime.insert(QStringLiteral("units"), units);
    EXPECT_FALSE(engine.restoreRuntimeState(runtime));
    EXPECT_DOUBLE_EQ(engine.unit(QStringLiteral("red_r1"))->hp(), originalHp);
}

TEST(ClientStateStoreTest, SnapshotApplicationIsAtomicAndSequenceChecked) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    QJsonObject snapshot = VisibleStateProjector::project(
        engine, identity(QStringLiteral("director")), 1, 0, 1);
    snapshot.insert(QStringLiteral("lobby"), QJsonObject{
        {QStringLiteral("redReady"), true},
        {QStringLiteral("blueReady"), false},
        {QStringLiteral("bothReady"), false},
        {QStringLiteral("preparation"), true},
    });
    snapshot.insert(QStringLiteral("chatMessages"), QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("chat-1")},
        {QStringLiteral("text"), QStringLiteral("测试消息")},
    }});
    ClientStateStore store;
    QString error;
    ASSERT_TRUE(store.applySnapshot(snapshot, &error));
    EXPECT_TRUE(store.lobby().value(QStringLiteral("redReady")).toBool());
    EXPECT_EQ(store.chatMessages().size(), 1);
    const int before = store.allUnits().size();
    QJsonObject invalid = snapshot;
    QJsonArray units = invalid.value(QStringLiteral("units")).toArray();
    units.append(units.first());
    invalid.insert(QStringLiteral("units"), units);
    EXPECT_FALSE(store.applySnapshot(invalid, &error));
    EXPECT_EQ(store.allUnits().size(), before);
    EXPECT_FALSE(store.applyDelta(QJsonObject{{QStringLiteral("sequence"), 9},
                                              {QStringLiteral("scenarioRevision"), 1}}, &error));

    ASSERT_TRUE(store.advanceSequence(2, &error)) << error.toStdString();
    QJsonObject updatedUnit = store.allUnits().first().toObject();
    updatedUnit.insert(QStringLiteral("speed"), 321.0);
    const QJsonObject delta{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("sequence"), 3},
        {QStringLiteral("scenarioRevision"), 1},
        {QStringLiteral("serverTick"), 1},
        {QStringLiteral("simTime"), 0.05},
        {QStringLiteral("running"), false},
        {QStringLiteral("readyForSim"), true},
        {QStringLiteral("cpIssues"), QString()},
        {QStringLiteral("messages"), QJsonArray{}},
        {QStringLiteral("upsertUnits"), QJsonArray{updatedUnit}},
        {QStringLiteral("removeUnitIds"), QJsonArray{}},
    };
    ASSERT_TRUE(store.applyDelta(delta, &error)) << error.toStdString();
    EXPECT_DOUBLE_EQ(store.unitAt(updatedUnit.value(QStringLiteral("id")).toString())
                         .value(QStringLiteral("speed")).toDouble(),
                     321.0);

    const QString updatedId = updatedUnit.value(QStringLiteral("id")).toString();
    ASSERT_TRUE(store.unitAt(updatedId).contains(QStringLiteral("status")));
    const QJsonObject removeFieldDelta{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("sequence"), 4},
        {QStringLiteral("scenarioRevision"), 1},
        {QStringLiteral("serverTick"), 2},
        {QStringLiteral("simTime"), 0.1},
        {QStringLiteral("running"), false},
        {QStringLiteral("readyForSim"), true},
        {QStringLiteral("cpIssues"), QString()},
        {QStringLiteral("upsertUnits"), QJsonArray{}},
        {QStringLiteral("removeFields"), QJsonArray{QJsonObject{
            {QStringLiteral("id"), updatedId},
            {QStringLiteral("fields"), QJsonArray{QStringLiteral("status")}},
        }}},
        {QStringLiteral("removeUnitIds"), QJsonArray{}},
    };
    ASSERT_TRUE(store.applyDelta(removeFieldDelta, &error)) << error.toStdString();
    EXPECT_FALSE(store.unitAt(updatedId).contains(QStringLiteral("status")));
}

TEST(WebSocketIntegrationTest, AuthSnapshotPermissionAndIdempotencyWorkEndToEnd) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.start()) << fixture.lastError.toStdString();
    QWebSocket client;
    QVector<QJsonObject> received;
    QObject::connect(&client, &QWebSocket::textMessageReceived, [&](const QString& text) {
        received.push_back(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(fixture.gateway->serverPort())));
    ASSERT_TRUE(waitUntil([&] { return client.state() == QAbstractSocket::ConnectedState; }));
    client.sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(ProtocolCodec::envelope(
        QString::fromLatin1(ProtocolType::Hello), {{QStringLiteral("token"), kRedToken}},
        QStringLiteral("hello-1"), {}, QStringLiteral("client-1")))));
    ASSERT_TRUE(waitUntil([&] {
        return std::any_of(received.cbegin(), received.cend(), [](const QJsonObject& value) {
            return value.value(QStringLiteral("type")).toString() == QLatin1String(ProtocolType::Snapshot);
        });
    }));
    const QString commandId = QStringLiteral("command-1");
    auto sendCommand = [&](const QString& unitId) {
        client.sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(ProtocolCodec::envelope(
            QString::fromLatin1(ProtocolType::Command),
            {{QStringLiteral("commandId"), commandId},
             {QStringLiteral("action"), QString::fromLatin1(CommandAction::SetSpeed)},
             {QStringLiteral("args"), QJsonObject{{QStringLiteral("unitId"), unitId},
                                                   {QStringLiteral("speed"), 77.0}}}},
            QStringLiteral("message-command"), {}, QStringLiteral("client-1"), -1, 1))));
    };
    sendCommand(QStringLiteral("red_r1"));
    ASSERT_TRUE(waitUntil([&] {
        return std::any_of(received.cbegin(), received.cend(), [&](const QJsonObject& value) {
            const QJsonObject payload = value.value(QStringLiteral("payload")).toObject();
            return value.value(QStringLiteral("type")).toString() == QLatin1String(ProtocolType::CommandResult)
                && payload.value(QStringLiteral("commandId")).toString() == commandId
                && payload.value(QStringLiteral("accepted")).toBool();
        });
    }));
    sendCommand(QStringLiteral("red_r1"));
    ASSERT_TRUE(waitUntil([&] {
        return std::any_of(received.cbegin(), received.cend(), [&](const QJsonObject& value) {
            const QJsonObject payload = value.value(QStringLiteral("payload")).toObject();
            return payload.value(QStringLiteral("commandId")).toString() == commandId
                && payload.value(QStringLiteral("duplicate")).toBool();
        });
    }));
    client.close();
    fixture.gateway->close();
}

TEST(WebSocketIntegrationTest, DuplicateCommandBypassesExhaustedRateLimit) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.start(8, 1, 1)) << fixture.lastError.toStdString();
    QWebSocket client;
    QVector<QJsonObject> received;
    QObject::connect(&client, &QWebSocket::textMessageReceived, [&](const QString& text) {
        received.push_back(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(fixture.gateway->serverPort())));
    ASSERT_TRUE(waitUntil([&] { return client.state() == QAbstractSocket::ConnectedState; }));
    client.sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(ProtocolCodec::envelope(
        QString::fromLatin1(ProtocolType::Hello), {{QStringLiteral("token"), kRedToken}},
        QStringLiteral("hello-rate"), {}, QStringLiteral("client-rate")))));
    ASSERT_TRUE(waitUntil([&] { return fixture.metrics.authenticatedConnections == 1; }));

    auto commandEnvelope = [](const QString& commandId, const QString& messageId) {
        return ProtocolCodec::envelope(
            QString::fromLatin1(ProtocolType::Command),
            {{QStringLiteral("commandId"), commandId},
             {QStringLiteral("action"), QString::fromLatin1(CommandAction::SetSpeed)},
             {QStringLiteral("args"), QJsonObject{{QStringLiteral("unitId"), QStringLiteral("red_r1")},
                                                   {QStringLiteral("speed"), 77.0}}}},
            messageId, {}, QStringLiteral("client-rate"), -1, 1);
    };
    client.sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(
        commandEnvelope(QStringLiteral("limited-1"), QStringLiteral("command-a")))));
    client.sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(
        commandEnvelope(QStringLiteral("limited-1"), QStringLiteral("command-b")))));
    client.sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(
        commandEnvelope(QStringLiteral("limited-2"), QStringLiteral("command-c")))));

    ASSERT_TRUE(waitUntil([&] {
        bool accepted = false;
        bool duplicate = false;
        bool limited = false;
        for (const auto& envelope : received) {
            const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
            if (payload.value(QStringLiteral("commandId")).toString() == QLatin1String("limited-1")) {
                accepted |= payload.value(QStringLiteral("accepted")).toBool();
                duplicate |= payload.value(QStringLiteral("duplicate")).toBool();
            }
            if (payload.value(QStringLiteral("commandId")).toString() == QLatin1String("limited-2")) {
                limited |= payload.value(QStringLiteral("code")).toString()
                    == QLatin1String(CommandCode::RateLimited);
            }
        }
        return accepted && duplicate && limited;
    }));
    EXPECT_DOUBLE_EQ(fixture.room->engine()->unit(QStringLiteral("red_r1"))->speed(), 77.0);
    EXPECT_EQ(fixture.metrics.commandsAccepted, 1);
    EXPECT_EQ(fixture.metrics.rateLimited, 1);
    client.close();
    fixture.gateway->close();
}

TEST(WebSocketIntegrationTest, ThreeProtocolViolationsCloseConnection) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.start()) << fixture.lastError.toStdString();
    QWebSocket client;
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(fixture.gateway->serverPort())));
    ASSERT_TRUE(waitUntil([&] { return client.state() == QAbstractSocket::ConnectedState; }));
    client.sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(ProtocolCodec::envelope(
        QString::fromLatin1(ProtocolType::Hello), {{QStringLiteral("token"), kRedToken}},
        QStringLiteral("hello-errors"), {}, QStringLiteral("client-errors")))));
    ASSERT_TRUE(waitUntil([&] { return fixture.metrics.authenticatedConnections == 1; }));
    for (int i = 0; i < 3; ++i) {
        client.sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(ProtocolCodec::envelope(
            QString::fromLatin1(ProtocolType::Welcome), {},
            QStringLiteral("invalid-%1").arg(i), {}, QStringLiteral("client-errors")))));
    }
    ASSERT_TRUE(waitUntil([&] {
        return client.state() == QAbstractSocket::UnconnectedState;
    }));
    EXPECT_EQ(fixture.metrics.protocolErrors, 3);
    EXPECT_EQ(fixture.metrics.authenticatedConnections, 0);
    fixture.gateway->close();
}

TEST(WebSocketIntegrationTest, PacketFloodClosesOnlyTheOffendingConnection) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.start()) << fixture.lastError.toStdString();
    QWebSocket client;
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(fixture.gateway->serverPort())));
    ASSERT_TRUE(waitUntil([&] { return client.state() == QAbstractSocket::ConnectedState; }));
    client.sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(ProtocolCodec::envelope(
        QString::fromLatin1(ProtocolType::Hello), {{QStringLiteral("token"), kRedToken}},
        QStringLiteral("hello-flood"), {}, QStringLiteral("client-flood")))));
    ASSERT_TRUE(waitUntil([&] { return fixture.metrics.authenticatedConnections == 1; }));
    for (int i = 0; i < 250; ++i) {
        client.sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(ProtocolCodec::envelope(
            QString::fromLatin1(ProtocolType::Pong), {},
            QStringLiteral("flood-%1").arg(i), {}, QStringLiteral("client-flood")))));
    }
    ASSERT_TRUE(waitUntil([&] {
        return client.state() == QAbstractSocket::UnconnectedState;
    }, 5000));
    EXPECT_EQ(fixture.metrics.rateLimited, 1);
    EXPECT_EQ(fixture.metrics.activeConnections, 0);
    fixture.gateway->close();
}

TEST(WebSocketIntegrationTest, RejectsUnexpectedWebSocketPath) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.start()) << fixture.lastError.toStdString();
    QWebSocket client;
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/not-ws")
                         .arg(fixture.gateway->serverPort())));
    ASSERT_TRUE(waitUntil([&] {
        return client.state() == QAbstractSocket::UnconnectedState;
    }));
    EXPECT_EQ(fixture.metrics.activeConnections, 0);
    fixture.gateway->close();
}

TEST(WebSocketIntegrationTest, SupportsThirtyTwoAuthenticatedConnections) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.start(32)) << fixture.lastError.toStdString();
    std::vector<std::unique_ptr<QWebSocket>> clients;
    clients.reserve(32);
    for (int i = 0; i < 32; ++i) {
        auto client = std::make_unique<QWebSocket>();
        QWebSocket* socket = client.get();
        QObject::connect(socket, &QWebSocket::connected, [socket, i] {
            socket->sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(
                ProtocolCodec::envelope(
                    QString::fromLatin1(ProtocolType::Hello),
                    {{QStringLiteral("token"), kRedToken}},
                    QStringLiteral("hello-load-%1").arg(i), {},
                    QStringLiteral("client-load-%1").arg(i)))));
        });
        socket->open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws")
                              .arg(fixture.gateway->serverPort())));
        clients.push_back(std::move(client));
    }
    ASSERT_TRUE(waitUntil([&] {
        return fixture.metrics.authenticatedConnections == 32;
    }, 5000));
    EXPECT_EQ(fixture.metrics.activeConnections, 32);
    ASSERT_TRUE(waitUntil([&] { return fixture.metrics.deltasSent > 0; }));
    for (const auto& client : clients) client->close();
    ASSERT_TRUE(waitUntil([&] { return fixture.metrics.activeConnections == 0; }));
    fixture.gateway->close();
}

TEST(RemoteSessionAdapterIntegrationTest, ReceivesProjectedStateWithoutLocalEngine) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.start()) << fixture.lastError.toStdString();
    RemoteSessionAdapter remote;
    remote.connectToServer(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(fixture.gateway->serverPort())),
                           kRedToken);
    ASSERT_TRUE(waitUntil([&] { return remote.connected() && !remote.allUnits().isEmpty(); }));
    EXPECT_EQ(remote.localEngine(), nullptr);
    for (const auto& value : remote.allUnits()) {
        EXPECT_EQ(value.toObject().value(QStringLiteral("side")).toString(), QStringLiteral("red"));
    }
    const QVariantMap queued = remote.command(
        QString::fromLatin1(CommandAction::SetSpeed),
        {{QStringLiteral("unitId"), QStringLiteral("red_r1")},
         {QStringLiteral("speed"), 66.0}});
    EXPECT_TRUE(queued.value(QStringLiteral("accepted")).toBool());
    EXPECT_TRUE(queued.value(QStringLiteral("pending")).toBool());
    ASSERT_TRUE(waitUntil([&] {
        return remote.unitAt(QStringLiteral("red_r1")).value(QStringLiteral("speed")).toDouble()
                == 66.0
            && fixture.metrics.deltasSent > 0;
    }));
    EXPECT_TRUE(remote.lastError().isEmpty()) << remote.lastError().toStdString();
    remote.disconnectFromServer();
    fixture.gateway->close();
}

TEST(RemoteSessionAdapterIntegrationTest, ReconnectResendsPendingCommandWithSameId) {
    QWebSocketServer server(QStringLiteral("pending-command-test"),
                            QWebSocketServer::NonSecureMode);
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0))
        << server.errorString().toStdString();
    SimulationEngine engine;
    engine.loadDefaultScenario();
    QString firstCommandId;
    QString retriedCommandId;
    int connectionNumber = 0;
    QObject::connect(&server, &QWebSocketServer::newConnection, [&] {
        QWebSocket* socket = server.nextPendingConnection();
        ASSERT_NE(socket, nullptr);
        const int thisConnection = ++connectionNumber;
        QObject::connect(socket, &QWebSocket::textMessageReceived,
                         [&, socket, thisConnection](const QString& text) {
            const QJsonObject request = QJsonDocument::fromJson(text.toUtf8()).object();
            const QString type = request.value(QStringLiteral("type")).toString();
            if (type == QLatin1String(ProtocolType::Hello)) {
                const QJsonObject welcome = ProtocolCodec::envelope(
                    QString::fromLatin1(ProtocolType::Welcome),
                    {{QStringLiteral("userId"), QStringLiteral("user-red")},
                     {QStringLiteral("role"), QStringLiteral("red")},
                     {QStringLiteral("side"), QStringLiteral("red")},
                     {QStringLiteral("roomId"), QStringLiteral("main")}},
                    QStringLiteral("welcome-%1").arg(thisConnection),
                    QStringLiteral("main"), QStringLiteral("fake-client"), 1, 1, 0);
                socket->sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(welcome)));
                QJsonObject snapshotPayload = VisibleStateProjector::project(
                    engine, identity(QStringLiteral("red"), QStringLiteral("red")), 1, 0, 2);
                const QJsonObject snapshot = ProtocolCodec::envelope(
                    QString::fromLatin1(ProtocolType::Snapshot), snapshotPayload,
                    QStringLiteral("snapshot-%1").arg(thisConnection),
                    QStringLiteral("main"), QStringLiteral("fake-client"), 2, 1, 0);
                socket->sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(snapshot)));
            } else if (type == QLatin1String(ProtocolType::Command)) {
                const QString commandId = request.value(QStringLiteral("payload")).toObject()
                                              .value(QStringLiteral("commandId")).toString();
                if (thisConnection == 1) {
                    firstCommandId = commandId;
                    socket->close(QWebSocketProtocol::CloseCodeGoingAway,
                                  QStringLiteral("模拟回执丢失"));
                } else {
                    retriedCommandId = commandId;
                }
            }
        });
    });

    RemoteSessionAdapter remote;
    remote.connectToServer(
        QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort())), kRedToken);
    ASSERT_TRUE(waitUntil([&] { return remote.connected() && !remote.allUnits().isEmpty(); }));
    const QVariantMap queued = remote.command(
        QString::fromLatin1(CommandAction::SetSpeed),
        {{QStringLiteral("unitId"), QStringLiteral("red_r1")},
         {QStringLiteral("speed"), 88.0}});
    ASSERT_TRUE(queued.value(QStringLiteral("pending")).toBool());
    ASSERT_TRUE(waitUntil([&] {
        return !firstCommandId.isEmpty() && retriedCommandId == firstCommandId;
    }, 5000));
    EXPECT_EQ(connectionNumber, 2);
    remote.disconnectFromServer();
    server.close();
}

TEST(RemoteSessionAdapterIntegrationTest, AuthenticationFailureDoesNotReconnectForever) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.start()) << fixture.lastError.toStdString();
    RemoteSessionAdapter remote;
    remote.connectToServer(
        QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(fixture.gateway->serverPort())),
        QStringLiteral("wrong-token-0123456789-0123456789-abcdef"));
    ASSERT_TRUE(waitUntil([&] {
        return fixture.metrics.authFailures == 1 && !remote.lastError().isEmpty();
    }));
    EXPECT_FALSE(waitUntil([&] { return fixture.metrics.authFailures > 1; }, 1500));
    EXPECT_EQ(fixture.metrics.authFailures, 1);
    remote.disconnectFromServer();
    fixture.gateway->close();
}
