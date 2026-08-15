#include <gtest/gtest.h>

#include "view/SimulationController.h"
#include "protocol/Protocol.h"

#include <QJsonArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>

#include <algorithm>
#include <functional>
#include <memory>

using namespace gbr;

namespace gbr {

class SimulationControllerTestPeer {
public:
    static QNetworkReply* loginReply(SimulationController& controller) {
        return controller.m_networkClient.m_loginReply;
    }

    static QString loginTokenCandidate(SimulationController& controller) {
        return controller.m_networkClient.m_loginTokenCandidate;
    }

    static void applyOnlineSnapshot(SimulationController& controller,
                                    const QJsonObject& payload) {
        controller.m_sessionMode = QStringLiteral("online");
        controller.applyRemoteSnapshot(payload);
    }

    static void seedActiveOnlineRoom(SimulationController& controller) {
        controller.m_sessionMode = QStringLiteral("online");
        controller.m_currentRoomId = QStringLiteral("main");
        controller.m_currentSeatId = QStringLiteral("red_commander");
        controller.m_currentSeatType = QStringLiteral("commander");
        controller.m_currentSeatSide = QStringLiteral("red");
        controller.m_onlineStage = QStringLiteral("battle");
        controller.m_seatReady = true;
        controller.m_matchPhase = QStringLiteral("running");
        controller.m_redReady = true;
        controller.m_blueReady = true;
        controller.m_onlineSeats = QVariantList{QVariantMap{
            {QStringLiteral("seatId"), QStringLiteral("red_commander")}}};
        controller.m_onlineMapMarks = QVariantList{QVariantMap{
            {QStringLiteral("label"), QStringLiteral("stale mark")}}};
        controller.m_chatMessages = QVariantList{QVariantMap{
            {QStringLiteral("text"), QStringLiteral("stale chat")}}};
    }

    static void seedOnlineRoomSelection(SimulationController& controller) {
        controller.m_sessionMode = QStringLiteral("online");
        controller.m_onlineStage = QStringLiteral("roomSelect");
        controller.m_currentRoomId.clear();
        controller.m_currentSeatId.clear();
        controller.m_currentSeatType.clear();
        controller.m_currentSeatSide.clear();
    }

    static void receiveRoomClosed(SimulationController& controller) {
        emit controller.m_networkClient.eventReceived(
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("roomClosed")},
                        {QStringLiteral("message"), QStringLiteral("room stopped")}});
    }

    static void receiveMatchReset(SimulationController& controller) {
        emit controller.m_networkClient.eventReceived(
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("matchReset")},
                        {QStringLiteral("message"), QStringLiteral("reset")}});
    }

    static void receiveRoomDirectory(SimulationController& controller,
                                     const QJsonArray& rooms = QJsonArray{}) {
        emit controller.m_networkClient.roomDirectoryReceived(rooms);
    }

    static void setLeaveRoomPending(SimulationController& controller, bool pending) {
        controller.m_leaveRoomPending = pending;
    }

    static void receiveCommandRejected(SimulationController& controller) {
        emit controller.m_networkClient.commandRejected(QStringLiteral("leave rejected"));
    }

    static void seedOnlineIntelRecords(SimulationController& controller,
                                       const QVariantList& records) {
        controller.m_sessionMode = QStringLiteral("online");
        controller.m_onlineIntelRecords = records;
    }

    static void seedPendingObserverJoin(SimulationController& controller) {
        controller.m_sessionMode = QStringLiteral("online");
        controller.m_isObserver = true;
        controller.m_observerJoinPending = true;
        controller.m_currentRoomId = QStringLiteral("main");
        controller.m_onlineStage = QStringLiteral("observer");
    }

    static void receiveSeatState(SimulationController& controller,
                                 const QJsonObject& state) {
        emit controller.m_networkClient.seatStateReceived(state);
    }

    static void receiveTransferEvent(SimulationController& controller,
                                     const QString& kind, qint64 revision = 12) {
        QJsonObject event{{QStringLiteral("kind"), kind},
                          {QStringLiteral("revision"), revision},
                          {QStringLiteral("userId"), 37},
                          {QStringLiteral("sourceSeatId"), QStringLiteral("red_attack_1")},
                          {QStringLiteral("targetSeatId"), QStringLiteral("red_recon_1")},
                          {QStringLiteral("templateId"), QStringLiteral("reconuav")}};
        if (kind != QLatin1String("transferRequested")) {
            event.insert(QStringLiteral("requestRevision"), 12);
        }
        emit controller.m_networkClient.transferEventReceived(event);
    }

    static void receiveForfeit(SimulationController& controller) {
        emit controller.m_networkClient.eventReceived(
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("forfeit")},
                        {QStringLiteral("winner"), QStringLiteral("blue")},
                        {QStringLiteral("loser"), QStringLiteral("red")}});
    }

    static void receiveTextMessage(NetworkClient& client, const QString& text) {
        client.onTextMessage(text);
    }

    static void openNetworkTestSocket(NetworkClient& client, const QUrl& url) {
        client.m_manualClose = false;
        client.m_token = QStringLiteral("network-test-token");
        client.m_webSocketUrl = url;
        client.m_socket.open(url);
    }

    static void markNetworkTestAuthenticated(NetworkClient& client) {
        client.m_authenticated = true;
        client.m_state = QStringLiteral("connected");
    }

    static bool networkTestSocketConnected(NetworkClient& client) {
        return client.m_socket.state() == QAbstractSocket::ConnectedState;
    }

    static quint64 networkTestLastSequence(NetworkClient& client) {
        return client.m_stateStore.lastSequence();
    }

    static bool networkTestWaitingForResync(NetworkClient& client) {
        return client.m_stateStore.waitingForResync();
    }
};

}

