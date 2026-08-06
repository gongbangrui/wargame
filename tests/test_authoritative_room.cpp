#include <gtest/gtest.h>

#include "AuthoritativeRoom.h"
#include "core/UnitBase.h"
#include "protocol/Protocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebSocket>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#define private public
#include "GameServer.h"
#undef private

using namespace gbr;

namespace {

AuthoritativeRoom configuredRoom() {
    AuthoritativeRoom room(7);
    QString error;
    EXPECT_TRUE(room.setTemplateCatalog(AuthoritativeRoom::defaultTemplateCatalog(), &error))
        << error.toStdString();
    return room;
}

void claimCommanders(AuthoritativeRoom& room) {
    ASSERT_TRUE(room.claimSeat(1, QStringLiteral("red"), QStringLiteral("red_commander"),
                               QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(room.claimSeat(2, QStringLiteral("blue"), QStringLiteral("blue_commander"),
                               QStringLiteral("commandpost")).ok);
}

bool waitFor(const std::function<bool()>& condition, int timeoutMs = 1000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return condition();
}

QJsonObject eventPayload(const QList<QJsonObject>& envelopes, const QString& kind) {
    for (const QJsonObject& envelope : envelopes) {
        if (envelope.value(QStringLiteral("type")).toString() != QLatin1String("event")) continue;
        const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
        if (payload.value(QStringLiteral("kind")).toString() == kind) return payload;
    }
    return {};
}

bool containsUnitId(const QJsonArray& units, const QString& unitId) {
    for (const QJsonValue& value : units) {
        if (value.toObject().value(QStringLiteral("id")).toString() == unitId) return true;
    }
    return false;
}

class OperationAcknowledgementReceiver final : public QObject {
public:
    explicit OperationAcknowledgementReceiver(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            ++m_connectionCount;
            QTcpSocket* socket = m_server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                m_requests.append(socket->readAll());
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                socket->disconnectFromHost();
            });
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    quint16 port() const { return m_server.serverPort(); }
    int connectionCount() const { return m_connectionCount; }
    const QList<QByteArray>& requests() const { return m_requests; }

private:
    QTcpServer m_server;
    int m_connectionCount = 0;
    QList<QByteArray> m_requests;
};

class RoomControlReceiver final : public QObject {
public:
    explicit RoomControlReceiver(QByteArray responseBody, QObject* parent = nullptr)
        : QObject(parent), m_responseBody(std::move(responseBody)) {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket* socket = m_server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                m_requests.append(socket->readAll());
                const QByteArray response = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ")
                    + QByteArray::number(m_responseBody.size())
                    + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + m_responseBody;
                socket->write(response);
                socket->disconnectFromHost();
            });
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    quint16 port() const { return m_server.serverPort(); }
    int requestCount() const { return m_requests.size(); }
    void setResponseBody(QByteArray responseBody) {
        m_responseBody = std::move(responseBody);
    }

private:
    QTcpServer m_server;
    QByteArray m_responseBody;
    QList<QByteArray> m_requests;
};

class HeldHttpReceiver final : public QObject {
public:
    explicit HeldHttpReceiver(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                ++m_connectionCount;
                connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                    ++m_disconnectionCount;
                    socket->deleteLater();
                });
            }
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    quint16 port() const { return m_server.serverPort(); }
    int connectionCount() const { return m_connectionCount; }
    int disconnectionCount() const { return m_disconnectionCount; }

private:
    QTcpServer m_server;
    int m_connectionCount = 0;
    int m_disconnectionCount = 0;
};

class AuthenticationReceiver final : public QObject {
public:
    explicit AuthenticationReceiver(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket* socket = m_server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                const QByteArray request = socket->readAll();
                if (request.startsWith("POST /api/internal/session ")) {
                    ++m_sessionRequests;
                    const QByteArray body =
                        "{\"valid\":true,\"userId\":3,\"username\":\"pilot\","
                        "\"displayName\":\"Pilot\"}";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                                  + QByteArray::number(body.size())
                                  + "\r\nConnection: close\r\n\r\n" + body);
                } else if (request.startsWith("GET /api/internal/rooms ")
                           && !m_roomControlResponse.isEmpty()) {
                    if (m_roomControlHook) m_roomControlHook();
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                                  + QByteArray::number(m_roomControlResponse.size())
                                  + "\r\nConnection: close\r\n\r\n" + m_roomControlResponse);
                } else {
                    socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                }
                socket->disconnectFromHost();
            });
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    quint16 port() const { return m_server.serverPort(); }
    int sessionRequests() const { return m_sessionRequests; }
    void setRoomControlResponse(const QByteArray& response) { m_roomControlResponse = response; }
    void setRoomControlHook(std::function<void()> hook) { m_roomControlHook = std::move(hook); }

private:
    QTcpServer m_server;
    int m_sessionRequests = 0;
    QByteArray m_roomControlResponse;
    std::function<void()> m_roomControlHook;
};

class RetryAuthenticationReceiver final : public QObject {
public:
    RetryAuthenticationReceiver(QList<int> sessionStatuses, bool timeoutFirst,
                                QObject* parent = nullptr)
        : QObject(parent), m_sessionStatuses(std::move(sessionStatuses)),
          m_timeoutFirst(timeoutFirst) {
        m_clock.start();
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket* socket = m_server.nextPendingConnection();
            const auto request = std::make_shared<QByteArray>();
            const auto handled = std::make_shared<bool>(false);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, request, handled]() {
                if (*handled) return;
                request->append(socket->readAll());
                if (!request->contains(QByteArrayLiteral("\r\n\r\n"))) return;
                *handled = true;
                handleRequest(socket, *request);
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    quint16 port() const { return m_server.serverPort(); }
    int sessionRequests() const { return m_sessionRequests; }
    QList<int> responseStatuses() const { return m_responseStatuses; }
    QList<qint64> sessionRequestTimes() const { return m_sessionRequestTimes; }

private:
    void handleRequest(QTcpSocket* socket, const QByteArray& request) {
        if (request.startsWith(QByteArrayLiteral("POST /api/internal/session "))) {
            ++m_sessionRequests;
            m_sessionRequestTimes.append(m_clock.elapsed());
            if (m_timeoutFirst && m_sessionRequests == 1) return;
            const int index = m_timeoutFirst ? m_sessionRequests - 2 : m_sessionRequests - 1;
            const int status = m_sessionStatuses.value(index, 200);
            const QByteArray body = status == 200
                ? QByteArrayLiteral("{\"valid\":true,\"userId\":3,\"username\":\"pilot\",\"displayName\":\"Pilot\"}")
                : QByteArrayLiteral("{\"detail\":\"authentication failure\"}");
            m_responseStatuses.append(status);
            writeResponse(socket, status, body);
            return;
        }
        if (request.startsWith(QByteArrayLiteral("GET /api/internal/rooms "))) {
            writeResponse(socket, 200, QByteArrayLiteral("{\"rooms\":[]}"));
            return;
        }
        writeResponse(socket, 404, QByteArrayLiteral("{}"));
    }

    void writeResponse(QTcpSocket* socket, int status, const QByteArray& body) {
        const QByteArray reason = status == 200 ? QByteArrayLiteral("OK")
            : status == 401 ? QByteArrayLiteral("Unauthorized")
            : status == 503 ? QByteArrayLiteral("Service Unavailable")
                             : QByteArrayLiteral("Not Found");
        const QByteArray response = QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status)
            + QByteArrayLiteral(" ") + reason
            + QByteArrayLiteral("\r\nContent-Type: application/json\r\nContent-Length: ")
            + QByteArray::number(body.size())
            + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QList<int> m_sessionStatuses;
    bool m_timeoutFirst = false;
    int m_sessionRequests = 0;
    QList<int> m_responseStatuses;
    QList<qint64> m_sessionRequestTimes;
    QElapsedTimer m_clock;
};

void sendClientEnvelope(QWebSocket& socket, const QString& type, const QString& messageId,
                        const QJsonObject& payload) {
    socket.sendTextMessage(QJsonDocument(QJsonObject{{QStringLiteral("protocolVersion"), 3},
                                                      {QStringLiteral("schemaVersion"), 2},
                                                      {QStringLiteral("type"), type},
                                                      {QStringLiteral("messageId"), messageId},
                                                      {QStringLiteral("payload"), payload}})
                               .toJson(QJsonDocument::Compact));
}

void configureGameServerEnvironment(const QTemporaryDir& temporary) {
    qputenv("AI_PROVIDER", QByteArray("rules"));
    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:1"));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());
}

void configureGameServerEnvironment(const QTemporaryDir& temporary, quint16 authPort) {
    configureGameServerEnvironment(temporary);
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:")
                                  + QByteArray::number(authPort));
}

}

TEST(GameServerAdmissionTest, ConfiguresExactPendingConnectionLimit) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;

    EXPECT_EQ(server.m_server.maxPendingConnections(), 64);
}

TEST(GameServerSnapshotTest, ChangedProjectionCarriesAdvancedRevisionAndIdleKeepsItStable) {
    int argc = 1;
    char applicationName[] = "snapshot_revision_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;
    ASSERT_TRUE(server.m_server.listen(QHostAddress::LocalHost, 0));
    QList<QJsonObject> messages;
    QWebSocket client;
    QObject::connect(&client, &QWebSocket::textMessageReceived, [&messages](const QString& text) {
        messages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1")
                         .arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* serverSocket = server.m_clients.keys().constFirst();
    auto& session = server.m_clients[serverSocket];
    session.authenticated = true;
    session.roomId = server.m_roomId;

    const quint64 initialRevision = server.m_stateRevision;
    server.broadcastSnapshots();
    ASSERT_TRUE(waitFor([&messages]() { return messages.size() == 1; }));
    ASSERT_EQ(messages.constFirst().value(QStringLiteral("type")).toString(),
              QLatin1String("snapshot"));
    const QJsonObject initialPayload = messages.constFirst()
                                           .value(QStringLiteral("payload")).toObject();
    ASSERT_FALSE(session.lastSnapshot.isEmpty());
    EXPECT_EQ(server.m_stateRevision, initialRevision + 1);
    EXPECT_EQ(initialPayload.value(QStringLiteral("stateRevision")).toInteger(),
              static_cast<qint64>(server.m_stateRevision));
    EXPECT_EQ(initialPayload.value(QStringLiteral("roomState")).toObject()
                  .value(QStringLiteral("stateRevision")).toInteger(),
              static_cast<qint64>(server.m_stateRevision));

    const QJsonObject initialSnapshot = session.lastSnapshot;
    server.broadcastSnapshots();
    EXPECT_FALSE(waitFor([&messages]() { return messages.size() > 1; }, 100));
    EXPECT_EQ(server.m_stateRevision, initialRevision + 1);
    EXPECT_EQ(session.lastSnapshot, initialSnapshot);

    server.m_roomName = QStringLiteral("Revision timing regression");
    server.broadcastSnapshots();
    ASSERT_TRUE(waitFor([&messages]() { return messages.size() == 2; }));
    ASSERT_EQ(messages.at(1).value(QStringLiteral("type")).toString(),
              QLatin1String("delta"));
    const QJsonObject changedPayload = messages.at(1).value(QStringLiteral("payload")).toObject();
    EXPECT_EQ(server.m_stateRevision, initialRevision + 2);
    EXPECT_EQ(changedPayload.value(QStringLiteral("stateRevision")).toInteger(),
              static_cast<qint64>(server.m_stateRevision));
    EXPECT_EQ(changedPayload.value(QStringLiteral("roomState")).toObject()
                  .value(QStringLiteral("stateRevision")).toInteger(),
              static_cast<qint64>(server.m_stateRevision));

    const QJsonObject changedSnapshot = session.lastSnapshot;
    server.broadcastSnapshots(true);
    ASSERT_TRUE(waitFor([&messages]() { return messages.size() == 3; }));
    ASSERT_EQ(messages.at(2).value(QStringLiteral("type")).toString(),
              QLatin1String("snapshot"));
    const QJsonObject forcedPayload = messages.at(2).value(QStringLiteral("payload")).toObject();
    EXPECT_EQ(server.m_stateRevision, initialRevision + 2);
    EXPECT_EQ(forcedPayload.value(QStringLiteral("stateRevision")).toInteger(),
              static_cast<qint64>(server.m_stateRevision));
    EXPECT_EQ(forcedPayload.value(QStringLiteral("roomState")).toObject()
                  .value(QStringLiteral("stateRevision")).toInteger(),
              static_cast<qint64>(server.m_stateRevision));
    EXPECT_EQ(session.lastSnapshot, changedSnapshot);

    client.close();
    EXPECT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

TEST(GameServerAdmissionTest, NormalizesIpv4MappedIpv6ForPerIpAdmission) {
    EXPECT_EQ(GameServer::normalizedPeerAddress(QHostAddress(QStringLiteral("::ffff:127.0.0.1"))),
              QHostAddress(QStringLiteral("127.0.0.1")));
}

TEST(GameServerAdmissionTest, NinthUnauthenticatedConnectionFromOneIpIsRejectedAndSlotRecovers) {
    int argc = 1;
    char applicationName[] = "authoritative_room_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    GameServer server;
    ASSERT_TRUE(server.m_server.listen(QHostAddress::LocalHost, 0));
    const QUrl endpoint(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort()));

    std::vector<std::unique_ptr<QWebSocket>> accepted;
    for (int i = 0; i < 8; ++i) {
        accepted.push_back(std::make_unique<QWebSocket>());
        accepted.back()->open(endpoint);
        ASSERT_TRUE(waitFor([&server, i]() { return server.m_clients.size() == i + 1; }));
    }
    QWebSocket rejected;
    QWebSocketProtocol::CloseCode rejectedCode = QWebSocketProtocol::CloseCodeNormal;
    QObject::connect(&rejected, &QWebSocket::disconnected, [&rejected, &rejectedCode]() {
        rejectedCode = rejected.closeCode();
    });

    rejected.open(endpoint);

    ASSERT_TRUE(waitFor([&rejectedCode]() {
        return rejectedCode != QWebSocketProtocol::CloseCodeNormal;
    }));
    EXPECT_EQ(rejectedCode, QWebSocketProtocol::CloseCodePolicyViolated);
    EXPECT_EQ(server.m_clients.size(), 8);

    accepted.front()->close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 7; }));
    QWebSocket replacement;
    replacement.open(endpoint);
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 8; }));
    replacement.close();
    for (const auto& socket : accepted) socket->close();
    EXPECT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

TEST(GameServerAdmissionTest, AuthenticatedCapacityDoesNotConsumeUnauthenticatedCapacity) {
    int argc = 1;
    char applicationName[] = "authoritative_room_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    GameServer server;
    ASSERT_TRUE(server.m_server.listen(QHostAddress::LocalHost, 0));
    std::vector<std::unique_ptr<QWebSocket>> authenticated;
    for (int i = 0; i < 64; ++i) {
        authenticated.push_back(std::make_unique<QWebSocket>());
        GameServer::ClientSession session;
        session.authenticated = true;
        session.userId = i + 1;
        server.m_clients.insert(authenticated.back().get(), session);
    }
    QWebSocket candidate;
    QWebSocketProtocol::CloseCode closeCode = QWebSocketProtocol::CloseCodeNormal;
    QObject::connect(&candidate, &QWebSocket::disconnected, [&candidate, &closeCode]() {
        closeCode = candidate.closeCode();
    });

    candidate.open(QUrl(QStringLiteral("ws://127.0.0.1:%1")
                            .arg(server.m_server.serverPort())));

    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 65; }));
    QWebSocket* candidateSocket = nullptr;
    for (auto it = server.m_clients.cbegin(); it != server.m_clients.cend(); ++it) {
        if (!it->authenticated) candidateSocket = it.key();
    }
    ASSERT_NE(candidateSocket, nullptr);

    server.finishAuthentication(candidateSocket,
                                QJsonObject{{QStringLiteral("valid"), true},
                                            {QStringLiteral("userId"), 1000},
                                            {QStringLiteral("username"), QStringLiteral("candidate")},
                                            {QStringLiteral("displayName"), QStringLiteral("Candidate")}});

    ASSERT_TRUE(waitFor([&closeCode]() {
        return closeCode != QWebSocketProtocol::CloseCodeNormal;
    }));
    EXPECT_EQ(closeCode, QWebSocketProtocol::CloseCodePolicyViolated);
    EXPECT_EQ(server.authenticatedClientCount(), 64);
}

TEST(GameServerAdmissionTest, ThirtyThirdUnauthenticatedConnectionIsRejectedGlobally) {
    int argc = 1;
    char applicationName[] = "authoritative_room_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    GameServer server;
    ASSERT_TRUE(server.m_server.listen(QHostAddress::LocalHost, 0));
    std::vector<std::unique_ptr<QWebSocket>> unauthenticated;
    for (int i = 0; i < 32; ++i) {
        unauthenticated.push_back(std::make_unique<QWebSocket>());
        GameServer::ClientSession session;
        session.peerAddress = QHostAddress(QStringLiteral("192.0.2.%1").arg(i + 1));
        server.m_clients.insert(unauthenticated.back().get(), session);
    }
    QWebSocket rejected;
    QWebSocketProtocol::CloseCode closeCode = QWebSocketProtocol::CloseCodeNormal;
    QObject::connect(&rejected, &QWebSocket::disconnected, [&rejected, &closeCode]() {
        closeCode = rejected.closeCode();
    });

    rejected.open(QUrl(QStringLiteral("ws://127.0.0.1:%1")
                           .arg(server.m_server.serverPort())));

    ASSERT_TRUE(waitFor([&closeCode]() {
        return closeCode != QWebSocketProtocol::CloseCodeNormal;
    }));
    EXPECT_EQ(closeCode, QWebSocketProtocol::CloseCodePolicyViolated);
    EXPECT_EQ(server.m_clients.size(), 32);
}

TEST(GameServerAdmissionTest, UnauthenticatedConnectionClosesAfterFiveSeconds) {
    int argc = 1;
    char applicationName[] = "authoritative_room_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    GameServer server;
    ASSERT_TRUE(server.m_server.listen(QHostAddress::LocalHost, 0));
    QWebSocket socket;
    QElapsedTimer elapsed;
    QWebSocketProtocol::CloseCode closeCode = QWebSocketProtocol::CloseCodeNormal;
    QObject::connect(&socket, &QWebSocket::connected, [&elapsed]() { elapsed.start(); });
    QObject::connect(&socket, &QWebSocket::disconnected, [&socket, &closeCode]() {
        closeCode = socket.closeCode();
    });

    socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1")
                         .arg(server.m_server.serverPort())));

    ASSERT_TRUE(waitFor([&closeCode]() {
        return closeCode != QWebSocketProtocol::CloseCodeNormal;
    }, 7000));
    EXPECT_EQ(closeCode, QWebSocketProtocol::CloseCodePolicyViolated);
    EXPECT_GE(elapsed.elapsed(), 4500);
    EXPECT_LT(elapsed.elapsed(), 6500);
}

TEST(GameServerIngressTest, QStringPreflightPrecedesAndRetainsUtf8ByteLimit) {
    const qsizetype preflightLimit = Protocol::MaxServerMessageBytes / 3;
    EXPECT_FALSE(GameServer::incomingTextExceedsPreflight(
        QString(preflightLimit, QLatin1Char('a'))));
    EXPECT_TRUE(GameServer::incomingTextExceedsPreflight(
        QString(preflightLimit + 1, QLatin1Char('a'))));
    const QString bmpBoundary(preflightLimit, QChar(0x4e2d));
    EXPECT_FALSE(GameServer::incomingTextExceedsPreflight(bmpBoundary));
    EXPECT_LE(bmpBoundary.toUtf8().size(), Protocol::MaxServerMessageBytes);
    QString surrogateBoundary;
    surrogateBoundary.reserve(preflightLimit);
    while (surrogateBoundary.size() + 2 <= preflightLimit) {
        surrogateBoundary.append(QChar::highSurrogate(0x1f642));
        surrogateBoundary.append(QChar::lowSurrogate(0x1f642));
    }
    if (surrogateBoundary.size() < preflightLimit) surrogateBoundary.append(QLatin1Char('a'));
    ASSERT_EQ(surrogateBoundary.size(), preflightLimit);
    EXPECT_FALSE(GameServer::incomingTextExceedsPreflight(surrogateBoundary));
    surrogateBoundary.append(QLatin1Char('a'));
    EXPECT_TRUE(GameServer::incomingTextExceedsPreflight(surrogateBoundary));

    const QString multibyte(Protocol::MaxMessageBytes / 2, QChar(0x4e2d));
    ASSERT_FALSE(GameServer::incomingTextExceedsPreflight(multibyte));
    ASSERT_GT(multibyte.toUtf8().size(), Protocol::MaxMessageBytes);

    int argc = 1;
    char applicationName[] = "authoritative_room_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    GameServer server;
    ASSERT_TRUE(server.m_server.listen(QHostAddress::LocalHost, 0));
    QWebSocket socket;
    QWebSocketProtocol::CloseCode closeCode = QWebSocketProtocol::CloseCodeNormal;
    QObject::connect(&socket, &QWebSocket::disconnected, [&socket, &closeCode]() {
        closeCode = socket.closeCode();
    });
    socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1")
                         .arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));

    socket.sendTextMessage(multibyte);

    ASSERT_TRUE(waitFor([&closeCode]() {
        return closeCode != QWebSocketProtocol::CloseCodeNormal;
    }));
    EXPECT_EQ(closeCode, QWebSocketProtocol::CloseCodeTooMuchData);
}

