#include "SimulationController.h"
#include "../core/UnitBase.h"
#include "../core/SnapshotCodec.h"

#include <QJsonArray>
#include <QDateTime>
#include <QSet>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QFile>
#include <QDir>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace gbr {

namespace {

ScenarioUnit scenarioUnitFromVariantMap(const QVariantMap& data, bool generateId) {
    ScenarioUnit unit;
    unit.id = data.value(QStringLiteral("id")).toString().trimmed();
    if (generateId && unit.id.isEmpty()) {
        unit.id = QStringLiteral("u_%1").arg(QDateTime::currentMSecsSinceEpoch());
    }
    unit.callsign = data.value(QStringLiteral("callsign")).toString();
    unit.kind = data.value(QStringLiteral("kind"), QStringLiteral("commandpost")).toString();
    unit.side = data.value(QStringLiteral("side"), QStringLiteral("red")).toString();
    if (data.contains(QStringLiteral("x"))) unit.pos.x = data.value(QStringLiteral("x")).toDouble();
    if (data.contains(QStringLiteral("y"))) unit.pos.y = data.value(QStringLiteral("y")).toDouble();
    if (data.contains(QStringLiteral("alt"))) unit.pos.alt = data.value(QStringLiteral("alt")).toDouble();
    if (data.contains(QStringLiteral("detectRange"))) unit.detectRange = data.value(QStringLiteral("detectRange")).toDouble();
    if (data.contains(QStringLiteral("attackRange"))) unit.attackRange = data.value(QStringLiteral("attackRange")).toDouble();
    if (data.contains(QStringLiteral("commRange"))) unit.commRange = data.value(QStringLiteral("commRange")).toDouble();
    if (data.contains(QStringLiteral("speed"))) unit.speed = data.value(QStringLiteral("speed")).toDouble();
    if (data.contains(QStringLiteral("maxHp"))) unit.maxHp = data.value(QStringLiteral("maxHp")).toDouble();
    if (data.contains(QStringLiteral("attackPower"))) unit.attackPower = data.value(QStringLiteral("attackPower")).toDouble();
    for (const auto& value : data.value(QStringLiteral("schedule")).toList()) {
        const auto pointMap = value.toMap();
        SchedulePoint point;
        point.time = pointMap.value(QStringLiteral("time")).toDouble();
        point.x = pointMap.value(QStringLiteral("x")).toDouble();
        point.y = pointMap.value(QStringLiteral("y")).toDouble();
        unit.schedule.push_back(point);
    }
    std::sort(unit.schedule.begin(), unit.schedule.end(),
              [](const SchedulePoint& a, const SchedulePoint& b) {
                  return a.time < b.time;
              });
    return unit;
}

} // namespace

