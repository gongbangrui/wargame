#include "Protocol.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>

#include <cmath>

namespace gbr::Protocol {

namespace {

const QSet<QString>& clientTypes() {
    static const QSet<QString> types{
        QStringLiteral("auth"), QStringLiteral("command"), QStringLiteral("control"),
        QStringLiteral("setReady"), QStringLiteral("chat"),
        QStringLiteral("scenarioUpsert"), QStringLiteral("scenarioRemove"),
        QStringLiteral("scenarioReplace"), QStringLiteral("resyncRequest"),
        QStringLiteral("ping"), QStringLiteral("heartbeat"),
        QStringLiteral("roomList"), QStringLiteral("joinRoom"),
        QStringLiteral("leaveRoom"), QStringLiteral("claimSeat"),
        QStringLiteral("releaseSeat"), QStringLiteral("seatReady"),
        QStringLiteral("deployment"), QStringLiteral("shareIntel"),
        QStringLiteral("mapMark"), QStringLiteral("setUnitName"),
        QStringLiteral("requestRedeploy"), QStringLiteral("redeploy")};
    return types;
}

const QSet<QString>& serverTypes() {
    static const QSet<QString> types{
        QStringLiteral("welcome"), QStringLiteral("snapshot"), QStringLiteral("delta"),
        QStringLiteral("commandResult"), QStringLiteral("event"), QStringLiteral("chat"),
        QStringLiteral("pong"), QStringLiteral("error"), QStringLiteral("roomDirectory"),
        QStringLiteral("seatState"), QStringLiteral("deploymentPrompt"),
        QStringLiteral("intelShare")};
    return types;
}

bool validIdentifier(const QJsonValue& value) {
    return value.isString() && !value.toString().isEmpty()
        && value.toString().size() <= MaxIdentifierLength;
}

bool validSeatIdentifier(const QJsonValue& value) {
    if (!validIdentifier(value)) return false;
    for (const QChar character : value.toString()) {
        if (character.isSpace() || character.isNull() || !character.isPrint()) return false;
    }
    return true;
}

bool validString(const QJsonValue& value, int maximumLength, bool allowEmpty = false) {
    return value.isString() && value.toString().size() <= maximumLength
        && (allowEmpty || !value.toString().trimmed().isEmpty());
}

bool validNonNegativeInteger(const QJsonValue& value) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= 0.0
        && number <= static_cast<double>(MaxSafeJsonInteger)
        && std::floor(number) == number;
}

bool validPoint(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject point = value.toObject();
    return point.value(QStringLiteral("x")).isDouble()
        && point.value(QStringLiteral("y")).isDouble()
        && std::isfinite(point.value(QStringLiteral("x")).toDouble())
        && std::isfinite(point.value(QStringLiteral("y")).toDouble());
}

bool validPointArray(const QJsonValue& value, bool requiresTime) {
    if (!value.isArray()) return false;
    for (const QJsonValue& item : value.toArray()) {
        if (!validPoint(item)) return false;
        if (requiresTime && (!item.toObject().value(QStringLiteral("time")).isDouble()
            || !std::isfinite(item.toObject().value(QStringLiteral("time")).toDouble()))) {
            return false;
        }
    }
    return true;
}

bool countJsonNodes(const QJsonValue& value, int depth, int* nodes) {
    if (depth > MaxJsonDepth || ++(*nodes) > MaxJsonNodes) return false;
    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            if (!countJsonNodes(item, depth + 1, nodes)) return false;
        }
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (!countJsonNodes(it.value(), depth + 1, nodes)) return false;
        }
    }
    return true;
}

ValidationResult validateComplexity(const QJsonObject& envelope) {
    int nodes = 0;
    if (!countJsonNodes(envelope, 0, &nodes)) {
        return ValidationResult::failure(QStringLiteral("MESSAGE_TOO_COMPLEX"),
                                         QStringLiteral("消息嵌套过深或字段过多"));
    }
    return ValidationResult::success();
}

ValidationResult validateCommon(const QJsonObject& envelope, bool fromServer) {
    const QJsonValue protocolVersion = envelope.value(QStringLiteral("protocolVersion"));
    if (!validNonNegativeInteger(protocolVersion) || protocolVersion.toInteger() != Version) {
        return ValidationResult::failure(QStringLiteral("PROTOCOL_MISMATCH"),
                                         QStringLiteral("协议版本不兼容"));
    }
    const QJsonValue schemaVersion = envelope.value(QStringLiteral("schemaVersion"));
    if (!validNonNegativeInteger(schemaVersion) || schemaVersion.toInteger() != SchemaVersion) {
        return ValidationResult::failure(QStringLiteral("SCHEMA_MISMATCH"),
                                         QStringLiteral("消息结构版本不兼容"));
    }
    const QJsonValue typeValue = envelope.value(QStringLiteral("type"));
    if (!typeValue.isString() || typeValue.toString().isEmpty()) {
        return ValidationResult::failure(QStringLiteral("INVALID_ENVELOPE"),
                                         QStringLiteral("消息类型缺失"));
    }
    const QString type = typeValue.toString();
    if ((fromServer && !isKnownServerMessageType(type))
        || (!fromServer && !isKnownClientMessageType(type))) {
        return ValidationResult::failure(QStringLiteral("UNKNOWN_MESSAGE"),
                                         QStringLiteral("不支持的消息类型"));
    }
    if (!envelope.value(QStringLiteral("payload")).isObject()) {
        return ValidationResult::failure(QStringLiteral("INVALID_ENVELOPE"),
                                         QStringLiteral("消息负载必须是对象"));
    }
    return ValidationResult::success();
}

} // namespace

ValidationResult ValidationResult::success() {
    return {true, QStringLiteral("OK"), {}};
}

ValidationResult ValidationResult::failure(const QString& code, const QString& message) {
    return {false, code, message};
}