TEST(GameServerAuthenticationTest, TimeoutAndServerErrorRetryThenRecovers) {
    int argc = 1;
    char applicationName[] = "authentication_retry_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    RetryAuthenticationReceiver authentication({503, 200}, true);
    ASSERT_TRUE(authentication.listen());
    configureGameServerEnvironment(temporary, authentication.port());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.listen(0));
    server.m_snapshotTimer.stop();
    server.m_roomSyncTimer.stop();
    server.m_presenceTimer.stop();
    server.m_monitorStatusTimer.stop();

    QList<QJsonObject> messages;
    QWebSocket client;
    QObject::connect(&client, &QWebSocket::textMessageReceived,
                     [&messages](const QString& text) {
                         messages.append(QJsonDocument::fromJson(text.toUtf8()).object());
                     });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1")
                         .arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* serverSocket = server.m_clients.keys().constFirst();
    sendClientEnvelope(client, QStringLiteral("auth"), QStringLiteral("auth-retry"),
                       QJsonObject{{QStringLiteral("token"), QStringLiteral("valid-token")}});

    ASSERT_TRUE(waitFor([&server, serverSocket]() {
        return server.m_clients.contains(serverSocket)
            && server.m_clients.value(serverSocket).authenticated;
    }, 14000)) << "sessionRequests=" << authentication.sessionRequests()
               << " responseStatuses=" << authentication.responseStatuses().size()
               << " requestTimes=" << authentication.sessionRequestTimes().size()
               << " attempts=" << server.m_totalAuthenticationAttempts
               << " retries=" << server.m_totalAuthenticationRetries
               << " pending=" << server.m_clients.value(serverSocket).authenticationPending;
    EXPECT_EQ(authentication.sessionRequests(), 3);
    EXPECT_EQ(authentication.responseStatuses(), QList<int>({503, 200}));
    const QList<qint64> requestTimes = authentication.sessionRequestTimes();
    ASSERT_EQ(requestTimes.size(), 3);
    EXPECT_GE(requestTimes.at(1) - requestTimes.at(0), 5000);
    EXPECT_LT(requestTimes.at(1) - requestTimes.at(0), 6500);
    EXPECT_GE(requestTimes.at(2) - requestTimes.at(1), 450);
    EXPECT_LT(requestTimes.at(2) - requestTimes.at(1), 1200);
    EXPECT_FALSE(server.m_clients.value(serverSocket).authenticationPending);
    EXPECT_TRUE(std::any_of(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
        return envelope.value(QStringLiteral("type")).toString() == QLatin1String("welcome");
    }));

    server.writeMonitorStatus();
    QFile statusFile(temporary.filePath(QStringLiteral("status.json")));
    ASSERT_TRUE(statusFile.open(QIODevice::ReadOnly));
    const QJsonObject status = QJsonDocument::fromJson(statusFile.readAll()).object();
    EXPECT_EQ(status.value(QStringLiteral("status")).toString(), QLatin1String("healthy"));
    EXPECT_EQ(status.value(QStringLiteral("authentication")).toObject()
                  .value(QStringLiteral("status")).toString(), QLatin1String("healthy"));

    client.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

TEST(GameServerAuthenticationTest, InvalidCredentialsFailImmediatelyWithoutRetry) {
    int argc = 1;
    char applicationName[] = "authentication_invalid_credentials_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    RetryAuthenticationReceiver authentication({401}, false);
    ASSERT_TRUE(authentication.listen());
    configureGameServerEnvironment(temporary, authentication.port());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.listen(0));
    server.m_snapshotTimer.stop();
    server.m_roomSyncTimer.stop();
    server.m_presenceTimer.stop();
    server.m_monitorStatusTimer.stop();

    bool invalidErrorReceived = false;
    bool pendingWasCleared = false;
    QWebSocket client;
    QWebSocket* serverSocket = nullptr;
    QObject::connect(&client, &QWebSocket::textMessageReceived,
                     [&server, &serverSocket, &invalidErrorReceived, &pendingWasCleared](
                         const QString& text) {
                         const QJsonObject envelope = QJsonDocument::fromJson(text.toUtf8()).object();
                         const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
                         if (envelope.value(QStringLiteral("type")).toString() != QLatin1String("error")
                             || payload.value(QStringLiteral("code")).toString()
                                    != QLatin1String("INVALID_TOKEN")) return;
                         invalidErrorReceived = true;
                         pendingWasCleared = serverSocket != nullptr
                             && server.m_clients.contains(serverSocket)
                             && !server.m_clients.value(serverSocket).authenticationPending;
                     });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1")
                         .arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    serverSocket = server.m_clients.keys().constFirst();
    sendClientEnvelope(client, QStringLiteral("auth"), QStringLiteral("auth-invalid"),
                       QJsonObject{{QStringLiteral("token"), QStringLiteral("invalid-token")}});

    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
    EXPECT_TRUE(invalidErrorReceived);
    EXPECT_TRUE(pendingWasCleared);
    EXPECT_EQ(authentication.sessionRequests(), 1);
    EXPECT_EQ(authentication.responseStatuses(), QList<int>({401}));

    server.writeMonitorStatus();
    QFile statusFile(temporary.filePath(QStringLiteral("status.json")));
    ASSERT_TRUE(statusFile.open(QIODevice::ReadOnly));
    const QJsonObject status = QJsonDocument::fromJson(statusFile.readAll()).object();
    EXPECT_EQ(status.value(QStringLiteral("status")).toString(), QLatin1String("healthy"));
    EXPECT_EQ(status.value(QStringLiteral("authentication")).toObject()
                  .value(QStringLiteral("status")).toString(), QLatin1String("healthy"));
}

TEST(GameServerAuthenticationTest, ExhaustedTransientFailuresCloseAndClearPending) {
    int argc = 1;
    char applicationName[] = "authentication_exhaustion_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    RetryAuthenticationReceiver authentication({503, 503, 503}, false);
    ASSERT_TRUE(authentication.listen());
    configureGameServerEnvironment(temporary, authentication.port());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.listen(0));
    server.m_snapshotTimer.stop();
    server.m_roomSyncTimer.stop();
    server.m_presenceTimer.stop();
    server.m_monitorStatusTimer.stop();

    QWebSocket client;
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1")
                         .arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* serverSocket = server.m_clients.keys().constFirst();
    sendClientEnvelope(client, QStringLiteral("auth"), QStringLiteral("auth-exhausted"),
                       QJsonObject{{QStringLiteral("token"), QStringLiteral("valid-token")}});

    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }, 5000));
    EXPECT_EQ(authentication.sessionRequests(), 3);
    EXPECT_EQ(authentication.responseStatuses(), QList<int>({503, 503, 503}));
    EXPECT_FALSE(server.m_clients.contains(serverSocket));

    server.writeMonitorStatus();
    QFile statusFile(temporary.filePath(QStringLiteral("status.json")));
    ASSERT_TRUE(statusFile.open(QIODevice::ReadOnly));
    const QJsonObject status = QJsonDocument::fromJson(statusFile.readAll()).object();
    EXPECT_EQ(status.value(QStringLiteral("status")).toString(), QLatin1String("degraded"));
    const QJsonObject authenticationStatus = status.value(QStringLiteral("authentication")).toObject();
    EXPECT_EQ(authenticationStatus.value(QStringLiteral("status")).toString(),
              QLatin1String("unavailable"));
    EXPECT_EQ(authenticationStatus.value(QStringLiteral("pending")).toInt(), 0);
}

TEST(GameServerPermissionTest, SeatActionAllowlistDefaultsToDenyUnknownActions) {
    int argc = 1;
    char applicationName[] = "authoritative_room_permission_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();

    GameServer::ClientSession commander;
    commander.seatId = QStringLiteral("red_commander");
    commander.seatType = QStringLiteral("commander");
    commander.side = QStringLiteral("red");
    EXPECT_TRUE(server.hasSeatPermission(commander, QStringLiteral("deployment")));
    EXPECT_TRUE(server.hasSeatPermission(commander, QStringLiteral("shareIntel")));
    EXPECT_FALSE(server.hasSeatPermission(commander, QStringLiteral("command")));
    EXPECT_FALSE(server.hasSeatPermission(commander, QStringLiteral("unknown")));

    GameServer::ClientSession participant = commander;
    participant.seatId = QStringLiteral("red_attack_1");
    participant.seatType = QStringLiteral("attack");
    EXPECT_FALSE(server.hasSeatPermission(participant, QStringLiteral("deployment")));
    EXPECT_TRUE(server.hasSeatPermission(participant, QStringLiteral("shareIntel")));

    participant.seatId.clear();
    EXPECT_FALSE(server.hasSeatPermission(participant, QStringLiteral("shareIntel")));
}

TEST(AuthoritativeRoomTest, ResetAndRedeployHaveDistinctAtomicSemantics) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_commander"), GeoPos{10.0, 10.0, 0.0}).ok);
    ASSERT_TRUE(room.deploy(2, QStringLiteral("blue_commander"), GeoPos{20.0, 20.0, 0.0}).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_attack_1"), GeoPos{100.0, 200.0, 20.0}).ok);
    ASSERT_TRUE(room.setReady(2, true).ok);
    ASSERT_TRUE(room.setReady(3, true).ok);
    ASSERT_TRUE(room.setReady(1, true).ok);

    const quint64 beforeRedeploy = room.revision();
    const auto redeploy = room.applyOperation(QStringLiteral("op-r"), QStringLiteral("redeploy"),
                                               beforeRedeploy);
    ASSERT_TRUE(redeploy.ok) << redeploy.code.toStdString();
    EXPECT_EQ(room.seats().size(), 3);
    EXPECT_TRUE(room.runtimeUnits().isEmpty());
    EXPECT_FALSE(room.seat(QStringLiteral("red_attack_1")).deployed);
    EXPECT_FALSE(room.seat(QStringLiteral("red_attack_1")).ready);
    EXPECT_FALSE(room.seat(QStringLiteral("red_attack_1")).pendingTransfer);
    EXPECT_EQ(room.seat(QStringLiteral("red_attack_1")).selectedTemplate,
              QStringLiteral("attackuav"));

    const quint64 beforeReset = room.revision();
    const auto reset = room.applyOperation(QStringLiteral("op-x"), QStringLiteral("reset"),
                                           beforeReset);
    ASSERT_TRUE(reset.ok) << reset.code.toStdString();
    EXPECT_TRUE(room.seats().isEmpty());
    EXPECT_TRUE(room.runtimeUnits().isEmpty());
    EXPECT_EQ(room.phase(), QStringLiteral("preparing"));
    EXPECT_FALSE(room.readiness().value(QStringLiteral("ready")).toBool());
}

TEST(AuthoritativeRoomTest, RedeployClearsPendingTransferState) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    const auto transfer = room.requestTransfer(3, QStringLiteral("red_recon_1"),
                                               QStringLiteral("reconuav"));
    EXPECT_EQ(transfer.code, QStringLiteral("TRANSFER_PENDING"));
    ASSERT_TRUE(room.applyOperation(QStringLiteral("redeploy-transfer"),
                                    QStringLiteral("redeploy"), room.revision()).ok);
    EXPECT_FALSE(room.seat(QStringLiteral("red_attack_1")).pendingTransfer);
    EXPECT_EQ(room.approveTransfer(1, 3, transfer.revision).code,
              QStringLiteral("STALE_TRANSFER"));
    EXPECT_TRUE(room.hasSeat(QStringLiteral("red_attack_1")));
}

TEST(AuthoritativeRoomTest, FriendlyRedeployRequestOnlyClearsRequestedUnit) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(room.claimSeat(4, QStringLiteral("scout"), QStringLiteral("red_recon_1"),
                               QStringLiteral("reconuav")).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_commander"), GeoPos{10.0, 10.0, 0.0}).ok);
    ASSERT_TRUE(room.deploy(2, QStringLiteral("blue_commander"), GeoPos{20.0, 20.0, 0.0}).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_attack_1"), GeoPos{30.0, 30.0, 10.0}).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_recon_1"), GeoPos{40.0, 40.0, 10.0}).ok);
    ASSERT_TRUE(room.setReady(3, true).ok);

    ASSERT_TRUE(room.requestRedeploy(3).ok);
    EXPECT_TRUE(room.seat(QStringLiteral("red_attack_1")).redeployRequested);
    EXPECT_EQ(room.redeploy(3).code, QStringLiteral("PERMISSION_DENIED"));
    ASSERT_TRUE(room.redeploy(1).ok);
    EXPECT_TRUE(room.seat(QStringLiteral("red_commander")).deployed);
    EXPECT_FALSE(room.seat(QStringLiteral("red_attack_1")).deployed);
    EXPECT_FALSE(room.seat(QStringLiteral("red_attack_1")).redeployRequested);
    EXPECT_TRUE(room.seat(QStringLiteral("red_recon_1")).deployed);
    EXPECT_TRUE(room.seat(QStringLiteral("blue_commander")).deployed);
}

TEST(AuthoritativeRoomTest, DeployedUnitUsesItsFactionCallsSign) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_attack_1"), GeoPos{10.0, 10.0, 0.0}).ok);
    const QJsonArray units = room.runtimeUnits();
    ASSERT_EQ(units.size(), 1);
    EXPECT_TRUE(units.at(0).toObject().value(QStringLiteral("callsign")).toString()
                    .startsWith(QStringLiteral("红方")));
}

TEST(AuthoritativeRoomTest, OperationsAreIdempotentAndRejectStaleRevision) {
    auto room = configuredRoom();
    const quint64 requested = room.revision();
    const auto first = room.applyOperation(QStringLiteral("same-op"), QStringLiteral("reset"),
                                           requested);
    ASSERT_TRUE(first.ok);
    const auto duplicate = room.applyOperation(QStringLiteral("same-op"), QStringLiteral("reset"),
                                               requested);
    EXPECT_TRUE(duplicate.ok);
    EXPECT_TRUE(duplicate.duplicate);
    EXPECT_EQ(duplicate.revision, first.revision);
    EXPECT_EQ(room.revision(), first.revision);

    const auto stale = room.applyOperation(QStringLiteral("stale-op"), QStringLiteral("redeploy"),
                                           requested);
    EXPECT_FALSE(stale.ok);
    EXPECT_EQ(stale.code, QStringLiteral("STALE_REVISION"));
}

TEST(GameServerCommandTest, FutureCommandResultIsStableForDuplicateCommandId) {
    int argc = 1;
    char applicationName[] = "authoritative_room_command_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
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
    QWebSocket socket;
    auto& session = server.m_clients[&socket];
    session.authenticated = true;
    session.userId = 7;
    server.m_phase = QStringLiteral("running");
    server.m_stateRevision = 5;
    const QJsonObject payload{{QStringLiteral("commandId"), QStringLiteral("stale-command")},
                              {QStringLiteral("action"), QStringLiteral("withdraw")},
                              {QStringLiteral("stateRevision"), 6},
                              {QStringLiteral("args"),
                               QJsonObject{{QStringLiteral("unitId"), QStringLiteral("red_r1")}}}};

    server.handleCommand(&socket, payload);
    const QString cacheKey = QStringLiteral("7:stale-command");
    ASSERT_TRUE(server.m_commandResults.contains(cacheKey));
    const QJsonObject first = server.m_commandResults.value(cacheKey);
    EXPECT_EQ(first.value(QStringLiteral("code")).toString(), QStringLiteral("STALE_REVISION"));

    server.m_stateRevision = 7;
    server.handleCommand(&socket, payload);
    EXPECT_EQ(server.m_commandResults.value(cacheKey), first);
    EXPECT_EQ(server.m_commandResultOrder.count(cacheKey), 1);
}

TEST(GameServerCommandTest, SharedUnitSpeedLimitIsEnforcedByAuthority) {
    int argc = 1;
    char applicationName[] = "authoritative_speed_limit_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        1, QStringLiteral("red-commander"), QStringLiteral("red_commander"),
        QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        3, QStringLiteral("blue-commander"), QStringLiteral("blue_commander"),
        QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        2, QStringLiteral("red-pilot"), QStringLiteral("red_attack_1"),
        QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
        1, QStringLiteral("red_commander"), GeoPos{1000.0, 1000.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
        1, QStringLiteral("red_attack_1"), GeoPos{1200.0, 1000.0, 2000.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
        3, QStringLiteral("blue_commander"), GeoPos{19000.0, 1000.0, 0.0}).ok);
    server.syncAuthoritativeSeats();
    QString scenarioError;
    ASSERT_TRUE(server.applyDeployedScenario(&scenarioError)) << scenarioError.toStdString();

    GameServer::ClientSession session;
    session.authenticated = true;
    session.userId = 2;
    session.roomId = server.m_roomId;
    session.seatId = QStringLiteral("red_attack_1");
    session.seatType = QStringLiteral("attack");
    session.side = QStringLiteral("red");
    session.role = session.seatId;
    const QString unitId = server.m_authoritativeRoom.seat(session.seatId).unitId;
    ASSERT_FALSE(unitId.isEmpty());

    QString code;
    QString reason;
    EXPECT_TRUE(server.validateCommandOwnership(
        session, QStringLiteral("setSpeed"),
        QVariantMap{{QStringLiteral("unitId"), unitId}, {QStringLiteral("speed"), 240.0}},
        &code, &reason));
    EXPECT_FALSE(server.validateCommandOwnership(
        session, QStringLiteral("setSpeed"),
        QVariantMap{{QStringLiteral("unitId"), unitId}, {QStringLiteral("speed"), 241.0}},
        &code, &reason));
    EXPECT_EQ(code, QStringLiteral("INVALID_ARGUMENT"));
    EXPECT_EQ(reason, QStringLiteral("速度必须大于 0 且不超过 240"));
}