SimulationController::SimulationController(QObject* parent) : QObject(parent) {
    m_engine.loadDefaultScenario();
    m_focusedSide = "red";
    const QVariantList storedServers = loadSetting(QStringLiteral("network/serverHistory"), QVariantList{}).toList();
    for (const QVariant& value : storedServers) {
        const QString server = value.toString().trimmed();
        if (!server.isEmpty() && !m_serverHistory.contains(server)) m_serverHistory.append(server);
    }
    setViewMode("commandpost-red");

    connect(&m_engine, &SimulationEngine::simTimeChanged, this, &SimulationController::simTimeForward);
    connect(&m_engine, &SimulationEngine::runningChanged, this, &SimulationController::runningForward);
    connect(&m_engine, &SimulationEngine::unitsChanged, this, [this]() {
        invalidateCaches();
        emit unitsForward();
    });
    connect(&m_engine, &SimulationEngine::messagesChanged, this, &SimulationController::messagesForward);
    connect(&m_engine, &SimulationEngine::mapChanged, this, &SimulationController::mapInfoForward);
    connect(&m_engine, &SimulationEngine::readyForSimChanged, this, &SimulationController::readyForSimForward);
    connect(&m_engine, &SimulationEngine::targetDestroyedVisual, this, &SimulationController::targetDestroyedVisual);
    connect(&m_engine, &SimulationEngine::eventPosted, this,
            [this](const QString& title, const QString& body, const QString& level,
                   const QString& sourceUnitId) {
                if (!isNetworked()) emit eventForward(title, body, level, sourceUnitId);
            });
    connect(&m_engine, &SimulationEngine::unitDestroyed, this, &SimulationController::onUnitDestroyed);
    connect(&m_engine, &SimulationEngine::errorOccurred, this, [this](const QString& message) {
        if (!isNetworked()) emit errorForward(message);
    });
    connect(&m_engine, &SimulationEngine::simulationEnded, this,
            [this](const QString& winner, const QString& loser) {
                if (!isNetworked()) emit simEndForward(winner, loser);
            });

    connect(&m_networkClient, &NetworkClient::stateChanged, this,
            [this](const QString& state, const QString& message) {
                m_networkState = state;
                m_networkStatus = message;
                emit networkStatusChanged();
            });
    connect(&m_networkClient, &NetworkClient::diagnosticsChanged, this,
            [this](const QString& state, const QString& message, int accountLatency, int gameLatency) {
                m_networkDiagnosticState = state;
                m_networkDiagnosticMessage = message;
                m_accountLatencyMs = accountLatency;
                m_gameLatencyMs = gameLatency;
                emit networkDiagnosticsChanged();
            });
    connect(&m_networkClient, &NetworkClient::authenticated, this,
            [this](const QString& username, const QString& displayName,
                   const QString& role, const QString& server) {
                const bool wasNetworked = isNetworked();
                m_username = username;
                m_displayName = displayName;
                m_userRole = role;
                m_serverAddress = server;
                rememberServerAddress(server);
                m_remoteScenarioRevision = -1;
                m_sessionMode = QStringLiteral("online");
                applyRoleView();
                emit sessionChanged();
                if (!wasNetworked) emit networkedChanged();
            });
    connect(&m_networkClient, &NetworkClient::snapshotReceived,
            this, &SimulationController::applyRemoteSnapshot);
    connect(&m_networkClient, &NetworkClient::chatHistoryReceived, this,
            [this](const QJsonArray& messages) {
                m_chatMessages = messages.toVariantList();
                emit chatMessagesChanged();
            });
    connect(&m_networkClient, &NetworkClient::chatReceived, this,
            [this](const QJsonObject& message) {
                m_chatMessages.append(message.toVariantMap());
                while (m_chatMessages.size() > 100) m_chatMessages.removeFirst();
                emit chatMessagesChanged();
            });
    connect(&m_networkClient, &NetworkClient::eventReceived, this,
            [this](const QJsonObject& event) {
        const QString kind = event.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("simulationEnded")) {
            m_matchPhase = QStringLiteral("finished");
            emit roomStateChanged();
            emit simEndForward(event.value(QStringLiteral("winner")).toString(),
                               event.value(QStringLiteral("loser")).toString());
        } else if (kind == QLatin1String("matchStarted")) {
            m_matchPhase = QStringLiteral("running");
            emit roomStateChanged();
            emit eventForward(QStringLiteral("联网推演"),
                              event.value(QStringLiteral("message")).toString(),
                              QStringLiteral("info"), QString());
        } else if (kind == QLatin1String("matchReset")) {
            // 重置事件先于完整快照抵达，先解除阵容编辑锁定以避免界面停留在旧阶段。
            m_matchPhase = QStringLiteral("preparing");
            m_redReady = false;
            m_blueReady = false;
            emit roomStateChanged();
            emit eventForward(QStringLiteral("联网推演"),
                              event.value(QStringLiteral("message")).toString(),
                              QStringLiteral("info"), QString());
        } else if (kind == QLatin1String("simulationEvent")) {
                    emit eventForward(event.value(QStringLiteral("title")).toString(),
                                      event.value(QStringLiteral("body")).toString(),
                                      event.value(QStringLiteral("level")).toString(),
                                      event.value(QStringLiteral("sourceUnitId")).toString());
                } else if (kind == QLatin1String("targetDestroyed")) {
                    emit targetDestroyedVisual(event.value(QStringLiteral("unitId")).toString(),
                                               event.value(QStringLiteral("x")).toDouble(),
                                               event.value(QStringLiteral("y")).toDouble());
                } else {
                    emit eventForward(QStringLiteral("联网推演"),
                                      event.value(QStringLiteral("message")).toString(),
                                      QStringLiteral("info"), QString());
                }
            });
    const auto reportNetworkError = [this](const QString& message) {
        m_remoteLastError = message;
        emit errorForward(message);
    };
    connect(&m_networkClient, &NetworkClient::fatalError, this, reportNetworkError);
    connect(&m_networkClient, &NetworkClient::commandRejected, this, reportNetworkError);
    connect(&m_networkClient, &NetworkClient::authenticationLost, this,
            [this](const QString& message) {
                logoutOnline();
                m_remoteLastError = message;
                emit errorForward(message);
            });
    connect(&m_networkClient, &NetworkClient::commandStatusChanged, this,
            [this](const QString& commandId, const QString&, const QString& status,
                   const QString&, const QString& message) {
                m_lastCommandId = commandId;
                m_lastCommandStatus = status;
                m_lastCommandMessage = message;
                emit commandStatusChanged();
            });
}