namespace {

bool validOptionalString(const QJsonObject& object, const QString& field, int maximumLength,
                         bool allowEmpty = true) {
    return !object.contains(field)
        || validString(object.value(field), maximumLength, allowEmpty);
}

bool validOptionalIdentifier(const QJsonObject& object, const QString& field,
                             bool allowEmpty = false) {
    if (!object.contains(field)) return true;
    const QJsonValue value = object.value(field);
    return (allowEmpty && value.isString() && value.toString().isEmpty()) || validIdentifier(value);
}

bool projectSeat(const QJsonObject& object, SeatProjection* projection) {
    if (!validIdentifier(object.value(QStringLiteral("seatId")))
        || !validOptionalString(object, QStringLiteral("seatType"), MaxIdentifierLength)
        || !validOptionalString(object, QStringLiteral("side"), MaxIdentifierLength)
        || !validOptionalString(object, QStringLiteral("displayName"), 128)
        || (object.contains(QStringLiteral("slot"))
            && !validNonNegativeInteger(object.value(QStringLiteral("slot"))))
        || (object.contains(QStringLiteral("capacity"))
            && !validNonNegativeInteger(object.value(QStringLiteral("capacity"))))
        || (object.contains(QStringLiteral("occupied"))
            && !object.value(QStringLiteral("occupied")).isBool())
        || (object.contains(QStringLiteral("ready"))
            && !object.value(QStringLiteral("ready")).isBool())
        || (object.contains(QStringLiteral("connected"))
            && !object.value(QStringLiteral("connected")).isBool())
        || (object.contains(QStringLiteral("deployed"))
            && !object.value(QStringLiteral("deployed")).isBool())
        || (object.contains(QStringLiteral("pendingTransfer"))
            && !object.value(QStringLiteral("pendingTransfer")).isBool())
        || (object.contains(QStringLiteral("redeployRequested"))
            && !object.value(QStringLiteral("redeployRequested")).isBool())
        || (object.contains(QStringLiteral("controllerType"))
            && (!object.value(QStringLiteral("controllerType")).isString()
                || (object.value(QStringLiteral("controllerType")).toString()
                    != QLatin1String("human")
                    && object.value(QStringLiteral("controllerType")).toString()
                       != QLatin1String("ai"))))
        || !validOptionalString(object, QStringLiteral("unitId"), MaxIdentifierLength)
        || !validOptionalString(object, QStringLiteral("selectedTemplate"), MaxIdentifierLength)
        || !validOptionalString(object, QStringLiteral("unitName"), 128)) {
        return false;
    }
    if (!projection) return true;
    projection->seatId = object.value(QStringLiteral("seatId")).toString();
    projection->seatType = object.value(QStringLiteral("seatType")).toString();
    projection->side = object.value(QStringLiteral("side")).toString();
    projection->slot = object.value(QStringLiteral("slot")).toInt();
    projection->capacity = object.value(QStringLiteral("capacity")).toInt();
    projection->occupied = object.value(QStringLiteral("occupied")).toBool();
    projection->displayName = object.value(QStringLiteral("displayName")).toString();
    projection->ready = object.value(QStringLiteral("ready")).toBool();
    projection->connected = object.value(QStringLiteral("connected")).toBool();
    projection->deployed = object.value(QStringLiteral("deployed")).toBool();
    projection->pendingTransfer = object.value(QStringLiteral("pendingTransfer")).toBool();
    projection->redeployRequested = object.value(QStringLiteral("redeployRequested")).toBool();
    projection->unitId = object.value(QStringLiteral("unitId")).toString();
    projection->selectedTemplate = object.value(QStringLiteral("selectedTemplate")).toString();
    projection->unitName = object.value(QStringLiteral("unitName")).toString();
    projection->controllerType = object.contains(QStringLiteral("controllerType"))
        ? object.value(QStringLiteral("controllerType")).toString() : QStringLiteral("human");
    return true;
}

bool projectSeats(const QJsonValue& value, QList<SeatProjection>* projection) {
    if (!value.isArray()) return false;
    QList<SeatProjection> seats;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isObject()) return false;
        SeatProjection seat;
        if (!projectSeat(item.toObject(), &seat)) return false;
        seats.append(seat);
    }
    if (projection) *projection = seats;
    return true;
}

bool hasOnlyFields(const QJsonObject& object, const QSet<QString>& allowed) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowed.contains(it.key())) return false;
    }
    return true;
}

bool validFiniteNumber(const QJsonValue& value, double minimum = -HUGE_VAL,
                       double maximum = HUGE_VAL) {
    return value.isDouble() && std::isfinite(value.toDouble())
        && value.toDouble() >= minimum && value.toDouble() <= maximum;
}

bool validObserverPosition(const QJsonValue& value) {
    if (!value.isArray()) return false;
    const QJsonArray position = value.toArray();
    if (position.size() < 2 || position.size() > 3) return false;
    for (const QJsonValue& coordinate : position) {
        if (!validFiniteNumber(coordinate)) return false;
    }
    return true;
}

bool validObserverSubsystems(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject subsystems = value.toObject();
    static const QSet<QString> fields{
        QStringLiteral("sensor"), QStringLiteral("comms"),
        QStringLiteral("mobility"), QStringLiteral("weapon")};
    if (subsystems.size() != fields.size() || !hasOnlyFields(subsystems, fields)) return false;
    for (const QString& field : fields) {
        if (!validFiniteNumber(subsystems.value(field), 0.0, 1.0)) return false;
    }
    return true;
}

bool validObserverRuntimeUnit(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject unit = value.toObject();
    static const QSet<QString> allowed{
        QStringLiteral("id"), QStringLiteral("callsign"), QStringLiteral("kind"),
        QStringLiteral("side"), QStringLiteral("movable"), QStringLiteral("position"),
        QStringLiteral("detectRange"), QStringLiteral("attackRange"),
        QStringLiteral("commRange"), QStringLiteral("speed"), QStringLiteral("baseSpeed"),
        QStringLiteral("maxHp"), QStringLiteral("attackPower"), QStringLiteral("armor"),
        QStringLiteral("hp"), QStringLiteral("alive"), QStringLiteral("subsystems"),
        QStringLiteral("serviceRequested"), QStringLiteral("serviceProgress"),
        QStringLiteral("ammoRemaining"), QStringLiteral("ammoCapacity"),
        QStringLiteral("cooldownRemaining"), QStringLiteral("cooldownSec"),
        QStringLiteral("fuelRemaining"), QStringLiteral("fuelCapacity"),
        QStringLiteral("turnaroundProgress")};
    if (!hasOnlyFields(unit, allowed)) return false;
    for (const QString& field : {QStringLiteral("id"), QStringLiteral("kind")}) {
        if (!validIdentifier(unit.value(field))) return false;
    }
    if (!validString(unit.value(QStringLiteral("callsign")), 128, true)) return false;
    const QString side = unit.value(QStringLiteral("side")).toString();
    if (side != QLatin1String("red") && side != QLatin1String("blue")) return false;
    if (!unit.value(QStringLiteral("movable")).isBool()
        || !validObserverPosition(unit.value(QStringLiteral("position")))
        || !unit.value(QStringLiteral("alive")).isBool()
        || !unit.value(QStringLiteral("serviceRequested")).isBool()
        || !validObserverSubsystems(unit.value(QStringLiteral("subsystems")))) {
        return false;
    }
    for (const QString& field : {QStringLiteral("detectRange"),
                                 QStringLiteral("attackRange"), QStringLiteral("commRange"),
                                 QStringLiteral("speed"), QStringLiteral("baseSpeed"),
                                 QStringLiteral("maxHp"), QStringLiteral("hp"),
                                 QStringLiteral("attackPower")}) {
        if (!validFiniteNumber(unit.value(field), 0.0)) return false;
    }
    for (const QString& field : {QStringLiteral("cooldownRemaining"),
                                 QStringLiteral("cooldownSec"), QStringLiteral("fuelRemaining"),
                                 QStringLiteral("fuelCapacity")}) {
        if (unit.contains(field) && !validFiniteNumber(unit.value(field), 0.0)) return false;
    }
    if (!validFiniteNumber(unit.value(QStringLiteral("armor")), 0.0, 1.0)
        || !validFiniteNumber(unit.value(QStringLiteral("serviceProgress")), 0.0, 1.0)) {
        return false;
    }
    if (unit.contains(QStringLiteral("turnaroundProgress"))
        && !validFiniteNumber(unit.value(QStringLiteral("turnaroundProgress")), 0.0, 1.0)) {
        return false;
    }
    for (const QString& field : {QStringLiteral("ammoRemaining"),
                                 QStringLiteral("ammoCapacity")}) {
        if (unit.contains(field) && !validNonNegativeInteger(unit.value(field))) return false;
    }
    if (unit.value(QStringLiteral("hp")).toDouble()
            > unit.value(QStringLiteral("maxHp")).toDouble()
        || unit.value(QStringLiteral("ammoRemaining")).toInteger()
            > unit.value(QStringLiteral("ammoCapacity")).toInteger()
        || unit.value(QStringLiteral("fuelRemaining")).toDouble()
            > unit.value(QStringLiteral("fuelCapacity")).toDouble()) {
        return false;
    }
    return true;
}