TEST(GameServerCommandTest, UnitNameRevisionChangesAndUnitOrderTargetsOwnedUnit) {
    int argc = 1;
    char applicationName[] = "authoritative_room_name_command_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    qputenv("INTERNAL_API_KEY", QByteArray(32, 'n'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:1"));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(1, QStringLiteral("commander"),
                                                     QStringLiteral("red_commander"),
                                                     QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(3, QStringLiteral("blue"),
                                                     QStringLiteral("blue_commander"),
                                                     QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(2, QStringLiteral("pilot"),
                                                     QStringLiteral("red_attack_1"),
                                                     QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(1, QStringLiteral("red_commander"),
                                                  GeoPos{1000.0, 1000.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(1, QStringLiteral("red_attack_1"),
                                                  GeoPos{3000.0, 3000.0, 2000.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(3, QStringLiteral("blue_commander"),
                                                  GeoPos{9000.0, 9000.0, 0.0}).ok);
    server.syncAuthoritativeSeats();
    QString scenarioError;
    ASSERT_TRUE(server.applyDeployedScenario(&scenarioError)) << scenarioError.toStdString();
    const QString commandPostId = server.m_authoritativeRoom.seat(QStringLiteral("red_commander")).unitId;
    const QString attackUnitId = server.m_authoritativeRoom.seat(QStringLiteral("red_attack_1")).unitId;
    ASSERT_NE(server.m_engine.unit(commandPostId), nullptr);
    ASSERT_NE(server.m_engine.unit(attackUnitId), nullptr);

    QWebSocket socket;
    auto& session = server.m_clients[&socket];
    session.authenticated = true;
    session.userId = 1;
    session.roomId = server.m_roomId;
    session.seatId = QStringLiteral("red_commander");
    session.seatType = QStringLiteral("commander");
    session.side = QStringLiteral("red");
    server.m_phase = QStringLiteral("preparing");
    const quint64 revisionBeforeName = server.m_scenarioRevision;
    server.handleSetUnitName(&socket, QJsonObject{{QStringLiteral("unitName"), QStringLiteral("长机一号")}});
    EXPECT_GT(server.m_scenarioRevision, revisionBeforeName);
    ASSERT_EQ(server.m_engine.unit(commandPostId)->callsign(),
              QStringLiteral("长机一号"));
    EXPECT_EQ(server.m_authoritativeRoom.seat(QStringLiteral("red_commander")).unitName,
              QStringLiteral("长机一号"));

    server.m_phase = QStringLiteral("running");
    server.m_stateRevision = 1;
    const QString blueCommandPostId = server.m_authoritativeRoom
                                          .seat(QStringLiteral("blue_commander")).unitId;
    server.m_sharedIntel[QStringLiteral("red_commander")].insert(blueCommandPostId);
    const QVariantList before = server.m_engine.unit(attackUnitId)->position();
    server.handleCommand(&socket,
                         QJsonObject{{QStringLiteral("commandId"), QStringLiteral("unit-order")},
                                     {QStringLiteral("action"), QStringLiteral("unitOrder")},
                                     {QStringLiteral("stateRevision"), 1},
                                     {QStringLiteral("args"),
                                      QJsonObject{{QStringLiteral("unitId"), attackUnitId},
                                                  {QStringLiteral("text"), QStringLiteral("保持队形")}}}});
    const QJsonObject result = server.m_commandResults.value(QStringLiteral("1:unit-order"));
    EXPECT_TRUE(result.value(QStringLiteral("accepted")).toBool());
    const QVariantList after = server.m_engine.unit(attackUnitId)->position();
    EXPECT_EQ(after, before);
    ASSERT_FALSE(server.m_engine.recentMessages().isEmpty());
    QVariantMap message = server.m_engine.recentMessages().constFirst().toMap();
    EXPECT_EQ(message.value(QStringLiteral("receiver")).toString(), attackUnitId);
    EXPECT_TRUE(message.value(QStringLiteral("payload")).toMap()
                    .value(QStringLiteral("notificationOnly")).toBool());

    server.handleCommand(&socket,
                         QJsonObject{{QStringLiteral("commandId"), QStringLiteral("attack-order")},
                                     {QStringLiteral("action"), QStringLiteral("assignTarget")},
                                     {QStringLiteral("stateRevision"), 1},
                                     {QStringLiteral("args"),
                                      QJsonObject{{QStringLiteral("attackerId"), attackUnitId},
                                                  {QStringLiteral("targetId"), blueCommandPostId}}}});
    const QJsonObject attackResult = server.m_commandResults.value(QStringLiteral("1:attack-order"));
    EXPECT_TRUE(attackResult.value(QStringLiteral("accepted")).toBool());
    message = server.m_engine.recentMessages().constFirst().toMap();
    EXPECT_EQ(message.value(QStringLiteral("type")).toString(), QStringLiteral("AttackOrder"));
    EXPECT_TRUE(message.value(QStringLiteral("payload")).toMap()
                    .value(QStringLiteral("notificationOnly")).toBool());
}

TEST(AuthoritativeRoomTest, UnitCreationIsStableAndRollsBackInvalidSelection) {
    auto room = configuredRoom();
    claimCommanders(room);
    const quint64 before = room.revision();
    const auto invalid = room.claimSeat(3, QStringLiteral("pilot"),
                                        QStringLiteral("red_attack_1"),
                                        QStringLiteral("missing"));
    EXPECT_FALSE(invalid.ok);
    EXPECT_EQ(room.revision(), before);
    EXPECT_FALSE(room.hasUser(3));

    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    const QString firstId = room.seat(QStringLiteral("red_attack_1")).unitId;
    ASSERT_FALSE(firstId.isEmpty());
    ASSERT_TRUE(room.leave(3).ok);
    ASSERT_TRUE(room.claimSeat(4, QStringLiteral("next"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    EXPECT_NE(room.seat(QStringLiteral("red_attack_1")).unitId, firstId);
}

TEST(AuthoritativeRoomTest, StartRequiresCommandersAndEveryOccupiedSeatReadyAndDeployed) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_commander"), GeoPos{10.0, 10.0, 0.0}).ok);
    ASSERT_TRUE(room.deploy(2, QStringLiteral("blue_commander"), GeoPos{20.0, 20.0, 0.0}).ok);
    ASSERT_TRUE(room.setReady(2, true).ok);
    EXPECT_FALSE(room.start().ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_attack_1"), GeoPos{30.0, 30.0, 10.0}).ok);
    EXPECT_FALSE(room.start().ok);
    ASSERT_TRUE(room.setReady(3, true).ok);
    ASSERT_TRUE(room.setReady(1, true).ok);
    EXPECT_TRUE(room.start().ok);
    EXPECT_EQ(room.phase(), QStringLiteral("running"));
}

TEST(AuthoritativeRoomTest, CommanderCannotReadyUntilEveryFriendlyOccupiedSeatIsReady) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_commander"), GeoPos{10.0, 10.0, 0.0}).ok);
    ASSERT_TRUE(room.deploy(2, QStringLiteral("blue_commander"), GeoPos{20.0, 20.0, 0.0}).ok);

    EXPECT_EQ(room.setReady(1, true).code, QStringLiteral("FRIENDLY_SEATS_NOT_READY"));

    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_attack_1"), GeoPos{30.0, 30.0, 10.0}).ok);
    EXPECT_EQ(room.setReady(1, true).code, QStringLiteral("FRIENDLY_SEATS_NOT_READY"));
    ASSERT_TRUE(room.setReady(3, true).ok);
    EXPECT_TRUE(room.setReady(1, true).ok);
}

TEST(AuthoritativeRoomTest, NewFriendlySeatClearsExistingCommanderReadiness) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_commander"), GeoPos{10.0, 10.0, 0.0}).ok);
    ASSERT_TRUE(room.setReady(1, true).ok);

    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    EXPECT_FALSE(room.seat(QStringLiteral("red_commander")).ready);
}

TEST(AuthoritativeRoomTest, SwitchRequiresSameSideCommanderApproval) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    EXPECT_EQ(room.requestTransfer(3, QStringLiteral("red_recon_1"),
                                   QStringLiteral("reconuav")).code,
              QStringLiteral("TRANSFER_PENDING"));
    EXPECT_EQ(room.approveTransfer(2, 3, room.revision()).code,
              QStringLiteral("PERMISSION_DENIED"));
    ASSERT_TRUE(room.approveTransfer(1, 3, room.revision()).ok);
    EXPECT_FALSE(room.hasSeat(QStringLiteral("red_attack_1")));
    EXPECT_EQ(room.seat(QStringLiteral("red_recon_1")).userId, 3);
}

TEST(AuthoritativeRoomTest, PausedRoomRejectsSeatTransferRequests) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_commander"), GeoPos{10.0, 10.0, 0.0}).ok);
    ASSERT_TRUE(room.deploy(2, QStringLiteral("blue_commander"), GeoPos{20.0, 20.0, 0.0}).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_attack_1"), GeoPos{30.0, 30.0, 10.0}).ok);
    ASSERT_TRUE(room.setReady(2, true).ok);
    ASSERT_TRUE(room.setReady(3, true).ok);
    ASSERT_TRUE(room.setReady(1, true).ok);
    ASSERT_TRUE(room.start().ok);
    ASSERT_TRUE(room.pause().ok);

    const auto transfer = room.requestTransfer(3, QStringLiteral("red_recon_1"),
                                               QStringLiteral("reconuav"));

    EXPECT_FALSE(transfer.ok);
    EXPECT_EQ(transfer.code, QStringLiteral("SEAT_LOCKED"));
    EXPECT_TRUE(room.hasSeat(QStringLiteral("red_attack_1")));
    EXPECT_FALSE(room.seat(QStringLiteral("red_attack_1")).pendingTransfer);
}

TEST(AuthoritativeRoomTest, TransferCancelRejectAndStaleApprovalKeepOriginalSeat) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);

    const auto requested = room.requestTransfer(3, QStringLiteral("red_recon_1"),
                                                QStringLiteral("reconuav"));
    ASSERT_EQ(requested.code, QStringLiteral("TRANSFER_PENDING"));
    EXPECT_TRUE(room.seat(QStringLiteral("red_attack_1")).pendingTransfer);
    EXPECT_EQ(room.requestTransfer(3, QStringLiteral("red_ground_1"),
                                   QStringLiteral("groundscout")).code,
              QStringLiteral("TRANSFER_ALREADY_PENDING"));
    EXPECT_TRUE(room.cancelTransfer(3, requested.revision).ok);
    EXPECT_FALSE(room.seat(QStringLiteral("red_attack_1")).pendingTransfer);
    EXPECT_TRUE(room.hasSeat(QStringLiteral("red_attack_1")));
    EXPECT_FALSE(room.hasSeat(QStringLiteral("red_recon_1")));

    const auto rejectedRequest = room.requestTransfer(3, QStringLiteral("red_recon_1"),
                                                      QStringLiteral("reconuav"));
    ASSERT_EQ(rejectedRequest.code, QStringLiteral("TRANSFER_PENDING"));
    EXPECT_TRUE(room.rejectTransfer(1, 3, rejectedRequest.revision).ok);
    EXPECT_TRUE(room.hasSeat(QStringLiteral("red_attack_1")));
    EXPECT_FALSE(room.hasSeat(QStringLiteral("red_recon_1")));

    const auto staleRequest = room.requestTransfer(3, QStringLiteral("red_recon_1"),
                                                   QStringLiteral("reconuav"));
    ASSERT_EQ(staleRequest.code, QStringLiteral("TRANSFER_PENDING"));
    ASSERT_TRUE(room.setUnitName(3, QStringLiteral("unchanged-seat")).ok);
    EXPECT_EQ(room.approveTransfer(1, 3, staleRequest.revision).code,
              QStringLiteral("STALE_TRANSFER"));
    EXPECT_TRUE(room.hasSeat(QStringLiteral("red_attack_1")));
    EXPECT_FALSE(room.hasSeat(QStringLiteral("red_recon_1")));
    EXPECT_FALSE(room.seat(QStringLiteral("red_attack_1")).pendingTransfer);

    const auto racedRequest = room.requestTransfer(3, QStringLiteral("red_ground_1"),
                                                   QStringLiteral("groundscout"));
    ASSERT_EQ(racedRequest.code, QStringLiteral("TRANSFER_PENDING"));
    ASSERT_TRUE(room.claimSeat(4, QStringLiteral("other"), QStringLiteral("red_ground_1"),
                               QStringLiteral("groundscout")).ok);
    EXPECT_EQ(room.approveTransfer(1, 3, racedRequest.revision).code,
              QStringLiteral("STALE_TRANSFER"));
    EXPECT_EQ(room.seat(QStringLiteral("red_attack_1")).userId, 3);
    EXPECT_EQ(room.seat(QStringLiteral("red_ground_1")).userId, 4);
    EXPECT_FALSE(room.seat(QStringLiteral("red_attack_1")).pendingTransfer);
}

TEST(AuthoritativeRoomTest, CommanderExitRequiresConfirmedFriendlySuccessor) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    EXPECT_EQ(room.leave(1).code, QStringLiteral("SUCCESSOR_REQUIRED"));
    ASSERT_TRUE(room.leave(1, 3).ok);
    EXPECT_EQ(room.seat(QStringLiteral("red_commander")).userId, 3);
    EXPECT_FALSE(room.hasUser(1));
}

TEST(AuthoritativeRoomTest, PreparingCommanderCanReleaseAnUnstaffedSeat) {
    auto room = configuredRoom();
    claimCommanders(room);

    ASSERT_TRUE(room.leaveRoom(1).ok);
    EXPECT_FALSE(room.hasUser(1));
    EXPECT_FALSE(room.hasSeat(QStringLiteral("red_commander")));
    EXPECT_EQ(room.phase(), QStringLiteral("preparing"));
}

TEST(AuthoritativeRoomTest, DisconnectPromotesDeterministicallyAndPersistsChoice) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("a"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(room.claimSeat(4, QStringLiteral("r"), QStringLiteral("red_recon_1"),
                               QStringLiteral("reconuav")).ok);
    const auto disconnected = room.disconnect(1);
    ASSERT_TRUE(disconnected.ok);
    ASSERT_GT(disconnected.successorUserId, 0);

    QString error;
    const QJsonObject saved = room.toJson();
    auto restored = configuredRoom();
    ASSERT_TRUE(restored.restore(saved, &error)) << error.toStdString();
    EXPECT_EQ(restored.seat(QStringLiteral("red_commander")).userId,
              disconnected.successorUserId);
    EXPECT_EQ(restored.rngState(), room.rngState());
}

TEST(AuthoritativeRoomTest, PreparingCommanderDisconnectDoesNotForfeitMatch) {
    auto room = configuredRoom();
    claimCommanders(room);

    const auto disconnected = room.disconnect(1);

    ASSERT_TRUE(disconnected.ok);
    EXPECT_FALSE(disconnected.forfeit);
    EXPECT_TRUE(disconnected.winner.isEmpty());
    EXPECT_EQ(room.phase(), QStringLiteral("preparing"));
    EXPECT_FALSE(room.hasSeat(QStringLiteral("red_commander")));
}

TEST(AuthoritativeRoomTest, RunningPromotionInheritsLiveCommandPostDeployment) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("attack"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_commander"),
                            GeoPos{1000.0, 1000.0, 0.0}).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_attack_1"),
                            GeoPos{1500.0, 1000.0, 1000.0}).ok);
    ASSERT_TRUE(room.deploy(2, QStringLiteral("blue_commander"),
                            GeoPos{9000.0, 9000.0, 0.0}).ok);
    ASSERT_TRUE(room.setReady(3, true).ok);
    ASSERT_TRUE(room.setReady(1, true).ok);
    ASSERT_TRUE(room.setReady(2, true).ok);
    const auto commanderBefore = room.seat(QStringLiteral("red_commander"));
    const QString subordinateUnitId = room.seat(QStringLiteral("red_attack_1")).unitId;
    ASSERT_TRUE(room.start().ok);

    const auto disconnected = room.disconnect(1);

    ASSERT_TRUE(disconnected.ok);
    EXPECT_EQ(disconnected.successorUserId, 3);
    const auto promoted = room.seat(QStringLiteral("red_commander"));
    EXPECT_EQ(promoted.userId, 3);
    EXPECT_EQ(promoted.unitId, commanderBefore.unitId);
    EXPECT_NE(promoted.unitId, subordinateUnitId);
    EXPECT_EQ(promoted.position.x, commanderBefore.position.x);
    EXPECT_TRUE(promoted.deployed);
    EXPECT_EQ(room.phase(), QStringLiteral("running"));
}

TEST(AuthoritativeRoomTest, DisconnectWithoutFriendlyOnlineForfeitsExactlyOnce) {
    auto room = configuredRoom();
    claimCommanders(room);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_commander"),
                            GeoPos{1000.0, 1000.0, 0.0}).ok);
    ASSERT_TRUE(room.deploy(2, QStringLiteral("blue_commander"),
                            GeoPos{9000.0, 9000.0, 0.0}).ok);
    ASSERT_TRUE(room.setReady(1, true).ok);
    ASSERT_TRUE(room.setReady(2, true).ok);
    ASSERT_TRUE(room.start().ok);
    const auto first = room.disconnect(1);
    ASSERT_TRUE(first.ok);
    EXPECT_TRUE(first.forfeit);
    EXPECT_EQ(first.winner, QStringLiteral("blue"));
    EXPECT_EQ(room.phase(), QStringLiteral("finished"));
    const quint64 revision = room.revision();

    const auto duplicate = room.disconnect(1);
    EXPECT_TRUE(duplicate.ok);
    EXPECT_TRUE(duplicate.duplicate);
    EXPECT_EQ(room.revision(), revision);
}

TEST(AuthoritativeRoomTest, OperationIdempotencyWindowIsBoundedAndOrdered) {
    auto room = configuredRoom();
    for (int index = 0; index < 300; ++index) {
        ASSERT_TRUE(room.applyOperation(QStringLiteral("operation-%1").arg(index),
                                        QStringLiteral("redeploy"), room.revision()).ok);
    }

    const QJsonArray operations = room.toJson().value(QStringLiteral("operations")).toArray();
    ASSERT_EQ(operations.size(), 256);
    EXPECT_EQ(operations.first().toObject().value(QStringLiteral("operationId")).toString(),
              QStringLiteral("operation-44"));
    EXPECT_EQ(operations.last().toObject().value(QStringLiteral("operationId")).toString(),
              QStringLiteral("operation-299"));

    QString error;
    auto restored = configuredRoom();
    ASSERT_TRUE(restored.restore(room.toJson(), &error)) << error.toStdString();
    const auto duplicate = restored.applyOperation(QStringLiteral("operation-299"),
                                                   QStringLiteral("redeploy"),
                                                   restored.revision());
    EXPECT_TRUE(duplicate.ok);
    EXPECT_TRUE(duplicate.duplicate);
}

TEST(GameServerDepartureTest, RunningSeatDepartureRemovesOnlyDepartedLiveUnit) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    GameServer server;
    claimCommanders(server.m_authoritativeRoom);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    3, QStringLiteral("attack"), QStringLiteral("red_attack_1"),
                    QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    1, QStringLiteral("red_commander"), GeoPos{1000.0, 1000.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    1, QStringLiteral("red_attack_1"), GeoPos{1500.0, 1000.0, 1000.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    2, QStringLiteral("blue_commander"), GeoPos{9000.0, 9000.0, 0.0}).ok);
    ASSERT_TRUE(server.applyDeployedScenario());
    ASSERT_TRUE(server.m_authoritativeRoom.setReady(3, true).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.setReady(1, true).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.setReady(2, true).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.start().ok);
    server.m_phase = QStringLiteral("running");
    server.m_engine.setRunning(true);
    const QString unitId = server.m_authoritativeRoom.seat(
        QStringLiteral("red_attack_1")).unitId;
    ASSERT_NE(server.m_engine.unit(unitId), nullptr);
    server.m_engine.unit(unitId)->setHp(37.0);
    const QString blueCommandPostId = server.m_authoritativeRoom.seat(
        QStringLiteral("blue_commander")).unitId;
    UnitBase* blueCommandPost = server.m_engine.unit(blueCommandPostId);
    ASSERT_NE(blueCommandPost, nullptr);
    blueCommandPost->setHp(111.0);
    const quint64 scenarioRevision = server.m_scenarioRevision;

    auto* participantSocket = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, &server);
    auto* commanderSocket = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, &server);
    GameServer::ClientSession participant;
    participant.authenticated = true;
    participant.userId = 3;
    participant.roomId = server.m_roomId;
    participant.seatId = QStringLiteral("red_attack_1");
    participant.seatType = QStringLiteral("attack");
    participant.side = QStringLiteral("red");
    GameServer::ClientSession commander;
    commander.authenticated = true;
    commander.userId = 1;
    commander.roomId = server.m_roomId;
    commander.seatId = QStringLiteral("red_commander");
    commander.seatType = QStringLiteral("commander");
    commander.side = QStringLiteral("red");
    server.m_clients.insert(participantSocket, participant);
    server.m_clients.insert(commanderSocket, commander);

    server.removeClient(participantSocket);

    EXPECT_FALSE(server.m_authoritativeRoom.hasUser(3));
    EXPECT_EQ(server.m_engine.unit(unitId), nullptr);
    EXPECT_DOUBLE_EQ(blueCommandPost->hp(), 111.0);
    EXPECT_TRUE(server.m_engine.running());
    EXPECT_EQ(server.m_phase, QStringLiteral("running"));
    EXPECT_EQ(server.m_scenarioRevision, scenarioRevision + 1);
    server.m_clients.clear();
}

TEST(GameServerRecoveryTest, AllowedResetArchivesEveryEventLogGeneration) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    const QString checkpointPath = temporary.filePath(QStringLiteral("checkpoint.json"));
    const QString eventPath = temporary.filePath(QStringLiteral("events.jsonl"));
    const QStringList corruptPaths{
        checkpointPath, eventPath, eventPath + QStringLiteral(".1"),
        eventPath + QStringLiteral(".2"), eventPath + QStringLiteral(".3")};
    for (const QString& path : corruptPaths) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_GT(file.write(QByteArrayLiteral("invalid\n")), 0);
    }
    qputenv("WARGAME_ALLOW_RECOVERY_RESET", QByteArrayLiteral("1"));
    {
        GameServer server;
        EXPECT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    }
    qunsetenv("WARGAME_ALLOW_RECOVERY_RESET");

    const QDir directory(temporary.path());
    for (const QString& path : corruptPaths) {
        if (path != checkpointPath) EXPECT_FALSE(QFileInfo::exists(path));
        const QString pattern = QFileInfo(path).fileName() + QStringLiteral(".incompatible-*");
        EXPECT_EQ(directory.entryList(QStringList{pattern}, QDir::Files).size(), 1)
            << path.toStdString();
    }
    EXPECT_TRUE(QFileInfo::exists(checkpointPath));
}

TEST(AuthoritativeRoomTest, FinishedVictoryIsIdempotentAndPersists) {
    auto room = configuredRoom();
    claimCommanders(room);
    const auto first = room.finish(QStringLiteral("red"));
    ASSERT_TRUE(first.ok);
    EXPECT_EQ(first.winner, QStringLiteral("red"));
    EXPECT_EQ(room.phase(), QStringLiteral("finished"));
    const quint64 revision = room.revision();

    const auto duplicate = room.finish(QStringLiteral("red"));
    EXPECT_TRUE(duplicate.ok);
    EXPECT_TRUE(duplicate.duplicate);
    EXPECT_EQ(duplicate.revision, revision);
    EXPECT_EQ(room.finish(QStringLiteral("blue")).code, QStringLiteral("RESULT_CONFLICT"));

    QString error;
    auto restored = configuredRoom();
    ASSERT_TRUE(restored.restore(room.toJson(), &error)) << error.toStdString();
    EXPECT_EQ(restored.phase(), QStringLiteral("finished"));
    EXPECT_EQ(restored.winner(), QStringLiteral("red"));
}

