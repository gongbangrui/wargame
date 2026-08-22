#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include "../core/SimulationEngine.h"
#include "../network/NetworkClient.h"

namespace gbr {

/// @brief QML-facing controller wrapping SimulationEngine.
/// @details Forwards engine signals as QML-bindable properties, manages
/// view mode, focused unit, and command dispatch.
class SimulationController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
    Q_PROPERTY(QString focusedSide READ focusedSide WRITE setFocusedSide NOTIFY focusedSideChanged)
    Q_PROPERTY(QString focusedUnitId READ focusedUnitId WRITE setFocusedUnitId NOTIFY focusedUnitIdChanged)
    Q_PROPERTY(QString focusedKind READ focusedKind NOTIFY focusedUnitIdChanged)
    Q_PROPERTY(double simTime READ simTime NOTIFY simTimeForward)
    Q_PROPERTY(bool running READ running NOTIFY runningForward)
    Q_PROPERTY(QVariantList units READ units NOTIFY unitsForward)
    Q_PROPERTY(QVariantList projectiles READ projectiles NOTIFY projectilesForward)
    Q_PROPERTY(quint64 unitStateRevision READ unitStateRevision NOTIFY unitsForward)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesForward)
    Q_PROPERTY(QJsonObject mapInfo READ mapInfo NOTIFY mapInfoForward)
    Q_PROPERTY(bool readyForSim READ readyForSim NOTIFY readyForSimForward)
    Q_PROPERTY(QString cpIssues READ cpIssues NOTIFY readyForSimForward)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorForward)
    Q_PROPERTY(bool networked READ isNetworked NOTIFY networkedChanged)
    Q_PROPERTY(QString sessionMode READ sessionMode NOTIFY sessionChanged)
    Q_PROPERTY(QString networkState READ networkState NOTIFY networkStatusChanged)
    Q_PROPERTY(QString networkStatus READ networkStatus NOTIFY networkStatusChanged)
    Q_PROPERTY(QString networkDiagnosticState READ networkDiagnosticState NOTIFY networkDiagnosticsChanged)
    Q_PROPERTY(QString networkDiagnosticMessage READ networkDiagnosticMessage NOTIFY networkDiagnosticsChanged)
    Q_PROPERTY(QString dataPlaneName READ dataPlaneName NOTIFY networkStatusChanged)
    Q_PROPERTY(int accountLatencyMs READ accountLatencyMs NOTIFY networkDiagnosticsChanged)
    Q_PROPERTY(int gameLatencyMs READ gameLatencyMs NOTIFY networkDiagnosticsChanged)
    Q_PROPERTY(QString lastCommandId READ lastCommandId NOTIFY commandStatusChanged)
    Q_PROPERTY(QString lastCommandCode READ lastCommandCode NOTIFY commandStatusChanged)
    Q_PROPERTY(QString lastCommandStatus READ lastCommandStatus NOTIFY commandStatusChanged)
    Q_PROPERTY(QString lastCommandMessage READ lastCommandMessage NOTIFY commandStatusChanged)
    Q_PROPERTY(QStringList serverHistory READ serverHistory NOTIFY serverHistoryChanged)
    Q_PROPERTY(QString username READ username NOTIFY sessionChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY sessionChanged)
    Q_PROPERTY(QString userRole READ userRole NOTIFY sessionChanged)
    Q_PROPERTY(bool isRoomAdmin READ isRoomAdmin NOTIFY sessionChanged)
    Q_PROPERTY(QString serverAddress READ serverAddress NOTIFY sessionChanged)
    Q_PROPERTY(QVariantList onlineRooms READ onlineRooms NOTIFY onlineRoomsChanged)
    Q_PROPERTY(QVariantList onlineSeats READ onlineSeats NOTIFY onlineSeatsChanged)
    Q_PROPERTY(QVariantList onlineMapMarks READ onlineMapMarks NOTIFY onlineMapMarksChanged)
    Q_PROPERTY(QVariantList onlineIntelRecords READ onlineIntelRecords NOTIFY onlineIntelChanged)
    Q_PROPERTY(qint64 onlineIntelRevision READ onlineIntelRevision NOTIFY onlineIntelChanged)
    Q_PROPERTY(QStringList onlineIntelShareTargets READ onlineIntelShareTargets NOTIFY onlineIntelChanged)
    Q_PROPERTY(QVariantList onlineIntelHistory READ onlineIntelHistory NOTIFY onlineIntelHistoryChanged)
    Q_PROPERTY(bool onlineIntelHistoryHasMore READ onlineIntelHistoryHasMore NOTIFY onlineIntelHistoryChanged)
    Q_PROPERTY(QString onlineIntelHistoryCursor READ onlineIntelHistoryCursor NOTIFY onlineIntelHistoryChanged)
    Q_PROPERTY(bool onlineIntelHistoryPending READ onlineIntelHistoryPending
               NOTIFY onlineIntelHistoryChanged)
    Q_PROPERTY(QJsonObject observerTrajectories READ observerTrajectories
               NOTIFY observerTrajectoriesChanged)
    Q_PROPERTY(QVariantList pendingSeatTransfers READ pendingSeatTransfers NOTIFY pendingSeatTransfersChanged)
    Q_PROPERTY(QString currentRoomId READ currentRoomId NOTIFY onlineStateChanged)
    Q_PROPERTY(QString currentSeatId READ currentSeatId NOTIFY onlineStateChanged)
    Q_PROPERTY(QString currentSeatType READ currentSeatType NOTIFY onlineStateChanged)
    Q_PROPERTY(QString currentSeatSide READ currentSeatSide NOTIFY onlineStateChanged)
    Q_PROPERTY(QString onlineStage READ onlineStage NOTIFY onlineStateChanged)
    Q_PROPERTY(bool isObserver READ isObserver NOTIFY onlineStateChanged)
    Q_PROPERTY(bool seatReady READ seatReady NOTIFY onlineStateChanged)
    Q_PROPERTY(bool leaveRoomPending READ leaveRoomPending NOTIFY leaveRoomPendingChanged)
    Q_PROPERTY(QString matchPhase READ matchPhase NOTIFY roomStateChanged)
    Q_PROPERTY(bool redReady READ redReady NOTIFY roomStateChanged)
    Q_PROPERTY(bool blueReady READ blueReady NOTIFY roomStateChanged)
    Q_PROPERTY(QString roomMode READ roomMode NOTIFY roomStateChanged)
    Q_PROPERTY(QString aiDifficulty READ aiDifficulty NOTIFY roomStateChanged)
    Q_PROPERTY(QString aiEffectiveEngine READ aiEffectiveEngine NOTIFY roomStateChanged)
    Q_PROPERTY(qint64 configVersion READ configVersion NOTIFY roomStateChanged)
    Q_PROPERTY(QString roomName READ roomName NOTIFY roomStateChanged)
    Q_PROPERTY(QString roomDescription READ roomDescription NOTIFY roomStateChanged)
    Q_PROPERTY(QString scenarioId READ scenarioId NOTIFY roomStateChanged)
    Q_PROPERTY(QJsonObject onlineSeatLimits READ onlineSeatLimits NOTIFY roomStateChanged)
    Q_PROPERTY(QJsonObject onlineSeatParameters READ onlineSeatParameters NOTIFY roomStateChanged)
    Q_PROPERTY(QString communicationState READ communicationState NOTIFY roomStateChanged)
    Q_PROPERTY(QVariantList chatMessages READ chatMessages NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString lastCommandAction READ lastCommandAction NOTIFY commandStatusChanged)
    Q_PROPERTY(bool canEditScenario READ canEditScenario NOTIFY roomStateChanged)
    Q_PROPERTY(bool canEditOwnRoster READ canEditOwnRoster NOTIFY sessionChanged)
    Q_PROPERTY(bool canDirect READ canDirect NOTIFY sessionChanged)
    Q_PROPERTY(QVariantList timeline READ timeline NOTIFY timelineForward)
    Q_PROPERTY(double replayDuration READ replayDuration NOTIFY timelineForward)
    Q_PROPERTY(QJsonObject vmfWorkflow READ vmfWorkflow NOTIFY vmfWorkflowChanged)
    Q_PROPERTY(QString protocolProfile READ protocolProfile NOTIFY roomStateChanged)
    Q_PROPERTY(QJsonObject vmfTasks READ vmfTasks NOTIFY vmfTasksChanged)
    Q_PROPERTY(QJsonObject vmfTrace READ vmfTrace NOTIFY vmfTraceChanged)