namespace {

bool waitFor(const std::function<bool()>& condition, int timeoutMs = 1000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return condition();
}

class ClientAccountReceiver final : public QObject {
public:
    explicit ClientAccountReceiver(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket* socket = m_server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                const QByteArray request = socket->readAll();
                m_requests.append(request);
                if (request.startsWith("POST /api/client/login ")) {
                    if (m_deferLoginReply) {
                        m_loginSocket = socket;
                        return;
                    }
                    writeResponse(socket, QByteArrayLiteral(
                        R"({"token":"lifecycle-token","gameWebSocketUrl":"ws://127.0.0.1:1"})"));
                    return;
                }
                if (request.startsWith("POST /api/client/logout ") && m_deferLogoutReply) {
                    m_logoutSocket = socket;
                    return;
                }
                writeResponse(socket, request.startsWith("POST /api/client/logout ")
                    ? QByteArrayLiteral(R"({})") : QByteArrayLiteral(R"({"status":"ok"})"));
            });
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    quint16 port() const { return m_server.serverPort(); }
    void deferLoginReply() { m_deferLoginReply = true; }
    bool releaseLoginReply() {
        if (!m_loginSocket) return false;
        writeResponse(m_loginSocket, QByteArrayLiteral(
            R"({"token":"lifecycle-token","gameWebSocketUrl":"ws://127.0.0.1:1"})"));
        m_loginSocket = nullptr;
        return true;
    }
    void deferLogoutReply() { m_deferLogoutReply = true; }
    bool releaseLogoutReply() {
        if (!m_logoutSocket) return false;
        writeResponse(m_logoutSocket, QByteArrayLiteral(R"({})"));
        m_logoutSocket = nullptr;
        return true;
    }
    bool hasLoginRequest() const {
        for (const QByteArray& request : m_requests) {
            if (request.startsWith("POST /api/client/login ")) return true;
        }
        return false;
    }
    bool hasLogoutRequest() const {
        for (const QByteArray& request : m_requests) {
            if (request.startsWith("POST /api/client/logout ")
                && request.contains("Authorization: Bearer lifecycle-token")) return true;
        }
        return false;
    }

private:
    void writeResponse(QTcpSocket* socket, const QByteArray& body) {
        socket->write(QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ")
                      + QByteArray::number(body.size())
                      + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QList<QByteArray> m_requests;
    bool m_deferLoginReply = false;
    bool m_deferLogoutReply = false;
    QTcpSocket* m_loginSocket = nullptr;
    QTcpSocket* m_logoutSocket = nullptr;
};

QString serverErrorEnvelopeAtLength(qsizetype targetLength, const QString& message) {
    QJsonObject payload{{QStringLiteral("code"), QStringLiteral("TEST_ERROR")},
                        {QStringLiteral("message"), message},
                        {QStringLiteral("padding"), QString()}};
    QJsonObject envelope = Protocol::makeServerEnvelope(QStringLiteral("error"), 1, payload);
    const QByteArray base = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (base.size() > targetLength) return {};

    payload[QStringLiteral("padding")] =
        QString(targetLength - base.size(), QLatin1Char('a'));
    envelope[QStringLiteral("payload")] = payload;
    return QString::fromLatin1(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
}

QJsonObject networkTestSnapshot(qint64 revision) {
    return {{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
            {QStringLiteral("stateRevision"), revision},
            {QStringLiteral("scenario"),
             QJsonObject{{QStringLiteral("schemaVersion"), 1},
                         {QStringLiteral("map"),
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("test")},
                                      {QStringLiteral("widthMeters"), 1000.0},
                                      {QStringLiteral("heightMeters"), 800.0}}},
                         {QStringLiteral("units"), QJsonArray{}}}},
            {QStringLiteral("units"), QJsonArray{}},
            {QStringLiteral("projectiles"), QJsonArray{}},
            {QStringLiteral("messages"), QJsonArray{}},
            {QStringLiteral("roomState"),
             QJsonObject{{QStringLiteral("scenarioRevision"), 1},
                         {QStringLiteral("simTime"), 0.0}}}};
}

void sendNetworkTestEnvelope(QWebSocket* socket, const QString& type, quint64 sequence,
                             const QJsonObject& payload) {
    socket->sendTextMessage(QString::fromUtf8(
        QJsonDocument(Protocol::makeServerEnvelope(type, sequence, payload))
            .toJson(QJsonDocument::Compact)));
}

}

TEST(SimulationControllerTest, UnitsJsonUsesCanonicalScenarioShape) {
    SimulationController controller;

    const QJsonObject json = controller.unitsJson();

    EXPECT_TRUE(json.value("map").isObject());
    EXPECT_TRUE(json.value("units").isArray());
    EXPECT_TRUE(json.contains("notes"));
}

TEST(NetworkClientTest, RejectsOversizedUtf16TextBeforeParsing) {
    NetworkClient client;
    QString fatalError;
    QString commandRejection;
    QObject::connect(&client, &NetworkClient::fatalError, &client,
                     [&fatalError](const QString& message) { fatalError = message; });
    QObject::connect(&client, &NetworkClient::commandRejected, &client,
                     [&commandRejection](const QString& message) { commandRejection = message; });

    const QString text = serverErrorEnvelopeAtLength(
        Protocol::MaxServerMessageBytes / 3 + 1, QStringLiteral("must not dispatch"));
    ASSERT_EQ(text.size(), Protocol::MaxServerMessageBytes / 3 + 1);

    SimulationControllerTestPeer::receiveTextMessage(client, text);

    EXPECT_EQ(fatalError, QStringLiteral("推演服务器返回的消息超过允许大小"));
    EXPECT_TRUE(commandRejection.isEmpty());
}

TEST(NetworkClientTest, ParsesUtf16TextAtConservativeIngressBoundary) {
    NetworkClient client;
    QString fatalError;
    QString commandRejection;
    QObject::connect(&client, &NetworkClient::fatalError, &client,
                     [&fatalError](const QString& message) { fatalError = message; });
    QObject::connect(&client, &NetworkClient::commandRejected, &client,
                     [&commandRejection](const QString& message) { commandRejection = message; });

    const QString text = serverErrorEnvelopeAtLength(
        Protocol::MaxServerMessageBytes / 3, QStringLiteral("boundary parsed"));
    ASSERT_EQ(text.size(), Protocol::MaxServerMessageBytes / 3);

    SimulationControllerTestPeer::receiveTextMessage(client, text);

    EXPECT_TRUE(fatalError.isEmpty());
    EXPECT_EQ(commandRejection, QStringLiteral("boundary parsed"));
}

TEST(NetworkClientTest, QueuesCommandUntilInitialSnapshotIsApplied) {
    int argc = 1;
    char applicationName[] = "network_command_queue_test";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);

    QWebSocketServer server(QStringLiteral("network command queue test"),
                            QWebSocketServer::NonSecureMode);
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost));
    QWebSocket* serverSocket = nullptr;
    QObject::connect(&server, &QWebSocketServer::newConnection, &server, [&]() {
        serverSocket = server.nextPendingConnection();
    });

    NetworkClient client;
    SimulationControllerTestPeer::openNetworkTestSocket(client, server.serverUrl());
    ASSERT_TRUE(waitFor([&]() {
        return serverSocket != nullptr
            && SimulationControllerTestPeer::networkTestSocketConnected(client);
    }));
    SimulationControllerTestPeer::markNetworkTestAuthenticated(client);

    QStringList messages;
    QObject::connect(serverSocket, &QWebSocket::textMessageReceived, &client,
                     [&messages](const QString& text) { messages.append(text); });
    client.sendCommand(QStringLiteral("activateScan"),
                       QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_r1")}});
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(std::none_of(messages.cbegin(), messages.cend(), [](const QString& text) {
        return QJsonDocument::fromJson(text.toUtf8()).object()
            .value(QStringLiteral("type")).toString() == QLatin1String("command");
    }));

    sendNetworkTestEnvelope(
        serverSocket, QStringLiteral("welcome"), 1,
        QJsonObject{{QStringLiteral("username"), QStringLiteral("red-user")},
                    {QStringLiteral("displayName"), QStringLiteral("红方用户")},
                    {QStringLiteral("role"), QStringLiteral("red")} });
    sendNetworkTestEnvelope(serverSocket, QStringLiteral("snapshot"), 2,
                             networkTestSnapshot(42));
    ASSERT_TRUE(waitFor([&]() {
        return std::any_of(messages.cbegin(), messages.cend(), [](const QString& text) {
            return QJsonDocument::fromJson(text.toUtf8()).object()
                .value(QStringLiteral("type")).toString() == QLatin1String("command");
        });
    }));
    QJsonObject commandEnvelope;
    for (const QString& text : messages) {
        const QJsonObject candidate = QJsonDocument::fromJson(text.toUtf8()).object();
        if (candidate.value(QStringLiteral("type")).toString() == QLatin1String("command")) {
            commandEnvelope = candidate;
            break;
        }
    }
    ASSERT_FALSE(commandEnvelope.isEmpty());
    EXPECT_EQ(commandEnvelope.value(QStringLiteral("payload")).toObject()
                  .value(QStringLiteral("stateRevision")).toInteger(), 42);

    client.close();
    serverSocket->close();
    server.close();
}

TEST(NetworkClientTest, QueuesCommandDuringResyncAndSendsAfterRecoverySnapshot) {
    int argc = 1;
    char applicationName[] = "network_resync_queue_test";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);

    QWebSocketServer server(QStringLiteral("network resync queue test"),
                            QWebSocketServer::NonSecureMode);
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost));
    QWebSocket* serverSocket = nullptr;
    QObject::connect(&server, &QWebSocketServer::newConnection, &server, [&]() {
        serverSocket = server.nextPendingConnection();
    });

