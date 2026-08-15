#include "Scenario.h"

#include "UnitBase.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonArray>
#include <QDir>
#include <QSaveFile>
#include <QSet>
#include <cmath>
#include <limits>

namespace gbr {

namespace {

bool readOptionalFiniteDouble(const QJsonObject& object, const char* name, double fallback,
                              double* output, QString* error, const QString& context) {
    const QJsonValue value = object.value(QLatin1String(name));
    if (value.isUndefined()) {
        *output = fallback;
        return true;
    }
    if (!value.isDouble() || !std::isfinite(value.toDouble())) {
        *error = QStringLiteral("%1.%2必须为有限数字").arg(context, QLatin1String(name));
        return false;
    }
    *output = value.toDouble();
    return true;
}

bool readOptionalInteger(const QJsonObject& object, const char* name, int fallback,
                         int* output, QString* error, const QString& context) {
    const QJsonValue value = object.value(QLatin1String(name));
    if (value.isUndefined()) {
        *output = fallback;
        return true;
    }
    const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number
        || number < static_cast<double>(std::numeric_limits<int>::min())
        || number > static_cast<double>(std::numeric_limits<int>::max())) {
        *error = QStringLiteral("%1.%2必须为有限整数").arg(context, QLatin1String(name));
        return false;
    }
    *output = static_cast<int>(number);
    return true;
}

bool readRequiredFiniteDouble(const QJsonObject& object, const char* name, double* output,
                              QString* error, const QString& context) {
    const QJsonValue value = object.value(QLatin1String(name));
    if (!value.isDouble() || !std::isfinite(value.toDouble())) {
        *error = QStringLiteral("%1.%2必须为有限数字").arg(context, QLatin1String(name));
        return false;
    }
    *output = value.toDouble();
    return true;
}

bool readRequiredString(const QJsonObject& object, const char* name, QString* output,
                        QString* error, const QString& context) {
    const QJsonValue value = object.value(QLatin1String(name));
    if (!value.isString() || value.toString().isEmpty()) {
        *error = QStringLiteral("%1.%2不能为空").arg(context, QLatin1String(name));
        return false;
    }
    *output = value.toString();
    return true;
}

}

