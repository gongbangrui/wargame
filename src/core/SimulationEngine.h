#pragma once

#include "Geo.h"
#include "MapProvider.h"
#include "MessageBus.h"
#include "Scenario.h"
#include "IClock.h"
#include "RealTimeClock.h"
#include "MessageLogRecorder.h"
#include "ITransport.h"
#include "CommandResult.h"

#include <QObject>
#include <QTimer>
#include <QJsonObject>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gbr {

class UnitBase;
class LocalTransport;

/// @brief Core simulation engine that drives the wargame.
/// @details Runs a 50ms real-time tick loop, scales by speedMul, and manages
/// all units, message bus, and map projection. Exposes QML-friendly properties.
class SimulationEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(double simTime READ simTime NOTIFY simTimeChanged)
    Q_PROPERTY(double speedMul READ speedMul WRITE setSpeedMul NOTIFY speedMulChanged)
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(QJsonObject mapInfo READ mapInfo NOTIFY mapChanged)
    Q_PROPERTY(QVariantList units READ unitsForView NOTIFY unitsChanged)
    Q_PROPERTY(QVariantList messages READ recentMessages NOTIFY messagesChanged)
    Q_PROPERTY(bool readyForSim READ readyForSim NOTIFY readyForSimChanged)
    Q_PROPERTY(QString cpIssues READ cpIssues NOTIFY readyForSimChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorOccurred)
public:
    explicit SimulationEngine(QObject* parent = nullptr);
    /// @brief 注入自定义推演域消息传输。
    /// @details When omitted, the engine creates a LocalTransport internally — same
    /// behaviour as the no-arg constructor. The transport is owned by the engine
    /// ONLY if it was created internally; externally-supplied transports must be
    /// kept alive by the caller (typically the SimulationController).
    explicit SimulationEngine(ITransport* transport, QObject* parent = nullptr);
    ~SimulationEngine();

    /// @brief Load and apply a full scenario, rebuilding all units.
    /// @returns true when the whole scenario passed validation and was applied.
    bool setScenario(const Scenario& s);
    void loadDefaultScenario();

    void setRunning(bool r);
    bool running() const { return m_running; }
    void setSpeedMul(double m);
    double speedMul() const { return m_clock->speedMul(); }
    Q_INVOKABLE void stepOnce(double simSeconds = 1.0);

    double simTime() const { return m_clock->simTime(); }

    MessageBus* bus() const { return m_transport ? m_transport->bus() : nullptr; }
    ITransport* transport() const { return m_transport; }
    MapProvider* map() const { return m_map.get(); }
    /// @brief Underlying clock (for advanced/test injection).
    IClock* clock() const { return m_clock.get(); }

    /// @brief Get a JSON snapshot of a single unit including detections.
    Q_INVOKABLE QJsonObject unitSnapshot(const QString& id) const;
    /// @brief Dispatch a named command and return a stable structured result.
    Q_INVOKABLE QVariantMap command(const QString& action, const QVariantMap& args);
    CommandResult executeCommand(const QString& action, const QVariantMap& args);

    /// @brief Incrementally add or update a unit without rebuilding all units.
    void addOrUpdateUnit(const ScenarioUnit& u);
    /// @brief Incrementally remove a unit without rebuilding all units.
    void removeUnit(const QString& id);
    QStringList unitIds() const;
    const Scenario& scenario() const { return m_scenario; }
    /// @brief Lookup index for scenario unit by id; returns nullptr if not found.
    ScenarioUnit* findScenarioUnit(const QString& id);
    /// @brief Persist current scenario to a JSON file.
    void persistScenario(const QString& path);

    /// @brief Enable/disable the append-only message log recorder.
    /// @details Default off (no IO). When enabled, every emitted message
    /// is appended as one line of compact JSON to @p path.
    void setMessageLogEnabled(bool enabled, const QString& path = QString());

    UnitBase* unit(const QString& id) const;

    QJsonArray collectPerceptionSnapshot(const QString& forSide) const;
    QJsonArray collectAllUnitsSnapshot() const;

    /// 应用权威运行时状态，但不启动本地计时器。
    void applyRemoteRuntimeState(const QJsonArray& units, double simTime,
                                 bool running, double speedMul);
    QJsonArray collectCheckpointState() const;
    bool restoreCheckpointState(const QJsonArray& units, double simTime,
                                bool running, double speedMul, QString* error = nullptr);

    QVariantList unitsForView() const;
    QVariantList recentMessages() const { return m_messageCache; }
    QJsonObject mapInfo() const { return m_map->describe(); }
    bool readyForSim() const { return m_readyForSim; }
    QString cpIssues() const { return m_cpIssues; }
    QString lastError() const { return m_lastError; }