bool validObserverScenario(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject scenario = value.toObject();
    static const QSet<QString> scenarioFields{
        QStringLiteral("schemaVersion"), QStringLiteral("map"), QStringLiteral("units")};
    if (!hasOnlyFields(scenario, scenarioFields)
        || !validNonNegativeInteger(scenario.value(QStringLiteral("schemaVersion")))
        || !scenario.value(QStringLiteral("map")).isObject()
        || !scenario.value(QStringLiteral("units")).isArray()) {
        return false;
    }
    for (const QJsonValue& unitValue : scenario.value(QStringLiteral("units")).toArray()) {
        if (!unitValue.isObject()) return false;
        const QJsonObject unit = unitValue.toObject();
        static const QSet<QString> allowed{
            QStringLiteral("id"), QStringLiteral("callsign"), QStringLiteral("kind"),
            QStringLiteral("side"), QStringLiteral("x"), QStringLiteral("y"),
            QStringLiteral("alt"), QStringLiteral("detectRange"),
            QStringLiteral("attackRange"), QStringLiteral("commRange"),
            QStringLiteral("speed"), QStringLiteral("maxHp"),
            QStringLiteral("attackPower"), QStringLiteral("armor"),
            QStringLiteral("ammoCapacity"), QStringLiteral("initialAmmo"),
            QStringLiteral("cooldownSec"), QStringLiteral("fuelCapacitySec"),
            QStringLiteral("initialFuelSec")};
        if (!hasOnlyFields(unit, allowed)) return false;
        for (const QString& field : {QStringLiteral("id"), QStringLiteral("kind")}) {
            if (!validIdentifier(unit.value(field))) return false;
        }
        if (!validString(unit.value(QStringLiteral("callsign")), 128, true)) return false;
        const QString side = unit.value(QStringLiteral("side")).toString();
        if (side != QLatin1String("red") && side != QLatin1String("blue")) return false;
        for (const QString& field : {QStringLiteral("x"), QStringLiteral("y"),
                                     QStringLiteral("alt")}) {
            if (!validFiniteNumber(unit.value(field))) return false;
        }
        for (const QString& field : {QStringLiteral("detectRange"),
                                     QStringLiteral("attackRange"),
                                     QStringLiteral("commRange"), QStringLiteral("speed"),
                                     QStringLiteral("maxHp"),
                                     QStringLiteral("attackPower"),
                                     QStringLiteral("cooldownSec"),
                                     QStringLiteral("fuelCapacitySec"),
                                     QStringLiteral("initialFuelSec")}) {
            if (!validFiniteNumber(unit.value(field), 0.0)) return false;
        }
        if (!validFiniteNumber(unit.value(QStringLiteral("armor")), 0.0, 1.0)
            || !validNonNegativeInteger(unit.value(QStringLiteral("ammoCapacity")))
            || !validNonNegativeInteger(unit.value(QStringLiteral("initialAmmo")))
            || unit.value(QStringLiteral("initialAmmo")).toInteger()
                > unit.value(QStringLiteral("ammoCapacity")).toInteger()
            || unit.value(QStringLiteral("initialFuelSec")).toDouble()
                > unit.value(QStringLiteral("fuelCapacitySec")).toDouble()) {
            return false;
        }
    }
    const QJsonObject map = scenario.value(QStringLiteral("map")).toObject();
    static const QSet<QString> mapFields{
        QStringLiteral("name"), QStringLiteral("widthMeters"),
        QStringLiteral("heightMeters"), QStringLiteral("backgroundResource")};
    if (map.size() != mapFields.size() || !hasOnlyFields(map, mapFields)
        || !validString(map.value(QStringLiteral("name")), MaxRoomNameLength, true)
        || !validString(map.value(QStringLiteral("backgroundResource")), 512, true)
        || !validFiniteNumber(map.value(QStringLiteral("widthMeters")), 0.0)
        || !validFiniteNumber(map.value(QStringLiteral("heightMeters")), 0.0)) {
        return false;
    }
    return true;
}

bool validObserverRoomState(const QJsonObject& roomState) {
    static const QSet<QString> allowed{
        QStringLiteral("phase"), QStringLiteral("roomId"), QStringLiteral("roomName"),
        QStringLiteral("roomStatus"), QStringLiteral("roomMode"),
        QStringLiteral("aiDifficulty"), QStringLiteral("aiEngine"),
        QStringLiteral("configVersion"),
        QStringLiteral("running"), QStringLiteral("simTime"), QStringLiteral("speed"),
        QStringLiteral("scenarioRevision"), QStringLiteral("stateRevision"),
        QStringLiteral("observer")};
    return hasOnlyFields(roomState, allowed)
        && roomState.value(QStringLiteral("observer")).isBool()
        && roomState.value(QStringLiteral("observer")).toBool();
}

bool validObserverUnits(const QJsonValue& value) {
    if (!value.isArray()) return false;
    for (const QJsonValue& unit : value.toArray()) {
        if (!validObserverRuntimeUnit(unit)) return false;
    }
    return true;
}

bool validObserverSnapshot(const QJsonObject& payload, const QJsonObject& roomState) {
    static const QSet<QString> allowed{
        QStringLiteral("schemaVersion"), QStringLiteral("stateRevision"),
        QStringLiteral("scenario"), QStringLiteral("units"), QStringLiteral("roomState")};
    return hasOnlyFields(payload, allowed) && validObserverRoomState(roomState)
        && validObserverScenario(payload.value(QStringLiteral("scenario")))
        && validObserverUnits(payload.value(QStringLiteral("units")));
}

bool validObserverDelta(const QJsonObject& payload, const QJsonObject& roomState) {
    static const QSet<QString> allowed{
        QStringLiteral("schemaVersion"), QStringLiteral("baseStateRevision"),
        QStringLiteral("stateRevision"), QStringLiteral("scenarioRevision"),
        QStringLiteral("units"), QStringLiteral("roomState")};
    return hasOnlyFields(payload, allowed) && validObserverRoomState(roomState)
        && validObserverUnits(payload.value(QStringLiteral("units")));
}

} // namespace

