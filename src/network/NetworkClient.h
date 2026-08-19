#pragma once

#include "ClientStateStore.h"
#include "FastDdsTransport.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QWebSocket>

namespace gbr {

class NetworkClient final : public QObject {
    Q_OBJECT
    friend class SimulationControllerTestPeer;
public:
    explicit NetworkClient(QObject* parent = nullptr);

    void login(const QString& accountServer, const QString& username, const QString& password);
    void diagnoseServer(const QString& accountServer);
    void close(bool waitForLogout = false);

    void sendCommand(const QString& action, const QVariantMap& args);
    void sendUnitOrder(const QString& unitId, const QString& text);
    void sendControl(const QString& action, double speed = -1.0);
    void sendReady(bool ready);
    void sendChat(const QString& text, const QStringList& recipientSeatIds = {});
    void requestRooms();
    void joinRoom(const QString& roomId);
    void observeRoom(const QString& roomId);
    void claimSeat(const QString& seatId);
    void approveSeatTransfer(const QString& seatId, qint64 userId, qint64 requestedRevision);
    void rejectSeatTransfer(const QString& seatId, qint64 userId, qint64 requestedRevision);
    void leaveRoom();
    void sendSeatReady(bool ready);
    void sendDeployment(const QString& unitId, const QVariantMap& position);
    void requestRedeploy();
    void redeploy(const QString& seatId);
    void sendUnitName(const QString& unitName);
    QString shareIntel(const QString& intelId, const QStringList& recipientSeatIds,
                       const QString& note = QString());
    QString createIntelReport(const QVariantMap& position, const QString& type,
                              const QString& title, const QString& note);
    QString requestIntelHistory(const QVariantMap& query = {});
    QString sendVmfMessage(const QJsonObject& message);
    void cancelIntelHistoryRequests();
    void sendMapMark(const QVariantMap& position, const QString& label,
                     const QStringList& recipientSeatIds = {});
    void setObserverTrajectories(const QStringList& unitIds);
    void sendScenarioUpsert(const QJsonObject& unit);
    void sendScenarioRemove(const QString& unitId);
    void sendScenarioReplace(const QJsonObject& scenario);

    QString accountServer() const { return m_accountServer; }
    QString state() const { return m_state; }
    QString diagnosticState() const { return m_diagnosticState; }
    QString diagnosticMessage() const { return m_diagnosticMessage; }
    int accountLatencyMs() const { return m_accountLatencyMs; }
    int gameLatencyMs() const { return m_gameLatencyMs; }
    QString dataPlaneName() const { return m_dataPlane.backendName(); }

signals:
    void stateChanged(const QString& state, const QString& message);
    void authenticated(const QString& username, const QString& displayName,
                       const QString& role, const QString& seatId,
                       const QString& accountServer);
    void roomDirectoryReceived(const QJsonArray& rooms);
    void seatStateReceived(const QJsonObject& state);
    void deploymentPromptReceived(const QJsonObject& prompt);
    void intelShareReceived(const QJsonObject& share);
    void intelHistoryPageReceived(const QJsonObject& page);
    void vmfEventReceived(const QJsonObject& event);
    void transferEventReceived(const QJsonObject& event);
    void snapshotReceived(const QJsonObject& payload);
    void deltaSnapshotReceived(const QJsonObject& payload,
                               const QStringList& changedUnitIds);
    void chatHistoryReceived(const QJsonArray& messages);
    void chatReceived(const QJsonObject& message);
    void eventReceived(const QJsonObject& event);
    void commandRejected(const QString& message);
    void commandStatusChanged(const QString& commandId, const QString& action,
                              const QString& status, const QString& code,
                              const QString& message);
    void fatalError(const QString& message);
    void authenticationLost(const QString& message);
    void diagnosticsChanged(const QString& state, const QString& message,
                            int accountLatencyMs, int gameLatencyMs);

private:
    void setState(const QString& state, const QString& message);
    void openWebSocket();
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onTextMessage(const QString& text);
    void fallbackToLegacyProtocol();
    void reconnectWithWireVersion(int protocolVersion, int schemaVersion,
                                  const QString& message);
    bool sendEnvelope(const QString& type, const QJsonObject& payload);
    bool sendEnvelope(const QString& type, const QJsonObject& payload,
                      const QString& messageId);
    void scheduleReconnect();
    void publishDiagnostics();
    void sendLatencyProbe();
    void requestResync();
    void processPendingCommands();
    void sendPendingCommand(const QString& commandId, bool retry);
    void retransmitPendingCommands();
    void processPendingIntelRequests();
    void sendPendingIntelRequest(const QString& requestId, bool retry);
    void retransmitPendingIntelRequests();
    void clearPendingCommands(const QString& status, const QString& message);
    void sendSimple(const QString& type, const QJsonObject& payload);
    QString sendIntelRequest(const QString& type, const QString& action,
                             const QJsonObject& payload);
    QUrl normalizeAccountServer(const QString& input) const;

    struct PendingCommand {
        QString action;
        QJsonObject args;
        qint64 lastSentAtMs = -1;
        int onlineWaitMs = 0;
        int attempts = 0;
    };

    struct PendingIntelRequest {
        QString type;
        QString action;
        QJsonObject payload;
        qint64 lastSentAtMs = -1;
        int onlineWaitMs = 0;
        int attempts = 0;
    };

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_loginReply;
    QPointer<QNetworkReply> m_logoutReply;
    QWebSocket m_socket;
    QTimer m_reconnectTimer;
    QTimer m_latencyTimer;
    QTimer m_connectTimer;
    QTimer m_authTimer;
    QTimer m_commandTimer;
    QElapsedTimer m_monotonic;
    QString m_accountServer;
    QString m_token;
    QString m_loginTokenCandidate;
    QString m_ddsTicket;
    QUrl m_webSocketUrl;
    QString m_state = QStringLiteral("disconnected");
    bool m_manualClose = false;
    int m_reconnectAttempt = 0;
    ClientStateStore m_stateStore;
    QHash<QString, PendingCommand> m_pendingCommands;
    QHash<QString, PendingIntelRequest> m_pendingIntelRequests;
    QJsonObject m_welcomePayload;
    FastDdsTransport m_dataPlane;
    quint64 m_loginGeneration = 0;
    bool m_authenticated = false;
    bool m_identityPublished = false;
    QString m_currentSeatId;
    QString m_diagnosticState = QStringLiteral("idle");
    QString m_diagnosticMessage = QStringLiteral("尚未检测服务器");
    int m_accountLatencyMs = -1;
    int m_gameLatencyMs = -1;
    qint64 m_pingSentAtMs = 0;
    bool m_pingPending = false;
    quint64 m_diagnosticGeneration = 0;
    int m_protocolVersion = Protocol::Version;
    int m_schemaVersion = Protocol::SchemaVersion;
    bool m_legacyFallbackAttempted = false;
};

} // namespace gbr
