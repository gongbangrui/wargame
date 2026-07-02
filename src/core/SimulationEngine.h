#pragma once

#include "Geo.h"
#include "MapProvider.h"
#include "MessageBus.h"
#include "Scenario.h"

#include <QObject>
#include <QTimer>
#include <QJsonObject>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gbr {

class UnitBase;

class SimulationEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(double simTime READ simTime NOTIFY simTimeChanged)
    Q_PROPERTY(double speedMul READ speedMul WRITE setSpeedMul NOTIFY speedMulChanged)
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(QJsonObject mapInfo READ mapInfo NOTIFY mapChanged)
    Q_PROPERTY(QVariantList units READ unitsForView NOTIFY unitsChanged)
    Q_PROPERTY(QVariantList messages READ recentMessages NOTIFY messagesChanged)
public:
    explicit SimulationEngine(QObject* parent = nullptr);
    ~SimulationEngine();

    void setScenario(const Scenario& s);
    void loadDefaultScenario();

    void setRunning(bool r);
    bool running() const { return m_running; }
    void setSpeedMul(double m);
    double speedMul() const { return m_speedMul; }
    Q_INVOKABLE void stepOnce(double simSeconds = 1.0);

    double simTime() const { return m_simTime; }

    MessageBus* bus() const { return m_bus.get(); }
    MapProvider* map() const { return m_map.get(); }

    Q_INVOKABLE QJsonObject unitSnapshot(const QString& id) const;
    Q_INVOKABLE void command(const QString& action, const QVariantMap& args);

    void addOrUpdateUnit(const ScenarioUnit& u);
    void removeUnit(const QString& id);
    QStringList unitIds() const;
    Scenario scenario() const { return m_scenario; }
    void persistScenario(const QString& path);

    UnitBase* unit(const QString& id) const;

    QJsonArray collectPerceptionSnapshot(const QString& forSide) const;
    QJsonArray collectAllUnitsSnapshot() const;

    QVariantList unitsForView() const;
    QVariantList recentMessages() const { return m_messageCache; }
    QJsonObject mapInfo() const { return m_map->describe(); }

signals:
    void simTimeChanged();
    void speedMulChanged();
    void runningChanged();
    void unitsChanged();
    void messagesChanged();
    void mapChanged();
    void perceptionUpdated(const QString& unitId, const QJsonObject& perception);
    void eventPosted(const QString& title, const QString& body, const QString& level);

private slots:
    void onMessagePosted(const QJsonObject& msg);

private:
    void rebuildUnitsFromScenario();
    void updateMessageCache(const QJsonObject& msg);
    void onTickInternal(bool manual, double manualDt);
    void applySchedules(double simTime);

    std::unique_ptr<MessageBus> m_bus;
    std::unique_ptr<MapProvider> m_map;
    std::unordered_map<QString, std::unique_ptr<UnitBase>> m_units;
    Scenario m_scenario;
    QTimer m_timer;
    double m_simTime = 0.0;
    double m_speedMul = 1.0;
    bool m_running = false;
    QVariantList m_messageCache;
};

} // namespace gbr