void SimulationController::onUnitDestroyed(const QString& unitId) {
    if (m_focusedUnitId == unitId) {
        m_focusedUnitId.clear();
        emit focusedUnitIdChanged();
        // For commandpost-red/blue view, re-pick the alive CP so UnitPanel
        // doesn't stay empty until the next setViewMode.
        ensureFocusedConsistent();
    }
}

void SimulationController::setViewMode(const QString& m) {
    if (!viewModeOptions().contains(m)) return;
    if (isNetworked()) {
        QString expected;
        if (m_userRole == QLatin1String("editor")) expected = QStringLiteral("editor");
        else if (m_userRole == QLatin1String("director")) expected = QStringLiteral("director");
        else if (m_userRole == QLatin1String("red")) expected = QStringLiteral("commandpost-red");
        else if (m_userRole == QLatin1String("blue")) expected = QStringLiteral("commandpost-blue");
        if (!expected.isEmpty() && m != expected) return;
    }
    if (m == m_viewMode) return;
    const QString previousSide = m_focusedSide;
    m_viewMode = m;
    ensureFocusedConsistent();
    if (previousSide != m_focusedSide) emit focusedSideChanged();
    emit viewModeChanged();
}

void SimulationController::setFocusedSide(const QString& s) {
    if (s != QLatin1String("red") && s != QLatin1String("blue")) return;
    if (isNetworked() && (m_userRole == QLatin1String("red") || m_userRole == QLatin1String("blue"))
        && s != m_userRole) return;
    if (m_focusedSide == s) return;
    const QString previousSide = m_focusedSide;
    m_focusedSide = s;
    ensureFocusedConsistent();
    if (previousSide != m_focusedSide) emit focusedSideChanged();
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
    if (isNetworked()) {
        if (m_userRole == QLatin1String("editor") && m_matchPhase == QLatin1String("preparing"))
            m_networkClient.sendScenarioReplace(ScenarioIo::toJson(ScenarioIo::defaultScenario()));
        return;
    }
    m_engine.loadDefaultScenario();
    ensureFocusedConsistent();
}

void SimulationController::saveScenario(const QString& path) {
    m_engine.persistScenario(path);
}

void SimulationController::loadScenario(const QString& path) {
    QString err;
    auto s = ScenarioIo::loadFromFile(path, &err);
    if (!err.isEmpty()) {
        emit errorForward(QStringLiteral("加载场景失败: %1").arg(err));
        return;
    }
    // Refuse to apply a scenario that has no command posts or zero units; this prevents
    // a silently-empty load from wiping the running world without diagnostic.
    int redCp = 0, blueCp = 0;
    for (const auto& u : s.units) {
        if (u.kind == QLatin1String("commandpost")) {
            if (u.side == QLatin1String("red")) ++redCp;
            else if (u.side == QLatin1String("blue")) ++blueCp;
        }
    }
    if (s.units.empty()) {
        emit errorForward(QStringLiteral("场景 '%1' 为空，已忽略").arg(path));
        return;
    }
    if (redCp != 1 || blueCp != 1) {
        emit errorForward(QStringLiteral("场景 '%1' 必须每方恰好 1 个指挥所（红=%2，蓝=%3），已忽略").arg(path).arg(redCp).arg(blueCp));
        return;
    }
    if (isNetworked()) {
        if (m_userRole == QLatin1String("editor") && m_matchPhase == QLatin1String("preparing"))
            m_networkClient.sendScenarioReplace(ScenarioIo::toJson(s));
        return;
    }
    if (m_engine.setScenario(s)) ensureFocusedConsistent();
}

