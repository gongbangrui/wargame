#include "SnapshotCodec.h"
#include "Scenario.h"
#include "SimulationEngine.h"
#include "UnitBase.h"
#include "../units/AttackUAV.h"

#include <QJsonValue>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QHash>
#include <cmath>

namespace gbr {

QJsonObject SnapshotCodec::encodeScenario(const Scenario& s) {
    return ScenarioIo::toJson(s);
}

Scenario SnapshotCodec::decodeScenario(const QJsonObject& obj) {
    return ScenarioIo::fromJson(obj);
}

QJsonArray SnapshotCodec::encodeRuntimeUnits(const SimulationEngine& engine) {
    // collectAllUnitsSnapshot already includes HP, alive, position, recentPath,
    // sharedKnowledge, status, schedule. That's exactly what the peer needs.
    return engine.collectAllUnitsSnapshot();
}

void SnapshotCodec::decodeRuntimeUnits(SimulationEngine& engine, const QJsonArray& units) {
    for (const auto& v : units) {
        const auto u = v.toObject();
        const QString id = u.value("id").toString();
        if (id.isEmpty()) continue;
        auto* unit = engine.unit(id);
        if (!unit) continue;

        // HP / alive
        const double hp = u.value("hp").toDouble();
        if (u.value("alive").toBool()) {
            if (hp > 0.0) unit->setHp(hp);
        } else {
            unit->setHp(0.0);
        }

        // Position — preserve existing altitude when the snapshot omits it,
        // so a 2D snapshot doesn't reset 3D placement.
        const auto pos = u.value("position").toArray();
        if (pos.size() >= 2) {
            const double x = pos.at(0).toDouble();
            const double y = pos.at(1).toDouble();
            const double alt = pos.size() >= 3 ? pos.at(2).toDouble() : unit->pos().alt;
            unit->setPosition(GeoPos{x, y, alt});
        }

        // Schedule
        QJsonArray sched = u.value("schedule").toArray();
        std::vector<SchedulePoint> sp;
        sp.reserve(sched.size());
        for (const auto& sv : sched) {
            const auto so = sv.toObject();
            SchedulePoint p;
            p.time = so.value("time").toDouble();
            p.x = so.value("x").toDouble();
            p.y = so.value("y").toDouble();
            sp.push_back(p);
        }
        unit->setSchedule(sp);

        // Status (best-effort; if the peer carries a richer status text, take it)
        const QString status = u.value("status").toString();
        if (!status.isEmpty()) unit->setStatus(status);

        // Jam factor — only if explicitly present (avoid zeroing on older peers)
        if (u.contains("jamFactor")) {
            unit->applyJamming(u.value("jamFactor").toDouble(1.0));
        }
        if (u.contains(QStringLiteral("subsystems"))) {
            unit->restoreSubsystemState(u.value(QStringLiteral("subsystems")).toObject());
        }
        unit->requestService(u.value(QStringLiteral("serviceRequested")).toBool(false));
        if (auto* attacker = qobject_cast<AttackUAV*>(unit);
            attacker && u.contains(QStringLiteral("ammoRemaining"))) {
            attacker->restoreRuntimeWeaponState(
                u.value(QStringLiteral("ammoRemaining")).toInt(-1),
                u.value(QStringLiteral("cooldownRemaining")).toDouble(0.0),
                u.value(QStringLiteral("lastShotOutcome")).toString(),
                u.value(QStringLiteral("fuelRemaining")).toDouble(-1.0),
                u.value(QStringLiteral("turnaroundElapsed")).toDouble(0.0));
        }
    }
}

QStringList SnapshotCodec::diffUnitIds(const QJsonArray& a, const QJsonArray& b) {
    auto pick = [](const QJsonArray& arr) {
        QHash<QString, QJsonObject> byId;
        for (const auto& v : arr) {
            const auto o = v.toObject();
            byId.insert(o.value("id").toString(), o);
        }
        return byId;
    };
    const auto ma = pick(a);
    const auto mb = pick(b);
    QStringList diffs;
    for (auto it = ma.constBegin(); it != ma.constEnd(); ++it) {
        const auto& left = it.value();
        if (!mb.contains(it.key())) { diffs << it.key(); continue; }
        const auto& right = mb.value(it.key());
        auto approxEq = [](double x, double y) { return std::abs(x - y) < 0.5; };
        const auto lp = left.value("position").toArray();
        const auto rp = right.value("position").toArray();
        if (lp.size() < 2 || rp.size() < 2) { diffs << it.key(); continue; }
        if (!approxEq(lp.at(0).toDouble(), rp.at(0).toDouble())
            || !approxEq(lp.at(1).toDouble(), rp.at(1).toDouble())) { diffs << it.key(); continue; }
        if (!approxEq(left.value("hp").toDouble(), right.value("hp").toDouble())) { diffs << it.key(); continue; }
    }
    for (auto it = mb.constBegin(); it != mb.constEnd(); ++it) {
        if (!ma.contains(it.key())) diffs << it.key();
    }
    return diffs;
}

QJsonArray SnapshotCodec::encodeCheckpointUnits(const SimulationEngine& engine) {
    QJsonArray result;
    result.append(QJsonObject{{QStringLiteral("checkpointType"), QStringLiteral("engine")},
                              {QStringLiteral("combatSeed"),
                               QString::number(engine.combatSeed(), 16)}});
    QStringList ids = engine.unitIds();
    ids.sort();
    for (const QString& id : ids) {
        if (UnitBase* unit = engine.unit(id)) result.append(unit->checkpointState());
    }
    return result;
}

bool SnapshotCodec::decodeCheckpointUnits(SimulationEngine& engine,
                                          const QJsonArray& units,
                                          QString* error) {
    if (error) error->clear();
    QSet<QString> restoredIds;
    bool restoredEngineState = false;
    for (const QJsonValue& value : units) {
        const QJsonObject state = value.toObject();
        if (state.value(QStringLiteral("checkpointType")).toString()
            == QLatin1String("engine")) {
            if (restoredEngineState) {
                if (error) *error = QStringLiteral("检查点包含重复引擎状态");
                return false;
            }
            bool seedOk = false;
            const quint64 seed = state.value(QStringLiteral("combatSeed")).toString()
                                     .toULongLong(&seedOk, 16);
            if (!seedOk || seed == 0) {
                if (error) *error = QStringLiteral("检查点战斗随机种子无效");
                return false;
            }
            engine.restoreCombatSeed(seed);
            restoredEngineState = true;
            continue;
        }
        const QString id = state.value(QStringLiteral("id")).toString();
        UnitBase* unit = engine.unit(id);
        if (id.isEmpty() || !unit || restoredIds.contains(id)) {
            if (error) *error = QStringLiteral("检查点包含未知或重复单元: %1").arg(id);
            return false;
        }
        QString unitError;
        if (!unit->restoreCheckpointState(state, &unitError)) {
            if (error) *error = unitError;
            return false;
        }
        restoredIds.insert(id);
    }
    const QStringList expectedIdList = engine.unitIds();
    const QSet<QString> expectedIds(expectedIdList.cbegin(), expectedIdList.cend());
    if (restoredIds != expectedIds) {
        if (error) *error = QStringLiteral("检查点单元集合与场景不一致");
        return false;
    }
    if (!restoredEngineState) {
        if (error) *error = QStringLiteral("检查点缺少引擎随机状态");
        return false;
    }
    return true;
}

} // namespace gbr
