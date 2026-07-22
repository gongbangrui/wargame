#pragma once

#include "core/Scenario.h"
#include "core/SimulationEngine.h"
#include "RoomPersistence.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QWebSocket>
#include <QWebSocketServer>

namespace gbr {

class GameServer final : public QObject {
    Q_OBJECT
public:
    explicit GameServer(QObject* parent = nullptr);
    ~GameServer() override;
    bool listen(quint16 port);

private:
    struct ClientSession {
        bool authenticated = false;
        bool authenticationPending = false;
        qint64 userId = 0;
        QString username;
        QString displayName;
        QString role;
        QString token;
        quint64 sequence = 0;
        qint64 lastChatAt = 0;
        qint64 rateWindowStartedAt = 0;
        int messagesInRateWindow = 0;
        QSet<QString> recentMessageIds;
        QStringList recentMessageIdOrder;
        QJsonObject lastSnapshot;
    };

    void onNewConnection();
    void onTextMessage(QWebSocket* socket, const QString& text);
    void authenticate(QWebSocket* socket, const QString& token);
    void validateActiveSessions();
    void finishAuthentication(QWebSocket* socket, const QJsonObject& identity);
    void removeClient(QWebSocket* socket);

    void handleCommand(QWebSocket* socket, const QJsonObject& payload);
    void handleControl(QWebSocket* socket, const QJsonObject& payload);
    void handleReady(QWebSocket* socket, const QJsonObject& payload);
    void handleChat(QWebSocket* socket, const QJsonObject& payload);
    void handleScenarioUpsert(QWebSocket* socket, const QJsonObject& payload);
    void handleScenarioRemove(QWebSocket* socket, const QJsonObject& payload);
    void handleScenarioReplace(QWebSocket* socket, const QJsonObject& payload);

    void sendEnvelope(QWebSocket* socket, const QString& type, const QJsonObject& payload);
    void sendError(QWebSocket* socket, const QString& code, const QString& message,
                   const QString& requestId = QString());
    void sendCommandResult(QWebSocket* socket, const QString& commandId,
                           const CommandResult& result);
    void broadcastSnapshots(bool forceFull = false);
    void sendFullSnapshot(QWebSocket* socket);
    void broadcastEvent(const QJsonObject& event, const QString& side = QString());
    void broadcastChat(const QJsonObject& message);

    QJsonObject snapshotFor(const ClientSession& session) const;
    QSet<QString> visibleUnitIds(const ClientSession& session) const;
    QJsonArray filteredMessages(const ClientSession& session) const;
    bool canEditSide(const ClientSession& session, const QString& side) const;
    QString controlledUnitId(const QString& action, const QVariantMap& args) const;
    bool validateCommandOwnership(const ClientSession& session, const QString& action,
                                  const QVariantMap& args, QString* code,
                                  QString* reason) const;
    void scenarioChanged();
    bool persistScenario(QString* error = nullptr);
    void resetReadiness();
    QJsonObject roomState() const;
    void audit(const QString& category, const QJsonObject& detail = QJsonObject{});
    void writeMonitorStatus();
    QString messageSummary(const QString& type, const QJsonObject& payload) const;
    bool recordDurableEvent(const QString& kind, const QJsonObject& payload,
                            QString* error = nullptr);
    bool persistRoomState(QString* error = nullptr);
    bool restoreRoomState(QString* error = nullptr);
    bool replayDurableEvents(QString* error = nullptr);
    bool applyDurableEvent(const QString& kind, const QJsonObject& payload,
                           QString* error = nullptr);

    QWebSocketServer m_server;
    QNetworkAccessManager m_network;
    QHash<QWebSocket*, ClientSession> m_clients;
    SimulationEngine m_engine;
    Scenario m_runInitialScenario;
    QTimer m_snapshotTimer;
    QTimer m_sessionValidationTimer;
    QTimer m_monitorStatusTimer;
    QTimer m_checkpointTimer;
    QString m_authServiceUrl;
    QString m_internalKey;
    QString m_scenarioPath;
    QString m_monitorLogPath;
    QString m_monitorStatusPath;
    RoomPersistence m_persistence;
    QString m_phase = QStringLiteral("preparing");
    bool m_redReady = false;
    bool m_blueReady = false;
    quint64 m_scenarioRevision = 1;
    quint64 m_stateRevision = 1;
    quint64 m_eventSequence = 0;
    quint64 m_chatSequence = 0;
    QJsonArray m_chatHistory;
    QHash<QString, QJsonObject> m_commandResults;
    QStringList m_commandResultOrder;
    QString m_recoveryError;
    QElapsedTimer m_uptime;
    quint64 m_totalConnections = 0;
    quint64 m_totalDisconnects = 0;
    quint64 m_totalResyncRequests = 0;
};

} // namespace gbr
