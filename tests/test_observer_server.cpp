#include <gtest/gtest.h>

#include "core/UnitBase.h"
#include "protocol/Protocol.h"
#include "protocol/StateDelta.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebSocket>

#include <algorithm>
#include <functional>
#include <memory>

#define private public
#include "GameServer.h"
#undef private

using namespace gbr;

namespace {

bool waitFor(const std::function<bool()>& condition, int timeoutMs = 1500) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return condition();
}

QJsonObject roomConfig(const QString& status, const QString& mode = QStringLiteral("pvp"),
                       bool enabled = true) {
    return QJsonObject{{QStringLiteral("roomId"), QStringLiteral("main")},
                       {QStringLiteral("name"), QStringLiteral("Observer Room")},
                       {QStringLiteral("status"), status},
                       {QStringLiteral("enabled"), enabled},
                       {QStringLiteral("hostedByGameServer"), true},
                       {QStringLiteral("mode"), mode},
                       {QStringLiteral("aiDifficulty"), QStringLiteral("normal")},
                       {QStringLiteral("configVersion"), 1}};
}

class ControlPlane final : public QObject {
public:
    explicit ControlPlane(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                const auto request = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, request]() {
                    request->append(socket->readAll());
                    if (!request->contains(QByteArrayLiteral("\r\n\r\n"))
                        || socket->property("responded").toBool()) {
                        return;
                    }
                    socket->setProperty("responded", true);
                    respond(socket, *request);
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    quint16 port() const { return m_server.serverPort(); }
    void setRooms(const QJsonArray& rooms) { m_rooms = rooms; }
    void setKickRequests(const QJsonArray& requests) { m_kickRequests = requests; }

private:
    void respond(QTcpSocket* socket, const QByteArray& request) {
        QByteArray body;
        if (request.startsWith(QByteArrayLiteral("POST /api/internal/session "))) {
            body = QByteArrayLiteral(
                R"({"valid":true,"userId":3,"username":"observer","displayName":"Observer"})");
        } else if (request.startsWith(QByteArrayLiteral("GET /api/internal/rooms "))) {
            body = QJsonDocument(QJsonObject{{QStringLiteral("rooms"), m_rooms},
                                             {QStringLiteral("kickRequests"), m_kickRequests},
                                             {QStringLiteral("logoutRequests"), QJsonArray{}}})
                       .toJson(QJsonDocument::Compact);
        } else {
            body = QByteArrayLiteral("{}");
        }
        socket->write(QByteArrayLiteral(
                          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ")
                      + QByteArray::number(body.size())
                      + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QJsonArray m_rooms;
    QJsonArray m_kickRequests;
};

void configureEnvironment(const QTemporaryDir& temporary, quint16 controlPort) {
    qputenv("AI_PROVIDER", QByteArrayLiteral("rules"));
    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL",
            QByteArrayLiteral("http://127.0.0.1:") + QByteArray::number(controlPort));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());
}

void stopBackgroundTimers(GameServer& server) {
    server.m_snapshotTimer.stop();
    server.m_sessionValidationTimer.stop();
    server.m_roomSyncTimer.stop();
    server.m_presenceTimer.stop();
    server.m_monitorStatusTimer.stop();
    server.m_checkpointTimer.stop();
    server.m_aiDecisionTimer.stop();
    server.m_aiProbeTimer.stop();
}

void sendClientEnvelope(QWebSocket& socket, const QString& type, const QString& messageId,
                        const QJsonObject& payload = {}) {
    const QJsonObject envelope = Protocol::makeClientEnvelope(type, messageId, payload);
    const Protocol::ValidationResult validation = Protocol::validateClientEnvelope(envelope);
    ASSERT_TRUE(validation.valid) << type.toStdString() << ": "
                                  << validation.message.toStdString();
    socket.sendTextMessage(QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact)));
}

QJsonObject latestPayload(const QList<QJsonObject>& messages, const QString& type,
                          const std::function<bool(const QJsonObject&)>& predicate = {}) {
    for (auto it = messages.crbegin(); it != messages.crend(); ++it) {
        if (it->value(QStringLiteral("type")).toString() != type) continue;
        const QJsonObject payload = it->value(QStringLiteral("payload")).toObject();
        if (!predicate || predicate(payload)) return payload;
    }
    return {};
}

