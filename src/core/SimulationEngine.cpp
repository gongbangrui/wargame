#include "SimulationEngine.h"
#include "UnitBase.h"
#include "../units/CommandPost.h"
#include "../units/ReconUAV.h"
#include "../units/AttackUAV.h"
#include "../units/GroundScout.h"

#include <QDateTime>
#include <QJsonArray>
#include <algorithm>
#include <QMutexLocker>
#include <cmath>

namespace gbr {

SimulationEngine::SimulationEngine(QObject* parent)
    : QObject(parent), m_bus(std::make_unique<MessageBus>()),
      m_map(std::make_unique<MapProvider>()) {
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, [this](){ onTickInternal(false, 0.0); });
    connect(m_bus.get(), &MessageBus::messagePosted, this, &SimulationEngine::onMessagePosted);
}

SimulationEngine::~SimulationEngine() {
    m_timer.stop();
}

void SimulationEngine::loadDefaultScenario() {
    setScenario(ScenarioIo::defaultScenario());
}

void SimulationEngine::setScenario(const Scenario& s) {
    m_scenario = s;
    m_map->setLogicalSizeMeters(s.map.widthMeters, s.map.heightMeters);
    m_map->setName(s.map.name);
    m_map->setZoom(1.0);
    m_simTime = 0.0;
    m_messageCache.clear();
    rebuildUnitsFromScenario();
    emit mapChanged();
    emit unitsChanged();
    emit messagesChanged();
    emit simTimeChanged();
}

void SimulationEngine::rebuildUnitsFromScenario() {
    m_units.clear();
    for (const auto& u : m_scenario.units) {
        UnitBase::Params p;
        p.detectRange = u.detectRange;
        p.attackRange = u.attackRange;
        p.commRange = u.commRange;
        p.speed = u.speed;
        p.maxHp = u.maxHp;
        p.pos = u.pos;
        auto unit = UnitBase::create(u.id, kindFromName(u.kind), sideFromName(u.side), m_bus.get(), this);
        unit->setCallsign(u.callsign);
        unit->setParams(p);
        unit->setSchedule(u.schedule);
        connect(unit.get(), &UnitBase::notifyEvent, this, [this](const QString& t, const QString& b, const QString& lvl){
            emit eventPosted(t, b, lvl);
        });
        connect(unit.get(), &UnitBase::perceptionChanged, this, [this, id=u.id]() {
            emit perceptionUpdated(id, unitSnapshot(id));
        });
        m_units.emplace(u.id, std::move(unit));
    }
}

void SimulationEngine::setRunning(bool r) {
    if (m_running == r) return;
    m_running = r;
    if (m_running) m_timer.start();
    else m_timer.stop();
    emit runningChanged();
}

void SimulationEngine::setSpeedMul(double m) {
    m_speedMul = std::max(0.0, m);
    emit speedMulChanged();
}

void SimulationEngine::stepOnce(double simSeconds) {
    if (m_running) return;
    onTickInternal(true, simSeconds);
}