TEST(GameServerRoomOperationTest, RedeployAcceptsOlderPublishedLifecycleRevision) {
    int argc = 1;
    char applicationName[] = "authoritative_room_tests";
    char* argv[] = {applicationName, nullptr};
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    OperationAcknowledgementReceiver acknowledgements;
    ASSERT_TRUE(acknowledgements.listen());

    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:")
                                  + QByteArray::number(acknowledgements.port()));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    const quint64 publishedRevision = server.m_authoritativeRoom.revision();
    server.m_authoritativeRoom.clearReadiness();
    const quint64 currentRevision = server.m_authoritativeRoom.revision();
    ASSERT_GT(currentRevision, publishedRevision);

    server.processRoomOperation(
        QJsonObject{{QStringLiteral("operationId"), QStringLiteral("redeploy-stale-monitor")},
                    {QStringLiteral("action"), QStringLiteral("redeploy")},
                    {QStringLiteral("state"), QStringLiteral("pending")},
                    {QStringLiteral("requestedRevision"),
                     static_cast<qint64>(publishedRevision)}});

    ASSERT_TRUE(waitFor([&acknowledgements]() { return acknowledgements.requests().size() == 1; }));
    EXPECT_TRUE(acknowledgements.requests().at(0).contains("\"state\":\"acknowledged\""));
    EXPECT_TRUE(acknowledgements.requests().at(0).contains(
        "\"revision\":" + QByteArray::number(currentRevision + 1)));
}

TEST(GameServerRoomLifecycleTest, DeleteAndStopNotifyEveryAffectedClientBeforeClearingIdentity) {
    int argc = 1;
    char applicationName[] = "authoritative_room_lifecycle_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }

    const QList<QByteArray> responses{
        QByteArrayLiteral(R"({"rooms":[],"kickRequests":[],"logoutRequests":[]})"),
        QByteArrayLiteral(R"({"rooms":[{"roomId":"main","status":"stopped","updatedAt":"2026-07-26T09:10:00Z"}],"kickRequests":[],"logoutRequests":[]})")};
    for (const QByteArray& response : responses) {
        SCOPED_TRACE(response.constData());
        QTemporaryDir temporary;
        ASSERT_TRUE(temporary.isValid());
        RoomControlReceiver roomControl(response);
        ASSERT_TRUE(roomControl.listen());

        qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
        qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:")
                                      + QByteArray::number(roomControl.port()));
        qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
        qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
        qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
        qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
        qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

        GameServer server;
        ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
        server.m_roomStatus = QStringLiteral("running");
        server.m_phase = QStringLiteral("running");
        ASSERT_TRUE(server.m_server.listen(QHostAddress::LocalHost, 0));
        const QUrl endpoint(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort()));
        QList<QJsonObject> seatedMessages;
        QList<QJsonObject> lobbyMessages;
        QWebSocket seatedClient;
        QWebSocket lobbyClient;
        QObject::connect(&seatedClient, &QWebSocket::textMessageReceived,
                         [&seatedMessages](const QString& text) {
            seatedMessages.append(QJsonDocument::fromJson(text.toUtf8()).object());
        });
        QObject::connect(&lobbyClient, &QWebSocket::textMessageReceived,
                         [&lobbyMessages](const QString& text) {
            lobbyMessages.append(QJsonDocument::fromJson(text.toUtf8()).object());
        });

        seatedClient.open(endpoint);
        ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
        QWebSocket* seatedSocket = server.m_clients.keys().constFirst();
        lobbyClient.open(endpoint);
        ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 2; }));
        QWebSocket* lobbySocket = nullptr;
        for (QWebSocket* candidate : server.m_clients.keys()) {
            if (candidate != seatedSocket) lobbySocket = candidate;
        }
        ASSERT_NE(lobbySocket, nullptr);

        auto& seatedSession = server.m_clients[seatedSocket];
        seatedSession.authenticated = true;
        seatedSession.userId = 1;
        seatedSession.role = QStringLiteral("player");
        seatedSession.roomId = server.m_roomId;
        seatedSession.seatId = QStringLiteral("red_commander");
        seatedSession.seatType = QStringLiteral("commander");
        seatedSession.side = QStringLiteral("red");
        auto& lobbySession = server.m_clients[lobbySocket];
        lobbySession.authenticated = true;
        lobbySession.userId = 2;
        lobbySession.role = QStringLiteral("player");
        lobbySession.roomId = server.m_roomId;

        server.syncRoomControl();

        EXPECT_TRUE(waitFor([&]() {
            return roomControl.requestCount() == 1
                && !eventPayload(seatedMessages, QStringLiteral("roomClosed")).isEmpty()
                && !eventPayload(lobbyMessages, QStringLiteral("roomClosed")).isEmpty();
        }));
        EXPECT_TRUE(server.m_clients[seatedSocket].roomId.isEmpty());
        EXPECT_TRUE(server.m_clients[seatedSocket].seatId.isEmpty());
        EXPECT_TRUE(server.m_clients[lobbySocket].roomId.isEmpty());
        seatedClient.close();
        lobbyClient.close();
        EXPECT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
    }
}

TEST(GameServerRoomDirectoryTest, PublishesAllConfiguredRoomsFromControlPlane) {
    int argc = 1;
    char applicationName[] = "room_directory_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);

    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    RoomControlReceiver roomControl(QByteArrayLiteral(
        R"({"rooms":[{"roomId":"main","name":"主推演室","status":"preparing","hostedByGameServer":true,"updatedAt":"2026-08-04T02:40:00Z"},{"roomId":"secondary","name":"第二推演室","description":"网页创建的房间","status":"preparing","hostedByGameServer":false,"updatedAt":"2026-08-04T02:41:00Z"}],"kickRequests":[],"logoutRequests":[]})"));
    ASSERT_TRUE(roomControl.listen());
    configureGameServerEnvironment(temporary, roomControl.port());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.listen(0));
    server.m_snapshotTimer.stop();
    server.m_roomSyncTimer.stop();
    ASSERT_TRUE(waitFor([&]() {
        return roomControl.requestCount() == 1
            && server.m_lastRoomUpdate == QLatin1String("2026-08-04T02:40:00Z");
    }));

    QList<QJsonObject> messages;
    QWebSocket client;
    QObject::connect(&client, &QWebSocket::textMessageReceived,
                     [&messages](const QString& text) {
        messages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* socket = server.m_clients.keys().constFirst();

    server.handleRoomList(socket);

    ASSERT_TRUE(waitFor([&messages]() {
        return std::any_of(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
            return envelope.value(QStringLiteral("type")).toString() == QLatin1String("roomDirectory");
        });
    }));
    const auto directory = std::find_if(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
        return envelope.value(QStringLiteral("type")).toString() == QLatin1String("roomDirectory");
    });
    ASSERT_NE(directory, messages.cend());
    const QJsonArray rooms = directory->value(QStringLiteral("payload")).toObject()
                                 .value(QStringLiteral("rooms")).toArray();
    ASSERT_EQ(rooms.size(), 2);
    EXPECT_EQ(rooms.at(0).toObject().value(QStringLiteral("roomId")).toString(), QStringLiteral("main"));
    EXPECT_EQ(rooms.at(1).toObject().value(QStringLiteral("roomId")).toString(), QStringLiteral("secondary"));
    EXPECT_EQ(rooms.at(1).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("第二推演室"));
    EXPECT_TRUE(rooms.at(0).toObject().value(QStringLiteral("hostedByGameServer")).toBool());
    EXPECT_FALSE(rooms.at(1).toObject().value(QStringLiteral("hostedByGameServer")).toBool());

    server.m_clients[socket].authenticated = true;
    server.m_clients[socket].userId = 3;
    messages.clear();
    roomControl.setResponseBody(QByteArrayLiteral(
        R"({"rooms":[{"roomId":"main","name":"主推演室","status":"preparing","hostedByGameServer":true,"updatedAt":"2026-08-04T02:40:00Z"},{"roomId":"secondary","name":"第二推演室","description":"网页创建的房间","status":"preparing","hostedByGameServer":false,"updatedAt":"2026-08-04T02:41:00Z"},{"roomId":"new-room","name":"刚创建的房间","status":"preparing","hostedByGameServer":false,"updatedAt":"2026-08-04T02:42:00Z"}],"kickRequests":[],"logoutRequests":[]})"));
    server.handleRoomList(socket);
    ASSERT_TRUE(waitFor([&roomControl]() { return roomControl.requestCount() == 2; }));
    ASSERT_TRUE(waitFor([&]() {
        return std::any_of(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
            const QJsonArray directory = envelope.value(QStringLiteral("payload")).toObject()
                                             .value(QStringLiteral("rooms")).toArray();
            return envelope.value(QStringLiteral("type")).toString() == QLatin1String("roomDirectory")
                && directory.size() == 3
                && directory.at(2).toObject().value(QStringLiteral("roomId")).toString()
                       == QLatin1String("new-room");
        });
    }));

    client.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

TEST(GameServerRoomDirectoryTest, CoalescesRepeatedRoomListRequestsPerSocket) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;
    QWebSocket requester;
    server.m_roomSyncInFlight = true;

    for (int index = 0; index < 1000; ++index) server.syncRoomControl(&requester);

    ASSERT_EQ(server.m_roomListWaiters.size(), 1);
    EXPECT_EQ(server.m_roomListWaiters.constFirst().data(), &requester);
}

TEST(GameServerRoomDirectoryTest, PrunesDisconnectedRoomListWaitersBeforeQueueing) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;
    server.m_roomSyncInFlight = true;
    auto* disconnected = new QWebSocket;
    server.syncRoomControl(disconnected);
    delete disconnected;

    QWebSocket requester;
    server.syncRoomControl(&requester);

    ASSERT_EQ(server.m_roomListWaiters.size(), 1);
    EXPECT_EQ(server.m_roomListWaiters.constFirst().data(), &requester);
}

TEST(GameServerAuditTest, ChatAuditSummaryExcludesMessageBody) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;
    const QString secret = QStringLiteral("chat-secret-that-must-not-be-persisted");
    const QString summary = server.messageSummary(
        QStringLiteral("chat"), QJsonObject{{QStringLiteral("text"), secret}});

    EXPECT_FALSE(summary.contains(secret));
    EXPECT_TRUE(summary.contains(QStringLiteral("chat message")));
}