QWebSocket* openAndAuthenticate(GameServer& server, QWebSocket& client,
                                QList<QJsonObject>& messages) {
    QObject::connect(&client, &QWebSocket::textMessageReceived, &client,
                     [&messages](const QString& text) {
        messages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort())));
    if (!waitFor([&server]() { return server.m_clients.size() == 1; })) return nullptr;
    QWebSocket* socket = server.m_clients.keys().constFirst();
    sendClientEnvelope(client, QStringLiteral("auth"), QStringLiteral("auth-1"),
                       QJsonObject{{QStringLiteral("token"), QStringLiteral("observer-token")}});
    if (!waitFor([&server, socket]() {
            return server.m_clients.contains(socket)
                && server.m_clients.value(socket).authenticated;
        })) {
        return nullptr;
    }
    return socket;
}

QJsonObject joinObserver(GameServer& server, QWebSocket* socket, QWebSocket& client,
                         QList<QJsonObject>& messages,
                         const QString& messageId = QStringLiteral("observe-1")) {
    sendClientEnvelope(client, QStringLiteral("joinRoom"), messageId,
                       QJsonObject{{QStringLiteral("roomId"), server.m_roomId},
                                   {QStringLiteral("asObserver"), true}});
    if (!waitFor([&server, socket, &messages]() {
            return server.m_clients.contains(socket) && server.m_clients.value(socket).observer
                && !latestPayload(messages, QStringLiteral("snapshot"), [](const QJsonObject& payload) {
                       return payload.value(QStringLiteral("roomState")).toObject()
                           .value(QStringLiteral("observer")).toBool();
                   }).isEmpty();
        })) {
        return {};
    }
    return latestPayload(messages, QStringLiteral("snapshot"), [](const QJsonObject& payload) {
        return payload.value(QStringLiteral("roomState")).toObject()
            .value(QStringLiteral("observer")).toBool();
    });
}

struct ValidMessage {
    QString type;
    QJsonObject payload;
};