void SimulationEngine::onTickInternal(bool manual, double manualDt) {
    const double dt = manual ? manualDt : 0.05 * m_speedMul;
    if (!manual && dt <= 0.0) return;
    m_simTime += dt;

    std::vector<std::pair<QString, GeoPos>> all;
    for (auto& [id, u] : m_units) all.emplace_back(id, u->pos());
    for (auto& [id, u] : m_units) {
        if (auto* a = dynamic_cast<AttackUAV*>(u.get())) {
            a->setOtherPositions(all);
        }
    }

    applySchedules(m_simTime);

    for (auto& [id, u] : m_units) u->onTick(dt);

    static double scanAccum = 0.0;
    scanAccum += dt;
    const double scanInterval = 1.5;
    if (scanAccum >= scanInterval) {
        scanAccum = 0.0;
        for (auto& [reconId, recon] : m_units) {
            if (recon->kind() != UnitKind::ReconUAV) continue;
            if (!recon->alive()) continue;
            const auto center = recon->pos();
            const double dr = recon->detectRange();
            for (auto& [tid, target] : m_units) {
                if (target->side() == recon->side()) continue;
                if (!target->alive()) continue;
                const double d = center.distanceTo(target->pos());
                if (d <= dr) {
                    Message dm;
                    dm.type = Message::Type::TargetDetect;
                    dm.sender = reconId;
                    dm.receiver = recon->sideStr() == "red" ? "red_cp" : "blue_cp";
                    dm.requiresAck = true;
                    dm.payload["targetId"] = tid;
                    dm.payload["callsign"] = target->callsign();
                    dm.payload["x"] = target->pos().x;
                    dm.payload["y"] = target->pos().y;
                    dm.payload["alt"] = target->pos().alt;
                    dm.payload["distance"] = d;
                    m_bus->send(dm);
                }
            }
        }
    }

    static int counter = 0;
    counter++;
    if (counter % 4 == 0 || manual) {
        for (auto& [id, u] : m_units) {
            Message m;
            m.type = Message::Type::PositionReport;
            m.sender = id;
            m.receiver = "*";
            m.payload["x"] = u->pos().x;
            m.payload["y"] = u->pos().y;
            m.payload["alt"] = u->pos().alt;
            m.payload["side"] = u->sideStr();
            m_bus->send(m);
        }
    }

    emit simTimeChanged();
    emit unitsChanged();
}

void SimulationEngine::applySchedules(double simTime) {
    for (auto& [id, u] : m_units) {
        if (!u->movable()) continue;
        if (!u->alive()) continue;
        if (u->hasActiveWaypoints()) continue; // skip units receiving direct commands
        const auto& sched = u->schedule();
        if (sched.empty()) continue;

        if (simTime <= sched.front().time) {
            const auto& p = sched.front();
            if (std::abs(u->pos().x - p.x) > 0.1 || std::abs(u->pos().y - p.y) > 0.1) {
                u->setPosition(GeoPos{p.x, p.y, u->pos().alt});
            }
            continue;
        }
        if (simTime >= sched.back().time) {
            const auto& p = sched.back();
            if (std::abs(u->pos().x - p.x) > 0.1 || std::abs(u->pos().y - p.y) > 0.1) {
                u->setPosition(GeoPos{p.x, p.y, u->pos().alt});
            }
            continue;
        }

        for (size_t i = 0; i + 1 < sched.size(); i++) {
            const auto& a = sched[i];
            const auto& b = sched[i + 1];
            if (simTime >= a.time && simTime <= b.time) {
                const double span = b.time - a.time;
                const double t = span > 1e-6 ? (simTime - a.time) / span : 1.0;
                const double nx = a.x + (b.x - a.x) * t;
                const double ny = a.y + (b.y - a.y) * t;
                u->setPosition(GeoPos{nx, ny, u->pos().alt});
                break;
            }
        }
    }
}

void SimulationEngine::onMessagePosted(const QJsonObject& msg) {
    updateMessageCache(msg);
    emit messagesChanged();

    // 当目标被摧毁时，将目标 HP 置零并通知被摧毁方
    if (msg.value("type").toString() == "TargetDestroyed") {
        const QString targetId = msg.value("payload").toObject().value("targetId").toString();
        const QString attackerId = msg.value("payload").toObject().value("attackerId").toString();
        auto it = m_units.find(targetId);
        if (it != m_units.end() && it->second->alive()) {
            it->second->setHp(0.0);  // need to add setHp method
            QString targetSide = it->second->sideStr();
            QString sideLabel = (targetSide == "red") ? QString::fromUtf8("红方") : QString::fromUtf8("蓝方");
            emit eventPosted(
                QString::fromUtf8("单元被摧毁"),
                QString::fromUtf8("%1单元 %2 被 %3 摧毁").arg(sideLabel, targetId, attackerId),
                "warn"
            );
        }
    }
}

void SimulationEngine::updateMessageCache(const QJsonObject& msg) {
    m_messageCache.prepend(msg);
    if (m_messageCache.size() > 200) m_messageCache.removeLast();
}

