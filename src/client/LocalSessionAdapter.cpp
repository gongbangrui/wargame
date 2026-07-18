#include "LocalSessionAdapter.h"

#include "core/Scenario.h"
#include "core/SimulationEngine.h"

namespace gbr {

LocalSessionAdapter::LocalSessionAdapter(SimulationEngine* engine, QObject* parent)
    : ISessionAdapter(parent), m_engine(engine) {
    Q_ASSERT(m_engine);
    connect(m_engine, &SimulationEngine::simTimeChanged, this, &ISessionAdapter::simTimeChanged);
    connect(m_engine, &SimulationEngine::runningChanged, this, &ISessionAdapter::runningChanged);
    connect(m_engine, &SimulationEngine::unitsChanged, this, &ISessionAdapter::unitsChanged);
    connect(m_engine, &SimulationEngine::messagesChanged, this, &ISessionAdapter::messagesChanged);
    connect(m_engine, &SimulationEngine::mapChanged, this, &ISessionAdapter::mapChanged);
    connect(m_engine, &SimulationEngine::readyForSimChanged, this, &ISessionAdapter::readyForSimChanged);
    connect(m_engine, &SimulationEngine::errorOccurred, this, &ISessionAdapter::errorOccurred);
    connect(m_engine, &SimulationEngine::eventPosted, this, &ISessionAdapter::eventPosted);
    connect(m_engine, &SimulationEngine::targetDestroyedVisual, this, &ISessionAdapter::targetDestroyedVisual);
    connect(m_engine, &SimulationEngine::unitDestroyed, this, &ISessionAdapter::unitDestroyed);
    connect(m_engine, &SimulationEngine::simulationEnded, this, &ISessionAdapter::simulationEnded);
}

double LocalSessionAdapter::simTime() const { return m_engine->simTime(); }
bool LocalSessionAdapter::running() const { return m_engine->running(); }
bool LocalSessionAdapter::readyForSim() const { return m_engine->readyForSim(); }
QString LocalSessionAdapter::cpIssues() const { return m_engine->cpIssues(); }
QString LocalSessionAdapter::lastError() const { return m_engine->lastError(); }
QVariantList LocalSessionAdapter::units() const { return m_engine->unitsForView(); }
QVariantList LocalSessionAdapter::messages() const { return m_engine->recentMessages(); }
QVariantMap LocalSessionAdapter::lobby() const {
    return {{QStringLiteral("preparation"), !m_engine->running() && m_engine->simTime() == 0.0},
            {QStringLiteral("redReady"), false},
            {QStringLiteral("blueReady"), false},
            {QStringLiteral("bothReady"), m_engine->readyForSim()}};
}
QJsonObject LocalSessionAdapter::mapInfo() const { return m_engine->mapInfo(); }
QJsonArray LocalSessionAdapter::allUnits() const { return m_engine->collectAllUnitsSnapshot(); }
QJsonObject LocalSessionAdapter::unitAt(const QString& id) const { return m_engine->unitSnapshot(id); }
QJsonObject LocalSessionAdapter::scenarioJson() const { return ScenarioIo::toJson(m_engine->scenario()); }
QVariantMap LocalSessionAdapter::command(const QString& action, const QVariantMap& args) {
    return m_engine->command(action, args).toVariantMap();
}
void LocalSessionAdapter::setRunning(bool running) { m_engine->setRunning(running); }
void LocalSessionAdapter::setSpeed(double speed) { m_engine->setSpeedMul(speed); }
void LocalSessionAdapter::stepOnce() { m_engine->stepOnce(1.0); }

} // namespace gbr