ValidationResult projectRoomLifecycle(const QJsonObject& roomState,
                                      RoomLifecycleProjection* projection) {
    const auto invalid = []() {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                         QStringLiteral("房间生命周期状态无效"));
    };
    for (const QString& field : {QStringLiteral("phase"), QStringLiteral("roomName"),
                                 QStringLiteral("roomStatus"), QStringLiteral("cpIssues")}) {
        if (!validOptionalString(roomState, field, 512)) return invalid();
    }
    if (!validOptionalIdentifier(roomState, QStringLiteral("roomId"))
        || !validOptionalIdentifier(roomState, QStringLiteral("phase"))
        || !validOptionalIdentifier(roomState, QStringLiteral("roomStatus"))
        || (roomState.contains(QStringLiteral("roomMode"))
            && (!roomState.value(QStringLiteral("roomMode")).isString()
                || (roomState.value(QStringLiteral("roomMode")).toString()
                    != QLatin1String("pvp")
                    && roomState.value(QStringLiteral("roomMode")).toString()
                       != QLatin1String("pve"))))
        || (roomState.contains(QStringLiteral("aiDifficulty"))
            && (!roomState.value(QStringLiteral("aiDifficulty")).isString()
                || (roomState.value(QStringLiteral("aiDifficulty")).toString()
                    != QLatin1String("easy")
                    && roomState.value(QStringLiteral("aiDifficulty")).toString()
                       != QLatin1String("normal")
                    && roomState.value(QStringLiteral("aiDifficulty")).toString()
                       != QLatin1String("hard"))))
        || (roomState.contains(QStringLiteral("aiEngine"))
            && (!roomState.value(QStringLiteral("aiEngine")).isString()
                || (roomState.value(QStringLiteral("aiEngine")).toString()
                    != QLatin1String("rules")
                    && roomState.value(QStringLiteral("aiEngine")).toString()
                       != QLatin1String("ollama"))))
        || (roomState.contains(QStringLiteral("configVersion"))
            && !validNonNegativeInteger(roomState.value(QStringLiteral("configVersion"))))) {
        return invalid();
    }
    const QString phase = roomState.value(QStringLiteral("phase")).toString(
        QStringLiteral("preparing"));
    if (phase != QLatin1String("preparing") && phase != QLatin1String("running")
        && phase != QLatin1String("paused") && phase != QLatin1String("finished")) {
        return invalid();
    }
    for (const QString& field : {QStringLiteral("redReady"), QStringLiteral("blueReady"),
                                 QStringLiteral("running"), QStringLiteral("readyForSim")}) {
        if (roomState.contains(field) && !roomState.value(field).isBool()) return invalid();
    }
    if (roomState.contains(QStringLiteral("observer"))
        && !roomState.value(QStringLiteral("observer")).isBool()) {
        return invalid();
    }
    for (const QString& field : {QStringLiteral("scenarioRevision"),
                                 QStringLiteral("stateRevision")}) {
        if (roomState.contains(field) && !validNonNegativeInteger(roomState.value(field))) {
            return invalid();
        }
    }
    for (const QString& field : {QStringLiteral("simTime"), QStringLiteral("speed")}) {
        if (roomState.contains(field) && (!roomState.value(field).isDouble()
            || !std::isfinite(roomState.value(field).toDouble()))) {
            return invalid();
        }
    }
    QList<SeatProjection> seats;
    if (roomState.contains(QStringLiteral("seats"))
        && !projectSeats(roomState.value(QStringLiteral("seats")), &seats)) {
        return invalid();
    }
    if (!projection) return ValidationResult::success();
    projection->phase = roomState.value(QStringLiteral("phase")).toString(
        QStringLiteral("preparing"));
    projection->roomId = roomState.value(QStringLiteral("roomId")).toString();
    projection->roomName = roomState.value(QStringLiteral("roomName")).toString();
    projection->roomStatus = roomState.value(QStringLiteral("roomStatus")).toString();
    projection->roomMode = roomState.contains(QStringLiteral("roomMode"))
        ? roomState.value(QStringLiteral("roomMode")).toString() : QStringLiteral("pvp");
    projection->aiDifficulty = roomState.contains(QStringLiteral("aiDifficulty"))
        ? roomState.value(QStringLiteral("aiDifficulty")).toString()
        : QStringLiteral("normal");
    projection->aiEngine = roomState.contains(QStringLiteral("aiEngine"))
        ? roomState.value(QStringLiteral("aiEngine")).toString()
        : QStringLiteral("rules");
    projection->configVersion = roomState.value(QStringLiteral("configVersion")).toInteger(1);
    projection->observer = roomState.value(QStringLiteral("observer")).toBool(false);
    projection->redReady = roomState.value(QStringLiteral("redReady")).toBool();
    projection->blueReady = roomState.value(QStringLiteral("blueReady")).toBool();
    projection->running = roomState.value(QStringLiteral("running")).toBool();
    projection->readyForSim = roomState.value(QStringLiteral("readyForSim")).toBool();
    projection->cpIssues = roomState.value(QStringLiteral("cpIssues")).toString();
    projection->simTime = roomState.value(QStringLiteral("simTime")).toDouble();
    projection->speed = roomState.value(QStringLiteral("speed")).toDouble(1.0);
    projection->scenarioRevision = roomState.value(QStringLiteral("scenarioRevision")).toInteger();
    projection->stateRevision = roomState.value(QStringLiteral("stateRevision")).toInteger();
    projection->seats = seats;
    return ValidationResult::success();
}

ValidationResult projectSnapshot(const QJsonObject& payload, SnapshotProjection* projection) {
    if (!payload.value(QStringLiteral("roomState")).isObject()) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                         QStringLiteral("完整快照缺少房间生命周期状态"));
    }
    const QJsonObject roomState = payload.value(QStringLiteral("roomState")).toObject();
    if (roomState.contains(QStringLiteral("stateRevision"))
        && payload.contains(QStringLiteral("stateRevision"))
        && roomState.value(QStringLiteral("stateRevision"))
               != payload.value(QStringLiteral("stateRevision"))) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                         QStringLiteral("完整快照状态版本不一致"));
    }
    RoomLifecycleProjection lifecycle;
    const ValidationResult result = projectRoomLifecycle(roomState, &lifecycle);
    if (!result.valid) return result;
    if (!projection) return ValidationResult::success();
    projection->lifecycle = lifecycle;
    return ValidationResult::success();
}

ValidationResult projectSeatDirectory(const QJsonObject& payload,
                                      SeatDirectoryProjection* projection) {
    if (!validIdentifier(payload.value(QStringLiteral("roomId")))
        || !validOptionalIdentifier(payload, QStringLiteral("yourSeatId"), true)) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                         QStringLiteral("战位目录身份无效"));
    }
    QList<SeatProjection> seats;
    if (!projectSeats(payload.value(QStringLiteral("seats")), &seats)) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                         QStringLiteral("战位目录结构无效"));
    }
    if (projection) {
        projection->roomId = payload.value(QStringLiteral("roomId")).toString();
        projection->yourSeatId = payload.value(QStringLiteral("yourSeatId")).toString();
        projection->seats = seats;
    }
    return ValidationResult::success();
}