void SimulationController::setRunning(bool r) {
    if (!isNetworked()) {
        m_engine.setRunning(r);
        return;
    }
    if (m_userRole != QLatin1String("director")) return;
    if (r) {
        m_networkClient.sendControl(m_matchPhase == QLatin1String("preparing")
                                        ? QStringLiteral("start") : QStringLiteral("resume"));
    } else {
        m_networkClient.sendControl(QStringLiteral("pause"));
    }
}

void SimulationController::setSpeed(double s) {
    if (isNetworked()) {
        if (m_userRole == QLatin1String("director"))
            m_networkClient.sendControl(QStringLiteral("speed"), s);
    } else {
        m_engine.setSpeedMul(s);
    }
}

void SimulationController::stepOnce() {
    if (isNetworked()) {
        if (m_userRole == QLatin1String("director"))
            m_networkClient.sendControl(QStringLiteral("step"));
    } else {
        m_engine.stepOnce(1.0);
    }
}

void SimulationController::command(const QString& action, const QVariantMap& args) {
    if (isNetworked()) m_networkClient.sendCommand(action, args);
    else m_engine.command(action, args);
    emit commandExecuted(action, args);
}

QJsonObject SimulationController::unitsJson() const {
    return ScenarioIo::toJson(m_engine.scenario());
}

void SimulationController::upsertUnit(const QVariantMap& data) {
    ScenarioUnit u = scenarioUnitFromVariantMap(data, true);
    if (isNetworked()) {
        m_networkClient.sendScenarioUpsert(scenarioUnitJson(u));
        return;
    }
    m_engine.addOrUpdateUnit(u);
    invalidateCaches();
}

bool SimulationController::replaceUnits(const QVariantList& units) {
    Scenario replacement = m_engine.scenario();
    replacement.units.clear();
    replacement.units.reserve(static_cast<size_t>(units.size()));
    for (const auto& value : units) {
        replacement.units.push_back(scenarioUnitFromVariantMap(value.toMap(), false));
    }
    if (isNetworked()) {
        m_networkClient.sendScenarioReplace(ScenarioIo::toJson(replacement));
        return true;
    }
    if (!m_engine.setScenario(replacement)) return false;
    invalidateCaches();
    ensureFocusedConsistent();
    return true;
}

bool SimulationController::replaceScenario(const QVariantMap& scenario) {
    const Scenario replacement = ScenarioIo::fromJson(QJsonObject::fromVariantMap(scenario));
    if (isNetworked()) {
        m_networkClient.sendScenarioReplace(ScenarioIo::toJson(replacement));
        return true;
    }
    if (!m_engine.setScenario(replacement)) return false;
    invalidateCaches();
    ensureFocusedConsistent();
    return true;
}

void SimulationController::removeUnit(const QString& id) {
    if (isNetworked()) {
        m_networkClient.sendScenarioRemove(id);
        return;
    }
    m_engine.removeUnit(id);
    invalidateCaches();
    if (m_focusedUnitId == id) {
        m_focusedUnitId.clear();
        emit focusedUnitIdChanged();
    }
}

Q_INVOKABLE void SimulationController::setUnitSchedule(const QString& uid, const QVariantList& schedule) {
    if (isNetworked()) {
        if (m_matchPhase == QLatin1String("running")) {
            m_networkClient.sendCommand(QStringLiteral("setSchedule"),
                                        QVariantMap{{QStringLiteral("unitId"), uid},
                                                    {QStringLiteral("schedule"), schedule}});
            return;
        }
        for (const auto& unit : m_engine.scenario().units) {
            if (unit.id != uid) continue;
            QVariantMap data = scenarioUnitJson(unit).toVariantMap();
            data[QStringLiteral("schedule")] = schedule;
            m_networkClient.sendScenarioUpsert(QJsonObject::fromVariantMap(data));
            return;
        }
        return;
    }
    m_engine.command(QStringLiteral("setSchedule"),
                     QVariantMap{{QStringLiteral("unitId"), uid},
                                 {QStringLiteral("schedule"), schedule}});
}

QJsonArray SimulationController::perceptionForSide(const QString& side) const {
    return m_engine.collectPerceptionSnapshot(side);
}

