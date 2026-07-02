#include "SimulationController.h"
#include "../core/UnitBase.h"

#include <QJsonArray>
#include <QDateTime>

namespace gbr {

SimulationController::SimulationController(QObject* parent) : QObject(parent) {
    m_engine.loadDefaultScenario();
    m_focusedSide = "red";
    setViewMode("commandpost-red");

    connect(&m_engine, &SimulationEngine::simTimeChanged, this, &SimulationController::simTimeForward);
    connect(&m_engine, &SimulationEngine::runningChanged, this, &SimulationController::runningForward);
    connect(&m_engine, &SimulationEngine::unitsChanged, this, &SimulationController::unitsForward);
    connect(&m_engine, &SimulationEngine::messagesChanged, this, &SimulationController::messagesForward);
    connect(&m_engine, &SimulationEngine::mapChanged, this, &SimulationController::mapInfoForward);
}

void SimulationController::setViewMode(const QString& m) {
    if (m == m_viewMode) return;
    m_viewMode = m;
    ensureFocusedConsistent();
    emit viewModeChanged();
}

void SimulationController::setFocusedSide(const QString& s) {
    if (m_focusedSide == s) return;
    m_focusedSide = s;
    ensureFocusedConsistent();
    emit focusedSideChanged();
}

void SimulationController::setFocusedUnitId(const QString& id) {
    if (m_focusedUnitId == id) return;
    m_focusedUnitId = id;
    emit focusedUnitIdChanged();
}

QString SimulationController::focusedKind() const {
    auto snap = m_engine.unitSnapshot(m_focusedUnitId);
    return snap.value("kind").toString();
}

void SimulationController::loadDefault() {
    m_engine.loadDefaultScenario();
    ensureFocusedConsistent();
}

void SimulationController::saveScenario(const QString& path) {
    m_engine.persistScenario(path);
}

void SimulationController::loadScenario(const QString& path) {
    QString err;
    auto s = ScenarioIo::loadFromFile(path, &err);
    if (err.isEmpty()) m_engine.setScenario(s);
    ensureFocusedConsistent();
}

void SimulationController::setRunning(bool r) { m_engine.setRunning(r); }
void SimulationController::setSpeed(double s) { m_engine.setSpeedMul(s); }
void SimulationController::stepOnce() { m_engine.stepOnce(1.0); }

void SimulationController::command(const QString& action, const QVariantMap& args) {
    m_engine.command(action, args);
    emit commandExecuted(action, args);
}

QJsonObject SimulationController::unitsJson() const {
    QJsonObject root;
    QJsonArray arr;
    for (const auto& u : m_engine.scenario().units) {
        QJsonObject o;
        o["id"] = u.id;
        o["callsign"] = u.callsign;
        o["kind"] = u.kind;
        o["side"] = u.side;
        o["x"] = u.pos.x; o["y"] = u.pos.y; o["alt"] = u.pos.alt;
        o["detectRange"] = u.detectRange;
        o["attackRange"] = u.attackRange;
        o["commRange"] = u.commRange;
        o["speed"] = u.speed;
        o["maxHp"] = u.maxHp;
        QJsonArray sched;
        for (const auto& sp : u.schedule) {
            QJsonObject p;
            p["time"] = sp.time;
            p["x"] = sp.x;
            p["y"] = sp.y;
            sched.append(p);
        }
        o["schedule"] = sched;
        arr.append(o);
    }
    root["units"] = arr;
    root["map"] = m_engine.scenario().map.name;
    return root;
}

void SimulationController::upsertUnit(const QVariantMap& data) {
    ScenarioUnit u;
    u.id = data.value("id").toString();
    if (u.id.isEmpty()) u.id = QString("u_%1").arg(QDateTime::currentMSecsSinceEpoch());
    u.callsign = data.value("callsign").toString();
    u.kind = data.value("kind").toString();
    if (u.kind.isEmpty()) u.kind = "commandpost";
    u.side = data.value("side").toString();
    if (u.side.isEmpty()) u.side = "red";
    u.pos.x = data.value("x").toDouble();
    u.pos.y = data.value("y").toDouble();
    u.pos.alt = data.value("alt").toDouble();
    u.detectRange = data.value("detectRange").toDouble();
    u.attackRange = data.value("attackRange").toDouble();
    u.commRange = data.value("commRange").toDouble();
    u.speed = data.value("speed").toDouble();
    u.maxHp = data.value("maxHp").toDouble();
    for (const auto& v : data.value("schedule").toList()) {
        const auto m = v.toMap();
        SchedulePoint sp;
        sp.time = m.value("time").toDouble();
        sp.x = m.value("x").toDouble();
        sp.y = m.value("y").toDouble();
        u.schedule.push_back(sp);
    }
    std::sort(u.schedule.begin(), u.schedule.end(),
              [](const SchedulePoint& a, const SchedulePoint& b){ return a.time < b.time; });
    m_engine.addOrUpdateUnit(u);
}

void SimulationController::removeUnit(const QString& id) {
    m_engine.removeUnit(id);
    if (m_focusedUnitId == id) m_focusedUnitId.clear();
}

Q_INVOKABLE void SimulationController::setUnitSchedule(const QString& uid, const QVariantList& schedule) {
    auto* u = m_engine.unit(uid);
    if (!u) return;
    std::vector<SchedulePoint> sched;
    for (const auto& v : schedule) {
        const auto m = v.toMap();
        SchedulePoint p;
        p.time = m.value("time").toDouble();
        p.x = m.value("x").toDouble();
        p.y = m.value("y").toDouble();
        sched.push_back(p);
    }
    u->setSchedule(sched);
    emit unitsForward();
}

const ScenarioUnit& SimulationController::findScenarioUnit(const QString& id) const {
    for (const auto& u : m_engine.scenario().units) {
        if (u.id == id) return u;
    }
    static ScenarioUnit empty;
    return empty;
}

QJsonArray SimulationController::perceptionForSide(const QString& side) const {
    return m_engine.collectPerceptionSnapshot(side);
}

QJsonArray SimulationController::allUnits() const {
    return m_engine.collectAllUnitsSnapshot();
}

QJsonObject SimulationController::unitAt(const QString& id) const {
    return m_engine.unitSnapshot(id);
}

QVariantList SimulationController::unitOptions(const QString& kindFilter, const QString& sideFilter) const {
    QVariantList out;
    for (const auto& u : m_engine.scenario().units) {
        if (!kindFilter.isEmpty() && u.kind != kindFilter) continue;
        if (!sideFilter.isEmpty() && u.side != sideFilter) continue;
        QVariantMap m;
        m["id"] = u.id;
        m["callsign"] = u.callsign;
        m["kind"] = u.kind;
        m["side"] = u.side;
        m["movable"] = u.kind != "commandpost";
        out.append(m);
    }
    return out;
}

QStringList SimulationController::viewModeOptions() const {
    return {
        "editor",
        "commandpost-red",
        "commandpost-blue",
        "director"
    };
}

QString SimulationController::pickDefaultUnit(const QString& kind, const QString& side) const {
    for (const auto& u : m_engine.scenario().units) {
        if (u.side == side && u.kind == kind) return u.id;
    }
    return QString();
}

void SimulationController::ensureFocusedConsistent() {
    if (m_viewMode == "commandpost-red") m_focusedSide = "red";
    if (m_viewMode == "commandpost-blue") m_focusedSide = "blue";

    if (m_viewMode == "commandpost-red" || m_viewMode == "commandpost-blue") {
        QString id = pickDefaultUnit("commandpost", m_focusedSide);
        if (m_focusedUnitId != id) {
            m_focusedUnitId = id;
        }
    }
    emit focusedUnitIdChanged();
}

} // namespace gbr