ValidationResult projectDeploymentPrompt(const QJsonObject& payload,
                                         DeploymentPromptProjection* projection) {
    if (!validIdentifier(payload.value(QStringLiteral("unitId")))
        || !validString(payload.value(QStringLiteral("message")), 512)
        || !validOptionalIdentifier(payload, QStringLiteral("seatId"))
        || !validOptionalIdentifier(payload, QStringLiteral("targetSeatId"))) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                         QStringLiteral("部署提示结构无效"));
    }
    if (projection) {
        projection->unitId = payload.value(QStringLiteral("unitId")).toString();
        projection->seatId = payload.value(QStringLiteral("seatId")).toString();
        projection->targetSeatId = payload.value(QStringLiteral("targetSeatId")).toString();
        projection->message = payload.value(QStringLiteral("message")).toString();
    }
    return ValidationResult::success();
}

ValidationResult projectIntelShare(const QJsonObject& payload,
                                   IntelShareProjection* projection) {
    if (!validIdentifier(payload.value(QStringLiteral("targetId")))
        || !validIdentifier(payload.value(QStringLiteral("senderSeatId")))
        || !validOptionalString(payload, QStringLiteral("sharedAt"), 64)
        || !validOptionalString(payload, QStringLiteral("note"), 1024)) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                         QStringLiteral("情报共享消息结构无效"));
    }
    if (projection) {
        projection->senderSeatId = payload.value(QStringLiteral("senderSeatId")).toString();
        projection->targetId = payload.value(QStringLiteral("targetId")).toString();
        projection->sharedAt = payload.value(QStringLiteral("sharedAt")).toString();
        projection->note = payload.value(QStringLiteral("note")).toString();
    }
    return ValidationResult::success();
}

ValidationResult projectCommandResult(const QJsonObject& payload,
                                      CommandResultProjection* projection) {
    if (!validIdentifier(payload.value(QStringLiteral("commandId")))
        || !payload.value(QStringLiteral("accepted")).isBool()
        || !validString(payload.value(QStringLiteral("code")), MaxIdentifierLength)
        || !validString(payload.value(QStringLiteral("message")), 1024, true)
        || (payload.contains(QStringLiteral("serverTime"))
            && (!payload.value(QStringLiteral("serverTime")).isDouble()
                || !std::isfinite(payload.value(QStringLiteral("serverTime")).toDouble())
                || payload.value(QStringLiteral("serverTime")).toDouble() < 0.0))) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                         QStringLiteral("命令回执结构无效"));
    }
    if (projection) {
        projection->commandId = payload.value(QStringLiteral("commandId")).toString();
        projection->accepted = payload.value(QStringLiteral("accepted")).toBool();
        projection->code = payload.value(QStringLiteral("code")).toString();
        projection->message = payload.value(QStringLiteral("message")).toString();
        projection->serverTime = payload.value(QStringLiteral("serverTime")).toDouble();
    }
    return ValidationResult::success();
}

ValidationResult projectTransferEvent(const QJsonObject& payload,
                                      TransferEventProjection* projection) {
    const QString kind = payload.value(QStringLiteral("kind")).toString();
    const bool completion = kind == QLatin1String("transferCompleted")
        || kind == QLatin1String("transferRejected");
    if ((kind != QLatin1String("transferRequested") && !completion)
        || !validNonNegativeInteger(payload.value(QStringLiteral("revision")))
        || payload.value(QStringLiteral("revision")).toInteger() <= 0
        || !validNonNegativeInteger(payload.value(QStringLiteral("userId")))
        || payload.value(QStringLiteral("userId")).toInteger() <= 0
        || !validIdentifier(payload.value(QStringLiteral("sourceSeatId")))
        || !validIdentifier(payload.value(QStringLiteral("targetSeatId")))
        || !validIdentifier(payload.value(QStringLiteral("templateId")))
        || (completion && (!validNonNegativeInteger(payload.value(QStringLiteral("requestRevision")))
            || payload.value(QStringLiteral("requestRevision")).toInteger() <= 0))
        || !validOptionalString(payload, QStringLiteral("reason"), 256)) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                         QStringLiteral("战位切换事件结构无效"));
    }
    if (!projection) return ValidationResult::success();
    projection->kind = kind;
    projection->revision = payload.value(QStringLiteral("revision")).toInteger();
    projection->requestRevision = payload.value(QStringLiteral("requestRevision")).toInteger();
    projection->userId = payload.value(QStringLiteral("userId")).toInteger();
    projection->sourceSeatId = payload.value(QStringLiteral("sourceSeatId")).toString();
    projection->targetSeatId = payload.value(QStringLiteral("targetSeatId")).toString();
    projection->templateId = payload.value(QStringLiteral("templateId")).toString();
    projection->reason = payload.value(QStringLiteral("reason")).toString();
    return ValidationResult::success();
}

ValidationResult projectServerEvent(const QJsonObject& payload) {
    if (!validString(payload.value(QStringLiteral("kind")), MaxIdentifierLength)) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                         QStringLiteral("事件类型无效"));
    }
    const QString kind = payload.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("transferRequested")
        || kind == QLatin1String("transferCompleted")
        || kind == QLatin1String("transferRejected")) {
        return projectTransferEvent(payload, nullptr);
    }
    return ValidationResult::success();
}

QVariantList seatVariants(const QList<SeatProjection>& seats) {
    QVariantList variants;
    variants.reserve(seats.size());
    for (const SeatProjection& seat : seats) {
        variants.append(QVariantMap{{QStringLiteral("seatId"), seat.seatId},
                                    {QStringLiteral("seatType"), seat.seatType},
                                    {QStringLiteral("side"), seat.side},
                                    {QStringLiteral("slot"), seat.slot},
                                    {QStringLiteral("capacity"), seat.capacity},
                                    {QStringLiteral("occupied"), seat.occupied},
                                    {QStringLiteral("displayName"), seat.displayName},
                                    {QStringLiteral("ready"), seat.ready},
                                    {QStringLiteral("connected"), seat.connected},
                                    {QStringLiteral("deployed"), seat.deployed},
                                    {QStringLiteral("pendingTransfer"), seat.pendingTransfer},
                                    {QStringLiteral("redeployRequested"), seat.redeployRequested},
                                    {QStringLiteral("unitId"), seat.unitId},
                                    {QStringLiteral("selectedTemplate"), seat.selectedTemplate},
                                    {QStringLiteral("unitName"), seat.unitName},
                                    {QStringLiteral("controllerType"), seat.controllerType}});
    }
    return variants;
}

bool isKnownClientMessageType(const QString& type) {
    return clientTypes().contains(type);
}

bool isKnownServerMessageType(const QString& type) {
    return serverTypes().contains(type);
}