signals:
    void simTimeChanged();
    void speedMulChanged();
    void runningChanged();
    void unitsChanged();
    void messagesChanged();
    void mapChanged();
    void perceptionUpdated(const QString& unitId, const QJsonObject& perception);
    void eventPosted(const QString& title, const QString& body, const QString& level, const QString& sourceUnitId);
    void readyForSimChanged();
    void targetDestroyedVisual(const QString& unitId, double x, double y);
    /// @brief Emitted when a unit is destroyed (HP reaches 0).
    void unitDestroyed(const QString& unitId);
    /// @brief Emitted when an error occurs (IO failure, validation failure, etc).
    void errorOccurred(const QString& message);
    /// @brief Emitted when one side's command post is destroyed.
    void simulationEnded(const QString& winner, const QString& loser);

private slots:
    void onMessagePosted(const QJsonObject& msg);
    void flushDirtyUnits();

private:
    void recomputeReadyForSim();
    void rebuildScenarioIndex();

    /// @brief Resolve the registered CommandPost id for a unit's side (dynamic, not hardcoded).
    QString commandSenderIdFor(const class UnitBase* u) const;

    void rebuildUnitsFromScenario();
    void createSingleUnit(const ScenarioUnit& u);
    void connectUnitSignals(UnitBase* unit, const QString& id);
    void updateMessageCache(const QJsonObject& msg);
    void onTickInternal(bool manual, double manualDt);
    void tickUnits(double dt);
    void applyEcmJamming();
    void scanReconDetections(double dt);
    void broadcastPositionReports(bool manual);
    void refreshDetectionCache();
    void applySchedules(double simTime, double dt);
    void markUnitsDirty();

    std::unique_ptr<LocalTransport> m_ownedTransport;
    ITransport* m_transport = nullptr;
    std::unique_ptr<MapProvider> m_map;
    std::unique_ptr<IClock> m_clock;
    std::unique_ptr<MessageLogRecorder> m_recorder;
    std::unordered_map<QString, std::unique_ptr<UnitBase>> m_units;
    Scenario m_scenario;
    /// Index into m_scenario.units by unit id — avoids O(N) std::find_if on
    /// every schedule update / lookup.
    std::unordered_map<QString, size_t> m_scenarioIndex;
    QTimer m_timer;
    QTimer m_dirtyTimer;
    bool m_running = false;
    QVariantList m_messageCache;
    bool m_readyForSim = true;
    QString m_cpIssues;
    QString m_lastError;
    bool m_unitsDirty = false;
    bool m_inTick = false;
    bool m_outcomeReported = false;
    QSet<QString> m_destroyedReported;
    double m_scanAccum = 0.0;
    int m_reportCounter = 0;
    std::unordered_map<QString, QJsonArray> m_cachedDetections;
    std::unordered_map<QString, std::function<void(const QVariantMap&)>> m_dispatch;

    void cmdAssignTarget(const QVariantMap& args);
    void cmdSetFlightPlan(const QVariantMap& args);
    void cmdEngageTarget(const QVariantMap& args);
    void cmdMoveTo(const QVariantMap& args);
    void cmdWithdraw(const QVariantMap& args);
    void cmdSetSpeed(const QVariantMap& args);
    void cmdPursue(const QVariantMap& args);
    void cmdGuideAttack(const QVariantMap& args);
    void cmdSetSchedule(const QVariantMap& args);
    void cmdHalt(const QVariantMap& args);
    void checkWinLoseCondition();
    void initCommandDispatch();
    CommandResult validateCommand(const QString& action, const QVariantMap& args) const;
};

} // namespace gbr