TEST(GameServerRoomDirectoryTest, RejectsJoinForVisibleRoomNotHostedByThisServer) {
    int argc = 1;
    char applicationName[] = "room_directory_join_contract_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);

    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    GameServer server;
    ASSERT_TRUE(server.m_server.listen(QHostAddress::LocalHost, 0));

    QList<QJsonObject> messages;
    QWebSocket client;
    QObject::connect(&client, &QWebSocket::textMessageReceived,
                     [&messages](const QString& text) {
        messages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1")
                         .arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* socket = server.m_clients.keys().constFirst();
    auto& session = server.m_clients[socket];
    session.authenticated = true;
    session.userId = 7;
    session.role = QStringLiteral("player");

    server.handleJoinRoom(socket, QJsonObject{{QStringLiteral("roomId"), QStringLiteral("secondary")} });

    ASSERT_TRUE(waitFor([&messages]() {
        return std::any_of(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
            return envelope.value(QStringLiteral("type")).toString() == QLatin1String("error");
        });
    }));
    const auto error = std::find_if(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
        return envelope.value(QStringLiteral("type")).toString() == QLatin1String("error");
    });
    ASSERT_NE(error, messages.cend());
    EXPECT_EQ(error->value(QStringLiteral("payload")).toObject()
                  .value(QStringLiteral("code")).toString(), QStringLiteral("ROOM_NOT_FOUND"));
    EXPECT_TRUE(server.m_clients.value(socket).roomId.isEmpty());
    client.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

TEST(GameServerRoomLifecycleTest, OpeningFinishedRoomCreatesCleanDeployableRound) {
    int argc = 1;
    char applicationName[] = "open_finished_room_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QByteArray operation = QByteArrayLiteral(
        R"({"operationId":"open-next-round","action":"open","expectedStatus":"preparing","state":"pending","requestedRevision":1})");
    const QByteArray response = QByteArrayLiteral(
        R"({"rooms":[{"roomId":"main","name":"Main","status":"preparing","updatedAt":"2026-08-02T12:00:00Z","operation":)")
        + operation + QByteArrayLiteral(R"(,"pendingOperation":)") + operation
        + QByteArrayLiteral(R"(}],"kickRequests":[],"logoutRequests":[]})");
    RoomControlReceiver roomControl(response);
    ASSERT_TRUE(roomControl.listen());
    configureGameServerEnvironment(temporary, roomControl.port());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    1, QStringLiteral("red"), QStringLiteral("red_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    2, QStringLiteral("blue"), QStringLiteral("blue_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    1, QStringLiteral("red_commander"), GeoPos{1000.0, 1000.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    2, QStringLiteral("blue_commander"), GeoPos{2000.0, 2000.0, 0.0}).ok);
    server.m_runInitialScenario.map.widthMeters = 54321.0;
    server.m_runInitialScenario.map.heightMeters = 12345.0;
    ASSERT_TRUE(server.applyDeployedScenario());
    ASSERT_TRUE(server.m_authoritativeRoom.finish(QStringLiteral("draw")).ok);
    server.m_phase = QStringLiteral("finished");
    server.m_roomStatus = QStringLiteral("finished");
    server.syncAuthoritativeSeats();
    const quint64 previousScenarioRevision = server.m_scenarioRevision;
    server.m_chatHistory = QJsonArray{QJsonObject{{QStringLiteral("text"),
                                                  QStringLiteral("previous round")}}};
    server.m_chatSequence = 1;
    ASSERT_TRUE(server.m_server.listen(QHostAddress::LocalHost, 0));
    QList<QJsonObject> participantMessages;
    QWebSocket participant;
    QObject::connect(&participant, &QWebSocket::textMessageReceived,
                     [&participantMessages](const QString& text) {
        participantMessages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    participant.open(QUrl(QStringLiteral("ws://127.0.0.1:%1")
                              .arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* participantSocket = server.m_clients.keys().constFirst();
    auto& participantSession = server.m_clients[participantSocket];
    participantSession.authenticated = true;
    participantSession.userId = 1;
    participantSession.role = QStringLiteral("player");
    participantSession.roomId = server.m_roomId;
    participantSession.seatId = QStringLiteral("red_commander");
    participantSession.seatType = QStringLiteral("commander");
    participantSession.side = QStringLiteral("red");

    server.syncRoomControl();

    ASSERT_TRUE(waitFor([&server, &participantMessages]() {
        return server.m_phase == QLatin1String("preparing")
            && server.m_authoritativeRoom.phase() == QLatin1String("preparing")
            && server.m_authoritativeRoom.seats().isEmpty()
            && server.m_engine.unitIds().isEmpty()
            && !eventPayload(participantMessages, QStringLiteral("matchReset")).isEmpty();
    }));
    EXPECT_EQ(server.m_roomStatus, QStringLiteral("preparing"));
    EXPECT_GT(server.m_scenarioRevision, previousScenarioRevision);
    EXPECT_DOUBLE_EQ(server.m_engine.scenario().map.widthMeters, 54321.0);
    EXPECT_DOUBLE_EQ(server.m_engine.scenario().map.heightMeters, 12345.0);
    EXPECT_TRUE(server.m_mapMarks.isEmpty());
    EXPECT_TRUE(server.m_sharedIntel.isEmpty());
    EXPECT_TRUE(server.m_chatHistory.isEmpty());
    EXPECT_EQ(server.m_chatSequence, 0);

    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    3, QStringLiteral("red-next"), QStringLiteral("red_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    4, QStringLiteral("blue-next"), QStringLiteral("blue_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    3, QStringLiteral("red_commander"), GeoPos{3000.0, 3000.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    4, QStringLiteral("blue_commander"), GeoPos{4000.0, 4000.0, 0.0}).ok);
    ASSERT_TRUE(server.applyDeployedScenario());
    server.syncAuthoritativeSeats();
    const quint64 deployedScenarioRevision = server.m_scenarioRevision;
    const quint64 deployedRoomRevision = server.m_authoritativeRoom.revision();

    ASSERT_TRUE(waitFor([&roomControl]() { return roomControl.requestCount() >= 2; }));
    const QByteArray refreshedResponse = QByteArrayLiteral(
        R"({"rooms":[{"roomId":"main","name":"Main","status":"preparing","updatedAt":"2026-08-02T12:01:00Z","operation":)")
        + operation + QByteArrayLiteral(R"(}],"kickRequests":[],"logoutRequests":[]})");
    roomControl.setResponseBody(refreshedResponse);
    server.syncRoomControl();

    ASSERT_TRUE(waitFor([&server]() {
        return server.m_lastRoomUpdate == QLatin1String("2026-08-02T12:01:00Z");
    }));
    EXPECT_EQ(server.m_authoritativeRoom.revision(), deployedRoomRevision);
    EXPECT_EQ(server.m_scenarioRevision, deployedScenarioRevision);
    EXPECT_EQ(server.m_authoritativeRoom.seats().size(), 2);
    EXPECT_EQ(server.m_engine.unitIds().size(), 2);
    participant.close();
    EXPECT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

TEST(GameServerRoomOperationTest, ResetAndRedeployPersistRuntimeThenAcknowledgeExactRevisions) {
    int argc = 1;
    char applicationName[] = "authoritative_room_tests";
    char* argv[] = {applicationName, nullptr};
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    OperationAcknowledgementReceiver acknowledgements;
    ASSERT_TRUE(acknowledgements.listen());

    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:")
                                  + QByteArray::number(acknowledgements.port()));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    QString replyError;
    QString replyUrl;
    QObject::connect(&server.m_network, &QNetworkAccessManager::finished, &server,
                     [&replyError, &replyUrl](QNetworkReply* reply) {
                replyError = reply->errorString();
                replyUrl = reply->url().toString();
                     });
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    1, QStringLiteral("red"), QStringLiteral("red_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    2, QStringLiteral("blue"), QStringLiteral("blue_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    1, QStringLiteral("red_commander"), GeoPos{10.0, 10.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    2, QStringLiteral("blue_commander"), GeoPos{20.0, 20.0, 0.0}).ok);
    ASSERT_TRUE(server.applyDeployedScenario());

    server.m_aiStickyRules = true;
    const quint64 generationBeforeRedeploy = server.m_matchGeneration;
    const quint64 redeployRevision = server.m_authoritativeRoom.revision();
    server.processRoomOperation(QJsonObject{{QStringLiteral("operationId"), QStringLiteral("redeploy-1")},
                                             {QStringLiteral("action"), QStringLiteral("redeploy")},
                                             {QStringLiteral("state"), QStringLiteral("pending")},
                                             {QStringLiteral("requestedRevision"), static_cast<qint64>(redeployRevision)}});
    ASSERT_TRUE(waitFor([&acknowledgements, &replyUrl]() {
        return acknowledgements.requests().size() == 1 || !replyUrl.isEmpty();
    }))
        << "connections=" << acknowledgements.connectionCount()
        << " roomRevision=" << server.m_authoritativeRoom.revision()
        << " roomPhase=" << server.m_authoritativeRoom.phase().toStdString()
        << " endpoint=" << server.m_authServiceUrl.toStdString()
        << " replyUrl=" << replyUrl.toStdString()
        << " replyError=" << replyError.toStdString();
    ASSERT_EQ(acknowledgements.requests().size(), 1)
        << "connections=" << acknowledgements.connectionCount()
        << " replyUrl=" << replyUrl.toStdString()
        << " replyError=" << replyError.toStdString();
    EXPECT_EQ(server.m_authoritativeRoom.seats().size(), 2);
    EXPECT_TRUE(server.m_authoritativeRoom.runtimeUnits().isEmpty());
    EXPECT_TRUE(server.m_engine.scenario().units.empty());
    EXPECT_EQ(server.m_phase, QStringLiteral("preparing"));
    EXPECT_EQ(server.m_matchGeneration, generationBeforeRedeploy + 1);
    EXPECT_FALSE(server.m_aiStickyRules);
    EXPECT_FALSE(server.m_aiPlanRequestInFlight);
    EXPECT_TRUE(QFile::exists(temporary.filePath(QStringLiteral("checkpoint.json"))));
    EXPECT_TRUE(acknowledgements.requests().at(0).contains("/operations/redeploy-1/ack"));
    EXPECT_TRUE(acknowledgements.requests().at(0).contains("\"state\":\"acknowledged\""));
    EXPECT_TRUE(acknowledgements.requests().at(0).contains("\"revision\":"
                                                            + QByteArray::number(redeployRevision + 1)));

    {
        GameServer restored;
        ASSERT_TRUE(restored.m_recoveryError.isEmpty())
            << restored.m_recoveryError.toStdString();
        EXPECT_EQ(restored.m_authoritativeRoom.revision(), redeployRevision + 1);
        EXPECT_EQ(restored.m_authoritativeRoom.seats().size(), 2);
        EXPECT_TRUE(restored.m_authoritativeRoom.runtimeUnits().isEmpty());
        EXPECT_TRUE(restored.m_engine.scenario().units.empty());
        EXPECT_EQ(restored.m_phase, QStringLiteral("preparing"));
    }

    server.m_aiStickyRules = true;
    const quint64 generationBeforeReset = server.m_matchGeneration;
    const quint64 resetRevision = server.m_authoritativeRoom.revision();
    server.processRoomOperation(QJsonObject{{QStringLiteral("operationId"), QStringLiteral("reset-1")},
                                             {QStringLiteral("action"), QStringLiteral("reset")},
                                             {QStringLiteral("state"), QStringLiteral("pending")},
                                             {QStringLiteral("requestedRevision"), static_cast<qint64>(resetRevision)}});
    ASSERT_TRUE(waitFor([&acknowledgements]() { return acknowledgements.requests().size() == 2; }));
    EXPECT_TRUE(server.m_authoritativeRoom.seats().isEmpty());
    EXPECT_TRUE(server.m_engine.scenario().units.empty());
    EXPECT_EQ(server.m_matchGeneration, generationBeforeReset + 1);
    EXPECT_FALSE(server.m_aiStickyRules);
    EXPECT_FALSE(server.m_aiPlanRequestInFlight);
    EXPECT_TRUE(acknowledgements.requests().at(1).contains("/operations/reset-1/ack"));
    EXPECT_TRUE(acknowledgements.requests().at(1).contains("\"revision\":"
                                                            + QByteArray::number(resetRevision + 1)));
}

TEST(GameServerTransferTest, TwoClientsReceiveRevisionedEventsAndOldUnitIsProjectedOut) {
    int argc = 1;
    char applicationName[] = "authoritative_room_transfer_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
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
    server.m_roomStatus = QStringLiteral("preparing");
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    1, QStringLiteral("red"), QStringLiteral("red_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    2, QStringLiteral("blue"), QStringLiteral("blue_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                    QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    1, QStringLiteral("red_commander"), GeoPos{10.0, 10.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    2, QStringLiteral("blue_commander"), GeoPos{20.0, 20.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    1, QStringLiteral("red_attack_1"), GeoPos{30.0, 30.0, 10.0}).ok);
    server.syncAuthoritativeSeats();
    ASSERT_TRUE(server.applyDeployedScenario());
    const QString oldUnitId = server.m_authoritativeRoom
                                  .seat(QStringLiteral("red_attack_1")).unitId;
    ASSERT_NE(server.m_engine.unit(oldUnitId), nullptr);

    ASSERT_TRUE(server.m_server.listen(QHostAddress::LocalHost, 0));
    const QUrl endpoint(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort()));
    QList<QJsonObject> commanderMessages;
    QList<QJsonObject> requesterMessages;
    QWebSocket commanderClient;
    QWebSocket requesterClient;
    QObject::connect(&commanderClient, &QWebSocket::textMessageReceived,
                     [&commanderMessages](const QString& text) {
        commanderMessages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    QObject::connect(&requesterClient, &QWebSocket::textMessageReceived,
                     [&requesterMessages](const QString& text) {
        requesterMessages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });

    commanderClient.open(endpoint);
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* commanderSocket = server.m_clients.keys().constFirst();
    requesterClient.open(endpoint);
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 2; }));
    QWebSocket* requesterSocket = nullptr;
    for (QWebSocket* candidate : server.m_clients.keys()) {
        if (candidate != commanderSocket) requesterSocket = candidate;
    }
    ASSERT_NE(requesterSocket, nullptr);

    auto& commanderSession = server.m_clients[commanderSocket];
    commanderSession.authenticated = true;
    commanderSession.userId = 1;
    commanderSession.username = QStringLiteral("red");
    commanderSession.roomId = server.m_roomId;
    commanderSession.seatId = QStringLiteral("red_commander");
    commanderSession.seatType = QStringLiteral("commander");
    commanderSession.side = QStringLiteral("red");
    auto& requesterSession = server.m_clients[requesterSocket];
    requesterSession.authenticated = true;
    requesterSession.userId = 3;
    requesterSession.username = QStringLiteral("pilot");
    requesterSession.roomId = server.m_roomId;
    requesterSession.seatId = QStringLiteral("red_attack_1");
    requesterSession.seatType = QStringLiteral("attack");
    requesterSession.side = QStringLiteral("red");

    server.handleClaimSeat(requesterSocket,
                           QJsonObject{{QStringLiteral("seatId"),
                                        QStringLiteral("red_recon_1")}});
    ASSERT_TRUE(waitFor([&]() {
        return !eventPayload(commanderMessages, QStringLiteral("transferRequested")).isEmpty()
            && !eventPayload(requesterMessages, QStringLiteral("transferRequested")).isEmpty();
    }));
    const QJsonObject requested = eventPayload(
        commanderMessages, QStringLiteral("transferRequested"));
    EXPECT_EQ(requested.value(QStringLiteral("userId")).toInteger(), 3);
    EXPECT_EQ(requested.value(QStringLiteral("sourceSeatId")).toString(),
              QStringLiteral("red_attack_1"));
    EXPECT_EQ(requested.value(QStringLiteral("targetSeatId")).toString(),
              QStringLiteral("red_recon_1"));
    EXPECT_GT(requested.value(QStringLiteral("revision")).toInteger(), 0);
    EXPECT_NE(server.m_engine.unit(oldUnitId), nullptr);

    server.handleClaimSeat(
        requesterSocket,
        QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_attack_1")},
                    {QStringLiteral("cancelTransfer"), true},
                    {QStringLiteral("requestedRevision"),
                     requested.value(QStringLiteral("revision"))}});
    ASSERT_TRUE(waitFor([&]() {
        return !eventPayload(commanderMessages, QStringLiteral("transferRejected")).isEmpty()
            && !eventPayload(requesterMessages, QStringLiteral("transferRejected")).isEmpty();
    }));
    EXPECT_EQ(eventPayload(requesterMessages, QStringLiteral("transferRejected"))
                  .value(QStringLiteral("reason")).toString(),
              QStringLiteral("REQUESTER_CANCELLED"));
    EXPECT_NE(server.m_engine.unit(oldUnitId), nullptr);

    commanderMessages.clear();
    requesterMessages.clear();
    server.handleClaimSeat(requesterSocket,
                           QJsonObject{{QStringLiteral("seatId"),
                                        QStringLiteral("red_recon_1")}});
    ASSERT_TRUE(waitFor([&]() {
        return !eventPayload(commanderMessages, QStringLiteral("transferRequested")).isEmpty()
            && !eventPayload(requesterMessages, QStringLiteral("transferRequested")).isEmpty();
    }));
    const QJsonObject rejectedRequest = eventPayload(
        commanderMessages, QStringLiteral("transferRequested"));
    server.handleClaimSeat(
        commanderSocket,
        QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_commander")},
                    {QStringLiteral("rejectUserId"), 3},
                    {QStringLiteral("requestedRevision"),
                     rejectedRequest.value(QStringLiteral("revision"))}});
    ASSERT_TRUE(waitFor([&]() {
        return !eventPayload(commanderMessages, QStringLiteral("transferRejected")).isEmpty()
            && !eventPayload(requesterMessages, QStringLiteral("transferRejected")).isEmpty();
    }));
    EXPECT_EQ(eventPayload(commanderMessages, QStringLiteral("transferRejected"))
                  .value(QStringLiteral("reason")).toString(),
              QStringLiteral("COMMANDER_REJECTED"));
    EXPECT_NE(server.m_engine.unit(oldUnitId), nullptr);

    commanderMessages.clear();
    requesterMessages.clear();
    server.handleClaimSeat(requesterSocket,
                           QJsonObject{{QStringLiteral("seatId"),
                                        QStringLiteral("red_recon_1")}});
    ASSERT_TRUE(waitFor([&]() {
        return !eventPayload(commanderMessages, QStringLiteral("transferRequested")).isEmpty()
            && !eventPayload(requesterMessages, QStringLiteral("transferRequested")).isEmpty();
    }));
    const QJsonObject approvedRequest = eventPayload(
        commanderMessages, QStringLiteral("transferRequested"));
    server.handleClaimSeat(
        commanderSocket,
        QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_commander")},
                    {QStringLiteral("approveUserId"), 3},
                    {QStringLiteral("requestedRevision"),
                     approvedRequest.value(QStringLiteral("revision"))}});
    ASSERT_TRUE(waitFor([&]() {
        return !eventPayload(commanderMessages, QStringLiteral("transferCompleted")).isEmpty()
            && !eventPayload(requesterMessages, QStringLiteral("transferCompleted")).isEmpty();
    }));
    const QJsonObject completed = eventPayload(
        requesterMessages, QStringLiteral("transferCompleted"));
    EXPECT_GT(completed.value(QStringLiteral("revision")).toInteger(),
              approvedRequest.value(QStringLiteral("revision")).toInteger());
    EXPECT_FALSE(server.m_authoritativeRoom.hasSeat(QStringLiteral("red_attack_1")));
    EXPECT_EQ(server.m_authoritativeRoom.seat(QStringLiteral("red_recon_1")).userId, 3);
    EXPECT_EQ(server.m_engine.unit(oldUnitId), nullptr);
    EXPECT_FALSE(containsUnitId(ScenarioIo::toJson(server.m_engine.scenario())
                                    .value(QStringLiteral("units")).toArray(), oldUnitId));
    const QJsonObject commanderProjection = server.snapshotFor(server.m_clients[commanderSocket]);
    const QJsonObject requesterProjection = server.snapshotFor(server.m_clients[requesterSocket]);
    EXPECT_FALSE(containsUnitId(commanderProjection.value(QStringLiteral("units")).toArray(),
                                oldUnitId));
    EXPECT_FALSE(containsUnitId(requesterProjection.value(QStringLiteral("units")).toArray(),
                                oldUnitId));
    EXPECT_FALSE(containsUnitId(commanderProjection.value(QStringLiteral("scenario")).toObject()
                                    .value(QStringLiteral("units")).toArray(), oldUnitId));
    EXPECT_FALSE(containsUnitId(requesterProjection.value(QStringLiteral("scenario")).toObject()
                                    .value(QStringLiteral("units")).toArray(), oldUnitId));

    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    1, QStringLiteral("red_recon_1"), GeoPos{40.0, 40.0, 10.0}).ok);
    ASSERT_TRUE(server.applyDeployedScenario());
    const QString switchedUnitId = server.m_authoritativeRoom
                                       .seat(QStringLiteral("red_recon_1")).unitId;
    const QString commanderUnitId = server.m_authoritativeRoom
                                        .seat(QStringLiteral("red_commander")).unitId;
    ASSERT_NE(server.m_engine.unit(switchedUnitId), nullptr);
    requesterClient.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    EXPECT_FALSE(server.m_authoritativeRoom.hasUser(3));
    EXPECT_EQ(server.m_engine.unit(switchedUnitId), nullptr);

    QWebSocket reconnectClient;
    reconnectClient.open(endpoint);
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 2; }));
    QWebSocket* reconnectSocket = nullptr;
    for (QWebSocket* candidate : server.m_clients.keys()) {
        if (candidate != commanderSocket) reconnectSocket = candidate;
    }
    ASSERT_NE(reconnectSocket, nullptr);
    EXPECT_FALSE(server.m_clients.value(reconnectSocket).authenticated);
    EXPECT_TRUE(server.m_clients.value(reconnectSocket).roomId.isEmpty());
    EXPECT_TRUE(server.m_clients.value(reconnectSocket).seatId.isEmpty());
    reconnectClient.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));

    commanderClient.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
    EXPECT_EQ(server.m_authoritativeRoom.phase(), QStringLiteral("preparing"));
    EXPECT_TRUE(server.m_authoritativeRoom.winner().isEmpty());
    EXPECT_TRUE(server.m_authoritativeRoom.seats().isEmpty());
    EXPECT_EQ(server.m_engine.unit(commanderUnitId), nullptr);
}

TEST(GameServerReconnectTest, AuthenticatedParticipantReconnectDoesNotReclaimReleasedSeat) {
    int argc = 1;
    char applicationName[] = "authenticated_reconnect_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    AuthenticationReceiver authentication;
    ASSERT_TRUE(authentication.listen());

    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:")
                                  + QByteArray::number(authentication.port()));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    server.m_roomStatus = QStringLiteral("preparing");
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    1, QStringLiteral("red"), QStringLiteral("red_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    2, QStringLiteral("blue"), QStringLiteral("blue_commander"),
                    QStringLiteral("commandpost")).ok);
    server.syncAuthoritativeSeats();
    ASSERT_TRUE(server.listen(0));
    const QUrl endpoint(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort()));

    QList<QJsonObject> firstMessages;
    QWebSocket firstClient;
    QObject::connect(&firstClient, &QWebSocket::textMessageReceived,
                     [&firstMessages](const QString& text) {
                         firstMessages.append(QJsonDocument::fromJson(text.toUtf8()).object());
                     });
    firstClient.open(endpoint);
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* firstSocket = server.m_clients.keys().constFirst();
    sendClientEnvelope(firstClient, QStringLiteral("auth"), QStringLiteral("auth-1"),
                       QJsonObject{{QStringLiteral("token"), QStringLiteral("same-token")}});
    ASSERT_TRUE(waitFor([&server, firstSocket]() {
        return server.m_clients.value(firstSocket).authenticated;
    }));
    sendClientEnvelope(firstClient, QStringLiteral("joinRoom"), QStringLiteral("join-1"),
                       QJsonObject{{QStringLiteral("roomId"), server.m_roomId}});
    ASSERT_TRUE(waitFor([&server, firstSocket]() {
        return server.m_clients.value(firstSocket).roomId == server.m_roomId;
    }));
    sendClientEnvelope(firstClient, QStringLiteral("claimSeat"), QStringLiteral("claim-1"),
                       QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_attack_1")}});
    ASSERT_TRUE(waitFor([&server, firstSocket]() {
        return server.m_clients.value(firstSocket).seatId == QLatin1String("red_attack_1")
            && server.m_authoritativeRoom.hasUser(3);
    }));
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    1, QStringLiteral("red_attack_1"), GeoPos{40.0, 40.0, 10.0}).ok);
    ASSERT_TRUE(server.applyDeployedScenario());
    const QString releasedUnitId = server.m_authoritativeRoom
                                       .seat(QStringLiteral("red_attack_1")).unitId;
    ASSERT_NE(server.m_engine.unit(releasedUnitId), nullptr);

    firstClient.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
    EXPECT_FALSE(server.m_authoritativeRoom.hasUser(3));
    EXPECT_FALSE(server.m_seats.contains(QStringLiteral("red_attack_1")));
    EXPECT_EQ(server.m_engine.unit(releasedUnitId), nullptr);

    QList<QJsonObject> reconnectMessages;
    QWebSocket reconnectClient;
    QObject::connect(&reconnectClient, &QWebSocket::textMessageReceived,
                     [&reconnectMessages](const QString& text) {
                         reconnectMessages.append(QJsonDocument::fromJson(text.toUtf8()).object());
                     });
    reconnectClient.open(endpoint);
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* reconnectSocket = server.m_clients.keys().constFirst();
    sendClientEnvelope(reconnectClient, QStringLiteral("auth"), QStringLiteral("auth-2"),
                       QJsonObject{{QStringLiteral("token"), QStringLiteral("same-token")}});
    ASSERT_TRUE(waitFor([&server, reconnectSocket, &reconnectMessages]() {
        return server.m_clients.value(reconnectSocket).authenticated
            && std::any_of(reconnectMessages.cbegin(), reconnectMessages.cend(),
                           [](const QJsonObject& envelope) {
                               return envelope.value(QStringLiteral("type")).toString()
                                   == QLatin1String("welcome");
                           });
    }));
    EXPECT_EQ(authentication.sessionRequests(), 2);
    EXPECT_TRUE(server.m_clients.value(reconnectSocket).roomId.isEmpty());
    EXPECT_TRUE(server.m_clients.value(reconnectSocket).seatId.isEmpty());
    EXPECT_FALSE(server.m_authoritativeRoom.hasUser(3));
    EXPECT_EQ(server.m_engine.unit(releasedUnitId), nullptr);

    const auto welcome = std::find_if(reconnectMessages.cbegin(), reconnectMessages.cend(),
                                      [](const QJsonObject& envelope) {
                                          return envelope.value(QStringLiteral("type")).toString()
                                              == QLatin1String("welcome");
                                      });
    ASSERT_NE(welcome, reconnectMessages.cend());
    const QJsonObject welcomePayload = welcome->value(QStringLiteral("payload")).toObject();
    EXPECT_FALSE(welcomePayload.contains(QStringLiteral("seatId")));
    const QJsonArray seats = welcomePayload.value(QStringLiteral("roomState")).toObject()
                                .value(QStringLiteral("seats")).toArray();
    const auto releasedSeat = std::find_if(seats.cbegin(), seats.cend(), [](const QJsonValue& value) {
        return value.toObject().value(QStringLiteral("seatId")).toString()
            == QLatin1String("red_attack_1");
    });
    ASSERT_NE(releasedSeat, seats.cend());
    EXPECT_FALSE(releasedSeat->toObject().value(QStringLiteral("occupied")).toBool());

    reconnectClient.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