ValidationResult validateClientEnvelope(const QJsonObject& envelope) {
    ValidationResult complexity = validateComplexity(envelope);
    if (!complexity.valid) return complexity;
    ValidationResult result = validateCommon(envelope, false);
    if (!result.valid) return result;
    const QJsonValue messageId = envelope.value(QStringLiteral("messageId"));
    if (!messageId.isString() || messageId.toString().isEmpty()
        || messageId.toString().size() > MaxIdentifierLength) {
        return ValidationResult::failure(QStringLiteral("INVALID_ENVELOPE"),
                                         QStringLiteral("消息 ID 缺失或过长"));
    }
    return validateClientPayload(envelope.value(QStringLiteral("type")).toString(),
                                 envelope.value(QStringLiteral("payload")).toObject());
}

ValidationResult validateServerEnvelope(const QJsonObject& envelope) {
    ValidationResult complexity = validateComplexity(envelope);
    if (!complexity.valid) return complexity;
    ValidationResult result = validateCommon(envelope, true);
    if (!result.valid) return result;
    const QJsonValue sequence = envelope.value(QStringLiteral("sequence"));
    if (!validNonNegativeInteger(sequence) || sequence.toInteger() <= 0) {
        return ValidationResult::failure(QStringLiteral("INVALID_ENVELOPE"),
                                         QStringLiteral("服务器消息序号无效"));
    }
    return validateServerPayload(envelope.value(QStringLiteral("type")).toString(),
                                 envelope.value(QStringLiteral("payload")).toObject());
}

