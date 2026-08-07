#pragma once

#include "core/Scenario.h"
#include "core/SimulationEngine.h"
#include "AuthoritativeRoom.h"
#include "OllamaConversationStore.h"
#include "RulesAi.h"
#include "OllamaProvider.h"
#include "RoomPersistence.h"
#include "FastDdsNode.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
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
    static constexpr int kMaxPendingConnections = 64;
    static constexpr int kMaxConnectedClients = 64;
    static constexpr int kMaxUnauthenticated = 32;
    static constexpr int kMaxUnauthenticatedPerIp = 8;
    static constexpr int kUnauthenticatedTimeoutMs = 5000;
    static constexpr int kAuthenticationMaxAttempts = 3;
    static constexpr int kAuthenticationDeadlineMs = 15000;
    static constexpr int kAuthenticationAttemptTimeoutMs = 5000;
    static constexpr int kAuthenticationBackoffBaseMs = 250;
    static constexpr int kAuthenticationJitterMs = 100;

    struct AiPlanRequestContext {
        quint64 matchGeneration = 0;
        quint64 planningGeneration = 0;
        quint64 sourceStateRevision = 0;
    };

    struct ClientSession {
        bool authenticated = false;
        bool authenticationPending = false;
        int authenticationAttempts = 0;
        qint64 authenticationDeadlineAtMs = 0;
        qint64 userId = 0;
        QString username;
        QString displayName;
        QString role;
        QString roomId;
        QString seatId;
        QString seatType;
        QString side;
        bool seatReady = false;
        bool observer = false;
        QSet<QString> observerTrajectorySelection;
        QString token;
        QString ddsTicket;
        qint64 ddsTicketExpiresAtMs = 0;
        QString connectedAt;
        QString lastSeenAt;
        quint64 sequence = 0;
        qint64 lastChatAt = 0;
        qint64 rateWindowStartedAt = 0;
        int messagesInRateWindow = 0;
        QSet<QString> recentMessageIds;
        QStringList recentMessageIdOrder;
        QJsonObject lastSnapshot;
        QHostAddress peerAddress;
    };

    struct RoomStateBackup {
        QJsonObject authoritativeRoom;
        Scenario scenario;
        QJsonArray runtimeUnits;
        QJsonObject engineState;
        double simTime = 0.0;
        bool running = false;
        double speed = 1.0;
        QString phase;
        QString roomStatus;
        bool redReady = false;
        bool blueReady = false;
        QJsonArray mapMarks;
        QHash<QString, QSet<QString>> sharedIntel;
        QJsonArray chatHistory;
        quint64 chatSequence = 0;
        QHash<QString, QJsonObject> commandResults;
        QStringList commandResultOrder;
        quint64 scenarioRevision = 1;
        quint64 stateRevision = 1;
        quint64 eventSequence = 0;
        quint64 matchGeneration = 1;
        quint64 aiCommandSequence = 0;
        quint64 aiPlanningGeneration = 0;
        quint64 aiRngState = 1;
        double aiNextDecisionAt = 0.0;
        double aiNextReplanAt = 0.0;
        AiPlanV1 aiPlan;
        bool aiStickyRules = false;
        int aiConsecutiveFailures = 0;
        QHash<QString, AiObservedTarget> aiContactMemory;
        QString aiStrategyPhase = QStringLiteral("recon");
        QString aiReplanReason;
        double aiNextPrivilegedSampleAt = 0.0;
        quint64 aiPrivilegedSampleSequence = 0;
        QString aiSelectedProvider;
        QString aiSelectedModel;
        QString aiResolvedModel;
        quint64 aiRoomConfigVersion = 1;
        quint64 aiOllamaConfigVersion = 1;
        QString aiFallbackReason;
    };

    struct MapMarkRateWindow {
        qint64 startedAt = 0;
        int count = 0;
    };

    static QHostAddress normalizedPeerAddress(const QHostAddress& address);
    static bool incomingTextExceedsPreflight(const QString& text);
    int authenticatedClientCount() const;
    int unauthenticatedClientCount() const;
    int unauthenticatedClientCount(const QHostAddress& peerAddress) const;
    void onNewConnection();
    void onTextMessage(QWebSocket* socket, const QString& text);
    void authenticate(QWebSocket* socket, const QString& token);
    void startAuthenticationAttempt(QWebSocket* socket);
    void handleAuthenticationReply(QWebSocket* socket, QNetworkReply* reply);
    void scheduleAuthenticationRetry(QWebSocket* socket, const QString& classification,
                                      int statusCode);
    void failAuthentication(QWebSocket* socket, const QString& code,
                            const QString& message, const QString& classification,
                            bool credentialFailure, int statusCode);
    void validateActiveSessions();
    void syncRoomControl(QWebSocket* requester = nullptr);
    void refreshRoomControlForJoin(QWebSocket* socket, const QJsonObject& payload,
                                   qint64 expectedUserId);
    void completeJoinRoom(QWebSocket* socket, const QJsonObject& payload,
                          qint64 expectedUserId);
    void reconcileSeatConfiguration(bool resetReadinessForParameterChanges);
    QJsonArray roomOccupants() const;
    void reportRoomStatus(const QString& status, const QString& reason,
                          const QString& winner = QString());
    void reportRoomPresence();
    void processKickRequests(const QJsonArray& requests);
    void processLogoutRequests(const QJsonArray& requests, QWebSocket* joiningSocket = nullptr);
    void finishAuthentication(QWebSocket* socket, const QJsonObject& identity);
    void removeClient(QWebSocket* socket);

    void handleCommand(QWebSocket* socket, const QJsonObject& payload);
    void handleControl(QWebSocket* socket, const QJsonObject& payload);
    void handleReady(QWebSocket* socket, const QJsonObject& payload);
    void handleChat(QWebSocket* socket, const QJsonObject& payload);
    void handleRoomList(QWebSocket* socket);
    void sendRoomDirectory(QWebSocket* socket);
    void broadcastRoomDirectory();
    void handleJoinRoom(QWebSocket* socket, const QJsonObject& payload);
    void handleClaimSeat(QWebSocket* socket, const QJsonObject& payload);
    void handleLeaveRoom(QWebSocket* socket, const QJsonObject& payload);
    void handleSeatReady(QWebSocket* socket, const QJsonObject& payload);
    void handleDeployment(QWebSocket* socket, const QJsonObject& payload);
    void handleRedeployRequest(QWebSocket* socket);
    void handleRedeploy(QWebSocket* socket, const QJsonObject& payload);
    void handleSetUnitName(QWebSocket* socket, const QJsonObject& payload);
    void handleShareIntel(QWebSocket* socket, const QJsonObject& payload);
    void handleMapMark(QWebSocket* socket, const QJsonObject& payload);
    void handleSetObserverTrajectories(QWebSocket* socket, const QJsonObject& payload);
    void sendSeatDirectory(QWebSocket* socket);
    QString normalizedRole(const ClientSession& session) const;
    bool hasSeatPermission(const ClientSession& session, const QString& action) const;
    UnitBase* seatUnit(const QString& seatId) const;
    void handleScenarioUpsert(QWebSocket* socket, const QJsonObject& payload);
    void handleScenarioRemove(QWebSocket* socket, const QJsonObject& payload);
    void handleScenarioReplace(QWebSocket* socket, const QJsonObject& payload);
    void handleFastDdsEnvelope(const QString& topic, const QJsonObject& payload);
    void runAiDecision();
    void applyAiConfiguration(const QJsonObject& config);
    void probeAiProvider();
    QList<AiSeatState> aiSeatStates() const;
    bool executeAiCommand(const AiCommand& command);
    void cancelAiPlanRequest();
    void resetAiMatchState();
    void handleAiPlanResult(const AiPlanRequestContext& context, OllamaResult result);
    void recordAiConversation(const OllamaResult& result, quint64 planningGeneration,
                              const QString& status, const AiPlanV1& finalPlan,
                              const QString& fallbackReason = QString());
    void updateAiContactMemory(const QList<AiSeatState>& states, double now,
                               const AiDifficultyParameters& parameters,
                               double mapWidth, double mapHeight);
    AiKnowledgeState buildAiKnowledge(const QList<AiSeatState>& states, double now,
                                      const AiDifficultyParameters& parameters) const;

    void sendEnvelope(QWebSocket* socket, const QString& type, const QJsonObject& payload);
    void sendError(QWebSocket* socket, const QString& code, const QString& message,
                   const QString& requestId = QString());
    void sendCommandResult(QWebSocket* socket, const QString& commandId,
                           const CommandResult& result);
    void closeRoomSessions(const QString& message);
    void broadcastSnapshots(bool forceFull = false);
    void sendFullSnapshot(QWebSocket* socket);
    void broadcastEvent(const QJsonObject& event, const QString& side = QString());
    void broadcastChat(const QJsonObject& message);

    QJsonObject snapshotFor(const ClientSession& session, quint64 projectedRevision = 0) const;
    void sampleObserverTrajectories();
    QJsonObject observerTrajectoriesFor(const ClientSession& session) const;
    QSet<QString> visibleUnitIds(const ClientSession& session) const;
    QJsonArray filteredMessages(const ClientSession& session) const;
    QJsonArray filteredChatHistory(const ClientSession& session) const;
    QJsonArray filteredMapMarks(const ClientSession& session) const;
    void appendMapMark(const QJsonObject& mark);
    void removeParticipantMarksForUser(qint64 userId, const QString& legacySeatId = QString());
    QString controlledUnitId(const QString& action, const QVariantMap& args) const;
    bool validateCommandOwnership(const ClientSession& session, const QString& action,
                                  const QVariantMap& args, QString* code,
                                  QString* reason) const;
    bool persistScenario(QString* error = nullptr);
    void resetReadiness();
    void syncAuthoritativeSeats();
    bool clearDeploymentRuntime(QString* error = nullptr);
    bool applyDeployedScenario(QString* error = nullptr);
    bool applyDeploymentIfPreparing(QString* error = nullptr);
    bool applyDepartureToRuntime(const QStringList& removedUnitIds,
                                 QString* error = nullptr);
    RoomStateBackup captureRoomState() const;
    bool restoreRoomStateBackup(const RoomStateBackup& backup, QString* error = nullptr);
    bool resetAuthoritativeRuntime(const QString& operationId, QString* error = nullptr);
    bool resetRoomIfEmpty(QString* error = nullptr);
    void processRoomOperation(const QJsonObject& operation);
    void acknowledgeRoomOperation(const QString& operationId, const QString& state,
                                  quint64 revision, const QString& code = QString());
    void clearRoomOperationTracking();
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
    FastDdsNode m_fastDds;
    QNetworkAccessManager m_network;
    QHash<QWebSocket*, ClientSession> m_clients;
    SimulationEngine m_engine;
    AuthoritativeRoom m_authoritativeRoom;
    Scenario m_runInitialScenario;
    QTimer m_snapshotTimer;
    QTimer m_sessionValidationTimer;
    QTimer m_roomSyncTimer;
    QTimer m_presenceTimer;
    QTimer m_monitorStatusTimer;
    QTimer m_checkpointTimer;
    QTimer m_aiDecisionTimer;
    QTimer m_aiProbeTimer;
    QString m_dataDir;
    OllamaConversationStore m_aiConversationStore;
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
    quint64 m_matchGeneration = 1;
    quint64 m_aiCommandSequence = 0;
    quint64 m_aiPlanningGeneration = 0;
    quint64 m_aiRngState = 0xA17A11ULL;
    double m_aiNextDecisionAt = 0.0;
    double m_aiNextReplanAt = 0.0;
    AiPlanV1 m_aiPlan;
    bool m_aiStickyRules = false;
    int m_aiConsecutiveFailures = 0;
    QHash<QString, AiObservedTarget> m_aiContactMemory;
    QString m_aiStrategyPhase = QStringLiteral("recon");
    QString m_aiReplanReason = QStringLiteral("match_start");
    double m_aiNextPrivilegedSampleAt = 0.0;
    quint64 m_aiPrivilegedSampleSequence = 0;
    OllamaProvider* m_ollamaProvider = nullptr;
    QString m_aiProviderMode = QStringLiteral("auto");
    QString m_aiSelectedProvider = QStringLiteral("rules");
    QString m_aiSelectedModel;
    QString m_aiResolvedModel;
    quint64 m_aiRoomConfigVersion = 1;
    quint64 m_aiOllamaConfigVersion = 1;
    QString m_aiFallbackReason;
    QString m_aiEffectiveEngine = QStringLiteral("rules");
    QString m_aiLastFailureClass;
    QString m_aiConnectionStatus = QStringLiteral("unknown");
    QString m_aiProbeFailureClass;
    QString m_aiLastProbeAt;
    quint64 m_aiConfigVersion = 0;
    bool m_aiConfigApplied = false;
    bool m_aiProbeInFlight = false;
    bool m_aiPlanRequestInFlight = false;
    quint64 m_aiPlanRequestGeneration = 0;
    quint64 m_aiPlanRequestPlanningGeneration = 0;
    quint64 m_aiProviderRequests = 0;
    quint64 m_aiProviderSuccesses = 0;
    quint64 m_aiProviderFailures = 0;
    qint64 m_aiLastLatencyMs = 0;
    qint64 m_aiAverageLatencyMs = 0;
    quint64 m_aiCommandAccepted = 0;
    quint64 m_aiCommandRejected = 0;
    quint64 m_aiResourceWithdrawals = 0;
    qint64 m_aiStrategyPlannerLatencyMs = 0;
    quint64 m_chatSequence = 0;
    QJsonArray m_chatHistory;
    QJsonArray m_mapMarks;
    QHash<QString, MapMarkRateWindow> m_mapMarkRateWindows;
    QHash<QString, QJsonObject> m_commandResults;
    QStringList m_commandResultOrder;
    QString m_recoveryError;
    QElapsedTimer m_uptime;
    quint64 m_totalConnections = 0;
    quint64 m_totalDisconnects = 0;
    quint64 m_totalResyncRequests = 0;
    QString m_authenticationHealth = QStringLiteral("healthy");
    QString m_lastAuthenticationFailureClass;
    quint64 m_totalAuthenticationAttempts = 0;
    quint64 m_totalAuthenticationRetries = 0;
    quint64 m_totalAuthenticationTransientFailures = 0;
    quint64 m_totalAuthenticationCredentialFailures = 0;
    quint64 m_totalAuthenticationFinalFailures = 0;
    struct SeatOccupant {
        QString seatId;
        QString seatType;
        QString side;
        qint64 userId = 0;
        QString username;
        QString controllerType = QStringLiteral("human");
        QString controllerId;
        bool ready = false;
    };
    QHash<QString, SeatOccupant> m_seats;
    QHash<QString, QSet<QString>> m_sharedIntel;
    QHash<qint64, QSet<QString>> m_observerSelectionCache;
    QHash<QString, QJsonArray> m_observerTrajectories;
    double m_nextObserverTrajectorySampleAt = 0.0;
    QHash<QString, int> m_seatLimits;
    QHash<QString, QJsonObject> m_seatParameters;
    QJsonArray m_roomDirectory;
    bool m_roomDirectoryLoaded = false;
    bool m_roomSyncInFlight = false;
    QList<QPointer<QWebSocket>> m_roomListWaiters;
    QString m_roomId = QStringLiteral("main");
    QString m_roomName = QStringLiteral("主推演室");
    QString m_roomStatus = QStringLiteral("stopped");
    bool m_observerJoinAllowed = false;
    QString m_roomMode = QStringLiteral("pvp");
    QString m_aiDifficulty = QStringLiteral("normal");
    quint64 m_configVersion = 1;
    QString m_lastRoomUpdate;
    QString m_lifecycleOperationInFlight;
    QString m_lifecycleOperationAction;
    quint64 m_lifecycleOperationRequestedRevision = 0;
    quint64 m_lifecycleOperationRequestedConfigVersion = 0;
    quint64 m_lifecycleOperationRequestedOllamaVersion = 0;
    QString m_lifecycleOperationAckState;
    QString m_lifecycleOperationAckCode;
    quint64 m_lifecycleOperationAckRevision = 0;
    bool m_lifecycleOperationAckInFlight = false;
};

} // namespace gbr