QList<ValidMessage> observerWriteMessages() {
    const QJsonObject point{{QStringLiteral("x"), 10.0}, {QStringLiteral("y"), 20.0}};
    return {
        {QStringLiteral("auth"), QJsonObject{{QStringLiteral("token"), QStringLiteral("again")}}},
        {QStringLiteral("joinRoom"), QJsonObject{{QStringLiteral("roomId"), QStringLiteral("main")}}},
        {QStringLiteral("command"),
         QJsonObject{{QStringLiteral("commandId"), QStringLiteral("observer-command")},
                     {QStringLiteral("action"), QStringLiteral("halt")},
                     {QStringLiteral("args"),
                      QJsonObject{{QStringLiteral("unitId"), QStringLiteral("red_cp")}}},
                     {QStringLiteral("stateRevision"), 1}}},
        {QStringLiteral("control"), QJsonObject{{QStringLiteral("action"), QStringLiteral("pause")}}},
        {QStringLiteral("setReady"), QJsonObject{{QStringLiteral("ready"), true}}},
        {QStringLiteral("chat"),
         QJsonObject{{QStringLiteral("text"), QStringLiteral("blocked")},
                     {QStringLiteral("recipientSeatIds"),
                      QJsonArray{QStringLiteral("red_commander")}}}},
        {QStringLiteral("scenarioUpsert"),
         QJsonObject{{QStringLiteral("unit"), QJsonObject{{QStringLiteral("id"), QStringLiteral("x")}}}}},
        {QStringLiteral("scenarioRemove"), QJsonObject{{QStringLiteral("unitId"), QStringLiteral("x")}}},
        {QStringLiteral("scenarioReplace"),
         QJsonObject{{QStringLiteral("scenario"), QJsonObject{{QStringLiteral("schemaVersion"), 1}}}}},
        {QStringLiteral("claimSeat"),
         QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_commander")}}},
        {QStringLiteral("releaseSeat"), {}},
        {QStringLiteral("seatReady"), QJsonObject{{QStringLiteral("ready"), true}}},
        {QStringLiteral("deployment"),
         QJsonObject{{QStringLiteral("unitId"), QStringLiteral("red_cp")},
                     {QStringLiteral("position"), point}}},
        {QStringLiteral("requestRedeploy"), {}},
        {QStringLiteral("redeploy"),
         QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_commander")}}},
        {QStringLiteral("shareIntel"),
         QJsonObject{{QStringLiteral("targetId"), QStringLiteral("blue_cp")},
                     {QStringLiteral("recipientSeatIds"),
                      QJsonArray{QStringLiteral("red_commander")}}}},
        {QStringLiteral("mapMark"),
         QJsonObject{{QStringLiteral("position"), point},
                     {QStringLiteral("label"), QStringLiteral("blocked")}}},
        {QStringLiteral("setUnitName"),
         QJsonObject{{QStringLiteral("unitName"), QStringLiteral("blocked")}}},
    };
}

}

TEST(GameServerObserverTest, AuthenticatedObserverJoinsBothModesInEveryLifecyclePhase) {
    const QStringList modes{QStringLiteral("pvp"), QStringLiteral("pve")};
    const QStringList phases{QStringLiteral("preparing"), QStringLiteral("running"),
                             QStringLiteral("paused"), QStringLiteral("finished")};
    for (const QString& mode : modes) {
        for (const QString& phase : phases) {
            SCOPED_TRACE(QStringLiteral("%1/%2").arg(mode, phase).toStdString());
            QTemporaryDir temporary;
            ASSERT_TRUE(temporary.isValid());
            ControlPlane control;
            ASSERT_TRUE(control.listen());
            control.setRooms(QJsonArray{roomConfig(phase, mode)});
            configureEnvironment(temporary, control.port());

            GameServer server;
            ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
            server.m_phase = phase;
            ASSERT_TRUE(server.listen(0));
            stopBackgroundTimers(server);
            const quint64 roomRevision = server.m_authoritativeRoom.revision();

            QList<QJsonObject> messages;
            QWebSocket client;
            QWebSocket* socket = openAndAuthenticate(server, client, messages);
            ASSERT_NE(socket, nullptr);
            const QJsonObject snapshot = joinObserver(server, socket, client, messages);
            ASSERT_FALSE(snapshot.isEmpty());
            const GameServer::ClientSession session = server.m_clients.value(socket);
            EXPECT_EQ(session.roomId, server.m_roomId);
            EXPECT_TRUE(session.seatId.isEmpty());
            EXPECT_TRUE(session.side.isEmpty());
            EXPECT_TRUE(session.observer);
            EXPECT_EQ(server.m_authoritativeRoom.revision(), roomRevision);
            EXPECT_TRUE(server.roomOccupants().isEmpty());
            EXPECT_EQ(snapshot.value(QStringLiteral("roomState")).toObject()
                          .value(QStringLiteral("roomMode")).toString(), mode);
            EXPECT_TRUE(latestPayload(messages, QStringLiteral("seatState")).isEmpty());

            client.close();
            ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
            EXPECT_EQ(server.m_authoritativeRoom.revision(), roomRevision);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }
}

TEST(GameServerObserverTest, RejectsStoppedDisabledAndDeletedRooms) {
    struct Case {
        const char* name;
        QJsonArray rooms;
    };
    const QList<Case> cases{
        {"stopped", QJsonArray{roomConfig(QStringLiteral("stopped"))}},
        {"disabled", QJsonArray{roomConfig(QStringLiteral("running"), QStringLiteral("pvp"), false)}},
        {"deleted", QJsonArray{}},
    };
    for (const Case& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        QTemporaryDir temporary;
        ASSERT_TRUE(temporary.isValid());
        ControlPlane control;
        ASSERT_TRUE(control.listen());
        control.setRooms(testCase.rooms);
        configureEnvironment(temporary, control.port());
        GameServer server;
        ASSERT_TRUE(server.listen(0));
        stopBackgroundTimers(server);
        server.m_phase = QStringLiteral("running");
        server.m_roomStatus = QStringLiteral("running");

        QList<QJsonObject> messages;
        QWebSocket client;
        QWebSocket* socket = openAndAuthenticate(server, client, messages);
        ASSERT_NE(socket, nullptr);
        sendClientEnvelope(client, QStringLiteral("joinRoom"), QStringLiteral("observe-rejected"),
                           QJsonObject{{QStringLiteral("roomId"), server.m_roomId},
                                       {QStringLiteral("asObserver"), true}});
        ASSERT_TRUE(waitFor([&messages]() {
            return latestPayload(messages, QStringLiteral("error"))
                       .value(QStringLiteral("code")).toString()
                == QLatin1String("ROOM_CLOSED");
        }));
        EXPECT_FALSE(server.m_clients.value(socket).observer);
        EXPECT_TRUE(server.m_clients.value(socket).roomId.isEmpty());
        client.close();
        ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

TEST(GameServerObserverTest, ReadOnlyGateCoversEveryWriteAndAllowsTransportOperations) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ControlPlane control;
    ASSERT_TRUE(control.listen());
    control.setRooms(QJsonArray{roomConfig(QStringLiteral("preparing"))});
    configureEnvironment(temporary, control.port());
    GameServer server;
    ASSERT_TRUE(server.listen(0));
    stopBackgroundTimers(server);

    QList<QJsonObject> messages;
    QWebSocket client;
    QWebSocket* socket = openAndAuthenticate(server, client, messages);
    ASSERT_NE(socket, nullptr);
    ASSERT_FALSE(joinObserver(server, socket, client, messages).isEmpty());
    const quint64 roomRevision = server.m_authoritativeRoom.revision();
    const quint64 scenarioRevision = server.m_scenarioRevision;

    const QList<ValidMessage> writes = observerWriteMessages();
    for (qsizetype index = 0; index < writes.size(); ++index) {
        sendClientEnvelope(client, writes.at(index).type,
                           QStringLiteral("blocked-%1").arg(index), writes.at(index).payload);
    }
    ASSERT_TRUE(waitFor([&messages, expected = writes.size()]() {
        return std::count_if(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
            const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
            return envelope.value(QStringLiteral("type")).toString() == QLatin1String("error")
                && payload.value(QStringLiteral("code")).toString()
                    == QLatin1String("OBSERVER_READ_ONLY");
        }) >= expected;
    }));
    for (qsizetype index = 0; index < writes.size(); ++index) {
        const QString requestId = QStringLiteral("blocked-%1").arg(index);
        EXPECT_FALSE(latestPayload(messages, QStringLiteral("error"),
                                   [&requestId](const QJsonObject& payload) {
            return payload.value(QStringLiteral("code")).toString()
                    == QLatin1String("OBSERVER_READ_ONLY")
                && payload.value(QStringLiteral("requestId")).toString() == requestId;
        }).isEmpty()) << writes.at(index).type.toStdString();
    }
    EXPECT_EQ(server.m_authoritativeRoom.revision(), roomRevision);
    EXPECT_EQ(server.m_scenarioRevision, scenarioRevision);

    const int pongCountBefore = std::count_if(
        messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
            return envelope.value(QStringLiteral("type")).toString() == QLatin1String("pong");
        });
    sendClientEnvelope(client, QStringLiteral("ping"), QStringLiteral("allowed-ping"));
    sendClientEnvelope(client, QStringLiteral("heartbeat"), QStringLiteral("allowed-heartbeat"));
    ASSERT_TRUE(waitFor([&messages, pongCountBefore]() {
        return std::count_if(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
            return envelope.value(QStringLiteral("type")).toString() == QLatin1String("pong");
        }) >= pongCountBefore + 2;
    }));

    const int observerSnapshotsBefore = std::count_if(
        messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
            return envelope.value(QStringLiteral("type")).toString() == QLatin1String("snapshot")
                && envelope.value(QStringLiteral("payload")).toObject()
                       .value(QStringLiteral("roomState")).toObject()
                       .value(QStringLiteral("observer")).toBool();
        });
    sendClientEnvelope(client, QStringLiteral("resyncRequest"), QStringLiteral("allowed-resync"),
                       QJsonObject{{QStringLiteral("stateRevision"),
                                    static_cast<qint64>(server.m_stateRevision)}});
    ASSERT_TRUE(waitFor([&messages, observerSnapshotsBefore]() {
        return std::count_if(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
            return envelope.value(QStringLiteral("type")).toString() == QLatin1String("snapshot")
                && envelope.value(QStringLiteral("payload")).toObject()
                       .value(QStringLiteral("roomState")).toObject()
                       .value(QStringLiteral("observer")).toBool();
        }) > observerSnapshotsBefore;
    }));
    sendClientEnvelope(client, QStringLiteral("roomList"), QStringLiteral("allowed-room-list"));
    ASSERT_TRUE(waitFor([&messages]() {
        return !latestPayload(messages, QStringLiteral("roomDirectory")).isEmpty();
    }));
    sendClientEnvelope(client, QStringLiteral("leaveRoom"), QStringLiteral("allowed-leave"));
    ASSERT_TRUE(waitFor([&server, socket]() {
        return server.m_clients.contains(socket) && !server.m_clients.value(socket).observer
            && server.m_clients.value(socket).roomId.isEmpty();
    }));
    EXPECT_EQ(server.m_authoritativeRoom.revision(), roomRevision);
    client.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

