#include "Scenario.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonArray>
#include <QDir>
#include <QSaveFile>

namespace gbr {

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
        o["maxHp"] = u.maxHp;
        o["attackPower"] = u.attackPower;
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

Scenario ScenarioIo::fromJson(const QJsonObject& o) {
    Scenario s;
    auto m = o.value("map").toObject();
    s.map.name = m.value("name").toString("default");
    s.map.widthMeters = m.value("widthMeters").toDouble(40000);
    s.map.heightMeters = m.value("heightMeters").toDouble(30000);
    s.map.backgroundResource = m.value("backgroundResource").toString();
    for (auto v : o.value("units").toArray()) {
        auto u = v.toObject();
        ScenarioUnit su;
        su.id = u.value("id").toString();
        su.callsign = u.value("callsign").toString();
        su.kind = u.value("kind").toString();
        su.side = u.value("side").toString();
        su.pos.x = u.value("x").toDouble();
        su.pos.y = u.value("y").toDouble();
        su.pos.alt = u.value("alt").toDouble();
        su.detectRange = u.value("detectRange").toDouble(5000);
        su.attackRange = u.value("attackRange").toDouble(1500);
        su.commRange = u.value("commRange").toDouble(20000);
        su.speed = u.value("speed").toDouble(50);
        su.maxHp = u.value("maxHp").toDouble(100);
        su.attackPower = u.value("attackPower").toDouble(100);
        for (auto sv : u.value("schedule").toArray()) {
            auto sp = sv.toObject();
            SchedulePoint pt;
            pt.time = sp.value("time").toDouble();
            pt.x = sp.value("x").toDouble();
            pt.y = sp.value("y").toDouble();
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
    return fromJson(doc.object());
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
        for (auto& p : sched) u.schedule.push_back(p);
        std::sort(u.schedule.begin(), u.schedule.end(),
                  [](const SchedulePoint& a, const SchedulePoint& b){ return a.time < b.time; });
        s.units.push_back(u);
    };

    add("red_cp", "红方指挥所", "commandpost", "red", 2000, 7500, 50,  5000, 0,    20000, 0,   200);
    add("red_r1", "红方侦察1",  "reconuav",     "red", 4000, 4000,  3000, 8000, 0,    20000, 80,  100,
        {{0, 4000, 4000}, {40, 7000, 5000}, {80, 10000, 6000}});
    add("red_a1", "红方攻击1",  "attackuav",    "red", 3000, 11000, 2000, 4000,  2500, 15000, 100, 120,
        {{0, 3000, 11000}, {30, 6000, 10500}, {60, 9000, 9500}, {90, 12000, 9000}});
    add("red_g1", "红方地面1",  "groundscout",  "red", 2000, 2000,  0,    3000, 0,    10000, 6,   80);

    add("blue_cp", "蓝方指挥所", "commandpost", "blue", 18000, 7500, 50,  5000, 0,    20000, 0,   200);
    add("blue_r1", "蓝方侦察1",  "reconuav",     "blue", 16000, 11000, 3000, 8000, 0,    20000, 80,  100,
        {{0, 16000, 11000}, {40, 13000, 10500}, {80, 10000, 9500}});
    add("blue_a1", "蓝方攻击1",  "attackuav",    "blue", 17000, 4000,  2000, 4000,  2500, 15000, 100, 120,
        {{0, 17000, 4000}, {30, 14000, 4500}, {60, 11000, 5500}, {90, 8000, 6000}});
    add("blue_g1", "蓝方地面1",  "groundscout",  "blue", 18000, 13000, 0,    3000, 0,    10000, 6,   80);

    add("red_j1", "红方干扰1",  "jammeruav",    "red", 4000, 7500,   4000, 6000, 0,    20000, 60,  80);
    add("blue_j1", "蓝方干扰1",  "jammeruav",    "blue", 16000, 7000, 4000, 6000, 0,    20000, 60,  80);

    s.notes = "默认推演场景。地图 20km x 15km，红西蓝东。";
    return s;
}

} // namespace gbr