QJsonObject SimulationEngine::unitSnapshot(const QString& id) const {
    auto it = m_units.find(id);
    if (it == m_units.end()) return {};
    auto& u = it->second;
    QJsonObject o;
    o["id"] = u->id();
    o["callsign"] = u->callsign();
    o["kind"] = u->kindStr();
    o["side"] = u->sideStr();
    o["movable"] = u->movable();
    QJsonArray pos;
    pos.append(u->pos().x);
    pos.append(u->pos().y);
    pos.append(u->pos().alt);
    o["position"] = pos;
    o["detectRange"] = u->detectRange();
    o["attackRange"] = u->attackRange();
    o["commRange"] = u->commRange();
    o["speed"] = u->speed();
    o["maxHp"] = u->maxHp();
    o["hp"] = u->hp();
    o["alive"] = u->alive();
    o["status"] = u->statusText();
    o["sharedKnowledge"] = u->sharedKnowledgeJson();
    QJsonArray dets;
    const auto center = u->pos();
    for (const auto& [oid, ou] : m_units) {
        if (oid == id) continue;
        if (ou->side() == u->side()) continue;
        const double d = center.distanceTo(ou->pos());
        if (d <= u->detectRange()) {
            QJsonObject det;
            det["id"] = oid;
            det["callsign"] = ou->callsign();
            det["kind"] = ou->kindStr();
            det["side"] = ou->sideStr();
            det["distance"] = d;
            QJsonArray p;
            p.append(ou->pos().x);
            p.append(ou->pos().y);
            p.append(ou->pos().alt);
            det["position"] = p;
            dets.append(det);
        }
    }
    o["detections"] = dets;
    QJsonArray sched;
    for (const auto& sp : u->schedule()) {
        QJsonObject p;
        p["time"] = sp.time;
        p["x"] = sp.x;
        p["y"] = sp.y;
        sched.append(p);
    }
    o["schedule"] = sched;
    return o;
}

void SimulationEngine::command(const QString& action, const QVariantMap& args) {
    if (action == "assignTarget") {
        const auto attackerId = args.value("attackerId").toString();
        const auto targetId = args.value("targetId").toString();
        auto* u = unit(attackerId);
        if (!u) return;
        Message m;
        m.type = Message::Type::AttackOrder;
        m.sender = u->sideStr() == "red" ? "red_cp" : "blue_cp";
        m.receiver = attackerId;
        m.requiresAck = true;
        m.payload["targetId"] = targetId;
        m_bus->send(m);
    } else if (action == "setFlightPlan") {
        const auto attackerId = args.value("attackerId").toString();
        auto* u = unit(attackerId);
        if (!u) return;
        Message m;
        m.type = Message::Type::FlightPlan;
        m.sender = u->sideStr() == "red" ? "red_cp" : "blue_cp";
        m.receiver = attackerId;
        QJsonArray wp;
        for (const auto& v : args.value("waypoints").toList()) {
            const auto p = v.toPointF();
            wp.append(QJsonObject{{"x", p.x()}, {"y", p.y()}, {"alt", 2000.0}});
        }
        m.payload["waypoints"] = wp;
        m_bus->send(m);
    } else if (action == "engageTarget") {
        const auto attackerId = args.value("attackerId").toString();
        auto* u = unit(attackerId);
        if (!u) return;
        Message m;
        m.type = Message::Type::AttackOrder;
        m.sender = u->sideStr() == "red" ? "red_cp" : "blue_cp";
        m.receiver = attackerId;
        m.payload["fireNow"] = true;
        m.payload["targetId"] = args.value("targetId").toString();
        m_bus->send(m);
    } else if (action == "moveTo") {
        const auto uid = args.value("unitId").toString();
        const auto p = args.value("pos").toPointF();
        auto* u = unit(uid);
        if (!u) return;
        Message m;
        m.type = Message::Type::Guidance;
        m.sender = u->sideStr() == "red" ? "red_cp" : "blue_cp";
        m.receiver = uid;
        m.payload["x"] = p.x();
        m.payload["y"] = p.y();
        m.payload["kind"] = QString("moveTo");
        m_bus->send(m);
    } else if (action == "withdraw") {
        const auto uid = args.value("unitId").toString();
        auto* u = unit(uid);
        if (!u) return;
        Message m;
        m.type = Message::Type::Withdraw;
        m.sender = u->sideStr() == "red" ? "red_cp" : "blue_cp";
        m.receiver = uid;
        m_bus->send(m);
    } else if (action == "guideAttack") {
        const auto guideId = args.value("guideId").toString();
        const auto attackerId = args.value("attackerId").toString();
        const auto targetId = args.value("targetId").toString();
        const auto p = args.value("targetPos").toPointF();
        auto* u = unit(guideId);
        if (!u) return;
        Message m;
        m.type = Message::Type::FlightPlan;
        m.sender = guideId;
        m.receiver = attackerId;
        QJsonArray wp;
        QJsonObject w0; w0["x"] = p.x(); w0["y"] = p.y(); w0["alt"] = 2000.0;
        wp.append(w0);
        m.payload["waypoints"] = wp;
        m.payload["targetId"] = targetId;
        m_bus->send(m);
    } else if (action == "setSchedule") {
        const auto uid = args.value("unitId").toString();
        auto* u = unit(uid);
        if (!u) return;
        std::vector<SchedulePoint> sched;
        for (const auto& v : args.value("schedule").toList()) {
            const auto m = v.toMap();
            SchedulePoint p;
            p.time = m.value("time").toDouble();
            p.x = m.value("x").toDouble();
            p.y = m.value("y").toDouble();
            sched.push_back(p);
        }
        u->setSchedule(sched);
    }
}