TEST(GameServerObserverTest, DeltaAndResyncRemainContiguousAndBilateral) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ControlPlane control;
    ASSERT_TRUE(control.listen());
    control.setRooms(QJsonArray{roomConfig(QStringLiteral("running"))});
    configureEnvironment(temporary, control.port());
    GameServer server;
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    1, QStringLiteral("red"), QStringLiteral("red_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    2, QStringLiteral("blue"), QStringLiteral("blue_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    1, QStringLiteral("red_commander"), GeoPos{100.0, 100.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    2, QStringLiteral("blue_commander"), GeoPos{500.0, 500.0, 0.0}).ok);
    ASSERT_TRUE(server.applyDeployedScenario());
    server.m_phase = QStringLiteral("running");
    ASSERT_TRUE(server.listen(0));
    stopBackgroundTimers(server);

    QList<QJsonObject> messages;
    QWebSocket client;
    QWebSocket* socket = openAndAuthenticate(server, client, messages);
    ASSERT_NE(socket, nullptr);
    QJsonObject reconstructed = joinObserver(server, socket, client, messages);
    ASSERT_FALSE(reconstructed.isEmpty());
    const QJsonArray units = reconstructed.value(QStringLiteral("units")).toArray();
    EXPECT_TRUE(std::any_of(units.cbegin(), units.cend(), [](const QJsonValue& value) {
        return value.toObject().value(QStringLiteral("side")).toString() == QLatin1String("red");
    }));
    EXPECT_TRUE(std::any_of(units.cbegin(), units.cend(), [](const QJsonValue& value) {
        return value.toObject().value(QStringLiteral("side")).toString() == QLatin1String("blue");
    }));

    const AuthoritativeRoom::Seat redCommander =
        server.m_authoritativeRoom.seat(QStringLiteral("red_commander"));
    ASSERT_TRUE(redCommander.deployed);
    ASSERT_FALSE(redCommander.unitId.isEmpty());
    UnitBase* red = server.m_engine.unit(redCommander.unitId);
    ASSERT_NE(red, nullptr);
    red->setPosition(GeoPos{red->pos().x + 25.0, red->pos().y + 15.0, red->pos().alt});
    server.broadcastSnapshots();
    ASSERT_TRUE(waitFor([&messages]() {
        return !latestPayload(messages, QStringLiteral("delta")).isEmpty();
    }));
    const QJsonObject delta = latestPayload(messages, QStringLiteral("delta"));
    QString error;
    ASSERT_TRUE(StateDelta::apply(reconstructed, delta, &error)) << error.toStdString();
    EXPECT_EQ(reconstructed, server.snapshotFor(server.m_clients.value(socket)));

    sendClientEnvelope(client, QStringLiteral("resyncRequest"), QStringLiteral("resync-after-delta"),
                       QJsonObject{{QStringLiteral("stateRevision"),
                                    reconstructed.value(QStringLiteral("stateRevision"))}});
    ASSERT_TRUE(waitFor([&messages, revision = reconstructed.value(QStringLiteral("stateRevision"))]() {
        return !latestPayload(messages, QStringLiteral("snapshot"), [revision](const QJsonObject& payload) {
            return payload.value(QStringLiteral("stateRevision")) == revision
                && payload.value(QStringLiteral("roomState")).toObject()
                    .value(QStringLiteral("observer")).toBool();
        }).isEmpty();
    }));
    EXPECT_EQ(latestPayload(messages, QStringLiteral("snapshot"),
                            [](const QJsonObject& payload) {
                                return payload.value(QStringLiteral("roomState")).toObject()
                                    .value(QStringLiteral("observer")).toBool();
                            }), reconstructed);
    client.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