ValidationResult validateClientPayload(const QString& type, const QJsonObject& payload) {
    auto invalid = [](const QString& message) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"), message);
    };
    if (type == QLatin1String("auth")) {
        if (!validString(payload.value(QStringLiteral("token")), MaxTokenLength)) {
            return invalid(QStringLiteral("登录令牌缺失或过长"));
        }
        for (const QString& field : {QStringLiteral("resumeSequence"),
                                     QStringLiteral("resumeStateRevision")}) {
            if (payload.contains(field) && !validNonNegativeInteger(payload.value(field))) {
                return invalid(QStringLiteral("恢复游标无效"));
            }
        }
    } else if (type == QLatin1String("command")) {
        if (!validIdentifier(payload.value(QStringLiteral("commandId")))) {
            return invalid(QStringLiteral("命令 ID 缺失或过长"));
        }
        if (!validString(payload.value(QStringLiteral("action")), MaxActionLength)
            || !payload.value(QStringLiteral("args")).isObject()
            || !validNonNegativeInteger(payload.value(QStringLiteral("stateRevision")))
            || payload.value(QStringLiteral("stateRevision")).toInteger() <= 0) {
            return invalid(QStringLiteral("命令操作或参数无效"));
        }
        const QString action = payload.value(QStringLiteral("action")).toString();
        const QJsonObject args = payload.value(QStringLiteral("args")).toObject();
        static const QSet<QString> commandActions{
            QStringLiteral("assignTarget"), QStringLiteral("setFlightPlan"),
            QStringLiteral("unitOrder"), QStringLiteral("attackAt"),
            QStringLiteral("engageTarget"), QStringLiteral("moveTo"),
            QStringLiteral("withdraw"), QStringLiteral("setSpeed"),
            QStringLiteral("pursue"), QStringLiteral("guideAttack"),
            QStringLiteral("setSchedule"), QStringLiteral("halt"),
            QStringLiteral("service"), QStringLiteral("cancelEngagement"),
            QStringLiteral("setRoe")};
        if (!commandActions.contains(action)) return invalid(QStringLiteral("未知命令操作"));
        auto validIds = [&args](std::initializer_list<QString> fields) {
            for (const QString& field : fields) {
                if (!validIdentifier(args.value(field))) return false;
            }
            return true;
        };
        if ((action == QLatin1String("assignTarget")
             || action == QLatin1String("engageTarget")
             || action == QLatin1String("pursue"))
            && !validIds({QStringLiteral("attackerId"), QStringLiteral("targetId")})) {
            return invalid(QStringLiteral("攻击命令的单元 ID 无效"));
        }
        if (action == QLatin1String("setFlightPlan")
            && (!validIds({QStringLiteral("attackerId")})
                || args.value(QStringLiteral("waypoints")).toArray().isEmpty()
                || !validPointArray(args.value(QStringLiteral("waypoints")), false))) {
            return invalid(QStringLiteral("航路命令参数类型无效"));
        }
        if ((action == QLatin1String("attackAt") || action == QLatin1String("moveTo"))
            && (!validIds({QStringLiteral("unitId")})
                || !validPoint(args.value(QStringLiteral("pos"))))) {
            return invalid(QStringLiteral("点位命令参数类型无效"));
        }
        if (action == QLatin1String("withdraw")
            && (!validIds({QStringLiteral("unitId")})
                || (args.contains(QStringLiteral("pos"))
                    && !validPoint(args.value(QStringLiteral("pos")))))) {
            return invalid(QStringLiteral("撤离命令参数类型无效"));
        }
        if ((action == QLatin1String("halt")
             || action == QLatin1String("service")
             || action == QLatin1String("cancelEngagement"))
            && !validIds({QStringLiteral("unitId")})) {
            return invalid(QStringLiteral("单元 ID 无效"));
        }
        if (action == QLatin1String("unitOrder")
            && (!validIds({QStringLiteral("unitId")})
                || !validString(args.value(QStringLiteral("text")), 420))) {
            return invalid(QStringLiteral("文本命令参数无效"));
        }
        if (action == QLatin1String("setRoe")
            && (!validIds({QStringLiteral("unitId")})
                || !validString(args.value(QStringLiteral("roe")), 16))) {
            return invalid(QStringLiteral("交战规则命令参数无效"));
        }
        if (action == QLatin1String("setSpeed")
            && (!validIds({QStringLiteral("unitId")})
                || !args.value(QStringLiteral("speed")).isDouble())) {
            return invalid(QStringLiteral("速度命令参数类型无效"));
        }
        if (action == QLatin1String("setSchedule")
            && (!validIds({QStringLiteral("unitId")})
                || !validPointArray(args.value(QStringLiteral("schedule")), true))) {
            return invalid(QStringLiteral("计划命令参数类型无效"));
        }
        if (action == QLatin1String("guideAttack")
            && (!validIds({QStringLiteral("guideId"), QStringLiteral("attackerId"),
                           QStringLiteral("targetId")})
                || !validPoint(args.value(QStringLiteral("targetPos"))))) {
            return invalid(QStringLiteral("引导命令参数类型无效"));
        }
    } else if (type == QLatin1String("control")) {
        if (!validString(payload.value(QStringLiteral("action")), MaxActionLength)) {
            return invalid(QStringLiteral("控制操作无效"));
        }
        if (payload.contains(QStringLiteral("speed"))
            && !payload.value(QStringLiteral("speed")).isDouble()) {
            return invalid(QStringLiteral("推演速率必须是数值"));
        }
    } else if (type == QLatin1String("setReady")) {
        if (!payload.value(QStringLiteral("ready")).isBool()) {
            return invalid(QStringLiteral("就绪状态必须是布尔值"));
        }
    } else if (type == QLatin1String("chat")) {
        if (!validString(payload.value(QStringLiteral("text")), MaxChatLength)) {
            return invalid(QStringLiteral("聊天内容为空或超过 500 字"));
        }
        if (!payload.value(QStringLiteral("recipientSeatIds")).isArray()
            || payload.value(QStringLiteral("recipientSeatIds")).toArray().isEmpty()) {
            return invalid(QStringLiteral("聊天必须指定至少一个接收战位"));
        }
        for (const QJsonValue& value : payload.value(QStringLiteral("recipientSeatIds")).toArray()) {
            if (!validIdentifier(value)) return invalid(QStringLiteral("聊天接收战位 ID 无效"));
        }
    } else if (type == QLatin1String("scenarioUpsert")) {
        if (!payload.value(QStringLiteral("unit")).isObject()
            || payload.value(QStringLiteral("unit")).toObject().isEmpty()) {
            return invalid(QStringLiteral("单元数据必须是非空对象"));
        }
    } else if (type == QLatin1String("scenarioRemove")) {
        if (!validIdentifier(payload.value(QStringLiteral("unitId")))) {
            return invalid(QStringLiteral("单元 ID 缺失或过长"));
        }
    } else if (type == QLatin1String("scenarioReplace")) {
        if (!payload.value(QStringLiteral("scenario")).isObject()
            || payload.value(QStringLiteral("scenario")).toObject().isEmpty()) {
            return invalid(QStringLiteral("场景数据必须是非空对象"));
        }
    } else if (type == QLatin1String("resyncRequest")) {
        for (const QString& field : {QStringLiteral("lastSequence"),
                                     QStringLiteral("stateRevision")}) {
            if (payload.contains(field) && !validNonNegativeInteger(payload.value(field))) {
                return invalid(QStringLiteral("重同步游标无效"));
            }
        }
    } else if (type == QLatin1String("joinRoom")) {
        if (!validIdentifier(payload.value(QStringLiteral("roomId")))) {
            return invalid(QStringLiteral("房间 ID 无效"));
        }
        if (payload.contains(QStringLiteral("asObserver"))
            && !payload.value(QStringLiteral("asObserver")).isBool()) {
            return invalid(QStringLiteral("观战请求必须是布尔值"));
        }
    } else if (type == QLatin1String("leaveRoom")) {
        const bool hasSuccessor = payload.contains(QStringLiteral("successorUserId"));
        if (payload.size() != (hasSuccessor ? 1 : 0)
            || (hasSuccessor
                && (!validNonNegativeInteger(payload.value(QStringLiteral("successorUserId")))
                    || payload.value(QStringLiteral("successorUserId")).toInteger() <= 0))) {
            return invalid(QStringLiteral("离开房间的指挥权移交参数无效"));
        }
    } else if (type == QLatin1String("claimSeat")) {
        if (!validIdentifier(payload.value(QStringLiteral("seatId")))) {
            return invalid(QStringLiteral("战位 ID 无效"));
        }
        const bool approve = payload.contains(QStringLiteral("approveUserId"));
        const bool reject = payload.contains(QStringLiteral("rejectUserId"));
        const bool cancel = payload.contains(QStringLiteral("cancelTransfer"));
        if (approve || reject || cancel) {
            if ((approve && reject) || (approve && cancel) || (reject && cancel)
                || (approve && (!validNonNegativeInteger(payload.value(QStringLiteral("approveUserId")))
                    || payload.value(QStringLiteral("approveUserId")).toInteger() <= 0))
                || (reject && (!validNonNegativeInteger(payload.value(QStringLiteral("rejectUserId")))
                    || payload.value(QStringLiteral("rejectUserId")).toInteger() <= 0))
                || (cancel && !payload.value(QStringLiteral("cancelTransfer")).toBool())
                || !validNonNegativeInteger(payload.value(QStringLiteral("requestedRevision")))
                || payload.value(QStringLiteral("requestedRevision")).toInteger() <= 0) {
                return invalid(QStringLiteral("战位切换审批或版本无效"));
            }
        }
    } else if (type == QLatin1String("releaseSeat")) {
        if (!payload.isEmpty()) return invalid(QStringLiteral("释放战位请求不应包含参数"));
    } else if (type == QLatin1String("seatReady")) {
        if (!payload.value(QStringLiteral("ready")).isBool()) {
            return invalid(QStringLiteral("战位就绪状态必须是布尔值"));
        }
    } else if (type == QLatin1String("deployment")) {
        if (!validIdentifier(payload.value(QStringLiteral("unitId")))
            || !validPoint(payload.value(QStringLiteral("position")))) {
            return invalid(QStringLiteral("部署单元或位置无效"));
        }
    } else if (type == QLatin1String("requestRedeploy")) {
        if (!payload.isEmpty()) return invalid(QStringLiteral("重新部署请求不应包含参数"));
    } else if (type == QLatin1String("redeploy")) {
        if (payload.size() != 1 || !validIdentifier(payload.value(QStringLiteral("seatId")))) {
            return invalid(QStringLiteral("重新部署目标战位无效"));
        }
    } else if (type == QLatin1String("shareIntel")) {
        if (!validIdentifier(payload.value(QStringLiteral("targetId")))
            || !payload.value(QStringLiteral("recipientSeatIds")).isArray()
            || payload.value(QStringLiteral("recipientSeatIds")).toArray().isEmpty()
            || !validOptionalString(payload, QStringLiteral("note"), 1024)) {
            return invalid(QStringLiteral("情报共享目标或接收战位无效"));
        }
        for (const QJsonValue& value : payload.value(QStringLiteral("recipientSeatIds")).toArray()) {
            if (!validIdentifier(value)) return invalid(QStringLiteral("接收战位 ID 无效"));
        }
    } else if (type == QLatin1String("mapMark")) {
        if (!validPoint(payload.value(QStringLiteral("position")))
            || !validString(payload.value(QStringLiteral("label")), MaxMapLabelLength, true)) {
            return invalid(QStringLiteral("地图标记无效"));
        }
        if (payload.contains(QStringLiteral("recipientSeatIds"))) {
            if (!payload.value(QStringLiteral("recipientSeatIds")).isArray()
                || payload.value(QStringLiteral("recipientSeatIds")).toArray().size() > 64) {
                return invalid(QStringLiteral("地图标记接收战位列表无效"));
            }
            for (const QJsonValue& value
                 : payload.value(QStringLiteral("recipientSeatIds")).toArray()) {
                if (!validSeatIdentifier(value)) {
                    return invalid(QStringLiteral("地图标记接收战位 ID 无效"));
                }
            }
        }
    } else if (type == QLatin1String("setUnitName")) {
        if (!validString(payload.value(QStringLiteral("unitName")), 128)) {
            return invalid(QStringLiteral("单位显示名称无效"));
        }
    }
    return ValidationResult::success();
}

