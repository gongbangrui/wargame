#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include "../core/SimulationEngine.h"

namespace gbr {

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
    QVariantList units() const { return m_engine.unitsForView(); }
    QVariantList messages() const { return m_engine.recentMessages(); }
    QJsonObject mapInfo() const { return m_engine.mapInfo(); }

    Q_INVOKABLE void loadDefault();
    Q_INVOKABLE void saveScenario(const QString& path);
    Q_INVOKABLE void loadScenario(const QString& path);

    Q_INVOKABLE void setRunning(bool r);
    Q_INVOKABLE void setSpeed(double s);
    Q_INVOKABLE void stepOnce();

    Q_INVOKABLE void command(const QString& action, const QVariantMap& args);

    Q_INVOKABLE QJsonObject unitsJson() const;
    Q_INVOKABLE void upsertUnit(const QVariantMap& data);
    Q_INVOKABLE void removeUnit(const QString& id);
    Q_INVOKABLE void setUnitSchedule(const QString& uid, const QVariantList& schedule);

    Q_INVOKABLE QJsonArray perceptionForSide(const QString& side) const;
    Q_INVOKABLE QJsonArray allUnits() const;
    Q_INVOKABLE QJsonObject unitAt(const QString& id) const;
    Q_INVOKABLE QVariantList unitOptions(const QString& kindFilter, const QString& sideFilter) const;
    Q_INVOKABLE QStringList viewModeOptions() const;

signals:
    void viewModeChanged();
    void focusedSideChanged();
    void focusedUnitIdChanged();
    void simTimeForward();
    void runningForward();
    void unitsForward();
    void messagesForward();
    void mapInfoForward();
    void commandExecuted(const QString& action, const QVariantMap& args);

private:
    QString pickDefaultUnit(const QString& kind, const QString& side) const;
    const ScenarioUnit& findScenarioUnit(const QString& id) const;
    void ensureFocusedConsistent();

    SimulationEngine m_engine;
    QString m_viewMode = "editor";
    QString m_focusedSide = "red";
    QString m_focusedUnitId;
};

} // namespace gbr