void SimulationEngine::addOrUpdateUnit(const ScenarioUnit& u) {
    auto it = std::find_if(m_scenario.units.begin(), m_scenario.units.end(),
                           [&](const ScenarioUnit& su){ return su.id == u.id; });
    if (it != m_scenario.units.end()) *it = u;
    else m_scenario.units.push_back(u);
    rebuildUnitsFromScenario();
    emit unitsChanged();
}

void SimulationEngine::removeUnit(const QString& id) {
    auto it = std::find_if(m_scenario.units.begin(), m_scenario.units.end(),
                           [&](const ScenarioUnit& su){ return su.id == id; });
    if (it != m_scenario.units.end()) m_scenario.units.erase(it);
    m_units.erase(id);
    rebuildUnitsFromScenario();
    emit unitsChanged();
}

QStringList SimulationEngine::unitIds() const {
    QStringList r;
    for (auto& [id, _] : m_units) r << id;
    return r;
}

void SimulationEngine::persistScenario(const QString& path) {
    QString err;
    ScenarioIo::saveToFile(m_scenario, path, &err);
}

UnitBase* SimulationEngine::unit(const QString& id) const {
    auto it = m_units.find(id);
    if (it == m_units.end()) return nullptr;
    return it->second.get();
}

QJsonArray SimulationEngine::collectPerceptionSnapshot(const QString& forSide) const {
    QJsonArray arr;
    for (const auto& [id, u] : m_units) {
        if (!forSide.isEmpty() && u->sideStr() != forSide) continue;
        arr.append(unitSnapshot(id));
    }
    return arr;
}

QJsonArray SimulationEngine::collectAllUnitsSnapshot() const {
    QJsonArray arr;
    for (const auto& [id, u] : m_units) arr.append(unitSnapshot(id));
    return arr;
}

QVariantList SimulationEngine::unitsForView() const {
    QVariantList l;
    for (const auto& [id, u] : m_units) {
        QVariantMap m;
        m["id"] = id;
        m["callsign"] = u->callsign();
        m["kind"] = u->kindStr();
        m["side"] = u->sideStr();
        m["hp"] = u->hp();
        m["alive"] = u->alive();
        m["movable"] = u->movable();
        QVariantList p; p << u->pos().x << u->pos().y << u->pos().alt;
        m["position"] = p;
        m["detectRange"] = u->detectRange();
        m["attackRange"] = u->attackRange();
        m["commRange"] = u->commRange();
        l.append(m);
    }
    return l;
}

} // namespace gbr