TEST(GameServerObserverTest, AuthenticatedPlayerCanSelectObserverRoleOverWebSocket) {
    int argc = 1;
    char applicationName[] = "observer_websocket_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    AuthenticationReceiver authentication;
    ASSERT_TRUE(authentication.listen());

    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:")
                                  + QByteArray::number(authentication.port()));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());
    authentication.setRoomControlResponse(
        QByteArrayLiteral(
            R"({"rooms":[{"roomId":"main","name":"Observer Room","status":"running",
"enabled":true,"hostedByGameServer":true,"mode":"pvp","aiDifficulty":"normal",
"configVersion":1}],"kickRequests":[],"logoutRequests":[]})"));

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    server.m_roomStatus = QStringLiteral("preparing");
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    1, QStringLiteral("red"), QStringLiteral("red_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    2, QStringLiteral("blue"), QStringLiteral("blue_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    1, QStringLiteral("red_commander"), GeoPos{10.0, 10.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    2, QStringLiteral("blue_commander"), GeoPos{20.0, 20.0, 0.0}).ok);
    ASSERT_TRUE(server.applyDeployedScenario());
    server.m_phase = QStringLiteral("running");
    ASSERT_TRUE(server.listen(0));
    server.m_snapshotTimer.stop();
    const QUrl endpoint(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort()));

    QList<QJsonObject> messages;
    QWebSocket client;
    QObject::connect(&client, &QWebSocket::textMessageReceived, [&messages](const QString& text) {
        messages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    client.open(endpoint);
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* socket = server.m_clients.keys().constFirst();
    sendClientEnvelope(client, QStringLiteral("auth"), QStringLiteral("observer-auth"),
                       QJsonObject{{QStringLiteral("token"), QStringLiteral("observer-token")}});
    ASSERT_TRUE(waitFor([&server, socket]() { return server.m_clients.value(socket).authenticated; }));
    sendClientEnvelope(client, QStringLiteral("joinRoom"), QStringLiteral("observer-join"),
                       QJsonObject{{QStringLiteral("roomId"), server.m_roomId},
                                   {QStringLiteral("asObserver"), true}});
    const bool observerJoined = waitFor([&server, socket, &messages]() {
        if (!server.m_clients.contains(socket)) return false;
        const GameServer::ClientSession& session = server.m_clients.value(socket);
        if (!session.observer || session.roomId != server.m_roomId || !session.seatId.isEmpty()
            || session.role != QLatin1String("observer")) {
            return false;
        }
        return std::any_of(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
            if (envelope.value(QStringLiteral("type")).toString() != QLatin1String("snapshot")) {
                return false;
            }
            return envelope.value(QStringLiteral("payload")).toObject()
                .value(QStringLiteral("roomState")).toObject()
                .value(QStringLiteral("observer")).toBool();
        });
    });
    ASSERT_TRUE(observerJoined);
    EXPECT_TRUE(server.m_clients.value(socket).observer);
    EXPECT_EQ(server.m_clients.value(socket).roomId, server.m_roomId);
    EXPECT_TRUE(server.m_clients.value(socket).seatId.isEmpty());
    EXPECT_EQ(server.m_clients.value(socket).role, QStringLiteral("observer"));
    EXPECT_FALSE(server.m_authoritativeRoom.hasUser(3));

    messages.clear();
    sendClientEnvelope(client, QStringLiteral("claimSeat"), QStringLiteral("observer-escalation"),
                       QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_commander")}});
    ASSERT_TRUE(waitFor([&messages]() {
        return std::any_of(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
            if (envelope.value(QStringLiteral("type")).toString() != QLatin1String("error")) {
                return false;
            }
            const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
            return payload.value(QStringLiteral("code")).toString()
                == QLatin1String("OBSERVER_READ_ONLY")
                && payload.value(QStringLiteral("requestId")).toString()
                    == QLatin1String("observer-escalation");
        });
    }));
    EXPECT_TRUE(server.m_clients.value(socket).observer);
    EXPECT_TRUE(server.m_clients.value(socket).seatId.isEmpty());
    EXPECT_FALSE(server.m_authoritativeRoom.hasUser(3));

    client.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

TEST(GameServerJoinRoomTest, SeatedPlayerCannotRejoinAndRetainsAuthoritativeSeat) {
    int argc = 1;
    char applicationName[] = "seated_rejoin_websocket_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    AuthenticationReceiver authentication;
    ASSERT_TRUE(authentication.listen());

    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:")
                                  + QByteArray::number(authentication.port()));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    server.m_roomStatus = QStringLiteral("preparing");
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    1, QStringLiteral("red"), QStringLiteral("red_commander"),
                    QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    2, QStringLiteral("blue"), QStringLiteral("blue_commander"),
                    QStringLiteral("commandpost")).ok);
    server.syncAuthoritativeSeats();
    ASSERT_TRUE(server.listen(0));
    server.m_snapshotTimer.stop();

    QList<QJsonObject> messages;
    QWebSocket client;
    QObject::connect(&client, &QWebSocket::textMessageReceived, [&messages](const QString& text) {
        messages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* socket = server.m_clients.keys().constFirst();
    sendClientEnvelope(client, QStringLiteral("auth"), QStringLiteral("rejoin-auth"),
                       QJsonObject{{QStringLiteral("token"), QStringLiteral("rejoin-token")}});
    ASSERT_TRUE(waitFor([&server, socket]() { return server.m_clients.value(socket).authenticated; }));
    sendClientEnvelope(client, QStringLiteral("joinRoom"), QStringLiteral("initial-join"),
                       QJsonObject{{QStringLiteral("roomId"), server.m_roomId}});
    ASSERT_TRUE(waitFor([&server, socket]() {
        return server.m_clients.value(socket).roomId == server.m_roomId;
    }));
    sendClientEnvelope(client, QStringLiteral("claimSeat"), QStringLiteral("claim-seat"),
                       QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_attack_1")}});
    ASSERT_TRUE(waitFor([&server, socket]() {
        return server.m_clients.value(socket).seatId == QLatin1String("red_attack_1");
    }));

    messages.clear();
    sendClientEnvelope(client, QStringLiteral("joinRoom"), QStringLiteral("seated-rejoin"),
                       QJsonObject{{QStringLiteral("roomId"), server.m_roomId}});
    const bool rejoinDenied = waitFor([&messages]() {
        return std::any_of(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
            return envelope.value(QStringLiteral("type")).toString() == QLatin1String("error")
                && envelope.value(QStringLiteral("payload")).toObject()
                       .value(QStringLiteral("code")).toString() == QLatin1String("ALREADY_SEATED");
        });
    });
    const QString retainedSeat = server.m_clients.value(socket).seatId;
    const bool retainedAuthoritativeUser = server.m_authoritativeRoom.hasUser(3);

    client.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
    EXPECT_TRUE(rejoinDenied);
    EXPECT_EQ(retainedSeat, QStringLiteral("red_attack_1"));
    EXPECT_TRUE(retainedAuthoritativeUser);
}

TEST(GameServerJoinRoomTest, StoppedRoomRefreshPreservesNormalJoin) {
    int argc = 1;
    char applicationName[] = "join_room_refresh_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    AuthenticationReceiver authentication;
    ASSERT_TRUE(authentication.listen());

    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:")
                                  + QByteArray::number(authentication.port()));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.listen(0));
    server.m_snapshotTimer.stop();
    authentication.setRoomControlResponse(
        QByteArrayLiteral(R"({"rooms":[{"roomId":"main","status":"preparing","seatLimits":{"red_commander":1}}],"kickRequests":[],"logoutRequests":[]})"));

    QWebSocket client;
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* socket = server.m_clients.keys().constFirst();
    sendClientEnvelope(client, QStringLiteral("auth"), QStringLiteral("refresh-auth"),
                       QJsonObject{{QStringLiteral("token"), QStringLiteral("refresh-token")}});
    ASSERT_TRUE(waitFor([&server, socket]() { return server.m_clients.value(socket).authenticated; }));

    sendClientEnvelope(client, QStringLiteral("joinRoom"), QStringLiteral("refresh-join"),
                       QJsonObject{{QStringLiteral("roomId"), server.m_roomId}});

    ASSERT_TRUE(waitFor([&server, socket]() {
        return server.m_clients.value(socket).roomId == server.m_roomId;
    }));
    sendClientEnvelope(client, QStringLiteral("claimSeat"), QStringLiteral("refresh-claim"),
                       QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_commander")}});
    ASSERT_TRUE(waitFor([&server, socket]() {
        return server.m_clients.value(socket).seatId == QLatin1String("red_commander");
    }));
    const qsizetype assignedSeatCount = std::count_if(
        server.m_authoritativeRoom.seats().cbegin(), server.m_authoritativeRoom.seats().cend(),
        [](const AuthoritativeRoom::Seat& seat) { return seat.userId == 3; });
    EXPECT_EQ(server.m_clients.value(socket).role, QStringLiteral("player"));
    EXPECT_EQ(assignedSeatCount, 1);
    client.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

TEST(GameServerJoinRoomTest, LogoutDuringStoppedRoomRefreshRejectsStaleJoin) {
    int argc = 1;
    char applicationName[] = "join_room_logout_reentrancy_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    AuthenticationReceiver authentication;
    ASSERT_TRUE(authentication.listen());

    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:")
                                  + QByteArray::number(authentication.port()));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.listen(0));
    server.m_snapshotTimer.stop();

    QList<QJsonObject> messages;
    QWebSocket client;
    QObject::connect(&client, &QWebSocket::textMessageReceived, [&messages](const QString& text) {
        messages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* socket = server.m_clients.keys().constFirst();
    sendClientEnvelope(client, QStringLiteral("auth"), QStringLiteral("reentrant-auth"),
                       QJsonObject{{QStringLiteral("token"), QStringLiteral("reentrant-token")}});
    ASSERT_TRUE(waitFor([&server, socket]() { return server.m_clients.value(socket).authenticated; }));
    authentication.setRoomControlResponse(
        QByteArrayLiteral(R"({"rooms":[{"roomId":"main","status":"preparing"}],"kickRequests":[],"logoutRequests":[{"id":71,"userId":3,"reason":"session revoked during join"}]})"));
    bool rehashInjected = false;
    authentication.setRoomControlHook([&server, &rehashInjected]() {
        std::vector<std::unique_ptr<QWebSocket>> sockets;
        sockets.reserve(128);
        for (int index = 0; index < 128; ++index) {
            sockets.push_back(std::make_unique<QWebSocket>());
            server.m_clients.insert(sockets.back().get(), GameServer::ClientSession{});
        }
        for (const auto& candidate : sockets) server.m_clients.remove(candidate.get());
        rehashInjected = true;
    });

    sendClientEnvelope(client, QStringLiteral("joinRoom"), QStringLiteral("reentrant-join"),
                       QJsonObject{{QStringLiteral("roomId"), server.m_roomId}});

    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
    EXPECT_TRUE(rehashInjected);
    EXPECT_TRUE(std::any_of(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
        if (envelope.value(QStringLiteral("type")).toString() != QLatin1String("error")) return false;
        const QString code = envelope.value(QStringLiteral("payload")).toObject()
                                 .value(QStringLiteral("code")).toString();
        return code == QLatin1String("SESSION_REVOKED");
    }));
}

TEST(GameServerSessionRevocationTest, LogoutSignalClosesActiveWebSocketImmediately) {
    int argc = 1;
    char applicationName[] = "logout_signal_websocket_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    AuthenticationReceiver authentication;
    ASSERT_TRUE(authentication.listen());

    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:")
                                  + QByteArray::number(authentication.port()));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.listen(0));
    server.m_snapshotTimer.stop();

    QList<QJsonObject> messages;
    QWebSocket client;
    QObject::connect(&client, &QWebSocket::textMessageReceived, [&messages](const QString& text) {
        messages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* socket = server.m_clients.keys().constFirst();
    sendClientEnvelope(client, QStringLiteral("auth"), QStringLiteral("logout-auth"),
                       QJsonObject{{QStringLiteral("token"), QStringLiteral("logout-token")}});
    ASSERT_TRUE(waitFor([&server, socket]() { return server.m_clients.value(socket).authenticated; }));

    server.processLogoutRequests(QJsonArray{QJsonObject{{QStringLiteral("id"), 41},
                                                        {QStringLiteral("userId"), 3},
                                                        {QStringLiteral("reason"), QStringLiteral("session invalidated")}}});

    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
    EXPECT_TRUE(std::any_of(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
        return envelope.value(QStringLiteral("type")).toString() == QLatin1String("error")
            && envelope.value(QStringLiteral("payload")).toObject()
                   .value(QStringLiteral("code")).toString() == QLatin1String("USER_KICKED_OFFLINE");
    }));
}

TEST(GameServerRoomKickTest, LastOccupantReturnsToLobbyAndRoomResetsWithoutLogout) {
    int argc = 1;
    char applicationName[] = "room_kick_lobby_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    AuthenticationReceiver authentication;
    ASSERT_TRUE(authentication.listen());

    qputenv("INTERNAL_API_KEY", QByteArray(32, 'k'));
    qputenv("AUTH_SERVICE_URL", QByteArray("http://127.0.0.1:")
                                  + QByteArray::number(authentication.port()));
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    server.m_roomStatus = QStringLiteral("preparing");
    claimCommanders(server.m_authoritativeRoom);
    server.syncAuthoritativeSeats();
    ASSERT_TRUE(server.listen(0));
    server.m_snapshotTimer.stop();

    QList<QJsonObject> messages;
    QWebSocket client;
    QObject::connect(&client, &QWebSocket::textMessageReceived, [&messages](const QString& text) {
        messages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort())));
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* socket = server.m_clients.keys().constFirst();
    sendClientEnvelope(client, QStringLiteral("auth"), QStringLiteral("kick-auth"),
                       QJsonObject{{QStringLiteral("token"), QStringLiteral("kick-token")}});
    ASSERT_TRUE(waitFor([&server, socket]() { return server.m_clients.value(socket).authenticated; }));
    sendClientEnvelope(client, QStringLiteral("joinRoom"), QStringLiteral("kick-join"),
                       QJsonObject{{QStringLiteral("roomId"), server.m_roomId}});
    ASSERT_TRUE(waitFor([&server, socket]() {
        return server.m_clients.value(socket).roomId == server.m_roomId;
    }));
    sendClientEnvelope(client, QStringLiteral("claimSeat"), QStringLiteral("kick-claim"),
                       QJsonObject{{QStringLiteral("seatId"), QStringLiteral("red_attack_1")}});
    ASSERT_TRUE(waitFor([&server, socket]() {
        return server.m_clients.value(socket).seatId == QLatin1String("red_attack_1");
    }));

    server.m_phase = QStringLiteral("running");
    server.m_engine.setRunning(true);
    messages.clear();
    server.processKickRequests(QJsonArray{QJsonObject{{QStringLiteral("id"), 51},
                                                       {QStringLiteral("roomId"), server.m_roomId},
                                                       {QStringLiteral("userId"), 3},
                                                       {QStringLiteral("reason"), QStringLiteral("room eviction")}}});

    ASSERT_TRUE(waitFor([&messages]() {
        return !eventPayload(messages, QStringLiteral("roomClosed")).isEmpty();
    }));
    ASSERT_TRUE(server.m_clients.contains(socket));
    EXPECT_TRUE(server.m_clients.value(socket).authenticated);
    EXPECT_TRUE(server.m_clients.value(socket).roomId.isEmpty());
    EXPECT_TRUE(server.m_clients.value(socket).seatId.isEmpty());
    EXPECT_EQ(client.state(), QAbstractSocket::ConnectedState);
    EXPECT_EQ(server.m_phase, QStringLiteral("preparing"));
    EXPECT_FALSE(server.m_engine.running());
    EXPECT_TRUE(server.m_authoritativeRoom.seats().isEmpty());

    client.close();
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
}

TEST(GameServerChatTest, PreparingBypassesRangeButRunningRequiresBilateralLink) {
    int argc = 1;
    char applicationName[] = "chat_communication_gate_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    qputenv("SCENARIO_PATH", temporary.filePath(QStringLiteral("scenario.json")).toUtf8());
    qputenv("MONITOR_LOG_PATH", temporary.filePath(QStringLiteral("monitor.jsonl")).toUtf8());
    qputenv("MONITOR_STATUS_PATH", temporary.filePath(QStringLiteral("status.json")).toUtf8());
    qputenv("CHECKPOINT_PATH", temporary.filePath(QStringLiteral("checkpoint.json")).toUtf8());
    qputenv("COMMAND_LOG_PATH", temporary.filePath(QStringLiteral("events.jsonl")).toUtf8());

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    claimCommanders(server.m_authoritativeRoom);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
                    3, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                    QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    1, QStringLiteral("red_commander"), GeoPos{1000.0, 1000.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
                    1, QStringLiteral("red_attack_1"), GeoPos{1500.0, 1000.0, 1000.0}).ok);
    ASSERT_TRUE(server.applyDeployedScenario());
    server.syncAuthoritativeSeats();

    auto* commanderSocket = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, &server);
    auto* attackSocket = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, &server);
    GameServer::ClientSession commander;
    commander.authenticated = true;
    commander.userId = 1;
    commander.roomId = server.m_roomId;
    commander.seatId = QStringLiteral("red_commander");
    commander.seatType = QStringLiteral("commander");
    commander.side = QStringLiteral("red");
    GameServer::ClientSession attack;
    attack.authenticated = true;
    attack.userId = 3;
    attack.roomId = server.m_roomId;
    attack.seatId = QStringLiteral("red_attack_1");
    attack.seatType = QStringLiteral("attack");
    attack.side = QStringLiteral("red");
    server.m_clients.insert(commanderSocket, commander);
    server.m_clients.insert(attackSocket, attack);

    UnitBase* commanderUnit = server.seatUnit(QStringLiteral("red_commander"));
    UnitBase* attackUnit = server.seatUnit(QStringLiteral("red_attack_1"));
    ASSERT_NE(commanderUnit, nullptr);
    ASSERT_NE(attackUnit, nullptr);
    const auto setCommRange = [](UnitBase* unit, double range) {
        UnitBase::Params params = unit->params();
        params.commRange = range;
        unit->setParams(params);
    };
    const QJsonObject payload{{QStringLiteral("text"), QStringLiteral("status report")},
                              {QStringLiteral("recipientSeatIds"),
                               QJsonArray{QStringLiteral("red_commander")}}};

    setCommRange(commanderUnit, 0.0);
    setCommRange(attackUnit, 0.0);
    server.m_phase = QStringLiteral("preparing");
    server.handleChat(attackSocket, payload);
    ASSERT_EQ(server.m_chatHistory.size(), 1);

    server.m_clients[attackSocket].lastChatAt = 0;
    setCommRange(commanderUnit, 0.0);
    setCommRange(attackUnit, 1000.0);
    server.m_phase = QStringLiteral("running");
    server.handleChat(attackSocket, payload);
    EXPECT_EQ(server.m_chatHistory.size(), 1);

    server.m_clients[attackSocket].lastChatAt = 0;
    setCommRange(commanderUnit, 1000.0);
    server.handleChat(attackSocket, payload);
    EXPECT_EQ(server.m_chatHistory.size(), 2);

    server.m_clients.clear();
}

TEST(GameServerChatTest, PreparingChatIsDeliveredOverWebSocketWithoutRuntimeUnits) {
    int argc = 1;
    char applicationName[] = "preparing_chat_delivery_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    server.m_phase = QStringLiteral("preparing");
    ASSERT_TRUE(server.m_server.listen(QHostAddress::LocalHost, 0));
    const QUrl endpoint(QStringLiteral("ws://127.0.0.1:%1").arg(server.m_server.serverPort()));

    QList<QJsonObject> commanderMessages;
    QList<QJsonObject> attackMessages;
    QWebSocket commanderClient;
    QWebSocket attackClient;
    QObject::connect(&commanderClient, &QWebSocket::textMessageReceived,
                     [&commanderMessages](const QString& text) {
        commanderMessages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    QObject::connect(&attackClient, &QWebSocket::textMessageReceived,
                     [&attackMessages](const QString& text) {
        attackMessages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    commanderClient.open(endpoint);
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 1; }));
    QWebSocket* commanderSocket = server.m_clients.keys().constFirst();
    attackClient.open(endpoint);
    ASSERT_TRUE(waitFor([&server]() { return server.m_clients.size() == 2; }));
    QWebSocket* attackSocket = nullptr;
    for (QWebSocket* candidate : server.m_clients.keys()) {
        if (candidate != commanderSocket) attackSocket = candidate;
    }
    ASSERT_NE(attackSocket, nullptr);

    auto& commander = server.m_clients[commanderSocket];
    commander.authenticated = true;
    commander.userId = 1;
    commander.roomId = server.m_roomId;
    commander.seatId = QStringLiteral("red_commander");
    commander.seatType = QStringLiteral("commander");
    commander.side = QStringLiteral("red");
    auto& attack = server.m_clients[attackSocket];
    attack.authenticated = true;
    attack.userId = 3;
    attack.username = QStringLiteral("pilot");
    attack.roomId = server.m_roomId;
    attack.seatId = QStringLiteral("red_attack_1");
    attack.seatType = QStringLiteral("attack");
    attack.side = QStringLiteral("red");

    server.handleChat(
        attackSocket,
        QJsonObject{{QStringLiteral("text"), QStringLiteral("ready check")},
                    {QStringLiteral("recipientSeatIds"),
                     QJsonArray{QStringLiteral("red_commander")}}});
    const auto receivedChat = [](const QList<QJsonObject>& messages) {
        return std::any_of(messages.cbegin(), messages.cend(), [](const QJsonObject& envelope) {
            return envelope.value(QStringLiteral("type")).toString() == QLatin1String("chat")
                && envelope.value(QStringLiteral("payload")).toObject()
                       .value(QStringLiteral("text")).toString() == QLatin1String("ready check");
        });
    };
    EXPECT_TRUE(waitFor([&]() {
        return receivedChat(commanderMessages) && receivedChat(attackMessages);
    }));

    commanderClient.close();
    attackClient.close();
    EXPECT_TRUE(waitFor([&server]() { return server.m_clients.isEmpty(); }));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

TEST(GameServerMapMarkTest, ParticipantMarksArePerSeatAndVisibleToCommander) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    const auto mark = [](const QString& seatId, const QString& side, const QString& type,
                         qint64 authorUserId, const QString& label,
                         const QJsonArray& visibleTo) {
        return QJsonObject{{QStringLiteral("kind"), QStringLiteral("mapMark")},
                           {QStringLiteral("seatId"), seatId},
                           {QStringLiteral("side"), side},
                           {QStringLiteral("markType"), type},
                           {QStringLiteral("authorUserId"), authorUserId},
                           {QStringLiteral("label"), label},
                           {QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), 10.0},
                                                                    {QStringLiteral("y"), 20.0}}},
                           {QStringLiteral("visibleToSeatIds"), visibleTo}};
    };

    server.appendMapMark(mark(QStringLiteral("red_attack_1"), QStringLiteral("red"),
                              QStringLiteral("self"), 10, QStringLiteral("old attack"),
                              QJsonArray{QStringLiteral("red_attack_1")}));
    server.appendMapMark(mark(QStringLiteral("red_recon_1"), QStringLiteral("red"),
                              QStringLiteral("self"), 10, QStringLiteral("recon"),
                              QJsonArray{QStringLiteral("red_recon_1")}));
    server.appendMapMark(mark(QStringLiteral("red_commander"), QStringLiteral("red"),
                              QStringLiteral("commander"), 1, QStringLiteral("order one"),
                              QJsonArray{QStringLiteral("red_commander"),
                                         QStringLiteral("red_attack_1")}));
    server.appendMapMark(mark(QStringLiteral("red_commander"), QStringLiteral("red"),
                              QStringLiteral("commander"), 1, QStringLiteral("order two"),
                              QJsonArray{QStringLiteral("red_commander"),
                                         QStringLiteral("red_attack_1")}));
    server.appendMapMark(mark(QStringLiteral("blue_attack_1"), QStringLiteral("blue"),
                              QStringLiteral("self"), 20, QStringLiteral("blue"),
                              QJsonArray{QStringLiteral("blue_attack_1")}));
    server.appendMapMark(mark(QStringLiteral("red_attack_1"), QStringLiteral("red"),
                              QStringLiteral("self"), 99, QStringLiteral("new attack"),
                              QJsonArray{QStringLiteral("red_attack_1")}));

    ASSERT_EQ(server.m_mapMarks.size(), 5);
    int attackMarkCount = 0;
    int commanderMarkCount = 0;
    for (const QJsonValue& value : server.m_mapMarks) {
        const QJsonObject stored = value.toObject();
        if (stored.value(QStringLiteral("seatId")) == QLatin1String("red_attack_1")) {
            ++attackMarkCount;
            EXPECT_EQ(stored.value(QStringLiteral("label")), QLatin1String("new attack"));
        }
        if (stored.value(QStringLiteral("markType")) == QLatin1String("commander")) {
            ++commanderMarkCount;
        }
    }
    EXPECT_EQ(attackMarkCount, 1);
    EXPECT_EQ(commanderMarkCount, 2);

    GameServer::ClientSession commander;
    commander.roomId = server.m_roomId;
    commander.seatId = QStringLiteral("red_commander");
    commander.seatType = QStringLiteral("commander");
    commander.side = QStringLiteral("red");
    const QJsonArray commanderMarks = server.filteredMapMarks(commander);
    ASSERT_EQ(commanderMarks.size(), 4);
    for (const QJsonValue& value : commanderMarks) {
        const QJsonObject projected = value.toObject();
        EXPECT_FALSE(projected.contains(QStringLiteral("authorUserId")));
        EXPECT_FALSE(projected.contains(QStringLiteral("visibleToSeatIds")));
    }

    GameServer::ClientSession participant = commander;
    participant.seatId = QStringLiteral("red_attack_1");
    participant.seatType = QStringLiteral("attack");
    participant.userId = 99;
    const QJsonArray participantMarks = server.filteredMapMarks(participant);
    ASSERT_EQ(participantMarks.size(), 3);
    EXPECT_FALSE(std::any_of(participantMarks.cbegin(), participantMarks.cend(),
                             [](const QJsonValue& value) {
                                 return value.toObject().value(QStringLiteral("label"))
                                     == QLatin1String("recon");
                             }));

    GameServer::ClientSession replacement = participant;
    replacement.userId = 100;
    const QJsonArray replacementMarks = server.filteredMapMarks(replacement);
    ASSERT_EQ(replacementMarks.size(), 2);
    EXPECT_TRUE(std::all_of(replacementMarks.cbegin(), replacementMarks.cend(),
                            [](const QJsonValue& value) {
                                return value.toObject().value(QStringLiteral("markType"))
                                    == QLatin1String("commander");
                            }));
}