    NetworkClient client;
    SimulationControllerTestPeer::openNetworkTestSocket(client, server.serverUrl());
    ASSERT_TRUE(waitFor([&]() {
        return serverSocket != nullptr
            && SimulationControllerTestPeer::networkTestSocketConnected(client);
    }));
    SimulationControllerTestPeer::markNetworkTestAuthenticated(client);
    QStringList messages;
    QObject::connect(serverSocket, &QWebSocket::textMessageReceived, &client,
                     [&messages](const QString& text) { messages.append(text); });

    sendNetworkTestEnvelope(
        serverSocket, QStringLiteral("welcome"), 1,
        QJsonObject{{QStringLiteral("username"), QStringLiteral("red-user")},
                    {QStringLiteral("displayName"), QStringLiteral("红方用户")},
                    {QStringLiteral("role"), QStringLiteral("red")} });
    sendNetworkTestEnvelope(serverSocket, QStringLiteral("snapshot"), 2,
                             networkTestSnapshot(10));
    ASSERT_TRUE(waitFor([&]() {
        return SimulationControllerTestPeer::networkTestLastSequence(client) == 2;
    }));

    sendNetworkTestEnvelope(
        serverSocket, QStringLiteral("delta"), 3,
        QJsonObject{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                    {QStringLiteral("baseStateRevision"), 10},
                    {QStringLiteral("stateRevision"), 11}});
    ASSERT_TRUE(waitFor([&]() {
        return SimulationControllerTestPeer::networkTestWaitingForResync(client);
    }));
    messages.clear();
    client.sendCommand(QStringLiteral("activateScan"),
                       QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_r1")}});
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(std::none_of(messages.cbegin(), messages.cend(), [](const QString& text) {
        return QJsonDocument::fromJson(text.toUtf8()).object()
            .value(QStringLiteral("type")).toString() == QLatin1String("command");
    }));

    sendNetworkTestEnvelope(serverSocket, QStringLiteral("snapshot"), 4,
                             networkTestSnapshot(20));
    ASSERT_TRUE(waitFor([&]() {
        return std::any_of(messages.cbegin(), messages.cend(), [](const QString& text) {
            return QJsonDocument::fromJson(text.toUtf8()).object()
                .value(QStringLiteral("type")).toString() == QLatin1String("command");
        });
    }));
    QJsonObject commandEnvelope;
    for (const QString& text : messages) {
        const QJsonObject candidate = QJsonDocument::fromJson(text.toUtf8()).object();
        if (candidate.value(QStringLiteral("type")).toString() == QLatin1String("command")) {
            commandEnvelope = candidate;
            break;
        }
    }
    ASSERT_FALSE(commandEnvelope.isEmpty());
    EXPECT_EQ(commandEnvelope.value(QStringLiteral("payload")).toObject()
                  .value(QStringLiteral("stateRevision")).toInteger(), 20);

    client.close();
    serverSocket->close();
    server.close();
}

TEST(SimulationControllerTest, EmptyAuthoritativeSnapshotClearsLocalUnits) {
    SimulationController controller;
    ASSERT_FALSE(controller.engine()->unitIds().isEmpty());
    Scenario projected = controller.engine()->scenario();
    projected.units.clear();
    const QJsonObject snapshot{
        {QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
        {QStringLiteral("stateRevision"), 1},
        {QStringLiteral("scenario"), ScenarioIo::toJson(projected)},
        {QStringLiteral("units"), QJsonArray{}},
        {QStringLiteral("messages"), QJsonArray{}},
        {QStringLiteral("mapMarks"), QJsonArray{}},
        {QStringLiteral("roomState"),
         QJsonObject{{QStringLiteral("phase"), QStringLiteral("preparing")},
                     {QStringLiteral("scenarioRevision"), 1},
                     {QStringLiteral("simTime"), 0.0},
                     {QStringLiteral("running"), false},
                     {QStringLiteral("speed"), 1.0}}},
    };

    SimulationControllerTestPeer::applyOnlineSnapshot(controller, snapshot);

    EXPECT_TRUE(controller.engine()->unitIds().isEmpty());
    EXPECT_TRUE(controller.units().isEmpty());
}

TEST(SimulationControllerTest, RoomClosedClearsRoomDerivedStateAndReturnsToRoomSelection) {
    SimulationController controller;
    SimulationControllerTestPeer::seedActiveOnlineRoom(controller);

    SimulationControllerTestPeer::receiveRoomClosed(controller);

    EXPECT_TRUE(controller.currentRoomId().isEmpty());
    EXPECT_TRUE(controller.currentSeatId().isEmpty());
    EXPECT_TRUE(controller.currentSeatType().isEmpty());
    EXPECT_TRUE(controller.currentSeatSide().isEmpty());
    EXPECT_FALSE(controller.seatReady());
    EXPECT_EQ(controller.onlineStage(), QStringLiteral("roomSelect"));
    EXPECT_EQ(controller.matchPhase(), QStringLiteral("preparing"));
    EXPECT_FALSE(controller.redReady());
    EXPECT_FALSE(controller.blueReady());
    EXPECT_TRUE(controller.onlineSeats().isEmpty());
    EXPECT_TRUE(controller.onlineMapMarks().isEmpty());
    EXPECT_TRUE(controller.chatMessages().isEmpty());
    EXPECT_TRUE(controller.engine()->unitIds().isEmpty());
}

TEST(SimulationControllerTest, MatchResetReturnsSeatedClientToSeatSelection) {
    SimulationController controller;
    SimulationControllerTestPeer::seedActiveOnlineRoom(controller);

    SimulationControllerTestPeer::receiveMatchReset(controller);

    EXPECT_EQ(controller.currentRoomId(), QStringLiteral("main"));
    EXPECT_TRUE(controller.currentSeatId().isEmpty());
    EXPECT_TRUE(controller.currentSeatType().isEmpty());
    EXPECT_TRUE(controller.currentSeatSide().isEmpty());
    EXPECT_FALSE(controller.seatReady());
    EXPECT_EQ(controller.onlineStage(), QStringLiteral("seatSelect"));
    EXPECT_EQ(controller.matchPhase(), QStringLiteral("preparing"));
    EXPECT_TRUE(controller.engine()->unitIds().isEmpty());
    EXPECT_TRUE(controller.chatMessages().isEmpty());
}

TEST(SimulationControllerTest, VacantDescriptorForPreviousSeatClearsOnlineIdentity) {
    SimulationController controller;
    SimulationControllerTestPeer::seedActiveOnlineRoom(controller);
    Scenario projected = controller.engine()->scenario();
    projected.units.clear();
    const QJsonObject snapshot{
        {QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
        {QStringLiteral("stateRevision"), 2},
        {QStringLiteral("scenario"), ScenarioIo::toJson(projected)},
        {QStringLiteral("units"), QJsonArray{}},
        {QStringLiteral("messages"), QJsonArray{}},
        {QStringLiteral("mapMarks"), QJsonArray{}},
        {QStringLiteral("roomState"),
         QJsonObject{{QStringLiteral("phase"), QStringLiteral("preparing")},
                     {QStringLiteral("roomId"), QStringLiteral("main")},
                     {QStringLiteral("scenarioRevision"), 2},
                     {QStringLiteral("simTime"), 0.0},
                     {QStringLiteral("running"), false},
                     {QStringLiteral("speed"), 1.0},
                     {QStringLiteral("seats"),
                      QJsonArray{QJsonObject{{QStringLiteral("seatId"),
                                             QStringLiteral("red_commander")},
                                            {QStringLiteral("seatType"),
                                             QStringLiteral("commander")},
                                            {QStringLiteral("side"), QStringLiteral("red")},
                                            {QStringLiteral("occupied"), false}}}}}},
    };

    SimulationControllerTestPeer::applyOnlineSnapshot(controller, snapshot);

    EXPECT_TRUE(controller.currentSeatId().isEmpty());
    EXPECT_TRUE(controller.currentSeatType().isEmpty());
    EXPECT_TRUE(controller.currentSeatSide().isEmpty());
    EXPECT_EQ(controller.onlineStage(), QStringLiteral("seatSelect"));
}

TEST(SimulationControllerTest, UnitStateRevisionRefreshesRuntimePositionAmmoAndCooldown) {
    SimulationController controller;
    SimulationControllerTestPeer::seedActiveOnlineRoom(controller);
    const Scenario scenario = controller.engine()->scenario();
    QJsonArray runtime = controller.engine()->collectAllUnitsSnapshot();
    const auto makeSnapshot = [&scenario](qint64 stateRevision, const QJsonArray& units) {
        return QJsonObject{
            {QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
            {QStringLiteral("stateRevision"), stateRevision},
            {QStringLiteral("scenario"), ScenarioIo::toJson(scenario)},
            {QStringLiteral("units"), units},
            {QStringLiteral("messages"), QJsonArray{}},
            {QStringLiteral("mapMarks"), QJsonArray{}},
            {QStringLiteral("roomState"),
             QJsonObject{{QStringLiteral("phase"), QStringLiteral("running")},
                         {QStringLiteral("roomId"), QStringLiteral("main")},
                         {QStringLiteral("scenarioRevision"), 7},
                         {QStringLiteral("stateRevision"), stateRevision},
                         {QStringLiteral("simTime"), 12.0},
                         {QStringLiteral("running"), true},
                         {QStringLiteral("speed"), 1.0},
                         {QStringLiteral("seats"),
                          QJsonArray{QJsonObject{
                              {QStringLiteral("seatId"), QStringLiteral("red_commander")},
                              {QStringLiteral("seatType"), QStringLiteral("commander")},
                              {QStringLiteral("side"), QStringLiteral("red")},
                              {QStringLiteral("occupied"), true},
                              {QStringLiteral("ready"), true}}}}}}};
    };

    SimulationControllerTestPeer::applyOnlineSnapshot(controller, makeSnapshot(1, runtime));
    const quint64 firstUnitRevision = controller.unitStateRevision();

    for (qsizetype i = 0; i < runtime.size(); ++i) {
        QJsonObject unit = runtime.at(i).toObject();
        if (unit.value(QStringLiteral("id")).toString() != QLatin1String("red_a1")) continue;
        unit[QStringLiteral("position")] = QJsonArray{4321.0, 8765.0, 2000.0};
        unit[QStringLiteral("ammoRemaining")] = 2;
        unit[QStringLiteral("cooldownRemaining")] = 2.5;
        unit[QStringLiteral("actions")] = QJsonObject{
            {QStringLiteral("engageTarget"),
             QJsonObject{{QStringLiteral("visible"), true},
                          {QStringLiteral("enabled"), false}}}};
        unit[QStringLiteral("incomingThreatCount")] = 1;
        unit[QStringLiteral("minimumThreatEta")] = 4.0;
        runtime.replace(i, unit);
        break;
    }
    SimulationControllerTestPeer::applyOnlineSnapshot(controller, makeSnapshot(2, runtime));

    const QJsonObject updated = controller.unitAt(QStringLiteral("red_a1"));
    ASSERT_FALSE(updated.isEmpty());
    EXPECT_GT(controller.unitStateRevision(), firstUnitRevision);
    EXPECT_DOUBLE_EQ(updated.value(QStringLiteral("position")).toArray().at(0).toDouble(), 4321.0);
    EXPECT_DOUBLE_EQ(updated.value(QStringLiteral("position")).toArray().at(1).toDouble(), 8765.0);
    EXPECT_EQ(updated.value(QStringLiteral("ammoRemaining")).toInt(), 2);
    EXPECT_DOUBLE_EQ(updated.value(QStringLiteral("cooldownRemaining")).toDouble(), 2.5);
    EXPECT_FALSE(updated.value(QStringLiteral("actions")).toObject()
                     .value(QStringLiteral("engageTarget")).toObject()
                     .value(QStringLiteral("enabled")).toBool());
    ASSERT_EQ(controller.units().size(), runtime.size());
    bool foundThreat = false;
    for (const QVariant& value : controller.units()) {
        const QVariantMap candidate = value.toMap();
        if (candidate.value(QStringLiteral("id")).toString() == QLatin1String("red_a1")) {
            EXPECT_EQ(candidate.value(QStringLiteral("incomingThreatCount")).toInt(), 1);
            foundThreat = true;
        }
    }
    EXPECT_TRUE(foundThreat);
}

TEST(SimulationControllerTest, OnlineAttackTargetsRequireServerActionableSensorIntel) {
    SimulationController controller;
    QStringList blueIds;
    for (const QJsonValue& value : controller.engine()->collectAllUnitsSnapshot()) {
        const QJsonObject unit = value.toObject();
        if (unit.value(QStringLiteral("side")).toString() == QLatin1String("blue")
            && unit.value(QStringLiteral("alive")).toBool()) {
            blueIds.append(unit.value(QStringLiteral("id")).toString());
        }
    }
    ASSERT_GE(blueIds.size(), 3);
    SimulationControllerTestPeer::seedOnlineIntelRecords(
        controller,
        QVariantList{
            QVariantMap{{QStringLiteral("intelId"), QStringLiteral("live-contact")},
                        {QStringLiteral("type"), QStringLiteral("sensorContact")},
                        {QStringLiteral("targetId"), blueIds.at(0)},
                        {QStringLiteral("freshness"), QStringLiteral("live")},
                        {QStringLiteral("actionable"), true}},
            QVariantMap{{QStringLiteral("intelId"), QStringLiteral("stale-contact")},
                        {QStringLiteral("type"), QStringLiteral("sensorContact")},
                        {QStringLiteral("targetId"), blueIds.at(1)},
                        {QStringLiteral("freshness"), QStringLiteral("stale")},
                        {QStringLiteral("actionable"), false}},
            QVariantMap{{QStringLiteral("intelId"), QStringLiteral("manual-report")},
                        {QStringLiteral("type"), QStringLiteral("manualReport")},
                        {QStringLiteral("targetId"), blueIds.at(2)},
                        {QStringLiteral("freshness"), QStringLiteral("live")},
                        {QStringLiteral("actionable"), true}}});

    const QVariantList targets = controller.detectedEnemyOptions(
        QString(), QStringLiteral("red"), QStringLiteral("blue"));

    ASSERT_EQ(targets.size(), 1);
    EXPECT_EQ(targets.constFirst().toMap().value(QStringLiteral("id")).toString(),
              blueIds.constFirst());
}

TEST(SimulationControllerTest, UnseatedRunningSnapshotStaysInRoomSelectionAndHidesRuntime) {
    SimulationController controller;
    SimulationControllerTestPeer::seedOnlineRoomSelection(controller);
    const Scenario scenario = controller.engine()->scenario();
    const QJsonObject snapshot{
        {QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
        {QStringLiteral("stateRevision"), 1},
        {QStringLiteral("scenario"), ScenarioIo::toJson(scenario)},
        {QStringLiteral("units"), controller.engine()->collectAllUnitsSnapshot()},
        {QStringLiteral("messages"), QJsonArray{}},
        {QStringLiteral("mapMarks"), QJsonArray{}},
        {QStringLiteral("roomState"),
         QJsonObject{{QStringLiteral("phase"), QStringLiteral("running")},
                     {QStringLiteral("roomId"), QStringLiteral("main")},
                     {QStringLiteral("scenarioRevision"), 7},
                     {QStringLiteral("stateRevision"), 1},
                     {QStringLiteral("simTime"), 12.0},
                     {QStringLiteral("running"), true},
                     {QStringLiteral("speed"), 1.0},
                     {QStringLiteral("seats"), QJsonArray{}}}}};

    SimulationControllerTestPeer::applyOnlineSnapshot(controller, snapshot);

    EXPECT_EQ(controller.onlineStage(), QStringLiteral("roomSelect"));
    EXPECT_TRUE(controller.currentRoomId().isEmpty());
    EXPECT_TRUE(controller.currentSeatId().isEmpty());
    EXPECT_TRUE(controller.engine()->unitIds().isEmpty());
    EXPECT_TRUE(controller.onlineMapMarks().isEmpty());
}

TEST(SimulationControllerTest, PreparingSnapshotWithoutCurrentSeatReturnsToSeatSelection) {
    SimulationController controller;
    SimulationControllerTestPeer::seedActiveOnlineRoom(controller);
    Scenario projected = controller.engine()->scenario();
    projected.units.clear();
    const QJsonObject snapshot{
        {QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
        {QStringLiteral("stateRevision"), 1},
        {QStringLiteral("scenario"), ScenarioIo::toJson(projected)},
        {QStringLiteral("units"), QJsonArray{}},
        {QStringLiteral("messages"), QJsonArray{}},
        {QStringLiteral("mapMarks"), QJsonArray{}},
        {QStringLiteral("roomState"),
         QJsonObject{{QStringLiteral("phase"), QStringLiteral("preparing")},
                     {QStringLiteral("roomId"), QStringLiteral("main")},
                     {QStringLiteral("scenarioRevision"), 2},
                     {QStringLiteral("simTime"), 0.0},
                     {QStringLiteral("running"), false},
                     {QStringLiteral("speed"), 1.0},
                     {QStringLiteral("seats"), QJsonArray{}}}},
    };

    SimulationControllerTestPeer::applyOnlineSnapshot(controller, snapshot);

    EXPECT_TRUE(controller.currentSeatId().isEmpty());
    EXPECT_EQ(controller.onlineStage(), QStringLiteral("seatSelect"));
}

TEST(SimulationControllerTest, PveSnapshotPublishesAuthorizedConfigurationAndAiSeat) {
    SimulationController controller;
    SimulationControllerTestPeer::seedActiveOnlineRoom(controller);
    Scenario projected = controller.engine()->scenario();
    projected.units.clear();
    const QJsonObject snapshot{
        {QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
        {QStringLiteral("stateRevision"), 1},
        {QStringLiteral("scenario"), ScenarioIo::toJson(projected)},
        {QStringLiteral("units"), QJsonArray{}},
        {QStringLiteral("messages"), QJsonArray{}},
        {QStringLiteral("mapMarks"), QJsonArray{}},
        {QStringLiteral("roomState"),
         QJsonObject{{QStringLiteral("phase"), QStringLiteral("preparing")},
                     {QStringLiteral("roomId"), QStringLiteral("main")},
                     {QStringLiteral("roomMode"), QStringLiteral("pve")},
                     {QStringLiteral("aiDifficulty"), QStringLiteral("hard")},
                     {QStringLiteral("aiEngine"), QStringLiteral("ollama")},
                     {QStringLiteral("configVersion"), 7},
                     {QStringLiteral("scenarioRevision"), 2},
                     {QStringLiteral("simTime"), 0.0},
                     {QStringLiteral("running"), false},
                     {QStringLiteral("speed"), 1.0},
                     {QStringLiteral("seats"), QJsonArray{QJsonObject{
                         {QStringLiteral("seatId"), QStringLiteral("red_commander")},
                         {QStringLiteral("seatType"), QStringLiteral("commander")},
                         {QStringLiteral("side"), QStringLiteral("red")},
                         {QStringLiteral("occupied"), true},
                         {QStringLiteral("controllerType"), QStringLiteral("human")}},
                         QJsonObject{{QStringLiteral("seatId"), QStringLiteral("blue_commander")},
                                     {QStringLiteral("seatType"), QStringLiteral("commander")},
                                     {QStringLiteral("side"), QStringLiteral("blue")},
                                     {QStringLiteral("occupied"), true},
                                     {QStringLiteral("controllerType"), QStringLiteral("ai")}}}}}}};

    SimulationControllerTestPeer::applyOnlineSnapshot(controller, snapshot);

    EXPECT_EQ(controller.property("roomMode").toString(), QStringLiteral("pve"));
    EXPECT_EQ(controller.property("aiDifficulty").toString(), QStringLiteral("hard"));
    EXPECT_EQ(controller.property("aiEffectiveEngine").toString(), QStringLiteral("ollama"));
    EXPECT_EQ(controller.property("configVersion").toLongLong(), 7);
    const QVariantList seats = controller.onlineSeats();
    ASSERT_EQ(seats.size(), 2);
    EXPECT_EQ(seats.at(1).toMap().value(QStringLiteral("controllerType")).toString(),
              QStringLiteral("ai"));
}

TEST(SimulationControllerTest, RoomDirectoryAfterLeaveClearsStaleSeatState) {
    SimulationController controller;
    SimulationControllerTestPeer::seedActiveOnlineRoom(controller);
    SimulationControllerTestPeer::setLeaveRoomPending(controller, true);

    SimulationControllerTestPeer::receiveRoomDirectory(controller);

    EXPECT_FALSE(controller.leaveRoomPending());
    EXPECT_TRUE(controller.currentRoomId().isEmpty());
    EXPECT_TRUE(controller.currentSeatId().isEmpty());
    EXPECT_TRUE(controller.currentSeatType().isEmpty());
    EXPECT_TRUE(controller.currentSeatSide().isEmpty());
    EXPECT_FALSE(controller.seatReady());
    EXPECT_EQ(controller.onlineStage(), QStringLiteral("roomSelect"));
    EXPECT_TRUE(controller.engine()->unitIds().isEmpty());
}

TEST(SimulationControllerTest, LeavePendingClearsWhenServerRejectsRequest) {
    SimulationController controller;
    SimulationControllerTestPeer::setLeaveRoomPending(controller, true);

    SimulationControllerTestPeer::receiveCommandRejected(controller);

    EXPECT_FALSE(controller.leaveRoomPending());
}

TEST(SimulationControllerTest, RejectedObserverJoinRestoresSeatSelectionFlow) {
    SimulationController controller;
    SimulationControllerTestPeer::seedPendingObserverJoin(controller);

    SimulationControllerTestPeer::receiveCommandRejected(controller);

    EXPECT_FALSE(controller.isObserver());
    EXPECT_TRUE(controller.currentRoomId().isEmpty());
    EXPECT_EQ(controller.onlineStage(), QStringLiteral("roomSelect"));

    const QJsonObject seatState{
        {QStringLiteral("roomId"), QStringLiteral("main")},
        {QStringLiteral("yourSeatId"), QString()},
        {QStringLiteral("seats"), QJsonArray{QJsonObject{
            {QStringLiteral("seatId"), QStringLiteral("red_commander")},
            {QStringLiteral("seatType"), QStringLiteral("commander")},
            {QStringLiteral("side"), QStringLiteral("red")},
            {QStringLiteral("occupied"), false}}}}};
    SimulationControllerTestPeer::receiveSeatState(controller, seatState);

    EXPECT_EQ(controller.currentRoomId(), QStringLiteral("main"));
    EXPECT_FALSE(controller.isObserver());
    EXPECT_EQ(controller.onlineStage(), QStringLiteral("seatSelect"));

    bool claimReachedNetworkClient = false;
    QObject::connect(&controller, &SimulationController::errorForward,
                     &controller, [&claimReachedNetworkClient](const QString&) {
                         claimReachedNetworkClient = true;
                     });
    controller.claimOnlineSeat(QStringLiteral("red_commander"));
    EXPECT_TRUE(claimReachedNetworkClient);
}

TEST(SimulationControllerTest, FriendlyCommanderCanApproveAndRejectPendingSeatTransfers) {
    SimulationController controller;
    SimulationControllerTestPeer::seedActiveOnlineRoom(controller);
    QList<QPair<QString, QVariantMap>> commands;
    QObject::connect(&controller, &SimulationController::commandExecuted, &controller,
                     [&commands](const QString& action, const QVariantMap& args) {
                         commands.append(qMakePair(action, args));
                     });

    SimulationControllerTestPeer::receiveTransferEvent(
        controller, QStringLiteral("transferRequested"));

    const QVariantList pending = controller.property("pendingSeatTransfers").toList();
    ASSERT_EQ(pending.size(), 1);
    EXPECT_EQ(pending.constFirst().toMap().value(QStringLiteral("userId")).toLongLong(), 37);
    EXPECT_EQ(pending.constFirst().toMap().value(QStringLiteral("targetSeatId")).toString(),
              QStringLiteral("red_recon_1"));

    EXPECT_TRUE(QMetaObject::invokeMethod(&controller, "approveSeatTransfer",
                                         Q_ARG(qint64, 37), Q_ARG(qint64, 12)));
    ASSERT_EQ(commands.size(), 1);
    EXPECT_EQ(commands.constFirst().first, QStringLiteral("approveSeatTransfer"));

    EXPECT_TRUE(QMetaObject::invokeMethod(&controller, "rejectSeatTransfer",
                                         Q_ARG(qint64, 37), Q_ARG(qint64, 12)));
    ASSERT_EQ(commands.size(), 2);
    EXPECT_EQ(commands.constLast().first, QStringLiteral("rejectSeatTransfer"));
}

TEST(SimulationControllerTest, ForfeitFinishesOnlineMatchAndKeepsBattleVisible) {
    SimulationController controller;
    SimulationControllerTestPeer::seedActiveOnlineRoom(controller);

    SimulationControllerTestPeer::receiveForfeit(controller);

    EXPECT_EQ(controller.matchPhase(), QStringLiteral("finished"));
    EXPECT_EQ(controller.onlineStage(), QStringLiteral("battle"));
}

TEST(SimulationControllerTest, RoomDirectoryRefreshPreservesActiveRoomState) {
    SimulationController controller;
    SimulationControllerTestPeer::seedActiveOnlineRoom(controller);

    SimulationControllerTestPeer::receiveRoomDirectory(controller,
        QJsonArray{QJsonObject{{QStringLiteral("roomId"), QStringLiteral("main")},
                               {QStringLiteral("status"), QStringLiteral("running")}}});

    EXPECT_EQ(controller.currentRoomId(), QStringLiteral("main"));
    EXPECT_EQ(controller.currentSeatId(), QStringLiteral("red_commander"));
    EXPECT_EQ(controller.onlineStage(), QStringLiteral("battle"));
    ASSERT_EQ(controller.chatMessages().size(), 1);
    EXPECT_EQ(controller.chatMessages().constFirst().toMap().value(QStringLiteral("text")),
              QStringLiteral("stale chat"));
}

TEST(SimulationControllerTest, ApplicationShutdownLogsOutWhenLoginReplyFinishedBeforeCallback) {
    int argc = 1;
    char applicationName[] = "controller_lifecycle_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    ClientAccountReceiver account;
    ASSERT_TRUE(account.listen());
    account.deferLoginReply();
    SimulationController controller;
    controller.loginOnline(QStringLiteral("http://127.0.0.1:%1").arg(account.port()),
                           QStringLiteral("pilot"), QStringLiteral("secret"));
    ASSERT_TRUE(waitFor([&account]() { return account.hasLoginRequest(); }));
    QNetworkReply* loginReply = SimulationControllerTestPeer::loginReply(controller);
    ASSERT_NE(loginReply, nullptr);
    QSignalBlocker blockLoginCallback(loginReply);
    ASSERT_TRUE(account.releaseLoginReply());
    ASSERT_TRUE(waitFor([loginReply]() { return loginReply->isFinished(); }));
    ASSERT_FALSE(loginReply->isRunning());

    ASSERT_TRUE(QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit",
                                          Qt::DirectConnection));

    EXPECT_TRUE(waitFor([&account]() { return account.hasLogoutRequest(); }));
}

TEST(SimulationControllerTest, SuccessfulLoginTokenSurvivesReplyReleaseUntilShutdown) {
    int argc = 1;
    char applicationName[] = "controller_login_token_lifecycle_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    ClientAccountReceiver account;
    ASSERT_TRUE(account.listen());
    account.deferLoginReply();
    SimulationController controller;
    controller.loginOnline(QStringLiteral("http://127.0.0.1:%1").arg(account.port()),
                           QStringLiteral("pilot"), QStringLiteral("secret"));
    ASSERT_TRUE(waitFor([&account]() { return account.hasLoginRequest(); }));
    ASSERT_TRUE(account.releaseLoginReply());
    ASSERT_TRUE(waitFor([&controller]() {
        return SimulationControllerTestPeer::loginReply(controller) == nullptr;
    }));
    EXPECT_EQ(SimulationControllerTestPeer::loginTokenCandidate(controller),
              QStringLiteral("lifecycle-token"));

    ASSERT_TRUE(QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit",
                                          Qt::DirectConnection));
    EXPECT_TRUE(waitFor([&account]() { return account.hasLogoutRequest(); }));
}

TEST(SimulationControllerTest, CanceledLoginResponseIsLoggedOutAfterGenerationChanges) {
    int argc = 1;
    char applicationName[] = "controller_canceled_login_cleanup_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    ClientAccountReceiver account;
    ASSERT_TRUE(account.listen());
    account.deferLoginReply();
    SimulationController controller;
    controller.loginOnline(QStringLiteral("http://127.0.0.1:%1").arg(account.port()),
                           QStringLiteral("pilot"), QStringLiteral("secret"));
    ASSERT_TRUE(waitFor([&account]() { return account.hasLoginRequest(); }));

    controller.logoutOnline();
    ASSERT_TRUE(account.releaseLoginReply());

    EXPECT_TRUE(waitFor([&account]() { return account.hasLogoutRequest(); }));
}

TEST(SimulationControllerTest, ApplicationShutdownWaitsForLogoutAlreadyInFlight) {
    int argc = 1;
    char applicationName[] = "controller_logout_lifecycle_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    ClientAccountReceiver account;
    ASSERT_TRUE(account.listen());
    account.deferLogoutReply();
    SimulationController controller;
    controller.loginOnline(QStringLiteral("http://127.0.0.1:%1").arg(account.port()),
                           QStringLiteral("pilot"), QStringLiteral("secret"));
    ASSERT_TRUE(waitFor([&controller]() {
        return SimulationControllerTestPeer::loginReply(controller) == nullptr;
    }));

    controller.logoutOnline();
    ASSERT_TRUE(waitFor([&account]() { return account.hasLogoutRequest(); }));
    bool logoutReleased = false;
    QTimer::singleShot(0, &account, [&account, &logoutReleased]() {
        logoutReleased = account.releaseLogoutReply();
    });

    ASSERT_TRUE(QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit",
                                          Qt::DirectConnection));

    EXPECT_TRUE(logoutReleased);
}

TEST(SimulationControllerTest, ReplaceUnitsRejectsInvalidInputAtomically) {
    SimulationController controller;
    const QStringList beforeIds = controller.engine()->unitIds();
    QVariantList replacement = controller.unitsJson().value("units").toArray().toVariantList();
    ASSERT_FALSE(replacement.isEmpty());

    QVariantMap invalid = replacement.front().toMap();
    invalid["maxHp"] = 0.0;
    replacement.front() = invalid;

    EXPECT_FALSE(controller.replaceUnits(replacement));
    EXPECT_EQ(controller.engine()->unitIds(), beforeIds);
    EXPECT_TRUE(controller.readyForSim());
}

TEST(SimulationControllerTest, UpsertGeneratedUnitReturnsStableId) {
    SimulationController controller;
    QVariantMap unit{{QStringLiteral("id"), QString()},
                     {QStringLiteral("callsign"), QStringLiteral("定位测试单元")},
                     {QStringLiteral("kind"), QStringLiteral("groundscout")},
                     {QStringLiteral("side"), QStringLiteral("red")},
                     {QStringLiteral("x"), 4321.0}, {QStringLiteral("y"), 6789.0},
                     {QStringLiteral("alt"), 0.0}, {QStringLiteral("detectRange"), 3000.0},
                     {QStringLiteral("attackRange"), 0.0}, {QStringLiteral("commRange"), 10000.0},
                     {QStringLiteral("speed"), 6.0}, {QStringLiteral("maxHp"), 80.0},
                     {QStringLiteral("attackPower"), 0.0}};

    const QString id = controller.upsertUnit(unit);

    EXPECT_FALSE(id.isEmpty());
    const QJsonObject scenario = controller.unitsJson();
    bool found = false;
    for (const QJsonValue& value : scenario.value(QStringLiteral("units")).toArray()) {
        if (value.toObject().value(QStringLiteral("id")).toString() == id) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(SimulationControllerTest, CommandPostViewKeepsValidSelectedUnit) {
    SimulationController controller;
    controller.setViewMode(QStringLiteral("commandpost-red"));
    const QVariantList reconUnits = controller.unitOptions(QStringLiteral("reconuav"),
                                                            QStringLiteral("red"));
    ASSERT_FALSE(reconUnits.isEmpty());
    const QString reconId = reconUnits.front().toMap().value(QStringLiteral("id")).toString();

    controller.setFocusedUnitId(reconId);
    controller.loadDefault(); // Scenario refresh must not reset focus to the CP.

    EXPECT_EQ(controller.focusedUnitId(), reconId);
}

TEST(SimulationControllerTest, TypedUnitOrderUsesSimulationCommandFeed) {
    SimulationController controller;

    controller.sendUnitOrder(QStringLiteral("red_r1"), QStringLiteral("  保持观察  "));

    ASSERT_FALSE(controller.messages().isEmpty());
    const QVariantMap message = controller.messages().constFirst().toMap();
    EXPECT_EQ(message.value(QStringLiteral("type")).toString(), QStringLiteral("UnitOrder"));
    EXPECT_EQ(message.value(QStringLiteral("receiver")).toString(), QStringLiteral("red_r1"));
    EXPECT_EQ(message.value(QStringLiteral("payload")).toMap().value(QStringLiteral("text")).toString(),
              QStringLiteral("保持观察"));
}

TEST(SimulationControllerTest, ShortcutSettingIsReadableWhenChangeIsSignaled) {
    QStandardPaths::setTestModeEnabled(true);
    SimulationController controller;
    QString valueObservedByQml;
    QObject::connect(&controller, &SimulationController::shortcutsChanged,
                     &controller, [&controller, &valueObservedByQml]() {
                         valueObservedByQml = controller.loadSetting(
                             QStringLiteral("shortcuts/testImmediateRead")).toString();
                     });

    controller.saveSetting(QStringLiteral("shortcuts/testImmediateRead"),
                           QStringLiteral("Ctrl+Alt+9"));

    EXPECT_EQ(valueObservedByQml, QStringLiteral("Ctrl+Alt+9"));
}

TEST(SimulationControllerTest, SettingIsReadableWhenGeneralChangeIsSignaled) {
    QStandardPaths::setTestModeEnabled(true);
    SimulationController controller;
    QString changedKey;
    QVariant valueObservedByQml;
    QObject::connect(&controller, &SimulationController::settingChanged,
                     &controller, [&controller, &changedKey, &valueObservedByQml](
                                      const QString& key) {
                         changedKey = key;
                         valueObservedByQml = controller.loadSetting(key);
                     });

    controller.saveSetting(QStringLiteral("online/intel/showLive"), false);

    EXPECT_EQ(changedKey, QStringLiteral("online/intel/showLive"));
    EXPECT_FALSE(valueObservedByQml.toBool());
}

TEST(SimulationControllerTest, NetworkPasswordIsNeverWrittenToSettingsJson) {
    QStandardPaths::setTestModeEnabled(true);
    SimulationController controller;
    int settingChangeCount = 0;
    QObject::connect(&controller, &SimulationController::settingChanged,
                     &controller, [&settingChangeCount](const QString&) {
                         ++settingChangeCount;
                     });

    controller.saveSetting(QStringLiteral("network/password"), QStringLiteral("plaintext-secret"));

    EXPECT_FALSE(controller.allSettings().contains(QStringLiteral("network/password")));
    EXPECT_EQ(settingChangeCount, 0);
}