QJsonObject ScenarioIo::toJson(const Scenario& s) {
    QJsonObject root;
    root["schemaVersion"] = SchemaVersion;
    QJsonObject m;
    m["name"] = s.map.name;
    m["widthMeters"] = s.map.widthMeters;
    m["heightMeters"] = s.map.heightMeters;
    m["backgroundResource"] = s.map.backgroundResource;
    root["map"] = m;
    QJsonArray arr;
    for (const auto& u : s.units) {
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
        o["collisionRadius"] = u.collisionRadius;
        o["collisionHalfHeight"] = u.collisionHalfHeight;
        o["maxHp"] = u.maxHp;
        o["armor"] = u.armor;
        o["repairRate"] = u.repairRate;
        o["subsystemRepairRate"] = u.subsystemRepairRate;
        o["attackPower"] = u.attackPower;
        o["ammoCapacity"] = u.ammoCapacity;
        o["initialAmmo"] = u.initialAmmo;
        o["hitProbability"] = u.hitProbability;
        o["optimalRange"] = u.optimalRange;
        o["minAttackRange"] = u.minAttackRange;
        o["cooldownSec"] = u.cooldownSec;
        o["damageMin"] = u.damageMin;
        o["damageMax"] = u.damageMax;
        o["rangeFalloff"] = u.rangeFalloff;
        o["fuelCapacitySec"] = u.fuelCapacitySec;
        o["initialFuelSec"] = u.initialFuelSec;
        o["rearmDurationSec"] = u.rearmDurationSec;
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
    root["notes"] = s.notes;
    return root;
}

Scenario ScenarioIo::fromJson(const QJsonObject& o, QString* err) {
    if (err) err->clear();
    auto reject = [err](const QString& message) {
        if (err) *err = message;
        return Scenario{};
    };

    Scenario s;
    const QJsonValue mapValue = o.value(QStringLiteral("map"));
    if (!mapValue.isUndefined() && !mapValue.isObject()) {
        return reject(QStringLiteral("map必须是对象"));
    }
    const QJsonObject m = mapValue.toObject();
    s.map.name = m.value("name").toString("default");
    QString parseError;
    if (!readOptionalFiniteDouble(m, "widthMeters", 40000.0, &s.map.widthMeters,
                                  &parseError, QStringLiteral("map"))
        || !readOptionalFiniteDouble(m, "heightMeters", 30000.0, &s.map.heightMeters,
                                     &parseError, QStringLiteral("map"))) {
        return reject(parseError);
    }
    s.map.backgroundResource = m.value("backgroundResource").toString();

    const QJsonValue unitsValue = o.value(QStringLiteral("units"));
    if (!unitsValue.isUndefined() && !unitsValue.isArray()) {
        return reject(QStringLiteral("units必须是数组"));
    }
    QSet<QString> unitIds;
    const QJsonArray units = unitsValue.toArray();
    for (qsizetype unitIndex = 0; unitIndex < units.size(); ++unitIndex) {
        const QJsonValue value = units.at(unitIndex);
        const QString context = QStringLiteral("units[%1]").arg(unitIndex);
        if (!value.isObject()) return reject(context + QStringLiteral("必须是对象"));

        const QJsonObject u = value.toObject();
        ScenarioUnit su;
        if (!readRequiredString(u, "id", &su.id, &parseError, context)
            || !readRequiredString(u, "kind", &su.kind, &parseError, context)
            || !readRequiredString(u, "side", &su.side, &parseError, context)) {
            return reject(parseError);
        }
        if (unitIds.contains(su.id)) {
            return reject(QStringLiteral("%1.id重复: %2").arg(context, su.id));
        }
        unitIds.insert(su.id);

        su.callsign = u.value("callsign").toString();
        if (!readOptionalFiniteDouble(u, "x", 0.0, &su.pos.x, &parseError, context)
            || !readOptionalFiniteDouble(u, "y", 0.0, &su.pos.y, &parseError, context)
            || !readOptionalFiniteDouble(u, "alt", 0.0, &su.pos.alt, &parseError, context)
            || !readOptionalFiniteDouble(u, "detectRange", 5000.0, &su.detectRange, &parseError, context)
            || !readOptionalFiniteDouble(u, "attackRange", 1500.0, &su.attackRange, &parseError, context)
            || !readOptionalFiniteDouble(u, "commRange",
                                         UnitBase::defaultCommRangeM(kindFromName(su.kind)),
                                         &su.commRange, &parseError, context)
            || !readOptionalFiniteDouble(u, "speed",
                                         UnitBase::defaultSpeedMps(kindFromName(su.kind)),
                                         &su.speed, &parseError, context)
            || !readOptionalFiniteDouble(u, "collisionRadius",
                                         UnitBase::defaultCollisionRadiusM(kindFromName(su.kind)),
                                         &su.collisionRadius, &parseError, context)
            || !readOptionalFiniteDouble(u, "collisionHalfHeight",
                                         UnitBase::defaultCollisionHalfHeightM(kindFromName(su.kind)),
                                         &su.collisionHalfHeight, &parseError, context)
            || !readOptionalFiniteDouble(u, "maxHp", 100.0, &su.maxHp, &parseError, context)
            || !readOptionalFiniteDouble(u, "armor", 0.0, &su.armor, &parseError, context)
            || !readOptionalFiniteDouble(u, "repairRate", 2.0, &su.repairRate, &parseError, context)
            || !readOptionalFiniteDouble(u, "subsystemRepairRate", 0.02,
                                         &su.subsystemRepairRate, &parseError, context)
            || !readOptionalFiniteDouble(u, "attackPower", 100.0, &su.attackPower,
                                         &parseError, context)) {
            return reject(parseError);
        }
        // v1 scenarios only carried attackPower and attackRange. Derive the
        // new weapon profile from those values so old files remain playable.
        if (!readOptionalInteger(u, "ammoCapacity", 4, &su.ammoCapacity, &parseError, context)
            || !readOptionalInteger(u, "initialAmmo", su.ammoCapacity, &su.initialAmmo,
                                    &parseError, context)
            || !readOptionalFiniteDouble(u, "hitProbability", kDefaultAttackUavHitProbability,
                                         &su.hitProbability,
                                         &parseError, context)
            || !readOptionalFiniteDouble(u, "optimalRange", su.attackRange, &su.optimalRange,
                                         &parseError, context)
            || !readOptionalFiniteDouble(u, "minAttackRange", 300.0, &su.minAttackRange,
                                         &parseError, context)
            || !readOptionalFiniteDouble(u, "cooldownSec", 5.0, &su.cooldownSec,
                                         &parseError, context)
            || !readOptionalFiniteDouble(u, "damageMin", 80.0, &su.damageMin,
                                         &parseError, context)
            || !readOptionalFiniteDouble(u, "damageMax", 110.0, &su.damageMax,
                                         &parseError, context)
            || !readOptionalFiniteDouble(u, "rangeFalloff", 0.25, &su.rangeFalloff,
                                         &parseError, context)
            || !readOptionalFiniteDouble(u, "fuelCapacitySec", 1800.0, &su.fuelCapacitySec,
                                         &parseError, context)
            || !readOptionalFiniteDouble(u, "initialFuelSec", su.fuelCapacitySec,
                                         &su.initialFuelSec, &parseError, context)
            || !readOptionalFiniteDouble(u, "rearmDurationSec", 8.0, &su.rearmDurationSec,
                                         &parseError, context)) {
            return reject(parseError);
        }

        const QJsonValue scheduleValue = u.value(QStringLiteral("schedule"));
        if (!scheduleValue.isUndefined() && !scheduleValue.isArray()) {
            return reject(context + QStringLiteral(".schedule必须是数组"));
        }
        const QJsonArray schedule = scheduleValue.toArray();
        su.schedule.reserve(schedule.size());
        for (qsizetype scheduleIndex = 0; scheduleIndex < schedule.size(); ++scheduleIndex) {
            const QJsonValue schedulePoint = schedule.at(scheduleIndex);
            const QString scheduleContext =
                QStringLiteral("%1.schedule[%2]").arg(context).arg(scheduleIndex);
            if (!schedulePoint.isObject()) {
                return reject(scheduleContext + QStringLiteral("必须是对象"));
            }
            const QJsonObject sp = schedulePoint.toObject();
            SchedulePoint pt;
            if (!readRequiredFiniteDouble(sp, "time", &pt.time, &parseError, scheduleContext)
                || !readRequiredFiniteDouble(sp, "x", &pt.x, &parseError, scheduleContext)
                || !readRequiredFiniteDouble(sp, "y", &pt.y, &parseError, scheduleContext)) {
                return reject(parseError);
            }
            su.schedule.push_back(pt);
        }
        std::sort(su.schedule.begin(), su.schedule.end(),
                  [](const SchedulePoint& a, const SchedulePoint& b){ return a.time < b.time; });
        s.units.push_back(su);
    }
    s.notes = o.value("notes").toString();
    return s;
}

Scenario ScenarioIo::loadFromFile(const QString& path, QString* err) {
    if (err) err->clear();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("无法打开文件: %1").arg(path);
        return {};
    }
    QJsonParseError parseError;
    auto doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (!doc.isObject()) {
        if (err) {
            *err = QStringLiteral("JSON 解析失败（偏移 %1）: %2")
                       .arg(parseError.offset)
                       .arg(parseError.errorString());
        }
        return {};
    }
    return fromJson(doc.object(), err);
}