public:
    explicit SimulationController(QObject* parent = nullptr);

    QString viewMode() const { return m_viewMode; }
    void setViewMode(const QString& m);
    QString focusedSide() const { return m_focusedSide; }
    void setFocusedSide(const QString& s);
    QString focusedUnitId() const { return m_focusedUnitId; }
    Q_INVOKABLE void setFocusedUnitId(const QString& id);
    QString focusedKind() const;

    SimulationEngine* engine() { return &m_engine; }

    double simTime() const { return m_engine.simTime(); }
    bool running() const { return m_engine.running(); }
    bool readyForSim() const { return isNetworked() ? m_remoteReadyForSim : m_engine.readyForSim(); }
    QString cpIssues() const { return isNetworked() ? m_remoteCpIssues : m_engine.cpIssues(); }
    QString lastError() const { return isNetworked() ? m_remoteLastError : m_engine.lastError(); }
    QVariantList units() const { return m_engine.unitsForView(); }
    QVariantList projectiles() const {
        return isNetworked() ? m_remoteProjectiles : m_engine.projectilesForView();
    }
    quint64 unitStateRevision() const { return m_unitStateRevision; }
    QVariantList messages() const { return isNetworked() ? m_remoteMessages : m_engine.recentMessages(); }
    QJsonObject mapInfo() const { return m_engine.mapInfo(); }

    bool isNetworked() const { return m_sessionMode == QLatin1String("online"); }
    QString sessionMode() const { return m_sessionMode; }
    QString networkState() const { return m_networkState; }
    QString networkStatus() const { return m_networkStatus; }
    QString networkDiagnosticState() const { return m_networkDiagnosticState; }
    QString networkDiagnosticMessage() const { return m_networkDiagnosticMessage; }
    QString dataPlaneName() const { return m_networkClient.dataPlaneName(); }
    int accountLatencyMs() const { return m_accountLatencyMs; }
    int gameLatencyMs() const { return m_gameLatencyMs; }
    QString lastCommandId() const { return m_lastCommandId; }
    QString lastCommandCode() const { return m_lastCommandCode; }
    QString lastCommandStatus() const { return m_lastCommandStatus; }
    QString lastCommandMessage() const { return m_lastCommandMessage; }
    QStringList serverHistory() const { return m_serverHistory; }
    QString username() const { return m_username; }
    QString displayName() const { return m_displayName; }
    QString userRole() const { return m_userRole; }
    bool isRoomAdmin() const {
        return isNetworked() && !m_isObserver
            && m_userRole == QLatin1String("room_admin");
    }
    QString serverAddress() const { return m_serverAddress; }
    QVariantList onlineRooms() const { return m_onlineRooms; }
    QVariantList onlineSeats() const { return m_onlineSeats; }
    QVariantList onlineMapMarks() const { return m_onlineMapMarks; }
    QVariantList onlineIntelRecords() const { return m_onlineIntelRecords; }
    qint64 onlineIntelRevision() const { return m_onlineIntelRevision; }
    QStringList onlineIntelShareTargets() const { return m_onlineIntelShareTargets; }
    QVariantList onlineIntelHistory() const { return m_onlineIntelHistory; }
    bool onlineIntelHistoryHasMore() const { return m_onlineIntelHistoryHasMore; }
    QString onlineIntelHistoryCursor() const { return m_onlineIntelHistoryCursor; }
    bool onlineIntelHistoryPending() const { return !m_onlineIntelHistoryRequestId.isEmpty(); }
    QJsonObject observerTrajectories() const { return m_observerTrajectories; }
    QVariantList pendingSeatTransfers() const { return m_pendingSeatTransfers; }
    QString currentRoomId() const { return m_currentRoomId; }
    QString currentSeatId() const { return m_currentSeatId; }
    QString currentSeatType() const { return m_currentSeatType; }
    QString currentSeatSide() const { return m_currentSeatSide; }
    QString onlineStage() const { return m_onlineStage; }
    bool isObserver() const { return m_isObserver; }
    bool seatReady() const { return m_seatReady; }
    bool leaveRoomPending() const { return m_leaveRoomPending; }
    QString matchPhase() const { return m_matchPhase; }
    bool redReady() const { return m_redReady; }
    bool blueReady() const { return m_blueReady; }
    QString roomMode() const { return m_roomMode; }
    QString aiDifficulty() const { return m_aiDifficulty; }
    QString aiEffectiveEngine() const { return m_aiEffectiveEngine; }
    qint64 configVersion() const { return m_configVersion; }
    QString roomName() const { return m_roomName; }
    QString roomDescription() const { return m_roomDescription; }
    QString scenarioId() const { return m_scenarioId; }
    QJsonObject onlineSeatLimits() const { return m_onlineSeatLimits; }
    QJsonObject onlineSeatParameters() const { return m_onlineSeatParameters; }
    QString communicationState() const { return m_communicationState; }
    QVariantList chatMessages() const { return m_chatMessages; }
    QString lastCommandAction() const { return m_lastCommandAction; }
    bool canEditScenario() const {
        if (!isNetworked()) return true;
        if (!isRoomAdmin() || m_currentRoomId.isEmpty()
            || m_matchPhase != QLatin1String("preparing")) return false;
        for (const QVariant& value : m_onlineSeats) {
            if (value.toMap().value(QStringLiteral("occupied")).toBool()) return false;
        }
        return true;
    }
    bool canEditOwnRoster() const { return false; }
    bool canDirect() const { return !isNetworked() || (!m_isObserver && m_currentSeatType == QLatin1String("commander")); }
    QVariantList timeline() const { return isNetworked() ? QVariantList{} : m_engine.timelineForView(); }
    double replayDuration() const { return isNetworked() ? 0.0 : m_engine.replayDuration(); }
    /// Projected guided-strike state for the focused side.  The QML layer sees
    /// only this bounded snapshot, never the engine workflow or MessageBus.
    QJsonObject vmfWorkflow() const;
    QString protocolProfile() const { return m_protocolProfile; }
    QJsonObject vmfTasks() const { return m_remoteVmfTasks; }
    QJsonObject vmfTrace() const { return m_remoteVmfTrace; }

    Q_INVOKABLE void loadDefault();
    Q_INVOKABLE void saveScenario(const QString& path);
    Q_INVOKABLE void loadScenario(const QString& path);

    Q_INVOKABLE void setRunning(bool r);
    Q_INVOKABLE void setSpeed(double s);
    Q_INVOKABLE void stepOnce();

    Q_INVOKABLE void command(const QString& action, const QVariantMap& args);

    /// Explicit VMF guided-strike facade used by local and online QML views.
    /// Local calls post through the authoritative MessageBus; online calls
    /// only enqueue a validated VMF envelope and wait for the server event.
    Q_INVOKABLE QVariantMap reportGuidedStrikeTarget(const QString& reconId,
                                                     const QString& targetId,
                                                     const QVariantMap& report = {});
    Q_INVOKABLE QVariantMap dispatchGuidedStrike(const QString& attackerId,
                                                 const QString& targetId,
                                                 const QVariantList& waypoints);
    Q_INVOKABLE QVariantMap commandGuidedStrikeGroundGuidance(
        const QString& guideId, const QString& attackerId, const QString& targetId);
    Q_INVOKABLE QVariantMap confirmGuidedStrikeAttack(
        const QString& guideId, const QString& attackerId,
        const QString& targetId, const QVariantList& waypoints);
    Q_INVOKABLE QVariantMap withdrawGuidedStrike(const QString& attackerId,
                                                 const QVariantMap& home);
    Q_INVOKABLE QVariantMap sendVmfTaskCommand(const QVariantMap& command);

    Q_INVOKABLE void saveSetting(const QString& key, const QVariant& value);
    Q_INVOKABLE QVariant loadSetting(const QString& key, const QVariant& defaultValue = QVariant()) const;
    Q_INVOKABLE QJsonObject allSettings() const;
    Q_INVOKABLE void saveRememberedPassword(const QString& server, const QString& username,
                                            const QString& password, bool remember);
    Q_INVOKABLE void loadRememberedPassword(const QString& server, const QString& username);
    /// @brief Tell QML that a setting was durably written and can be re-read.
    Q_SIGNAL void settingChanged(const QString& key);
    /// @brief Compatibility signal for the root shortcut cache.
    Q_SIGNAL void shortcutsChanged();
    Q_SIGNAL void rememberedPasswordLoaded(const QString& password);

    Q_INVOKABLE void useLocalMode();
    Q_INVOKABLE void loginOnline(const QString& server, const QString& username,
                                 const QString& password);
    Q_INVOKABLE void diagnoseServer(const QString& server);
    Q_INVOKABLE void logoutOnline();
    Q_INVOKABLE void setReady(bool ready);
    Q_INVOKABLE void endMatch();
    Q_INVOKABLE void sendChat(const QString& text, const QStringList& recipientSeatIds = {});
    Q_INVOKABLE void sendUnitOrder(const QString& unitId, const QString& text);
    Q_INVOKABLE void requestOnlineRooms();
    Q_INVOKABLE void joinOnlineRoom(const QString& roomId);
    Q_INVOKABLE void observeOnlineRoom(const QString& roomId);
    Q_INVOKABLE void claimOnlineSeat(const QString& seatId);
    Q_INVOKABLE void approveSeatTransfer(qint64 userId, qint64 requestedRevision);
    Q_INVOKABLE void rejectSeatTransfer(qint64 userId, qint64 requestedRevision);
    Q_INVOKABLE void leaveOnlineRoom();
    Q_INVOKABLE void setSeatReady(bool ready);
    Q_INVOKABLE void updateOnlineRoomConfig(const QVariantMap& config);
    Q_INVOKABLE void deployOnlineUnit(const QString& unitId, const QVariantMap& position);
    Q_INVOKABLE void requestOnlineRedeploy();
    Q_INVOKABLE void redeployOnlineUnit(const QString& seatId);
    Q_INVOKABLE void setOnlineUnitName(const QString& unitName);
    Q_INVOKABLE QString shareOnlineIntel(const QString& intelId,
                                         const QStringList& recipientSeatIds,
                                         const QString& note = QString());
    Q_INVOKABLE QString createOnlineIntelReport(const QVariantMap& position, const QString& type,
                                                const QString& title = QString(),
                                                const QString& note = QString());
    Q_INVOKABLE QString requestOnlineIntelHistory(const QVariantMap& query = {});
    Q_INVOKABLE void resetOnlineIntelHistory();
    Q_INVOKABLE void markOnlineMap(const QVariantMap& position, const QString& label = QString(),
                                   const QStringList& recipientSeatIds = {});
    Q_INVOKABLE void setObserverTrajectories(const QStringList& unitIds);

    /// 兼容旧 QML 调用的连接提示接口。
    Q_INVOKABLE QString connectToPeer(const QString& host, int port);
    Q_INVOKABLE void disconnectFromPeer();

    Q_INVOKABLE QJsonObject unitsJson() const;
    /// Adds or updates a scenario unit and returns its final ID. Empty IDs are generated here
    /// so QML can keep selection stable after a state refresh.
    Q_INVOKABLE QString upsertUnit(const QVariantMap& data);
    /// @brief Atomically replace only the unit list, preserving map metadata.
    Q_INVOKABLE bool replaceUnits(const QVariantList& units);
    /// @brief Atomically replace a complete scenario JSON object.
    Q_INVOKABLE bool replaceScenario(const QVariantMap& scenario);
    Q_INVOKABLE void removeUnit(const QString& id);
    Q_INVOKABLE bool removeUnits(const QStringList& ids);
    Q_INVOKABLE bool batchUpdateUnits(const QStringList& ids, const QVariantMap& changes);
    Q_INVOKABLE bool transformUnits(const QStringList& ids, const QString& operation,
                                    double value = 0.0);
    Q_INVOKABLE QVariantList copyUnits(const QStringList& ids) const;
    Q_INVOKABLE QStringList pasteUnits(const QVariantList& copied, double offsetX,
                                       double offsetY, const QString& sideOverride = QString());
    Q_INVOKABLE QVariantList scenarioValidationIssues() const;
    Q_INVOKABLE QVariantList unitTemplates() const;
    Q_INVOKABLE void setUnitSchedule(const QString& uid, const QVariantList& schedule);
    Q_INVOKABLE bool seekReplay(double targetTime);
    Q_INVOKABLE bool stepReplayEvent(int direction);
    Q_INVOKABLE QJsonObject battleReport() const;
    Q_INVOKABLE QString exportBattleReport(const QString& path, const QString& format);

    Q_INVOKABLE QJsonArray perceptionForSide(const QString& side) const;
    Q_INVOKABLE QJsonArray allUnits() const;
    Q_INVOKABLE QJsonObject unitAt(const QString& id) const;
    Q_INVOKABLE QVariantList unitOptions(const QString& kindFilter, const QString& sideFilter) const;
    Q_INVOKABLE QStringList viewModeOptions() const;
    /// @brief Return the alive command post id for a given side ("red"/"blue").
    Q_INVOKABLE QString commandPostIdFor(const QString& side) const;

    /// @brief Find enemies within attacker's attack range.
    Q_INVOKABLE QVariantList attackableTargets(const QString& attackerId, const QString& enemySide) const;
    /// @brief Find enemies detected by friendly recon or within attacker range.
    Q_INVOKABLE QVariantList detectedEnemyOptions(const QString& attackerId, const QString& friendlySide, const QString& enemySide) const;
    /// @brief Check if focused unit (attackUAV) has a target in attack range.
    Q_INVOKABLE bool hasTargetInAttackRange(const QString& unitId, const QString& enemySide) const;
    /// @brief Check if focused unit (groundscout) has targets in friendly recon detect range.
    Q_INVOKABLE bool hasTargetInDetectShared(const QString& unitId, const QString& friendlySide, const QString& enemySide) const;

    /// @brief Return all enemy unit IDs detected by any friendly unit (direct + shared knowledge).
    Q_INVOKABLE QStringList detectedEnemyIds(const QString& friendlySide) const;

    void invalidateCaches();