TEST(GameServerMapMarkTest, RateLimitIsSharedBySeatAcrossConnections) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        1, QStringLiteral("red"), QStringLiteral("red_commander"),
        QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
        1, QStringLiteral("red_commander"), GeoPos{1000.0, 1000.0, 0.0}).ok);
    server.syncAuthoritativeSeats();
    QString deploymentError;
    ASSERT_TRUE(server.applyDeployedScenario(&deploymentError))
        << deploymentError.toStdString();

    QWebSocket firstSocket;
    auto& first = server.m_clients[&firstSocket];
    first.authenticated = true;
    first.userId = 1;
    first.roomId = server.m_roomId;
    first.seatId = QStringLiteral("red_commander");
    first.seatType = QStringLiteral("commander");
    first.side = QStringLiteral("red");
    const QJsonObject position{{QStringLiteral("x"), 1000.0},
                               {QStringLiteral("y"), 1000.0}};
    for (int index = 0; index < 8; ++index) {
        server.handleMapMark(&firstSocket,
                             QJsonObject{{QStringLiteral("position"), position},
                                         {QStringLiteral("label"),
                                          QStringLiteral("mark-%1").arg(index)}});
    }
    ASSERT_EQ(server.m_mapMarkRateWindows.value(QStringLiteral("red_commander")).count, 8);

    QWebSocket secondSocket;
    auto& second = server.m_clients[&secondSocket];
    second.authenticated = true;
    second.userId = 1;
    second.roomId = server.m_roomId;
    second.seatId = first.seatId;
    second.seatType = first.seatType;
    second.side = first.side;
    server.handleMapMark(&secondSocket,
                         QJsonObject{{QStringLiteral("position"), position},
                                     {QStringLiteral("label"), QStringLiteral("blocked")}});
    EXPECT_EQ(server.m_mapMarkRateWindows.value(QStringLiteral("red_commander")).count, 9);
    ASSERT_EQ(server.m_mapMarks.size(), 8);
    EXPECT_FALSE(std::any_of(server.m_mapMarks.cbegin(), server.m_mapMarks.cend(),
                             [](const QJsonValue& value) {
                                 return value.toObject().value(QStringLiteral("label"))
                                     == QLatin1String("blocked");
                             }));
}

TEST(AuthoritativeRoomPveTest, ReservesBlueSeatsAndMirrorsRedRoster) {
    auto room = configuredRoom();
    ASSERT_TRUE(room.setMode(QStringLiteral("pve")).ok);
    EXPECT_EQ(room.mode(), QStringLiteral("pve"));
    ASSERT_TRUE(room.hasSeat(QStringLiteral("blue_commander")));
    EXPECT_EQ(room.seat(QStringLiteral("blue_commander")).controllerType,
              QStringLiteral("ai"));
    EXPECT_EQ(room.seat(QStringLiteral("blue_commander")).controllerId,
              QStringLiteral("ai:blue_commander"));

    ASSERT_TRUE(room.claimSeat(1, QStringLiteral("red"), QStringLiteral("red_commander"),
                               QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(room.claimSeat(2, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(room.syncAiRoster().ok);
    EXPECT_TRUE(room.hasSeat(QStringLiteral("blue_attack_1")));
    EXPECT_EQ(room.seat(QStringLiteral("blue_attack_1")).controllerType,
              QStringLiteral("ai"));
    EXPECT_EQ(room.seat(QStringLiteral("blue_attack_1")).selectedTemplate,
              QStringLiteral("attackuav"));

    EXPECT_EQ(room.claimSeat(3, QStringLiteral("blue-human"), QStringLiteral("blue_attack_2"),
                              QStringLiteral("attackuav")).code,
              QStringLiteral("SIDE_RESERVED_FOR_AI"));
}

TEST(AuthoritativeRoomPveTest, AiDeploymentIsDeterministicAndRosterFreezesAfterStart) {
    auto room = configuredRoom();
    ASSERT_TRUE(room.setMode(QStringLiteral("pve")).ok);
    ASSERT_TRUE(room.claimSeat(1, QStringLiteral("red"), QStringLiteral("red_commander"),
                               QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(room.claimSeat(2, QStringLiteral("pilot"), QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(room.syncAiRoster().ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_commander"), GeoPos{100.0, 100.0, 0.0}).ok);
    ASSERT_TRUE(room.deploy(1, QStringLiteral("red_attack_1"), GeoPos{200.0, 200.0, 20.0}).ok);
    ASSERT_TRUE(room.setReady(2, true).ok);
    ASSERT_TRUE(room.setReady(1, true).ok);
    ASSERT_TRUE(room.deployAiSeats(1000.0, 800.0, 7).ok);
    EXPECT_TRUE(room.seat(QStringLiteral("blue_commander")).deployed);
    EXPECT_TRUE(room.seat(QStringLiteral("blue_commander")).ready);
    ASSERT_TRUE(room.start().ok);

    EXPECT_EQ(room.syncAiRoster().code, QStringLiteral("SEAT_LOCKED"));
    const GeoPos position = room.seat(QStringLiteral("blue_commander")).position;
    EXPECT_GE(position.x, 0.0);
    EXPECT_LE(position.x, 1000.0);
    EXPECT_GE(position.y, 0.0);
    EXPECT_LE(position.y, 800.0);
}

TEST(GameServerPveLifecycleTest, DeploysAiBeforeRedCommanderReadiness) {
    int argc = 1;
    char applicationName[] = "game_server_pve_readiness_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.setMode(QStringLiteral("pve")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        1, QStringLiteral("red-player"), QStringLiteral("red_commander"),
        QStringLiteral("commandpost")).ok);
    server.m_roomMode = QStringLiteral("pve");
    server.m_phase = QStringLiteral("preparing");
    server.m_roomStatus = QStringLiteral("preparing");
    server.syncAuthoritativeSeats();

    QWebSocket socket;
    auto& session = server.m_clients[&socket];
    session.authenticated = true;
    session.userId = 1;
    session.roomId = server.m_roomId;
    session.seatId = QStringLiteral("red_commander");
    session.seatType = QStringLiteral("commander");
    session.side = QStringLiteral("red");
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
        1, QStringLiteral("red_commander"), GeoPos{1000.0, 1000.0, 0.0}).ok);
    server.syncAuthoritativeSeats();

    server.handleSeatReady(&socket, QJsonObject{{QStringLiteral("ready"), true}});

    const auto blueCommander = server.m_authoritativeRoom.seat(QStringLiteral("blue_commander"));
    EXPECT_TRUE(blueCommander.deployed);
    EXPECT_TRUE(blueCommander.ready);
    EXPECT_TRUE(server.m_authoritativeRoom.seat(QStringLiteral("red_commander")).ready);
    EXPECT_TRUE(server.m_authoritativeRoom.readiness().value(QStringLiteral("ready")).toBool());
    EXPECT_NE(server.m_engine.unit(QStringLiteral("blue_cp")), nullptr);
}

TEST(GameServerAiExecutionTest, AttackBudgetKeepsGeneratedMovementInAuthoritativeRoom) {
    int argc = 1;
    char applicationName[] = "game_server_ai_execution_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.setMode(QStringLiteral("pve")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        1, QStringLiteral("red-commander"), QStringLiteral("red_commander"),
        QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        2, QStringLiteral("red-attack-1"), QStringLiteral("red_attack_1"),
        QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        3, QStringLiteral("red-attack-2"), QStringLiteral("red_attack_2"),
        QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        4, QStringLiteral("red-attack-3"), QStringLiteral("red_attack_3"),
        QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        5, QStringLiteral("red-recon"), QStringLiteral("red_recon_1"),
        QStringLiteral("reconuav")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.syncAiRoster().ok);

    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
        1, QStringLiteral("red_commander"), GeoPos{1000.0, 1000.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
        1, QStringLiteral("red_attack_1"), GeoPos{1500.0, 1000.0, 20.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
        1, QStringLiteral("red_attack_2"), GeoPos{2000.0, 1000.0, 20.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
        1, QStringLiteral("red_attack_3"), GeoPos{2500.0, 1000.0, 20.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
        1, QStringLiteral("red_recon_1"), GeoPos{3000.0, 1000.0, 20.0}).ok);
    for (const qint64 userId : {2, 3, 4, 5}) {
        ASSERT_TRUE(server.m_authoritativeRoom.setReady(userId, true).ok);
    }
    ASSERT_TRUE(server.m_authoritativeRoom.setReady(1, true).ok);

    const QJsonObject map = server.m_engine.mapInfo();
    const double mapWidth = map.value(QStringLiteral("widthMeters")).toDouble();
    const double mapHeight = map.value(QStringLiteral("heightMeters")).toDouble();
    ASSERT_TRUE(server.m_authoritativeRoom.deployAiSeats(
        mapWidth, mapHeight, server.m_matchGeneration).ok);
    ASSERT_TRUE(server.applyDeployedScenario());
    server.syncAuthoritativeSeats();
    ASSERT_TRUE(server.m_authoritativeRoom.start().ok);
    server.m_roomMode = QStringLiteral("pve");
    server.m_roomStatus = QStringLiteral("running");
    server.m_phase = QStringLiteral("running");
    server.m_aiDifficulty = QStringLiteral("easy");

    const QString blueAttackId = server.m_authoritativeRoom
        .seat(QStringLiteral("blue_attack_1")).unitId;
    const QString redCommandPostId = server.m_authoritativeRoom
        .seat(QStringLiteral("red_commander")).unitId;
    const QString redAttackId = server.m_authoritativeRoom
        .seat(QStringLiteral("red_attack_1")).unitId;
    UnitBase* blueAttack = server.m_engine.unit(blueAttackId);
    UnitBase* redCommandPost = server.m_engine.unit(redCommandPostId);
    UnitBase* redAttack = server.m_engine.unit(redAttackId);
    ASSERT_NE(blueAttack, nullptr);
    ASSERT_NE(redCommandPost, nullptr);
    ASSERT_NE(redAttack, nullptr);
    const GeoPos blueAttackPosition = blueAttack->pos();
    const GeoPos redCommandPostPosition = redCommandPost->pos();
    const GeoPos redAttackPosition = redAttack->pos();
    blueAttack->setPosition(GeoPos{1000.0, 1000.0, 20.0});
    redAttack->setPosition(GeoPos{1050.0, 1000.0, 20.0});
    redCommandPost->setPosition(GeoPos{1400.0, 1000.0, 0.0});
    server.m_engine.stepOnce(0.01);
    const QList<AiSeatState> prioritizedStates = server.aiSeatStates();
    const auto prioritizedAttack = std::find_if(
        prioritizedStates.cbegin(), prioritizedStates.cend(), [](const AiSeatState& state) {
            return state.seatId == QLatin1String("blue_attack_1");
        });
    ASSERT_NE(prioritizedAttack, prioritizedStates.cend());
    EXPECT_TRUE(prioritizedAttack->targetVisible);
    EXPECT_EQ(prioritizedAttack->targetId, redCommandPostId);
    EXPECT_EQ(prioritizedAttack->targetKind, QStringLiteral("commandpost"));
    blueAttack->setPosition(blueAttackPosition);
    redCommandPost->setPosition(redCommandPostPosition);
    redAttack->setPosition(redAttackPosition);
    server.m_engine.stepOnce(0.01);

    QList<AiSeatState> states = server.aiSeatStates();
    ASSERT_EQ(states.size(), 4);
    EXPECT_TRUE(std::all_of(states.cbegin(), states.cend(), [](const AiSeatState& state) {
        return state.alive && state.movable && state.unitId != QLatin1String("blue_cp");
    }));
    const QString firstTarget = server.m_authoritativeRoom
        .seat(QStringLiteral("red_attack_1")).unitId;
    const QString secondTarget = server.m_authoritativeRoom
        .seat(QStringLiteral("red_attack_2")).unitId;
    for (AiSeatState& state : states) {
        if (state.seatId == QLatin1String("blue_attack_1")
            || state.seatId == QLatin1String("blue_attack_2")) {
            state.targetVisible = true;
            state.targetId = firstTarget;
        } else if (state.seatId == QLatin1String("blue_attack_3")) {
            state.targetVisible = true;
            state.targetId = secondTarget;
        }
    }
    server.m_sharedIntel[QStringLiteral("blue_attack_1")].insert(firstTarget);

    const double now = server.m_engine.simTime();
    server.m_aiPlan = RulesAi::makeCommanderPlan(
        states, QStringLiteral("authoritative-budget"), server.m_matchGeneration,
        server.m_stateRevision, now + 60.0, nullptr, 0.0,
        mapWidth, mapHeight, 1);
    server.m_aiNextDecisionAt = 0.0;
    server.m_aiNextReplanAt = now + 60.0;
    const QString reconId = server.m_authoritativeRoom
        .seat(QStringLiteral("blue_recon_1")).unitId;
    UnitBase* recon = server.m_engine.unit(reconId);
    ASSERT_NE(recon, nullptr);
    const GeoPos initial = recon->pos();

    server.runAiDecision();

    EXPECT_EQ(server.m_aiCommandSequence, 2U);
    ASSERT_EQ(server.m_commandResults.size(), 2);
    for (auto it = server.m_commandResults.cbegin(); it != server.m_commandResults.cend(); ++it) {
        EXPECT_TRUE(it.value().value(QStringLiteral("accepted")).toBool());
    }
    server.m_engine.stepOnce(1.0);
    const GeoPos final = recon->pos();
    ::testing::Test::RecordProperty("initialX", QString::number(initial.x).toStdString());
    ::testing::Test::RecordProperty("initialY", QString::number(initial.y).toStdString());
    ::testing::Test::RecordProperty("finalX", QString::number(final.x).toStdString());
    ::testing::Test::RecordProperty("finalY", QString::number(final.y).toStdString());
    ::testing::Test::RecordProperty("executedCommands",
                                    QString::number(server.m_aiCommandSequence).toStdString());
    EXPECT_GT(std::hypot(final.x - initial.x, final.y - initial.y), 0.0);

    UnitBase* destroyed = server.m_engine.unit(server.m_authoritativeRoom
        .seat(QStringLiteral("blue_attack_3")).unitId);
    ASSERT_NE(destroyed, nullptr);
    destroyed->setHp(0.0);
    const QList<AiSeatState> afterDamage = server.aiSeatStates();
    EXPECT_TRUE(std::none_of(afterDamage.cbegin(), afterDamage.cend(),
                             [](const AiSeatState& state) {
                                 return state.seatId == QLatin1String("blue_attack_3");
                             }));
}

TEST(GameServerPveReadinessTest, AiParameterReconciliationPreservesReadyRedSubordinates) {
    int argc = 1;
    char applicationName[] = "game_server_pve_readiness_reconciliation_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) application = std::make_unique<QCoreApplication>(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.setMode(QStringLiteral("pve")).ok);
    server.m_roomMode = QStringLiteral("pve");
    server.m_phase = QStringLiteral("preparing");
    server.m_roomStatus = QStringLiteral("preparing");
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        1, QStringLiteral("red-commander"), QStringLiteral("red_commander"),
        QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        2, QStringLiteral("red-attack"), QStringLiteral("red_attack_1"),
        QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
        1, QStringLiteral("red_commander"), GeoPos{1000.0, 1000.0, 0.0}).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.deploy(
        1, QStringLiteral("red_attack_1"), GeoPos{1500.0, 1000.0, 20.0}).ok);
    ASSERT_TRUE(server.applyDeployedScenario());
    server.syncAuthoritativeSeats();
    server.m_seatParameters.insert(
        QStringLiteral("blue_commander"),
        QJsonObject{{QStringLiteral("communicationRange"), 1.0}});

    QWebSocket commanderSocket;
    QWebSocket attackSocket;
    auto& commander = server.m_clients[&commanderSocket];
    commander.authenticated = true;
    commander.userId = 1;
    commander.roomId = server.m_roomId;
    commander.seatId = QStringLiteral("red_commander");
    commander.seatType = QStringLiteral("commander");
    commander.side = QStringLiteral("red");
    auto& attack = server.m_clients[&attackSocket];
    attack.authenticated = true;
    attack.userId = 2;
    attack.roomId = server.m_roomId;
    attack.seatId = QStringLiteral("red_attack_1");
    attack.seatType = QStringLiteral("attack");
    attack.side = QStringLiteral("red");

    server.handleSeatReady(&attackSocket, QJsonObject{{QStringLiteral("ready"), true}});
    ASSERT_TRUE(server.m_authoritativeRoom.seat(QStringLiteral("red_attack_1")).ready);

    const QJsonObject map = server.m_engine.mapInfo();
    ASSERT_TRUE(server.m_authoritativeRoom.deployAiSeats(
        map.value(QStringLiteral("widthMeters")).toDouble(),
        map.value(QStringLiteral("heightMeters")).toDouble(), server.m_matchGeneration).ok);
    ASSERT_TRUE(server.applyDeployedScenario());
    server.syncAuthoritativeSeats();
    ASSERT_TRUE(server.m_authoritativeRoom.seat(QStringLiteral("red_attack_1")).ready);

    ASSERT_NE(server.seatUnit(QStringLiteral("blue_commander")), nullptr);
    EXPECT_NE(server.seatUnit(QStringLiteral("blue_commander"))->commRange(), 1.0);
    server.reconcileSeatConfiguration(false);
    EXPECT_DOUBLE_EQ(server.seatUnit(QStringLiteral("blue_commander"))->commRange(), 1.0);

    EXPECT_TRUE(server.m_authoritativeRoom.seat(QStringLiteral("red_attack_1")).ready)
        << "AI parameter reconciliation cleared a ready red subordinate";
    server.handleSeatReady(&commanderSocket, QJsonObject{{QStringLiteral("ready"), true}});
    EXPECT_TRUE(server.m_authoritativeRoom.seat(QStringLiteral("red_commander")).ready)
        << "red commander readiness was rejected after AI parameter reconciliation";
}

TEST(GameServerAiLifecycleTest, PauseFinishAndEndCancelProviderWithCorrectMatchScope) {
    int argc = 1;
    char applicationName[] = "game_server_ai_lifecycle_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    HeldHttpReceiver ollama;
    ASSERT_TRUE(ollama.listen());
    configureGameServerEnvironment(temporary);
    qputenv("AI_PROVIDER", QByteArray("ollama"));
    qputenv("OLLAMA_BASE_URL", QByteArray("http://127.0.0.1:")
                                    + QByteArray::number(ollama.port()));

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    const auto startHeldRequest = [&server, &ollama]() {
        const int expectedConnections = ollama.connectionCount() + 1;
        server.m_aiPlanRequestInFlight = true;
        server.m_aiPlanRequestGeneration = server.m_matchGeneration;
        server.m_aiPlanRequestPlanningGeneration = server.m_aiPlanningGeneration;
        server.m_ollamaProvider->requestPlan(
            {}, QStringLiteral("lifecycle-request"), server.m_matchGeneration,
            server.m_stateRevision, server.m_aiPlanningGeneration, [](OllamaResult) {});
        ASSERT_TRUE(waitFor([&ollama, expectedConnections]() {
            return ollama.connectionCount() == expectedConnections;
        }));
        ASSERT_TRUE(server.m_ollamaProvider->inFlight());
    };

    server.m_phase = QStringLiteral("running");
    server.m_aiStickyRules = true;
    const quint64 pausedGeneration = server.m_matchGeneration;
    startHeldRequest();
    QString error;
    ASSERT_TRUE(server.applyDurableEvent(
        QStringLiteral("control"),
        QJsonObject{{QStringLiteral("action"), QStringLiteral("pause")}}, &error))
        << error.toStdString();
    EXPECT_EQ(server.m_phase, QStringLiteral("paused"));
    EXPECT_FALSE(server.m_ollamaProvider->inFlight());
    EXPECT_FALSE(server.m_aiPlanRequestInFlight);
    EXPECT_TRUE(server.m_aiStickyRules);
    EXPECT_EQ(server.m_matchGeneration, pausedGeneration);

    server.m_runInitialScenario = server.m_engine.scenario();
    startHeldRequest();
    const quint64 endedGeneration = server.m_matchGeneration;
    ASSERT_TRUE(server.applyDurableEvent(
        QStringLiteral("control"),
        QJsonObject{{QStringLiteral("action"), QStringLiteral("end")}}, &error))
        << error.toStdString();
    EXPECT_EQ(server.m_phase, QStringLiteral("preparing"));
    EXPECT_FALSE(server.m_ollamaProvider->inFlight());
    EXPECT_FALSE(server.m_aiPlanRequestInFlight);
    EXPECT_FALSE(server.m_aiStickyRules);
    EXPECT_EQ(server.m_matchGeneration, endedGeneration + 1);

    server.m_aiStickyRules = true;
    startHeldRequest();
    const quint64 finishedGeneration = server.m_matchGeneration;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &server.m_engine, "simulationEnded", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("red")), Q_ARG(QString, QStringLiteral("blue"))));
    EXPECT_EQ(server.m_phase, QStringLiteral("finished"));
    EXPECT_FALSE(server.m_ollamaProvider->inFlight());
    EXPECT_FALSE(server.m_aiPlanRequestInFlight);
    EXPECT_TRUE(server.m_aiStickyRules);
    EXPECT_EQ(server.m_matchGeneration, finishedGeneration);

    qunsetenv("OLLAMA_BASE_URL");
    qputenv("AI_PROVIDER", QByteArray("rules"));
}

TEST(GameServerAiLifecycleTest, DestructionCancelsOwnedProviderRequest) {
    int argc = 1;
    char applicationName[] = "game_server_ai_destruction_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    HeldHttpReceiver ollama;
    ASSERT_TRUE(ollama.listen());
    configureGameServerEnvironment(temporary);
    qputenv("AI_PROVIDER", QByteArray("ollama"));
    qputenv("OLLAMA_BASE_URL", QByteArray("http://127.0.0.1:")
                                    + QByteArray::number(ollama.port()));
    bool callbackCalled = false;

    {
        auto server = std::make_unique<GameServer>();
        ASSERT_TRUE(server->m_recoveryError.isEmpty())
            << server->m_recoveryError.toStdString();
        server->m_aiPlanRequestInFlight = true;
        server->m_ollamaProvider->requestPlan(
            {}, QStringLiteral("destruction-request"), server->m_matchGeneration,
            server->m_stateRevision, server->m_aiPlanningGeneration,
            [&callbackCalled](OllamaResult) { callbackCalled = true; });
        ASSERT_TRUE(waitFor([&ollama]() { return ollama.connectionCount() == 1; }));
        ASSERT_TRUE(server->m_ollamaProvider->inFlight());
    }

    EXPECT_TRUE(waitFor([&ollama]() { return ollama.disconnectionCount() == 1; }));
    EXPECT_FALSE(callbackCalled);
    qunsetenv("OLLAMA_BASE_URL");
    qputenv("AI_PROVIDER", QByteArray("rules"));
}

TEST(GameServerAiConversationTest, RecordsCompletedRejectedFailedAndCancelledAttempts) {
    int argc = 1;
    char applicationName[] = "game_server_ai_conversation_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    HeldHttpReceiver ollama;
    ASSERT_TRUE(ollama.listen());
    configureGameServerEnvironment(temporary);
    qputenv("AI_PROVIDER", QByteArray("ollama"));
    qputenv("OLLAMA_MODEL", QByteArray("auto"));
    qputenv("OLLAMA_BASE_URL", QByteArray("http://127.0.0.1:")
                                    + QByteArray::number(ollama.port()));

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    const auto arm = [&server](quint64 planningGeneration) {
        server.m_aiPlanningGeneration = planningGeneration;
        server.m_aiPlanRequestInFlight = true;
        server.m_aiPlanRequestGeneration = server.m_matchGeneration;
        server.m_aiPlanRequestPlanningGeneration = planningGeneration;
    };
    const auto resultFor = [](const QString& requestId, const QString& configured,
                              const QString& resolved) {
        OllamaResult result;
        result.requestId = requestId;
        result.configuredModel = configured;
        result.resolvedModel = resolved;
        result.messages = QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                                 {QStringLiteral("password"),
                                                  QStringLiteral("do-not-store")}}};
        result.rawResponse = QStringLiteral("Authorization: Bearer should-not-store");
        result.latencyMs = 7;
        return result;
    };

    AiPlanV1 fallback;
    fallback.requestId = QStringLiteral("rules-fallback");
    fallback.matchGeneration = server.m_matchGeneration;
    fallback.sourceStateRevision = server.m_stateRevision;
    fallback.planningGeneration = 1;
    server.m_aiPlan = fallback;

    arm(1);
    OllamaResult completed = resultFor(QStringLiteral("ai-plan:1:1"),
                                       QStringLiteral("auto"),
                                       QStringLiteral("qwen3.5:2b"));
    completed.ok = true;
    completed.plan.requestId = completed.requestId;
    completed.plan.matchGeneration = server.m_matchGeneration;
    completed.plan.sourceStateRevision = server.m_stateRevision;
    completed.plan.planningGeneration = 1;
    server.handleAiPlanResult(
        GameServer::AiPlanRequestContext{server.m_matchGeneration, 1, server.m_stateRevision},
        std::move(completed));

    server.m_aiPlan = fallback;
    arm(2);
    OllamaResult rejected = resultFor(QStringLiteral("ai-plan:1:2"),
                                      QStringLiteral("auto"),
                                      QStringLiteral("qwen3.5:2b"));
    rejected.failureClass = QStringLiteral("stale_response");
    server.handleAiPlanResult(
        GameServer::AiPlanRequestContext{server.m_matchGeneration, 2, server.m_stateRevision},
        std::move(rejected));

    server.m_aiPlan = fallback;
    arm(3);
    OllamaResult failed = resultFor(QStringLiteral("ai-plan:1:3"),
                                    QStringLiteral("auto"), QString());
    failed.failureClass = QStringLiteral("timeout");
    server.handleAiPlanResult(
        GameServer::AiPlanRequestContext{server.m_matchGeneration, 3, server.m_stateRevision},
        std::move(failed));

    server.m_aiPlan = fallback;
    arm(4);
    server.m_ollamaProvider->requestPlan(
        {}, QStringLiteral("ai-plan:1:4"), server.m_matchGeneration,
        server.m_stateRevision, 4, [](OllamaResult) {});
    ASSERT_TRUE(waitFor([&ollama]() { return ollama.connectionCount() == 1; }));
    server.cancelAiPlanRequest();

    QString error;
    const QVector<QJsonObject> records = server.m_aiConversationStore.readRecords(&error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(records.size(), 4);
    QHash<QString, QJsonObject> byStatus;
    for (const QJsonObject& record : records) {
        byStatus.insert(record.value(QStringLiteral("status")).toString(), record);
    }
    ASSERT_TRUE(byStatus.contains(QStringLiteral("completed")));
    ASSERT_TRUE(byStatus.contains(QStringLiteral("rejected")));
    ASSERT_TRUE(byStatus.contains(QStringLiteral("failed")));
    ASSERT_TRUE(byStatus.contains(QStringLiteral("cancelled")));
    const QJsonObject completedRecord = byStatus.value(QStringLiteral("completed"));
    EXPECT_EQ(completedRecord.value(QStringLiteral("configuredModel")).toString(),
              QStringLiteral("auto"));
    EXPECT_EQ(completedRecord.value(QStringLiteral("resolvedModel")).toString(),
              QStringLiteral("qwen3.5:2b"));
    EXPECT_EQ(completedRecord.value(QStringLiteral("final")).toObject()
                  .value(QStringLiteral("engine")).toString(), QStringLiteral("ollama"));
    EXPECT_EQ(completedRecord.value(QStringLiteral("messages")).toArray().at(0).toObject()
                  .value(QStringLiteral("password")).toString(), QStringLiteral("[REDACTED]"));
    EXPECT_FALSE(QJsonDocument(completedRecord).toJson().contains("Bearer should-not-store"));
    for (const QString& status : {QStringLiteral("rejected"), QStringLiteral("failed"),
                                  QStringLiteral("cancelled")}) {
        const QJsonObject record = byStatus.value(status);
        EXPECT_EQ(record.value(QStringLiteral("final")).toObject()
                      .value(QStringLiteral("engine")).toString(), QStringLiteral("rules"));
        EXPECT_EQ(record.value(QStringLiteral("fallback")).toObject()
                      .value(QStringLiteral("engine")).toString(), QStringLiteral("rules"));
        EXPECT_EQ(record.value(QStringLiteral("final")).toObject()
                      .value(QStringLiteral("plan")).toObject()
                      .value(QStringLiteral("requestId")).toString(),
                  QStringLiteral("rules-fallback"));
    }
    server.writeMonitorStatus();
    QFile statusFile(temporary.filePath(QStringLiteral("status.json")));
    ASSERT_TRUE(statusFile.open(QIODevice::ReadOnly));
    const QJsonObject aiStatus = QJsonDocument::fromJson(statusFile.readAll())
                                     .object().value(QStringLiteral("ai")).toObject();
    EXPECT_EQ(aiStatus.value(QStringLiteral("configuredModel")).toString(),
              QStringLiteral("auto"));
    EXPECT_EQ(aiStatus.value(QStringLiteral("resolvedModel")).toString(),
              QString());

    qunsetenv("OLLAMA_BASE_URL");
    qunsetenv("OLLAMA_MODEL");
    qputenv("AI_PROVIDER", QByteArray("rules"));
}