bool ScenarioIo::saveToFile(const Scenario& s, const QString& path, QString* err) {
    if (err) err->clear();
    const QString parentDir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(parentDir)) {
        if (err) *err = QStringLiteral("无法创建目录: %1").arg(parentDir);
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = QStringLiteral("无法写入文件: %1").arg(path);
        return false;
    }
    const QByteArray data = QJsonDocument(toJson(s)).toJson(QJsonDocument::Indented);
    if (f.write(data) != data.size() || !f.commit()) {
        if (err) *err = QStringLiteral("场景文件写入失败: %1").arg(path);
        return false;
    }
    return true;
}

Scenario ScenarioIo::defaultScenario() {
    Scenario s;
    s.map.widthMeters = 20000;
    s.map.heightMeters = 15000;
    s.map.name = "default";
    auto add = [&](const QString& id, const QString& cs, const QString& kind, const QString& side,
                   double x, double y, double alt,
                   double d, double a, double c, double sp, double hp,
                   std::initializer_list<SchedulePoint> sched = {}) {
        ScenarioUnit u;
        u.id = id; u.callsign = cs; u.kind = kind; u.side = side;
        u.pos = GeoPos{x, y, alt};
        u.detectRange = d; u.attackRange = a; u.commRange = c;
        u.speed = sp; u.maxHp = hp;
        const UnitKind unitKind = kindFromName(kind);
        u.collisionRadius = UnitBase::defaultCollisionRadiusM(unitKind);
        u.collisionHalfHeight = UnitBase::defaultCollisionHalfHeightM(unitKind);
        for (auto& p : sched) u.schedule.push_back(p);
        std::sort(u.schedule.begin(), u.schedule.end(),
                  [](const SchedulePoint& a, const SchedulePoint& b){ return a.time < b.time; });
        s.units.push_back(u);
    };

    add("red_cp", "红方指挥所", "commandpost", "red", 2000, 7500, 50,  5000, 0,    7000, 4,   200);
    add("red_r1", "红方侦察1",  "reconuav",     "red", 4000, 4000,  3000, 8000, 0,    6000, 150, 100,
        {{0, 4000, 4000}, {40, 7000, 5000}, {80, 10000, 6000}});
    add("red_a1", "红方攻击1",  "attackuav",    "red", 3000, 11000, 2000, 4000,  2500, 5000, 200, 120,
        {{0, 3000, 11000}, {30, 6000, 10500}, {60, 9000, 9500}, {90, 12000, 9000}});
    add("red_g1", "红方地面1",  "groundscout",  "red", 2000, 2000,  0,    3000, 0,    4000, 18,  80);

    add("blue_cp", "蓝方指挥所", "commandpost", "blue", 18000, 7500, 50,  5000, 0,    7000, 4,   200);
    add("blue_r1", "蓝方侦察1",  "reconuav",     "blue", 16000, 11000, 3000, 8000, 0,    6000, 150, 100,
        {{0, 16000, 11000}, {40, 13000, 10500}, {80, 10000, 9500}});
    add("blue_a1", "蓝方攻击1",  "attackuav",    "blue", 17000, 4000,  2000, 4000,  2500, 5000, 200, 120,
        {{0, 17000, 4000}, {30, 14000, 4500}, {60, 11000, 5500}, {90, 8000, 6000}});
    add("blue_g1", "蓝方地面1",  "groundscout",  "blue", 18000, 13000, 0,    3000, 0,    4000, 18,  80);

    add("red_j1", "红方干扰1",  "jammeruav",    "red", 4000, 7500,   4000, 6000, 0,    6500, 120, 80);
    add("blue_j1", "蓝方干扰1",  "jammeruav",    "blue", 16000, 7000, 4000, 6000, 0,    6500, 120, 80);

    s.notes = "默认推演场景。地图 20km x 15km，红西蓝东。";
    return s;
}

} // namespace gbr
