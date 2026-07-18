#pragma once

#include "AuthPolicy.h"

#include <QObject>
#include <QJsonObject>
#include <QQueue>
#include <QTimer>
#include <QWebSocketServer>
#include <map>

namespace gbr {

class PersistenceStore;
class SimulationRoom;
struct ServerConfig;
struct ServerMetrics;

class SessionGateway : public QObject {
    Q_OBJECT
public:
    SessionGateway(const ServerConfig& config, AuthPolicy* auth,
                   PersistenceStore* persistence, SimulationRoom* room,
                   ServerMetrics* metrics, QObject* parent = nullptr);
    ~SessionGateway() override;

    bool listen(QString* error = nullptr);
    void close();
    quint16 serverPort() const { return m_server.serverPort(); }

private:
    struct Connection {
        SessionIdentity identity;
        QString clientId;
        qint64 sequence = 0;
        qint64 connectedAtMs = 0;
        qint64 lastSeenMs = 0;
        int protocolErrors = 0;
        bool authenticated = false;
        bool closing = false;
        double packetTokens = 0.0;
        qint64 lastPacketRefillMs = 0;
        double commandTokens = 0.0;
        qint64 lastTokenRefillMs = 0;
        qint64 lastResyncMs = 0;
        QJsonObject lastProjectedState;
    };

    void onNewConnection();
    void onDisconnected(QWebSocket* socket);
    void onTextMessage(QWebSocket* socket, const QString& message);
    void handleHello(QWebSocket* socket, Connection& connection,
                     const QJsonObject& envelope);
    void handleAuthenticated(QWebSocket* socket, Connection& connection,
                             const QJsonObject& envelope);
    void handleCommand(QWebSocket* socket, Connection& connection,
                       const QJsonObject& envelope);
    void sendSnapshot(QWebSocket* socket, Connection& connection,
                      const QJsonObject* projectedBase = nullptr);
    void sendStateUpdate(QWebSocket* socket, Connection& connection,
                         const QJsonObject& projectedBase);
    void broadcastStateUpdates();
    void heartbeat();
    bool sendEnvelope(QWebSocket* socket, Connection& connection,
                      const QString& type, const QJsonObject& payload,
                      const QString& messageId = QString());
    void sendError(QWebSocket* socket, Connection& connection,
                   const QString& code, const QString& message,
                   bool closeAfter = false);
    void recordProtocolError(QWebSocket* socket, Connection& connection,
                             const QString& code, const QString& message,
                             bool closeAfter = false);
    bool consumeCommandToken(Connection& connection);
    bool consumePacketToken(Connection& connection);

    const ServerConfig& m_config;
    AuthPolicy* m_auth = nullptr;
    PersistenceStore* m_persistence = nullptr;
    SimulationRoom* m_room = nullptr;
    ServerMetrics* m_metrics = nullptr;
    QWebSocketServer m_server;
    std::map<QWebSocket*, Connection> m_connections;
    QTimer m_snapshotTimer;
    QTimer m_heartbeatTimer;
};

} // namespace gbr