QJsonArray SimulationController::allUnits() const {
    if (!m_snapshotCacheValid) {
        m_snapshotCache = m_engine.collectAllUnitsSnapshot();
        m_snapshotCacheValid = true;
    }
    return m_snapshotCache;
}

void SimulationController::invalidateCaches() {
    m_cpCache.clear();
    m_snapshotCacheValid = false;
}

QJsonObject SimulationController::unitAt(const QString& id) const {
    return m_engine.unitSnapshot(id);
}

QVariantList SimulationController::unitOptions(const QString& kindFilter, const QString& sideFilter) const {
    QVariantList out;
    for (const auto& u : m_engine.scenario().units) {
        if (!kindFilter.isEmpty() && u.kind != kindFilter) continue;
        if (!sideFilter.isEmpty() && u.side != sideFilter) continue;
        auto* pu = m_engine.unit(u.id);
        if (!pu || !pu->alive()) continue;
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

QString SimulationController::commandPostIdFor(const QString& side) const {
    auto it = m_cpCache.find(side);
    if (it != m_cpCache.end()) return it.value();
    for (const auto& u : m_engine.scenario().units) {
        if (u.kind != QLatin1String("commandpost")) continue;
        if (u.side != side) continue;
        auto* pu = m_engine.unit(u.id);
        if (pu && pu->alive()) {
            m_cpCache[side] = u.id;
            return u.id;
        }
    }
    QString fallback = side == QLatin1String("red") ? QStringLiteral("red_cp") : QStringLiteral("blue_cp");
    m_cpCache[side] = fallback;
    return fallback;
}

QVariantList SimulationController::attackableTargets(const QString& attackerId, const QString& enemySide) const {
    QVariantList out;
    auto* atk = m_engine.unit(attackerId);
    if (!atk || !atk->alive()) return out;
    const double atkRange = atk->attackRange();
    const GeoPos atkPos = atk->pos();
    const auto all = m_engine.collectAllUnitsSnapshot();
    for (const auto& v : all) {
        const auto e = v.toObject();
        if (e.value("side").toString() != enemySide) continue;
        if (!e.value("alive").toBool()) continue;
        QJsonArray epos = e.value("position").toArray();
        if (epos.size() < 2) continue;
        const double dx = atkPos.x - epos.at(0).toDouble();
        const double dy = atkPos.y - epos.at(1).toDouble();
        if (std::sqrt(dx*dx + dy*dy) <= atkRange) {
            QVariantMap m;
            m["id"] = e["id"]; m["callsign"] = e["callsign"];
            m["kind"] = e["kind"]; m["side"] = e["side"];
            out.append(m);
        }
    }
    return out;
}

QVariantList SimulationController::detectedEnemyOptions(const QString& attackerId, const QString& friendlySide, const QString& enemySide) const {
    QVariantList out;
    QSet<QString> added;
    const auto allUnits = m_engine.collectAllUnitsSnapshot();
    // gather friendly recon positions
    QVariantList reconList;
    for (const auto& v : allUnits) {
        const auto o = v.toObject();
        if (o.value("side").toString() == friendlySide && o.value("kind").toString() == QLatin1String("reconuav") && o.value("alive").toBool())
            reconList.append(v.toVariant());
    }
    // attacker info
    double atkRange = -1.0;
    QJsonArray atkPosArr;
    if (!attackerId.isEmpty()) {
        auto* atk = m_engine.unit(attackerId);
        if (atk && atk->alive() && atk->position().size() >= 2) {
            atkRange = atk->attackRange();
            atkPosArr = QJsonArray{atk->position().at(0).toDouble(), atk->position().at(1).toDouble()};
        }
    }
    for (const auto& v : allUnits) {
        const auto e = v.toObject();
        if (e.value("side").toString() != enemySide) continue;
        if (!e.value("alive").toBool()) continue;
        if (added.contains(e.value("id").toString())) continue;
        QJsonArray epos = e.value("position").toArray();
        if (epos.size() < 2) continue;
        const double ex = epos.at(0).toDouble(), ey = epos.at(1).toDouble();
        bool found = false;
        for (const auto& rv : reconList) {
            const auto r = rv.toJsonObject();
            const auto rpos = r.value("position").toArray();
            if (rpos.size() < 2) continue;
            const double dx = rpos.at(0).toDouble() - ex;
            const double dy = rpos.at(1).toDouble() - ey;
            if (std::sqrt(dx*dx + dy*dy) <= r.value("detectRange").toDouble(0)) { found = true; break; }
        }
        if (!found && atkRange > 0 && !atkPosArr.isEmpty()) {
            const double adx = atkPosArr.at(0).toDouble() - ex;
            const double ady = atkPosArr.at(1).toDouble() - ey;
            if (std::sqrt(adx*adx + ady*ady) <= atkRange) found = true;
        }
        if (found) {
            QVariantMap m;
            m["id"] = e["id"]; m["callsign"] = e["callsign"];
            m["kind"] = e["kind"]; m["side"] = e["side"];
            out.append(m);
            added.insert(e.value("id").toString());
        }
    }
    return out;
}

bool SimulationController::hasTargetInAttackRange(const QString& unitId, const QString& enemySide) const {
    auto* u = m_engine.unit(unitId);
    if (!u || u->kindStr() != QLatin1String("attackuav") || !u->alive()) return false;
    const double atkRange = u->attackRange();
    const GeoPos myPos = u->pos();
    const auto all = m_engine.collectAllUnitsSnapshot();
    for (const auto& v : all) {
        const auto e = v.toObject();
        if (e.value("side").toString() != enemySide) continue;
        if (!e.value("alive").toBool()) continue;
        QJsonArray epos = e.value("position").toArray();
        if (epos.size() < 2) continue;
        const double dx = myPos.x - epos.at(0).toDouble();
        const double dy = myPos.y - epos.at(1).toDouble();
        if (std::sqrt(dx*dx + dy*dy) <= atkRange) return true;
    }
    return false;
}

bool SimulationController::hasTargetInDetectShared(const QString& unitId, const QString& friendlySide, const QString& enemySide) const {
    auto* u = m_engine.unit(unitId);
    if (!u || u->kindStr() != QLatin1String("groundscout") || !u->alive()) return false;
    const auto allUnits = m_engine.collectAllUnitsSnapshot();
    QVariantList reconList;
    for (const auto& v : allUnits) {
        const auto o = v.toObject();
        if (o.value("side").toString() == friendlySide && o.value("kind").toString() == QLatin1String("reconuav") && o.value("alive").toBool())
            reconList.append(v.toVariant());
    }
    for (const auto& v : allUnits) {
        const auto e = v.toObject();
        if (e.value("side").toString() != enemySide) continue;
        if (!e.value("alive").toBool()) continue;
        QJsonArray epos = e.value("position").toArray();
        if (epos.size() < 2) continue;
        const double ex = epos.at(0).toDouble(), ey = epos.at(1).toDouble();
        for (const auto& rv : reconList) {
            const auto r = rv.toJsonObject();
            const auto rpos = r.value("position").toArray();
            if (rpos.size() < 2) continue;
            const double dx = rpos.at(0).toDouble() - ex;
            const double dy = rpos.at(1).toDouble() - ey;
            if (std::sqrt(dx*dx + dy*dy) <= r.value("detectRange").toDouble(0)) return true;
        }
    }
    return false;
}

QStringList SimulationController::detectedEnemyIds(const QString& friendlySide) const {
    QSet<QString> ids;
    const auto allUnits = m_engine.collectAllUnitsSnapshot();

    // 1) Direct detection: any friendly unit's detectRange covers the enemy
    for (const auto& v : allUnits) {
        const auto fu = v.toObject();
        if (fu.value("side").toString() != friendlySide) continue;
        if (!fu.value("alive").toBool()) continue;
        QJsonArray fpos = fu.value("position").toArray();
        if (fpos.size() < 2) continue;
        const double fx = fpos.at(0).toDouble(), fy = fpos.at(1).toDouble();
        const double fdr = fu.value("detectRange").toDouble(0);
        for (const auto& v2 : allUnits) {
            const auto eu = v2.toObject();
            if (eu.value("side").toString() == friendlySide) continue;
            if (!eu.value("alive").toBool()) continue;
            QJsonArray epos = eu.value("position").toArray();
            if (epos.size() < 2) continue;
            const double dx = fx - epos.at(0).toDouble();
            const double dy = fy - epos.at(1).toDouble();
            if (std::sqrt(dx*dx + dy*dy) <= fdr)
                ids.insert(eu.value("id").toString());
        }
    }

    // 2) Shared knowledge: any friendly unit stored SharedDetect info
    for (const auto& v : allUnits) {
        const auto fu = v.toObject();
        if (fu.value("side").toString() != friendlySide) continue;
        const auto sk = fu.value("sharedKnowledge").toObject();
        for (auto it = sk.begin(); it != sk.end(); ++it) {
            const QString key = it.key();
            if (key.startsWith(QLatin1String("shared:detect:"))) {
                const auto info = it.value().toObject();
                const QString tid = info.value("targetId").toString();
                if (!tid.isEmpty()) ids.insert(tid);
            }
        }
    }

    return {ids.begin(), ids.end()};
}

QString SimulationController::pickDefaultUnit(const QString& kind, const QString& side) const {
    for (const auto& u : m_engine.scenario().units) {
        if (u.side != side || u.kind != kind) continue;
        auto* runtimeUnit = m_engine.unit(u.id);
        if (runtimeUnit && runtimeUnit->alive()) return u.id;
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
            emit focusedUnitIdChanged();
        }
        return;
    }
}

void SimulationController::saveSetting(const QString& key, const QVariant& value) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    QString path = dir + "/settings.json";
    QJsonObject obj;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        obj = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
    }
    QJsonValue jv = QJsonValue::fromVariant(value);
    obj[key] = jv;
    const QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        emit errorForward(QStringLiteral("保存设置失败: %1").arg(key));
        return;
    }
    // 信号会同步触发 QML 读取；先关闭文件，避免读取到尚未刷新的旧快捷键配置。
    f.close();
    if (key.startsWith(QStringLiteral("shortcuts/"))) emit shortcutsChanged();
}