ValidationResult validateServerPayload(const QString& type, const QJsonObject& payload) {
    auto invalid = [](const QString& message) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"), message);
    };
    if (type == QLatin1String("welcome")) {
        if (!validString(payload.value(QStringLiteral("username")), 128)
            || !validString(payload.value(QStringLiteral("displayName")), 128, true)) {
            return invalid(QStringLiteral("欢迎消息中的账号身份无效"));
        }
        if (payload.contains(QStringLiteral("seatId"))
            && !validIdentifier(payload.value(QStringLiteral("seatId")))) {
            return invalid(QStringLiteral("欢迎消息中的战位无效"));
        }
        if (payload.contains(QStringLiteral("ddsTicket"))
            && !validString(payload.value(QStringLiteral("ddsTicket")), MaxDdsTicketLength)) {
            return invalid(QStringLiteral("欢迎消息中的 DDS 票据无效"));
        }
        if (payload.contains(QStringLiteral("ddsTicketExpiresAt"))
            && (!validNonNegativeInteger(payload.value(QStringLiteral("ddsTicketExpiresAt")))
                || payload.value(QStringLiteral("ddsTicketExpiresAt")).toInteger() <= 0)) {
            return invalid(QStringLiteral("欢迎消息中的 DDS 票据有效期无效"));
        }
    } else if (type == QLatin1String("snapshot")) {
        if (!validNonNegativeInteger(payload.value(QStringLiteral("schemaVersion")))
            || payload.value(QStringLiteral("schemaVersion")).toInteger() != SchemaVersion
            || !validNonNegativeInteger(payload.value(QStringLiteral("stateRevision")))
            || payload.value(QStringLiteral("stateRevision")).toInteger() <= 0
            || !payload.value(QStringLiteral("scenario")).isObject()
            || !payload.value(QStringLiteral("units")).isArray()
            || (payload.contains(QStringLiteral("messages"))
                && !payload.value(QStringLiteral("messages")).isArray())
            || !payload.value(QStringLiteral("roomState")).isObject()) {
            return invalid(QStringLiteral("完整快照结构无效"));
        }
        if (!projectSnapshot(payload, nullptr).valid) {
            return invalid(QStringLiteral("完整快照生命周期状态无效"));
        }
        const QJsonObject roomState = payload.value(QStringLiteral("roomState")).toObject();
        if (roomState.value(QStringLiteral("observer")).toBool(false)
            && !validObserverSnapshot(payload, roomState)) {
            return invalid(QStringLiteral("观察员快照结构无效"));
        }
        if (payload.contains(QStringLiteral("mapMarks"))
            && !payload.value(QStringLiteral("mapMarks")).isArray()) {
            return invalid(QStringLiteral("完整快照地图标记结构无效"));
        }
    } else if (type == QLatin1String("delta")) {
        if (!validNonNegativeInteger(payload.value(QStringLiteral("schemaVersion")))
            || payload.value(QStringLiteral("schemaVersion")).toInteger() != SchemaVersion
            || !validNonNegativeInteger(payload.value(QStringLiteral("baseStateRevision")))
            || !validNonNegativeInteger(payload.value(QStringLiteral("stateRevision")))
            || !validNonNegativeInteger(payload.value(QStringLiteral("scenarioRevision")))
            || !payload.value(QStringLiteral("units")).isArray()
            || !payload.value(QStringLiteral("roomState")).isObject()) {
            return invalid(QStringLiteral("状态增量结构无效"));
        }
        if (!projectRoomLifecycle(payload.value(QStringLiteral("roomState")).toObject(),
                                  nullptr).valid) {
            return invalid(QStringLiteral("状态增量生命周期状态无效"));
        }
        const QJsonObject roomState = payload.value(QStringLiteral("roomState")).toObject();
        if (roomState.value(QStringLiteral("observer")).toBool(false)
            && !validObserverDelta(payload, roomState)) {
            return invalid(QStringLiteral("观察员增量结构无效"));
        }
        if (payload.contains(QStringLiteral("mapMarks"))
            && !payload.value(QStringLiteral("mapMarks")).isArray()) {
            return invalid(QStringLiteral("状态增量地图标记结构无效"));
        }
    } else if (type == QLatin1String("commandResult")) {
        return projectCommandResult(payload, nullptr);
    } else if (type == QLatin1String("event")) {
        return projectServerEvent(payload);
    } else if (type == QLatin1String("error")) {
        if (!validString(payload.value(QStringLiteral("code")), MaxIdentifierLength)
            || !validString(payload.value(QStringLiteral("message")), 1024)) {
            return invalid(QStringLiteral("错误消息结构无效"));
        }
    } else if (type == QLatin1String("chat")) {
        if (!validString(payload.value(QStringLiteral("text")), MaxChatLength)) {
            return invalid(QStringLiteral("聊天消息结构无效"));
        }
    } else if (type == QLatin1String("roomDirectory")) {
        if (!payload.value(QStringLiteral("rooms")).isArray()) {
            return invalid(QStringLiteral("房间目录结构无效"));
        }
        for (const QJsonValue& value : payload.value(QStringLiteral("rooms")).toArray()) {
            if (!value.isObject()) return invalid(QStringLiteral("房间目录项无效"));
            const QJsonObject room = value.toObject();
            const QString mode = room.value(QStringLiteral("mode")).toString(
                QStringLiteral("pvp"));
            const QString difficulty = room.value(QStringLiteral("aiDifficulty")).toString(
                QStringLiteral("normal"));
            if ((mode != QLatin1String("pvp") && mode != QLatin1String("pve"))
                || (difficulty != QLatin1String("easy")
                    && difficulty != QLatin1String("normal")
                    && difficulty != QLatin1String("hard"))
                || (room.contains(QStringLiteral("configVersion"))
                    && !validNonNegativeInteger(room.value(QStringLiteral("configVersion"))))) {
                return invalid(QStringLiteral("房间模式配置无效"));
            }
        }
    } else if (type == QLatin1String("seatState")) {
        return projectSeatDirectory(payload, nullptr);
    } else if (type == QLatin1String("deploymentPrompt")) {
        return projectDeploymentPrompt(payload, nullptr);
    } else if (type == QLatin1String("intelShare")) {
        return projectIntelShare(payload, nullptr);
    }
    return ValidationResult::success();
}

QJsonObject makeClientEnvelope(const QString& type, const QString& messageId,
                               const QJsonObject& payload) {
    return {{QStringLiteral("protocolVersion"), Version},
            {QStringLiteral("schemaVersion"), SchemaVersion},
            {QStringLiteral("type"), type},
            {QStringLiteral("messageId"), messageId},
            {QStringLiteral("payload"), payload}};
}

QJsonObject makeServerEnvelope(const QString& type, quint64 sequence,
                               const QJsonObject& payload) {
    return {{QStringLiteral("protocolVersion"), Version},
            {QStringLiteral("schemaVersion"), SchemaVersion},
            {QStringLiteral("type"), type},
            {QStringLiteral("sequence"), static_cast<qint64>(sequence)},
            {QStringLiteral("sentAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("payload"), payload}};
}

} // namespace gbr::Protocol