TEST(GameServerAiLifecycleTest, TwoFailuresAreStickyOnlyUntilNextMatch) {
    int argc = 1;
    char applicationName[] = "game_server_ai_sticky_fallback_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QTcpServer reservation;
    ASSERT_TRUE(reservation.listen(QHostAddress::LocalHost, 0));
    const quint16 refusedPort = reservation.serverPort();
    reservation.close();
    configureGameServerEnvironment(temporary);
    qputenv("AI_PROVIDER", QByteArray("ollama"));
    qputenv("OLLAMA_BASE_URL", QByteArray("http://127.0.0.1:")
                                    + QByteArray::number(refusedPort));

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.setMode(QStringLiteral("pve")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        1, QStringLiteral("red-commander"), QStringLiteral("red_commander"),
        QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        2, QStringLiteral("red-attack"), QStringLiteral("red_attack_1"),
        QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.syncAiRoster().ok);
    const QJsonObject map = server.m_engine.mapInfo();
    ASSERT_TRUE(server.m_authoritativeRoom.deployAiSeats(
        map.value(QStringLiteral("widthMeters")).toDouble(),
        map.value(QStringLiteral("heightMeters")).toDouble(),
        server.m_matchGeneration).ok);
    ASSERT_TRUE(server.applyDeployedScenario());
    server.m_roomMode = QStringLiteral("pve");
    server.m_phase = QStringLiteral("running");

    for (quint64 expectedFailures = 1; expectedFailures <= 2; ++expectedFailures) {
        server.m_aiNextDecisionAt = 0.0;
        server.m_aiNextReplanAt = 0.0;
        server.m_aiPlan = {};
        server.runAiDecision();
        ASSERT_TRUE(waitFor([&server, expectedFailures]() {
            return server.m_aiProviderFailures == expectedFailures;
        }));
    }
    EXPECT_TRUE(server.m_aiStickyRules);
    EXPECT_EQ(server.m_aiConsecutiveFailures, 2);
    EXPECT_EQ(server.m_aiEffectiveEngine, QStringLiteral("rules"));
    const quint64 priorGeneration = server.m_matchGeneration;
    const quint64 priorRequests = server.m_aiProviderRequests;

    server.resetAiMatchState();
    EXPECT_EQ(server.m_matchGeneration, priorGeneration + 1);
    EXPECT_FALSE(server.m_aiStickyRules);
    EXPECT_EQ(server.m_aiConsecutiveFailures, 0);
    server.m_aiNextDecisionAt = 0.0;
    server.runAiDecision();
    ASSERT_TRUE(waitFor([&server, priorRequests]() {
        return server.m_aiProviderRequests == priorRequests + 1;
    }));

    qunsetenv("OLLAMA_BASE_URL");
    qputenv("AI_PROVIDER", QByteArray("rules"));
}

TEST(GameServerAiProviderTest, PauseCancelsTrackedProviderRequest) {
    int argc = 1;
    char applicationName[] = "game_server_ai_pause_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    server.m_phase = QStringLiteral("running");
    server.m_aiPlanRequestInFlight = true;
    server.m_aiPlanRequestGeneration = server.m_matchGeneration;
    server.m_aiPlanRequestPlanningGeneration = 4;

    QString error;
    ASSERT_TRUE(server.applyDurableEvent(
        QStringLiteral("control"),
        QJsonObject{{QStringLiteral("action"), QStringLiteral("pause")}}, &error))
        << error.toStdString();

    EXPECT_FALSE(server.m_aiPlanRequestInFlight);
    EXPECT_EQ(server.m_aiPlanRequestGeneration, 0U);
    EXPECT_EQ(server.m_aiPlanRequestPlanningGeneration, 0U);
}

TEST(GameServerAiProviderTest, CancelTrackedProbeClearsProbeState) {
    int argc = 1;
    char applicationName[] = "game_server_ai_probe_cancel_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);

    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    server.m_aiProbeInFlight = true;

    server.cancelAiPlanRequest();

    EXPECT_FALSE(server.m_aiProbeInFlight);
}

TEST(GameServerAiProviderTest, StaleResponseDoesNotClearNewerTrackedRequest) {
    int argc = 1;
    char applicationName[] = "game_server_ai_stale_response_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    server.m_matchGeneration = 7;
    server.m_aiPlanningGeneration = 4;
    server.m_aiPlanRequestInFlight = true;
    server.m_aiPlanRequestGeneration = 7;
    server.m_aiPlanRequestPlanningGeneration = 4;
    const quint64 failuresBefore = server.m_aiProviderFailures;

    OllamaResult staleResult;
    staleResult.failureClass = QStringLiteral("stale-response-test");
    server.handleAiPlanResult(GameServer::AiPlanRequestContext{6, 3, server.m_stateRevision},
                              std::move(staleResult));

    EXPECT_TRUE(server.m_aiPlanRequestInFlight);
    EXPECT_EQ(server.m_aiPlanRequestGeneration, 7U);
    EXPECT_EQ(server.m_aiPlanRequestPlanningGeneration, 4U);
    EXPECT_EQ(server.m_aiProviderFailures, failuresBefore);
}

TEST(GameServerAiProviderTest, LateValidResponseRebasesOverRulesReplanAndIsRecorded) {
    int argc = 1;
    char applicationName[] = "game_server_ai_rebase_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    GameServer server;
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();
    ASSERT_TRUE(server.m_authoritativeRoom.setMode(QStringLiteral("pve")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        1, QStringLiteral("red-commander"), QStringLiteral("red_commander"),
        QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.claimSeat(
        2, QStringLiteral("red-attack"), QStringLiteral("red_attack_1"),
        QStringLiteral("attackuav")).ok);
    ASSERT_TRUE(server.m_authoritativeRoom.syncAiRoster().ok);
    const QJsonObject map = server.m_engine.mapInfo();
    ASSERT_TRUE(server.m_authoritativeRoom.deployAiSeats(
        map.value(QStringLiteral("widthMeters")).toDouble(),
        map.value(QStringLiteral("heightMeters")).toDouble(), server.m_matchGeneration).ok);
    ASSERT_TRUE(server.applyDeployedScenario());
    server.m_roomMode = QStringLiteral("pve");
    server.m_phase = QStringLiteral("running");
    server.m_matchGeneration = 7;
    server.m_aiPlanningGeneration = 4;
    server.m_aiPlanRequestInFlight = true;
    server.m_aiPlanRequestGeneration = 7;
    server.m_aiPlanRequestPlanningGeneration = 3;
    server.m_aiNextReplanAt = 100.0;

    OllamaResult result;
    result.ok = true;
    result.requestId = QStringLiteral("ai-plan:7:3");
    result.configuredModel = QStringLiteral("configured-model");
    result.resolvedModel = QStringLiteral("resolved-model");
    result.latencyMs = 32;
    result.messages = QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                              {QStringLiteral("content"), QStringLiteral("plan")}}};
    const QList<AiSeatState> states = server.aiSeatStates();
    ASSERT_FALSE(states.isEmpty());
    quint64 rngState = 1;
    result.plan = RulesAi::makeCommanderPlan(
        states, result.requestId, server.m_matchGeneration, server.m_stateRevision, 5.0,
        &rngState, 0.0, map.value(QStringLiteral("widthMeters")).toDouble(),
        map.value(QStringLiteral("heightMeters")).toDouble(), 3);

    server.handleAiPlanResult(
        GameServer::AiPlanRequestContext{7, 3, server.m_stateRevision}, std::move(result));

    EXPECT_FALSE(server.m_aiPlanRequestInFlight);
    EXPECT_EQ(server.m_aiPlanRequestGeneration, 0U);
    EXPECT_EQ(server.m_aiPlanRequestPlanningGeneration, 0U);
    EXPECT_EQ(server.m_aiProviderSuccesses, 1U);
    EXPECT_EQ(server.m_aiEffectiveEngine, QStringLiteral("ollama"));
    EXPECT_EQ(server.m_aiPlan.planningGeneration, 3U);
    EXPECT_DOUBLE_EQ(server.m_aiNextReplanAt, 5.0);

    QString error;
    const QVector<QJsonObject> records = server.m_aiConversationStore.readRecords(&error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(records.size(), 1);
    EXPECT_EQ(records.constFirst().value(QStringLiteral("status")).toString(),
              QStringLiteral("completed"));
    EXPECT_EQ(records.constFirst().value(QStringLiteral("requestId")).toString(),
              QStringLiteral("ai-plan:7:3"));
}

TEST(GameServerAiProviderTest, TwoFailuresStickForMatchAndResetReprobesModel) {
    int argc = 1;
    char applicationName[] = "game_server_ai_provider_tests";
    char* argv[] = {applicationName, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    const QByteArray tagsBody = QJsonDocument(QJsonObject{
        {QStringLiteral("models"),
         QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("other-model")}}}}})
        .toJson(QJsonDocument::Compact);
    RoomControlReceiver ollama(tagsBody);
    ASSERT_TRUE(ollama.listen());
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    configureGameServerEnvironment(temporary);
    qputenv("AI_PROVIDER", QByteArray("ollama"));
    qputenv("OLLAMA_MODEL", QByteArray("test-model"));
    qputenv("OLLAMA_BASE_URL", QByteArray("http://127.0.0.1:")
                                    + QByteArray::number(ollama.port()));
    GameServer server;
    qputenv("AI_PROVIDER", QByteArray("rules"));
    ASSERT_TRUE(server.m_recoveryError.isEmpty()) << server.m_recoveryError.toStdString();

    const auto requestFailure = [&server](quint64 planningGeneration) {
        const GameServer::AiPlanRequestContext context{
            server.m_matchGeneration, planningGeneration, server.m_stateRevision};
        server.m_aiPlanningGeneration = planningGeneration;
        server.m_aiPlanRequestInFlight = true;
        server.m_aiPlanRequestGeneration = context.matchGeneration;
        server.m_aiPlanRequestPlanningGeneration = context.planningGeneration;
        bool completed = false;
        server.m_ollamaProvider->requestPlan(
            {}, QStringLiteral("ai-plan:%1:%2")
                    .arg(context.matchGeneration).arg(context.planningGeneration),
            context.matchGeneration, context.sourceStateRevision, context.planningGeneration,
            [&server, context, &completed](OllamaResult result) {
                server.handleAiPlanResult(context, std::move(result));
                completed = true;
            });
        EXPECT_TRUE(waitFor([&completed]() { return completed; }));
    };

    requestFailure(1);
    EXPECT_EQ(ollama.requestCount(), 1);
    EXPECT_FALSE(server.m_aiStickyRules);
    requestFailure(2);
    EXPECT_EQ(ollama.requestCount(), 1);
    EXPECT_TRUE(server.m_aiStickyRules);
    EXPECT_EQ(server.m_aiConsecutiveFailures, 2);
    EXPECT_EQ(server.m_aiEffectiveEngine, QStringLiteral("rules"));

    const quint64 previousMatchGeneration = server.m_matchGeneration;
    server.resetAiMatchState();
    EXPECT_EQ(server.m_matchGeneration, previousMatchGeneration + 1);
    EXPECT_FALSE(server.m_aiStickyRules);
    EXPECT_EQ(server.m_aiConsecutiveFailures, 0);
    bool reprobeCompleted = false;
    server.m_ollamaProvider->requestPlan(
        {}, QStringLiteral("ai-plan:%1:1").arg(server.m_matchGeneration),
        server.m_matchGeneration, server.m_stateRevision, 1,
        [&reprobeCompleted](OllamaResult) { reprobeCompleted = true; });
    EXPECT_TRUE(waitFor([&reprobeCompleted]() { return reprobeCompleted; }));
    EXPECT_EQ(ollama.requestCount(), 2);
}