QVariant SimulationController::loadSetting(const QString& key, const QVariant& defaultValue) const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString path = dir + "/settings.json";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return defaultValue;
    QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    if (!obj.contains(key)) return defaultValue;
    return obj[key].toVariant();
}

void SimulationController::useLocalMode() {
    const bool wasNetworked = isNetworked();
    m_networkClient.close();
    if (wasNetworked && !m_savedLocalScenario.units.empty()) m_engine.setScenario(m_savedLocalScenario);
    m_sessionMode = QStringLiteral("local");
    m_networkState = QStringLiteral("disconnected");
    m_networkStatus = QStringLiteral("本地模式");
    m_username.clear();
    m_displayName.clear();
    m_userRole.clear();
    m_serverAddress.clear();
    m_remoteMessages.clear();
    m_chatMessages.clear();
    m_remoteScenarioRevision = -1;
    m_lastCommandId.clear();
    m_lastCommandStatus.clear();
    m_lastCommandMessage.clear();
    emit sessionChanged();
    emit networkStatusChanged();
    emit messagesForward();
    emit chatMessagesChanged();
    emit commandStatusChanged();
    if (wasNetworked) emit networkedChanged();
    ensureFocusedConsistent();
}

void SimulationController::loginOnline(const QString& server, const QString& username,
                                       const QString& password) {
    if (!isNetworked()) m_savedLocalScenario = m_engine.scenario();
    m_networkClient.login(server, username, password);
}

