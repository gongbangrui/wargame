#include "SnapshotCodec.h"
#include "Scenario.h"
#include "SimulationEngine.h"
#include "UnitBase.h"

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

QJsonObject SnapshotCodec::encodeRuntime(const SimulationEngine& engine,
                                         qint64 serverTick,
                                         qint64 scenarioRevision) {
    if (serverTick < 0) serverTick = qRound64(engine.simTime() / 0.05);
    return {{QStringLiteral("schemaVersion"), RuntimeSchemaVersion},
            {QStringLiteral("scenarioRevision"), scenarioRevision},
            {QStringLiteral("serverTick"), serverTick},
            {QStringLiteral("simTime"), engine.simTime()},
            {QStringLiteral("running"), engine.running()},
            {QStringLiteral("units"), encodeRuntimeUnits(engine)}};
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

} // namespace gbr
