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
    Q_PROPERTY(SimulationEngine* engine READ engine CONSTANT)
    Q_PROPERTY(double simTime READ simTime NOTIFY simTimeForward)
    Q_PROPERTY(bool running READ running NOTIFY runningForward)
    Q_PROPERTY(QVariantList units READ units NOTIFY unitsForward)
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
    Q_PROPERTY(int accountLatencyMs READ accountLatencyMs NOTIFY networkDiagnosticsChanged)
    Q_PROPERTY(int gameLatencyMs READ gameLatencyMs NOTIFY networkDiagnosticsChanged)
    Q_PROPERTY(QString lastCommandId READ lastCommandId NOTIFY commandStatusChanged)
    Q_PROPERTY(QString lastCommandStatus READ lastCommandStatus NOTIFY commandStatusChanged)
    Q_PROPERTY(QString lastCommandMessage READ lastCommandMessage NOTIFY commandStatusChanged)
    Q_PROPERTY(QStringList serverHistory READ serverHistory NOTIFY serverHistoryChanged)
    Q_PROPERTY(QString username READ username NOTIFY sessionChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY sessionChanged)
    Q_PROPERTY(QString userRole READ userRole NOTIFY sessionChanged)
    Q_PROPERTY(QString serverAddress READ serverAddress NOTIFY sessionChanged)
    Q_PROPERTY(QString matchPhase READ matchPhase NOTIFY roomStateChanged)
    Q_PROPERTY(bool redReady READ redReady NOTIFY roomStateChanged)
    Q_PROPERTY(bool blueReady READ blueReady NOTIFY roomStateChanged)
    Q_PROPERTY(QVariantList chatMessages READ chatMessages NOTIFY chatMessagesChanged)
    Q_PROPERTY(bool canEditScenario READ canEditScenario NOTIFY sessionChanged)
    Q_PROPERTY(bool canEditOwnRoster READ canEditOwnRoster NOTIFY sessionChanged)
    Q_PROPERTY(bool canDirect READ canDirect NOTIFY sessionChanged)
    Q_PROPERTY(QVariantList timeline READ timeline NOTIFY timelineForward)
    Q_PROPERTY(double replayDuration READ replayDuration NOTIFY timelineForward)
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
    QVariantList messages() const { return isNetworked() ? m_remoteMessages : m_engine.recentMessages(); }
    QJsonObject mapInfo() const { return m_engine.mapInfo(); }

    bool isNetworked() const { return m_sessionMode == QLatin1String("online"); }
    QString sessionMode() const { return m_sessionMode; }
    QString networkState() const { return m_networkState; }
    QString networkStatus() const { return m_networkStatus; }
    QString networkDiagnosticState() const { return m_networkDiagnosticState; }
    QString networkDiagnosticMessage() const { return m_networkDiagnosticMessage; }
    int accountLatencyMs() const { return m_accountLatencyMs; }
    int gameLatencyMs() const { return m_gameLatencyMs; }
    QString lastCommandId() const { return m_lastCommandId; }
    QString lastCommandStatus() const { return m_lastCommandStatus; }
    QString lastCommandMessage() const { return m_lastCommandMessage; }
    QStringList serverHistory() const { return m_serverHistory; }
    QString username() const { return m_username; }
    QString displayName() const { return m_displayName; }
    QString userRole() const { return m_userRole; }
    QString serverAddress() const { return m_serverAddress; }
    QString matchPhase() const { return m_matchPhase; }
    bool redReady() const { return m_redReady; }
    bool blueReady() const { return m_blueReady; }
    QVariantList chatMessages() const { return m_chatMessages; }
    bool canEditScenario() const { return !isNetworked() || m_userRole == QLatin1String("editor"); }
    bool canEditOwnRoster() const { return isNetworked() && (m_userRole == QLatin1String("red") || m_userRole == QLatin1String("blue")); }
    bool canDirect() const { return !isNetworked() || m_userRole == QLatin1String("director"); }
    QVariantList timeline() const { return isNetworked() ? QVariantList{} : m_engine.timelineForView(); }
    double replayDuration() const { return isNetworked() ? 0.0 : m_engine.replayDuration(); }

    Q_INVOKABLE void loadDefault();
    Q_INVOKABLE void saveScenario(const QString& path);
    Q_INVOKABLE void loadScenario(const QString& path);

    Q_INVOKABLE void setRunning(bool r);
    Q_INVOKABLE void setSpeed(double s);
    Q_INVOKABLE void stepOnce();

    Q_INVOKABLE void command(const QString& action, const QVariantMap& args);

    Q_INVOKABLE void saveSetting(const QString& key, const QVariant& value);
    Q_INVOKABLE QVariant loadSetting(const QString& key, const QVariant& defaultValue = QVariant()) const;
    Q_INVOKABLE QJsonObject allSettings() const;
    /// @brief Tell QML "your cached settings are stale, please re-read".
    /// @details Emitted whenever a shortcut or other UI-binding setting changes,
    /// so QML can re-call reloadAllShortcuts()/applySettings() without waiting
    /// for the settings panel to close.
    Q_SIGNAL void shortcutsChanged();

    Q_INVOKABLE void useLocalMode();
    Q_INVOKABLE void loginOnline(const QString& server, const QString& username,
                                 const QString& password);
    Q_INVOKABLE void diagnoseServer(const QString& server);
    Q_INVOKABLE void logoutOnline();
    Q_INVOKABLE void setReady(bool ready);
    Q_INVOKABLE void endMatch();
    Q_INVOKABLE void sendChat(const QString& text);

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
    void chatMessagesChanged();
    void commandStatusChanged();
    void timelineForward();

    void viewModeChanged();
    void focusedSideChanged();
    void focusedUnitIdChanged();
    void simTimeForward();
    void runningForward();
    void unitsForward();
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
    QString pickDefaultUnit(const QString& kind, const QString& side) const;
    void ensureFocusedConsistent();
    void applyRemoteSnapshot(const QJsonObject& payload);
    void applyRoleView();
    void rememberServerAddress(const QString& server);
    QJsonObject scenarioUnitJson(const ScenarioUnit& unit) const;
    bool applyScenarioReplacement(const Scenario& replacement);

    SimulationEngine m_engine;
    NetworkClient m_networkClient;
    Scenario m_savedLocalScenario;
    QString m_viewMode = "editor";
    QString m_focusedSide = "red";
    QString m_focusedUnitId;
    mutable QHash<QString, QString> m_cpCache;
    mutable QJsonArray m_snapshotCache;
    mutable bool m_snapshotCacheValid = false;
    QString m_sessionMode = QStringLiteral("unselected");
    QString m_networkState = QStringLiteral("disconnected");
    QString m_networkStatus = QStringLiteral("请选择运行模式");
    QString m_networkDiagnosticState = QStringLiteral("idle");
    QString m_networkDiagnosticMessage = QStringLiteral("尚未检测服务器");
    int m_accountLatencyMs = -1;
    int m_gameLatencyMs = -1;
    QString m_lastCommandId;
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
    qint64 m_remoteScenarioRevision = -1;
    QVariantList m_remoteMessages;
    QVariantList m_chatMessages;
};

} // namespace gbr