void SimulationController::diagnoseServer(const QString& server) {
    m_networkClient.diagnoseServer(server);
}

void SimulationController::rememberServerAddress(const QString& server) {
    QString normalized = server.trimmed();
    if (normalized.isEmpty()) return;
    if (!normalized.contains(QStringLiteral("://"))) normalized.prepend(QStringLiteral("http://"));
    QUrl url(normalized);
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme().toLower() != QLatin1String("http")
            && url.scheme().toLower() != QLatin1String("https"))) return;
    normalized = url.toString(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    while (normalized.endsWith(QLatin1Char('/'))) normalized.chop(1);
    if (normalized.isEmpty()) return;

    m_serverHistory.removeAll(normalized);
    m_serverHistory.prepend(normalized);
    while (m_serverHistory.size() > 6) m_serverHistory.removeLast();
    saveSetting(QStringLiteral("network/server"), normalized);
    saveSetting(QStringLiteral("network/serverHistory"), QVariant::fromValue(m_serverHistory));
    emit serverHistoryChanged();
}

void SimulationController::logoutOnline() {
    const bool wasNetworked = isNetworked();
    useLocalMode();
    m_sessionMode = QStringLiteral("unselected");
    m_networkStatus = QStringLiteral("请选择运行模式");
    emit sessionChanged();
    emit networkStatusChanged();
    if (!wasNetworked) emit networkedChanged();
}