TEST(GameServerObserverTest, StopDeleteAndKickReturnObserverToDirectory) {
    for (const QString& action : {QStringLiteral("stop"), QStringLiteral("delete"),
                                  QStringLiteral("kick")}) {
        SCOPED_TRACE(action.toStdString());
        QTemporaryDir temporary;
        ASSERT_TRUE(temporary.isValid());
        ControlPlane control;
        ASSERT_TRUE(control.listen());
        control.setRooms(QJsonArray{roomConfig(QStringLiteral("running"))});
        configureEnvironment(temporary, control.port());
        GameServer server;
        server.m_phase = QStringLiteral("running");
        ASSERT_TRUE(server.listen(0));
        stopBackgroundTimers(server);

        QList<QJsonObject> messages;
        QWebSocket client;
        QWebSocket* socket = openAndAuthenticate(server, client, messages);
        ASSERT_NE(socket, nullptr);
        ASSERT_FALSE(joinObserver(server, socket, client, messages).isEmpty());
        if (action == QLatin1String("stop")) {
            control.setRooms(QJsonArray{roomConfig(QStringLiteral("stopped"))});
        } else if (action == QLatin1String("delete")) {
            control.setRooms({});
        } else {
            control.setKickRequests(QJsonArray{
                QJsonObject{{QStringLiteral("id"), 7},
                            {QStringLiteral("roomId"), server.m_roomId},
                            {QStringLiteral("userId"), 3},
                            {QStringLiteral("reason"), QStringLiteral("observer kicked")}}});
        }
        sendClientEnvelope(client, QStringLiteral("roomList"), QStringLiteral("lifecycle-refresh"));
        ASSERT_TRUE(waitFor([&server, socket]() {
            return server.m_clients.contains(socket) && !server.m_clients.value(socket).observer
                && server.m_clients.value(socket).roomId.isEmpty();
        }));
        ASSERT_TRUE(waitFor([&messages]() {
            return !latestPayload(messages, QStringLiteral("event"), [](const QJsonObject& payload) {
                return payload.value(QStringLiteral("kind")).toString()
                    == QLatin1String("roomClosed");
            }).isEmpty();
        }));
        ASSERT_TRUE(waitFor([&messages]() {
            return !latestPayload(messages, QStringLiteral("roomDirectory")).isEmpty();
        }));
        client.close();
        ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

TEST(GameServerObserverTest, LastParticipantDisconnectResetsWhileObserverRemains) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ControlPlane control;
    ASSERT_TRUE(control.listen());
    control.setRooms(QJsonArray{roomConfig(QStringLiteral("running"))});
    configureEnvironment(temporary, control.port());
    GameServer server;
    ASSERT_TRUE(server.listen(0));
    stopBackgroundTimers(server);

    QList<QJsonObject> observerMessages;
    QWebSocket observerClient;
    QWebSocket* observerSocket = openAndAuthenticate(server, observerClient, observerMessages);
    ASSERT_NE(observerSocket, nullptr);
    ASSERT_FALSE(joinObserver(server, observerSocket, observerClient, observerMessages).isEmpty());

    QWebSocket participantClient;
    participantClient.open(QUrl(QStringLiteral("ws://127.0.0.1:%1")
                                    .arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 2; }));
    QWebSocket* participantSocket = nullptr;
    for (QWebSocket* candidate : server.m_clients.keys()) {
        if (candidate != observerSocket) participantSocket = candidate;
    }
    ASSERT_NE(participantSocket, nullptr);
    GameServer::ClientSession& participant = server.m_clients[participantSocket];
    participant.authenticated = true;
    participant.userId = 1;
    participant.username = QStringLiteral("red");
    participant.displayName = QStringLiteral("Red");
    participant.role = QStringLiteral("player");
    participant.roomId = server.m_roomId;
    participant.seatId = QStringLiteral("red_commander");
    participant.seatType = QStringLiteral("commander");
    participant.side = QStringLiteral("red");
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    1, QStringLiteral("red"), QStringLiteral("red_commander"),
                    QStringLiteral("commandpost")).ok);
    server.syncAuthoritativeSeats();
    server.m_phase = QStringLiteral("running");
    server.m_roomStatus = QStringLiteral("running");

    participantClient.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    EXPECT_TRUE(server.m_clients.contains(observerSocket));
    EXPECT_TRUE(server.m_clients.value(observerSocket).observer);
    EXPECT_EQ(server.m_clients.value(observerSocket).roomId, server.m_roomId);
    EXPECT_TRUE(server.m_authoritativeRoom.seats().isEmpty());
    EXPECT_EQ(server.m_phase, QStringLiteral("preparing"));
    EXPECT_TRUE(server.roomOccupants().isEmpty());
    ASSERT_TRUE(waitFor([&observerMessages]() {
        return !latestPayload(observerMessages, QStringLiteral("snapshot"),
                              [](const QJsonObject& payload) {
            const QJsonObject room = payload.value(QStringLiteral("roomState")).toObject();
            return room.value(QStringLiteral("observer")).toBool()
                && room.value(QStringLiteral("phase")).toString()
                    == QLatin1String("preparing");
        }).isEmpty();
    }));

    observerClient.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