signals:
    void networkedChanged();
    void networkStatusChanged();
    void networkDiagnosticsChanged();
    void serverHistoryChanged();
    void sessionChanged();
    void roomStateChanged();
    void onlineRoomsChanged();
    void onlineSeatsChanged();
    void onlineMapMarksChanged();
    void onlineIntelChanged();
    void onlineIntelHistoryChanged();
    void onlineIntelHistoryReset();
    void observerTrajectoriesChanged();
    void pendingSeatTransfersChanged();
    void onlineStateChanged();
    void leaveRoomPendingChanged();
    void deploymentPrompt(const QVariantMap& prompt);
    void intelShareReceived(const QVariantMap& share);
    void onlineIntelCommandStatus(const QString& action, const QString& requestId,
                                  const QString& status, const QString& code,
                                  const QString& message);
    void transferEventReceived(const QVariantMap& event);
    void chatMessagesChanged();
    void commandStatusChanged();
    void timelineForward();
    void vmfWorkflowChanged();
    void vmfTasksChanged();
    void vmfTraceChanged();

    void viewModeChanged();
    void focusedSideChanged();
    void focusedUnitIdChanged();
    void simTimeForward();
    void runningForward();
    void unitsForward();
    void projectilesForward();
    void messagesForward();
    void mapInfoForward();
    void commandExecuted(const QString& action, const QVariantMap& args);
    void readyForSimForward();
    void targetDestroyedVisual(const QString& unitId, double x, double y);
    void eventForward(const QString& title, const QString& body, const QString& level, const QString& sourceUnitId);
    /// @brief Forwarded error signal from engine (IO/validation failures).
    void errorForward(const QString& message);
    /// @brief Forwarded simulation ended signal.
    void simEndForward(const QString& winner, const QString& loser);