void SimulationController::setReady(bool ready) {
    if (isNetworked() && (m_userRole == QLatin1String("red") || m_userRole == QLatin1String("blue")))
        m_networkClient.sendReady(ready);
}

void SimulationController::endMatch() {
    if (isNetworked() && m_userRole == QLatin1String("director"))
        m_networkClient.sendControl(QStringLiteral("end"));
}

void SimulationController::sendChat(const QString& text) {
    if (isNetworked() && !text.trimmed().isEmpty()) m_networkClient.sendChat(text.trimmed());
}

QString SimulationController::connectToPeer(const QString& host, int port) {
    const QString message = QStringLiteral("请通过联网登录界面连接账号服务器: http://%1:%2")
                                .arg(host).arg(port);
    m_networkStatus = message;
    emit networkStatusChanged();
    return message;
}

void SimulationController::disconnectFromPeer() {
    useLocalMode();
}

void SimulationController::applyRoleView() {
    QString target;
    if (m_userRole == QLatin1String("editor")) target = QStringLiteral("editor");
    else if (m_userRole == QLatin1String("director")) target = QStringLiteral("director");
    else if (m_userRole == QLatin1String("red")) target = QStringLiteral("commandpost-red");
    else if (m_userRole == QLatin1String("blue")) target = QStringLiteral("commandpost-blue");
    if (!target.isEmpty()) setViewMode(target);
    ensureFocusedConsistent();
}

QJsonObject SimulationController::scenarioUnitJson(const ScenarioUnit& unit) const {
    Scenario wrapper;
    wrapper.units.push_back(unit);
    const QJsonArray units = ScenarioIo::toJson(wrapper).value(QStringLiteral("units")).toArray();
    return units.isEmpty() ? QJsonObject{} : units.at(0).toObject();
}

void SimulationController::applyRemoteSnapshot(const QJsonObject& payload) {
    if (!isNetworked()) return;
    const QJsonObject room = payload.value(QStringLiteral("roomState")).toObject();
    m_matchPhase = room.value(QStringLiteral("phase")).toString(QStringLiteral("preparing"));
    m_redReady = room.value(QStringLiteral("redReady")).toBool();
    m_blueReady = room.value(QStringLiteral("blueReady")).toBool();
    m_remoteReadyForSim = room.value(QStringLiteral("readyForSim")).toBool();
    m_remoteCpIssues = room.value(QStringLiteral("cpIssues")).toString();
    const qint64 revision = room.value(QStringLiteral("scenarioRevision")).toInteger();

    const QJsonObject scenarioObject = payload.value(QStringLiteral("scenario")).toObject();
    const Scenario incomingScenario = ScenarioIo::fromJson(scenarioObject);
    QStringList incomingIds;
    incomingIds.reserve(static_cast<qsizetype>(incomingScenario.units.size()));
    for (const auto& unit : incomingScenario.units) incomingIds.append(unit.id);
    QStringList currentIds = m_engine.unitIds();
    std::sort(incomingIds.begin(), incomingIds.end());
    std::sort(currentIds.begin(), currentIds.end());
    if (revision != m_remoteScenarioRevision || incomingIds != currentIds) {
        if (!incomingScenario.units.empty()) m_engine.setScenario(incomingScenario);
        m_remoteScenarioRevision = revision;
    }

    m_remoteMessages = payload.value(QStringLiteral("messages")).toArray().toVariantList();
    m_engine.applyRemoteRuntimeState(payload.value(QStringLiteral("units")).toArray(),
                                     room.value(QStringLiteral("simTime")).toDouble(),
                                     room.value(QStringLiteral("running")).toBool(),
                                     room.value(QStringLiteral("speed")).toDouble(1.0));
    invalidateCaches();
    ensureFocusedConsistent();
    emit messagesForward();
    emit readyForSimForward();
    emit roomStateChanged();
    emit mapInfoForward();
}

QJsonObject SimulationController::allSettings() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString path = dir + "/settings.json";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

} // namespace gbr