private slots:
    void onUnitDestroyed(const QString& unitId);

private:
    friend class SimulationControllerTestPeer;
    QString pickDefaultUnit(const QString& kind, const QString& side) const;
    void ensureFocusedConsistent();
    void applyRemoteSnapshot(const QJsonObject& payload);
    void applyRemoteState(const QJsonObject& payload, const QStringList& changedUnitIds,
                          bool partialRuntime);
    void clearOnlineRoomDerivedState(bool preserveRoomId);
    void applyRoleView();
    void rememberServerAddress(const QString& server);
    QJsonObject scenarioUnitJson(const ScenarioUnit& unit) const;
    bool applyScenarioReplacement(const Scenario& replacement);
    QString guidedStrikeSide() const;
    QVariantMap guidedStrikeResult(bool accepted, const QString& code,
                                   const QString& message,
                                   const QString& requestId = QString()) const;
    QVariantMap sendGuidedStrikeVmf(Message message);

    SimulationEngine m_engine;
    NetworkClient m_networkClient;
    Scenario m_savedLocalScenario;
    QString m_viewMode = "editor";
    QString m_focusedSide = "red";
    QString m_focusedUnitId;
    mutable QHash<QString, QString> m_cpCache;
    mutable QJsonArray m_snapshotCache;
    mutable bool m_snapshotCacheValid = false;
    quint64 m_unitStateRevision = 0;
    QString m_sessionMode = QStringLiteral("unselected");
    QString m_networkState = QStringLiteral("disconnected");
    QString m_networkStatus = QStringLiteral("请选择运行模式");
    QString m_networkDiagnosticState = QStringLiteral("idle");
    QString m_networkDiagnosticMessage = QStringLiteral("尚未检测服务器");
    int m_accountLatencyMs = -1;
    int m_gameLatencyMs = -1;
    QString m_lastCommandId;
    QString m_lastCommandCode;
    QString m_lastCommandAction;
    QString m_lastCommandStatus;
    QString m_lastCommandMessage;
    QStringList m_serverHistory;
    QString m_username;
    QString m_displayName;
    QString m_userRole;
    QString m_serverAddress;
    QString m_matchPhase = QStringLiteral("preparing");
    QString m_remoteCpIssues;
    QString m_remoteLastError;
    bool m_remoteReadyForSim = false;
    bool m_redReady = false;
    bool m_blueReady = false;
    QString m_roomMode = QStringLiteral("pvp");
    QString m_aiDifficulty = QStringLiteral("normal");
    QString m_aiEffectiveEngine = QStringLiteral("rules");
    qint64 m_configVersion = 1;
    QString m_roomName;
    QString m_roomDescription;
    QString m_scenarioId = QStringLiteral("default");
    QJsonObject m_onlineSeatLimits;
    QJsonObject m_onlineSeatParameters;
    QString m_communicationState = QStringLiteral("disconnected");
    qint64 m_remoteScenarioRevision = -1;
    QVariantList m_remoteMessages;
    QVariantList m_remoteProjectiles;
    QJsonObject m_remoteVmfWorkflow;
    QJsonObject m_remoteVmfTasks;
    QJsonObject m_remoteVmfTrace;
    QString m_protocolProfile = QStringLiteral("native");
    QVariantList m_chatMessages;
    QVariantList m_onlineRooms;
    QVariantList m_onlineSeats;
    QVariantList m_onlineMapMarks;
    QVariantList m_onlineIntelRecords;
    qint64 m_onlineIntelRevision = 0;
    QStringList m_onlineIntelShareTargets;
    QVariantList m_onlineIntelHistory;
    bool m_onlineIntelHistoryHasMore = false;
    QString m_onlineIntelHistoryCursor;
    bool m_onlineIntelHistoryAppendPending = false;
    QString m_onlineIntelHistoryRequestId;
    QJsonObject m_observerTrajectories;
    QVariantList m_pendingSeatTransfers;
    QString m_currentRoomId;
    QString m_currentSeatId;
    QString m_currentSeatType;
    QString m_currentSeatSide;
    QString m_onlineStage = QStringLiteral("login");
    bool m_isObserver = false;
    bool m_observerJoinPending = false;
    bool m_seatReady = false;
    bool m_leaveRoomPending = false;
};

} // namespace gbr
