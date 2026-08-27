#include "Protocol.h"
#include "IntelProtocol.h"

#include <QDateTime>
#include <QByteArray>
#include <QHash>
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
        QStringLiteral("createIntelReport"), QStringLiteral("requestIntelHistory"),
        QStringLiteral("mapMark"), QStringLiteral("setUnitName"),
        QStringLiteral("requestRedeploy"), QStringLiteral("redeploy"),
        QStringLiteral("setObserverTrajectories"), QStringLiteral("setObserverTrails"),
        QStringLiteral("vmfMessage"), QStringLiteral("vmfTaskCommand"),
        QStringLiteral("demoAction"), QStringLiteral("demoControl")};
    return types;
}

const QSet<QString>& serverTypes() {
    static const QSet<QString> types{
        QStringLiteral("welcome"), QStringLiteral("snapshot"), QStringLiteral("delta"),
        QStringLiteral("commandResult"), QStringLiteral("event"), QStringLiteral("chat"),
        QStringLiteral("pong"), QStringLiteral("error"), QStringLiteral("roomDirectory"),
        QStringLiteral("seatState"), QStringLiteral("deploymentPrompt"),
        QStringLiteral("intelShare"), QStringLiteral("intelHistoryPage"),
        QStringLiteral("vmfTaskResult"), QStringLiteral("vmfTrace"),
        QStringLiteral("vmfEvent"), QStringLiteral("demoState"),
        QStringLiteral("demoTrace"), QStringLiteral("demoResult"),
        QStringLiteral("demoError")};
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

bool validAccountText(const QJsonValue& value, bool allowEmpty = false) {
    return value.isString() && (allowEmpty || !value.toString().trimmed().isEmpty());
}

bool validNonNegativeInteger(const QJsonValue& value) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= 0.0
        && number <= static_cast<double>(MaxSafeJsonInteger)
        && std::floor(number) == number;
}

bool validVmfWire(const QJsonObject& payload) {
    const QJsonValue encodedValue = payload.value(QStringLiteral("wireBytes"));
    const QJsonValue bitLengthValue = payload.value(QStringLiteral("wireBitLength"));
    if (!encodedValue.isString() || encodedValue.toString().isEmpty()
        || encodedValue.toString().size() > (MaxVmfWireBytes * 4 / 3 + 4)
        || !validNonNegativeInteger(bitLengthValue)) return false;
    const QByteArray bytes = QByteArray::fromBase64(encodedValue.toString().toLatin1());
    if (bytes.isEmpty() || bytes.size() > MaxVmfWireBytes) return false;
    const qint64 bitLength = bitLengthValue.toInteger();
    if (bytes.toBase64() != encodedValue.toString().toLatin1()) return false;
    if (bitLength <= 0 || bitLength > static_cast<qint64>(bytes.size()) * 8) return false;
    const int remainder = static_cast<int>(bitLength % 8);
    if (remainder != 0) {
        const unsigned char unusedMask = static_cast<unsigned char>((1U << (8 - remainder)) - 1U);
        if ((static_cast<unsigned char>(bytes.at(bytes.size() - 1)) & unusedMask) != 0U) return false;
    }
    return true;
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

ValidationResult validateCommon(const QJsonObject& envelope, bool fromServer,
                                bool allowCompatibleVersion) {
    const QJsonValue protocolVersion = envelope.value(QStringLiteral("protocolVersion"));
    if (!validNonNegativeInteger(protocolVersion)
        || (!allowCompatibleVersion && protocolVersion.toInteger() != Version)) {
        return ValidationResult::failure(QStringLiteral("PROTOCOL_MISMATCH"),
                                         QStringLiteral("协议版本不兼容"));
    }
    const QJsonValue schemaVersion = envelope.value(QStringLiteral("schemaVersion"));
    if (!validNonNegativeInteger(schemaVersion)
        || (!allowCompatibleVersion && schemaVersion.toInteger() != SchemaVersion)
        || (allowCompatibleVersion
            && !isSupportedWireVersion(protocolVersion.toInteger(), schemaVersion.toInteger()))) {
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

bool validOptionalAccountText(const QJsonObject& object, const QString& field) {
    return !object.contains(field) || validAccountText(object.value(field), true);
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
        || !validOptionalAccountText(object, QStringLiteral("displayName"))
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
                       != QLatin1String("ai")
                    && object.value(QStringLiteral("controllerType")).toString()
                       != QLatin1String("placeholder"))))
        || (object.contains(QStringLiteral("controlMode"))
            && (!object.value(QStringLiteral("controlMode")).isString()
                || (object.value(QStringLiteral("controlMode")).toString()
                        != QLatin1String("human")
                    && object.value(QStringLiteral("controlMode")).toString()
                        != QLatin1String("ai")
                    && object.value(QStringLiteral("controlMode")).toString()
                        != QLatin1String("vmf-auto")
                    && object.value(QStringLiteral("controlMode")).toString()
                        != QLatin1String("fixed-target"))))
        || (object.contains(QStringLiteral("claimable"))
            && !object.value(QStringLiteral("claimable")).isBool())
        || !validOptionalString(object, QStringLiteral("sourceUnitId"), MaxIdentifierLength)
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
    projection->controlMode = object.value(QStringLiteral("controlMode"))
                                  .toString(projection->controllerType == QLatin1String("ai")
                                                ? QStringLiteral("ai")
                                                : QStringLiteral("human"));
    projection->claimable = object.contains(QStringLiteral("claimable"))
        ? object.value(QStringLiteral("claimable")).toBool()
        : !projection->occupied;
    projection->sourceUnitId = object.value(QStringLiteral("sourceUnitId")).toString();
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

bool validVmfTaskCommand(const QJsonObject& payload) {
    static const QSet<QString> allowed{
        QStringLiteral("requestId"), QStringLiteral("taskId"),
        QStringLiteral("expectedTaskRevision"), QStringLiteral("action"),
        QStringLiteral("messages"), QStringLiteral("commanderSeatId"),
        QStringLiteral("reconSeatId"), QStringLiteral("attackSeatId"),
        QStringLiteral("groundSeatId"), QStringLiteral("targetId"),
        QStringLiteral("correlationId")};
    if (!hasOnlyFields(payload, allowed)
        || !validIdentifier(payload.value(QStringLiteral("requestId")))
        || !validIdentifier(payload.value(QStringLiteral("taskId")))
        || !validNonNegativeInteger(payload.value(QStringLiteral("expectedTaskRevision")))
        || !validString(payload.value(QStringLiteral("action")), MaxActionLength)
        || !payload.value(QStringLiteral("messages")).isArray()) return false;
    const QString action = payload.value(QStringLiteral("action")).toString();
    if (action == QLatin1String("createTask")) {
        if (payload.value(QStringLiteral("expectedTaskRevision")).toInteger() != 0) {
            return false;
        }
        for (const QString& field : {QStringLiteral("commanderSeatId"),
                                     QStringLiteral("reconSeatId"),
                                     QStringLiteral("attackSeatId"),
                                     QStringLiteral("groundSeatId"),
                                     QStringLiteral("targetId"),
                                     QStringLiteral("correlationId")}) {
            if (!validIdentifier(payload.value(field))) return false;
        }
    } else {
        for (const QString& field : {QStringLiteral("commanderSeatId"),
                                     QStringLiteral("reconSeatId"),
                                     QStringLiteral("attackSeatId"),
                                     QStringLiteral("groundSeatId"),
                                     QStringLiteral("targetId"),
                                     QStringLiteral("correlationId")}) {
            if (payload.contains(field)) return false;
        }
    }
    const QJsonArray messages = payload.value(QStringLiteral("messages")).toArray();
    if (messages.size() > 16
        || (action == QLatin1String("createTask") && !messages.isEmpty())) {
        return false;
    }
    for (const QJsonValue& value : messages) {
        if (!value.isObject()) return false;
        const QJsonObject message = value.toObject();
        static const QSet<QString> messageFields{
            QStringLiteral("messageId"), QStringLiteral("traceId"),
            QStringLiteral("timestamp"),
            QStringLiteral("domainType"), QStringLiteral("vmfMessage"),
            QStringLiteral("catalogId"), QStringLiteral("payload"),
            QStringLiteral("wireBytes"), QStringLiteral("wireBitLength"),
            QStringLiteral("correlationId")};
        if (!hasOnlyFields(message, messageFields)
            || !validIdentifier(message.value(QStringLiteral("messageId")))
            || !validIdentifier(message.value(QStringLiteral("traceId")))
            || !validString(message.value(QStringLiteral("timestamp")),
                            MaxIntelTimestampLength)
            || !validString(message.value(QStringLiteral("domainType")), MaxIdentifierLength)
            || !validString(message.value(QStringLiteral("vmfMessage")), MaxIdentifierLength)
            || !validIdentifier(message.value(QStringLiteral("catalogId")))
            || !message.value(QStringLiteral("payload")).isObject()
            || !validVmfWire(message)
            || !validOptionalString(message, QStringLiteral("correlationId"), MaxIdentifierLength)) {
            return false;
        }
    }
    return true;
}

bool validFiniteNumber(const QJsonValue& value, double minimum = -HUGE_VAL,
                       double maximum = HUGE_VAL) {
    return value.isDouble() && std::isfinite(value.toDouble())
        && value.toDouble() >= minimum && value.toDouble() <= maximum;
}

bool validRoomSeatBaseKey(const QString& key) {
    static const QSet<QString> keys{
        QStringLiteral("red_commander"), QStringLiteral("red_attack"),
        QStringLiteral("red_recon"), QStringLiteral("red_ground"),
        QStringLiteral("red_jammer"), QStringLiteral("blue_commander"),
        QStringLiteral("blue_attack"), QStringLiteral("blue_recon"),
        QStringLiteral("blue_ground"), QStringLiteral("blue_jammer")};
    return keys.contains(key);
}

bool validRoomSeatParameterKey(const QString& key) {
    const QStringList parts = key.split(QLatin1Char('_'));
    if (parts.size() != 2 && parts.size() != 3) return false;
    if (!validRoomSeatBaseKey(parts.at(0) + QLatin1Char('_') + parts.at(1))) return false;
    if (parts.size() == 2) return true;
    bool indexOk = false;
    const int index = parts.at(2).toInt(&indexOk);
    return indexOk && index > 0 && index <= 64;
}

bool validRoomSeatLimits(const QJsonValue& value, bool requireAllBases = false) {
    if (!value.isObject()) return false;
    const QJsonObject limits = value.toObject();
    static const QSet<QString> baseKeys{
        QStringLiteral("red_commander"), QStringLiteral("red_attack"),
        QStringLiteral("red_recon"), QStringLiteral("red_ground"),
        QStringLiteral("red_jammer"), QStringLiteral("blue_commander"),
        QStringLiteral("blue_attack"), QStringLiteral("blue_recon"),
        QStringLiteral("blue_ground"), QStringLiteral("blue_jammer")};
    if (limits.size() > baseKeys.size() || (requireAllBases && limits.size() != baseKeys.size())) {
        return false;
    }
    for (auto it = limits.constBegin(); it != limits.constEnd(); ++it) {
        if (!validRoomSeatBaseKey(it.key()) || !validNonNegativeInteger(it.value())
            || it.value().toInteger() > 64) {
            return false;
        }
        if (it.key().endsWith(QStringLiteral("_commander"))
            && it.value().toInteger() != 1) {
            return false;
        }
    }
    for (const QString& key : baseKeys) {
        if (requireAllBases && !limits.contains(key)) return false;
    }
    return true;
}

bool validRoomSeatParameters(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject parameters = value.toObject();
    if (parameters.size() > 64) return false;
    static const QSet<QString> allowed{
        QStringLiteral("communicationRange"), QStringLiteral("detectRange")};
    for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it) {
        if (!validRoomSeatParameterKey(it.key()) || !it.value().isObject()) return false;
        const QJsonObject seatParameters = it.value().toObject();
        if (!hasOnlyFields(seatParameters, allowed)) return false;
        for (auto parameter = seatParameters.constBegin(); parameter != seatParameters.constEnd(); ++parameter) {
            if (!validFiniteNumber(parameter.value(), 0.0, 1000000.0)) return false;
        }
    }
    return true;
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

bool validObserverUnitIdList(const QJsonValue& value) {
    if (!value.isArray()
        || value.toArray().size() > MaxObserverTrajectoryUnits) return false;
    QSet<QString> ids;
    for (const QJsonValue& item : value.toArray()) {
        if (!validIdentifier(item) || ids.contains(item.toString())) return false;
        ids.insert(item.toString());
    }
    return true;
}

bool validChangedUnitIdList(const QJsonValue& value) {
    if (!value.isArray() || value.toArray().size() > MaxChangedUnitIds) return false;
    QSet<QString> ids;
    for (const QJsonValue& item : value.toArray()) {
        if (!validIdentifier(item) || ids.contains(item.toString())) return false;
        ids.insert(item.toString());
    }
    return true;
}

QSet<QString> observerUnitIds(const QJsonValue& value) {
    QSet<QString> ids;
    if (!value.isArray()) return ids;
    for (const QJsonValue& item : value.toArray()) ids.insert(item.toString());
    return ids;
}

bool validTrajectoryPoint(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject point = value.toObject();
    static const QSet<QString> allowed{
        QStringLiteral("time"), QStringLiteral("simTime"),
        QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("alt")};
    if (!hasOnlyFields(point, allowed)
        || (!point.contains(QStringLiteral("time"))
            && !point.contains(QStringLiteral("simTime")))
        || !validFiniteNumber(point.value(point.contains(QStringLiteral("time"))
                                  ? QStringLiteral("time") : QStringLiteral("simTime")))
        || !validFiniteNumber(point.value(QStringLiteral("x")))
        || !validFiniteNumber(point.value(QStringLiteral("y")))) {
        return false;
    }
    return !point.contains(QStringLiteral("alt"))
        || validFiniteNumber(point.value(QStringLiteral("alt")));
}

bool validObserverTrajectories(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject trajectories = value.toObject();
    static const QSet<QString> allowed{
        QStringLiteral("selectedUnitIds"), QStringLiteral("trails")};
    if (!hasOnlyFields(trajectories, allowed)
        || !validObserverUnitIdList(trajectories.value(QStringLiteral("selectedUnitIds")))
        || !trajectories.value(QStringLiteral("trails")).isArray()) return false;
    const QSet<QString> selected = observerUnitIds(
        trajectories.value(QStringLiteral("selectedUnitIds")));
    const QJsonArray trails = trajectories.value(QStringLiteral("trails")).toArray();
    if (trails.size() > MaxObserverTrajectoryUnits) return false;
    QSet<QString> seen;
    for (const QJsonValue& value : trails) {
        if (!value.isObject()) return false;
        const QJsonObject trail = value.toObject();
        static const QSet<QString> trailFields{QStringLiteral("unitId"), QStringLiteral("points")};
        if (!hasOnlyFields(trail, trailFields)
            || !validIdentifier(trail.value(QStringLiteral("unitId")))
            || !selected.contains(trail.value(QStringLiteral("unitId")).toString())
            || seen.contains(trail.value(QStringLiteral("unitId")).toString())
            || !trail.value(QStringLiteral("points")).isArray()
            || trail.value(QStringLiteral("points")).toArray().size()
                > MaxObserverTrajectoryPoints) return false;
        seen.insert(trail.value(QStringLiteral("unitId")).toString());
        for (const QJsonValue& point : trail.value(QStringLiteral("points")).toArray()) {
            if (!validTrajectoryPoint(point)) return false;
        }
    }
    return true;
}

bool validObserverTrajectoryDelta(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject delta = value.toObject();
    static const QSet<QString> allowed{
        QStringLiteral("selectedUnitIds"), QStringLiteral("updates")};
    if (!hasOnlyFields(delta, allowed)
        || !validObserverUnitIdList(delta.value(QStringLiteral("selectedUnitIds")))
        || !delta.value(QStringLiteral("updates")).isArray()) return false;
    const QSet<QString> selected = observerUnitIds(
        delta.value(QStringLiteral("selectedUnitIds")));
    const QJsonArray updates = delta.value(QStringLiteral("updates")).toArray();
    if (updates.size() > MaxObserverTrajectoryUnits) return false;
    QSet<QString> seen;
    for (const QJsonValue& value : updates) {
        if (!value.isObject()) return false;
        const QJsonObject update = value.toObject();
        static const QSet<QString> fields{
            QStringLiteral("unitId"), QStringLiteral("points"),
            QStringLiteral("reset"), QStringLiteral("trimBefore")};
        if (!hasOnlyFields(update, fields)
            || !validIdentifier(update.value(QStringLiteral("unitId")))
            || !selected.contains(update.value(QStringLiteral("unitId")).toString())
            || seen.contains(update.value(QStringLiteral("unitId")).toString())
            || !update.value(QStringLiteral("points")).isArray()
            || update.value(QStringLiteral("points")).toArray().size()
                > MaxObserverTrajectoryPoints
            || (update.contains(QStringLiteral("reset"))
                && !update.value(QStringLiteral("reset")).isBool())
            || (update.contains(QStringLiteral("trimBefore"))
                && !validFiniteNumber(update.value(QStringLiteral("trimBefore")), 0.0))) {
            return false;
        }
        seen.insert(update.value(QStringLiteral("unitId")).toString());
        for (const QJsonValue& point : update.value(QStringLiteral("points")).toArray()) {
            if (!validTrajectoryPoint(point)) return false;
        }
    }
    return true;
}

bool validProjectileShape(const QJsonValue& value, bool anonymous) {
    if (!value.isObject()) return false;
    const QJsonObject projectile = value.toObject();
    static const QSet<QString> allowed{
        QStringLiteral("id"), QStringLiteral("side"), QStringLiteral("position"),
        QStringLiteral("headingRad"), QStringLiteral("speed"), QStringLiteral("age"),
        QStringLiteral("lifetime"), QStringLiteral("active"),
        QStringLiteral("terminalReason"), QStringLiteral("terminalAge"),
        QStringLiteral("resultSettled"), QStringLiteral("threatRadius"), QStringLiteral("attackerId"),
        QStringLiteral("targetId")};
    if (!hasOnlyFields(projectile, allowed)
        || !validIdentifier(projectile.value(QStringLiteral("id")))
        || !validObserverPosition(projectile.value(QStringLiteral("position")))
        || !validFiniteNumber(projectile.value(QStringLiteral("headingRad")))
        || !validFiniteNumber(projectile.value(QStringLiteral("speed")), 0.0)
        || !validFiniteNumber(projectile.value(QStringLiteral("age")), 0.0)
        || !validFiniteNumber(projectile.value(QStringLiteral("lifetime")), 0.000001)
        || !projectile.value(QStringLiteral("active")).isBool()
        || !projectile.value(QStringLiteral("resultSettled")).isBool()
        || !validOptionalString(projectile, QStringLiteral("terminalReason"),
                                MaxIdentifierLength, true)
        || (projectile.contains(QStringLiteral("terminalAge"))
            && !validFiniteNumber(projectile.value(QStringLiteral("terminalAge")), 0.0))
        || (projectile.contains(QStringLiteral("threatRadius"))
            && !validFiniteNumber(projectile.value(QStringLiteral("threatRadius")), 0.0))
        || !validOptionalIdentifier(projectile, QStringLiteral("attackerId"))
        || !validOptionalIdentifier(projectile, QStringLiteral("targetId"))) {
        return false;
    }
    const QString side = projectile.value(QStringLiteral("side")).toString();
    if (side != QLatin1String("red") && side != QLatin1String("blue")) return false;
    if (anonymous && (projectile.contains(QStringLiteral("attackerId"))
                      || projectile.contains(QStringLiteral("targetId")))) {
        return false;
    }
    const bool active = projectile.value(QStringLiteral("active")).toBool();
    const QString terminalReason = projectile.value(QStringLiteral("terminalReason")).toString();
    static const QSet<QString> terminalReasons{
        QStringLiteral("hit"), QStringLiteral("miss"), QStringLiteral("expired"),
        QStringLiteral("out_of_bounds"), QStringLiteral("out_of_range"),
        QStringLiteral("target_lost"),
        QStringLiteral("countermeasured")};
    const bool resultSettled = projectile.value(QStringLiteral("resultSettled")).toBool();
    if ((active && (!terminalReason.isEmpty() || resultSettled))
        || (!active && (!terminalReasons.contains(terminalReason) || !resultSettled))) {
        return false;
    }
    const QString attackerId = projectile.value(QStringLiteral("attackerId")).toString();
    const QString targetId = projectile.value(QStringLiteral("targetId")).toString();
    return attackerId.isEmpty() || targetId.isEmpty() || attackerId != targetId;
}

bool validProjectileArrayShape(const QJsonValue& value, bool anonymous) {
    if (!value.isArray()) return false;
    const QJsonArray projectiles = value.toArray();
    if (projectiles.size() > MaxProjectileRecords) return false;
    QSet<QString> ids;
    int activeCount = 0;
    for (const QJsonValue& projectileValue : projectiles) {
        if (!validProjectileShape(projectileValue, anonymous)) return false;
        const QJsonObject projectile = projectileValue.toObject();
        const QString id = projectile.value(QStringLiteral("id")).toString();
        if (ids.contains(id)) return false;
        ids.insert(id);
        if (projectile.value(QStringLiteral("active")).toBool() && ++activeCount > MaxProjectiles) {
            return false;
        }
    }
    return true;
}

bool validProjectedProjectiles(const QJsonObject& payload, bool anonymous) {
    const QJsonValue projectilesValue = payload.value(QStringLiteral("projectiles"));
    if (!validProjectileArrayShape(projectilesValue, anonymous)) return false;
    const QJsonArray projectiles = projectilesValue.toArray();
    if (projectiles.isEmpty()) return true;

    const QJsonObject scenario = payload.value(QStringLiteral("scenario")).toObject();
    const QJsonObject map = scenario.value(QStringLiteral("map")).toObject();
    const double width = map.value(QStringLiteral("widthMeters")).toDouble(-1.0);
    const double height = map.value(QStringLiteral("heightMeters")).toDouble(-1.0);
    if (!std::isfinite(width) || width <= 0.0
        || !std::isfinite(height) || height <= 0.0) {
        return false;
    }

    QHash<QString, QString> unitSides;
    const auto collectUnitSides = [&unitSides](const QJsonValue& unitsValue) {
        if (!unitsValue.isArray()) return false;
        for (const QJsonValue& unitValue : unitsValue.toArray()) {
            if (!unitValue.isObject()) return false;
            const QJsonObject unit = unitValue.toObject();
            const QString id = unit.value(QStringLiteral("id")).toString();
            const QString side = unit.value(QStringLiteral("side")).toString();
            if (id.isEmpty()) continue;
            if (!validIdentifier(unit.value(QStringLiteral("id")))
                || (side != QLatin1String("red") && side != QLatin1String("blue"))
                || (unitSides.contains(id) && unitSides.value(id) != side)) {
                return false;
            }
            unitSides.insert(id, side);
        }
        return true;
    };
    if (!collectUnitSides(scenario.value(QStringLiteral("units")))
        || !collectUnitSides(payload.value(QStringLiteral("units")))) {
        return false;
    }

    for (const QJsonValue& projectileValue : projectiles) {
        const QJsonObject projectile = projectileValue.toObject();
        const QJsonArray position = projectile.value(QStringLiteral("position")).toArray();
        const double x = position.at(0).toDouble();
        const double y = position.at(1).toDouble();
        if (x < 0.0 || x > width || y < 0.0 || y > height) return false;
        const QString side = projectile.value(QStringLiteral("side")).toString();
        const QString attackerId = projectile.value(QStringLiteral("attackerId")).toString();
        const QString targetId = projectile.value(QStringLiteral("targetId")).toString();
        if ((!attackerId.isEmpty()
             && (!unitSides.contains(attackerId) || unitSides.value(attackerId) != side))
            || (!targetId.isEmpty()
                && (!unitSides.contains(targetId) || unitSides.value(targetId) == side))) {
            return false;
        }
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

bool validActionCapabilities(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject capabilities = value.toObject();
    if (capabilities.size() > 32) return false;
    static const QSet<QString> actionNames{
        QStringLiteral("assignTarget"), QStringLiteral("setFlightPlan"),
        QStringLiteral("unitOrder"), QStringLiteral("attackAt"),
        QStringLiteral("engageTarget"), QStringLiteral("moveTo"),
        QStringLiteral("withdraw"), QStringLiteral("setSpeed"),
        QStringLiteral("pursue"), QStringLiteral("guideAttack"),
        QStringLiteral("setSchedule"), QStringLiteral("halt"),
        QStringLiteral("service"), QStringLiteral("cancelEngagement"),
        QStringLiteral("setRoe"), QStringLiteral("activateCountermeasure"),
        QStringLiteral("activateScan"), QStringLiteral("attemptFieldRepair"),
        QStringLiteral("cancelService")};
    for (auto it = capabilities.constBegin(); it != capabilities.constEnd(); ++it) {
        if (!actionNames.contains(it.key())) return false;
        if (it.value().isBool()) continue;
        if (!it.value().isObject()) return false;
        const QJsonObject entry = it.value().toObject();
        static const QSet<QString> entryFields{
            QStringLiteral("visible"), QStringLiteral("enabled"), QStringLiteral("reason")};
        if (!hasOnlyFields(entry, entryFields)
            || !entry.value(QStringLiteral("visible")).isBool()
            || !entry.value(QStringLiteral("enabled")).isBool()
            || (entry.contains(QStringLiteral("reason"))
                && !validString(entry.value(QStringLiteral("reason")), 256, true))) {
            return false;
        }
    }
    return true;
}

bool validRuntimeActionFields(const QJsonValue& value) {
    if (!value.isArray()) return false;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isObject()) return false;
        const QJsonObject unit = item.toObject();
        if (unit.contains(QStringLiteral("actions"))
            && unit.contains(QStringLiteral("actionCapabilities"))) {
            return false;
        }
        for (const QString& field : {QStringLiteral("actions"),
                                     QStringLiteral("actionCapabilities")}) {
            if (unit.contains(field) && !validActionCapabilities(unit.value(field))) return false;
        }
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
        QStringLiteral("maxCommandedSpeed"),
        QStringLiteral("maxHp"), QStringLiteral("attackPower"), QStringLiteral("armor"),
        QStringLiteral("hp"), QStringLiteral("alive"), QStringLiteral("subsystems"),
        QStringLiteral("serviceRequested"), QStringLiteral("serviceProgress"),
        QStringLiteral("ammoRemaining"), QStringLiteral("ammoCapacity"),
        QStringLiteral("cooldownRemaining"), QStringLiteral("cooldownSec"),
        QStringLiteral("activeProjectileCount"),
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
                                 QStringLiteral("maxCommandedSpeed"),
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
                                 QStringLiteral("ammoCapacity"),
                                 QStringLiteral("activeProjectileCount")}) {
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
        QStringLiteral("schemaVersion"), QStringLiteral("map"), QStringLiteral("units"),
        QStringLiteral("communicationPolicy")};
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
            QStringLiteral("id"), QStringLiteral("vmfUrn"), QStringLiteral("callsign"), QStringLiteral("kind"),
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
        if (unit.contains(QStringLiteral("vmfUrn"))
            && !validIdentifier(unit.value(QStringLiteral("vmfUrn")))) return false;
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
    if (scenario.contains(QStringLiteral("communicationPolicy"))) {
        const QJsonObject policy = scenario.value(QStringLiteral("communicationPolicy")).toObject();
        static const QSet<QString> fields{
            QStringLiteral("format"), QStringLiteral("vmfProfile"),
            QStringLiteral("ackTimeoutSec"), QStringLiteral("maxRetries"),
            QStringLiteral("automaticAck")};
        const QString format = policy.value(QStringLiteral("format")).toString();
        if (!hasOnlyFields(policy, fields)
            || (format != QLatin1String("native") && format != QLatin1String("vmf-design-v1"))
            || !validString(policy.value(QStringLiteral("vmfProfile")), MaxIdentifierLength, true)
            || !validFiniteNumber(policy.value(QStringLiteral("ackTimeoutSec")), 0.000001)
            || !validNonNegativeInteger(policy.value(QStringLiteral("maxRetries")))
            || !policy.value(QStringLiteral("automaticAck")).isBool()) {
            return false;
        }
    }
    return true;
}

bool validVmfWorkflow(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject workflow = value.toObject();
    static const QSet<QString> fields{
        QStringLiteral("taskId"), QStringLiteral("stage"), QStringLiteral("side"),
        QStringLiteral("reconId"), QStringLiteral("targetId"), QStringLiteral("attackerId"),
        QStringLiteral("guideId"), QStringLiteral("correlationId"),
        QStringLiteral("createdAt"), QStringLiteral("updatedAt")};
    if (!hasOnlyFields(workflow, fields)) return false;
    for (const QString& field : {QStringLiteral("taskId"), QStringLiteral("stage"),
                                 QStringLiteral("side"), QStringLiteral("reconId"),
                                 QStringLiteral("targetId"),
                                 QStringLiteral("attackerId"), QStringLiteral("guideId"),
                                 QStringLiteral("correlationId")}) {
        if (!validOptionalString(workflow, field, MaxIdentifierLength, true)) return false;
    }
    const QString stage = workflow.value(QStringLiteral("stage")).toString();
    static const QSet<QString> stages{
        QStringLiteral("idle"), QStringLiteral("targetReported"),
        QStringLiteral("dispatchPending"), QStringLiteral("strikeDispatched"),
        QStringLiteral("groundGuidancePending"), QStringLiteral("engaging"),
        QStringLiteral("targetDestroyed"), QStringLiteral("withdrawPending"),
        QStringLiteral("withdrawn"), QStringLiteral("failed")};
    if (!stages.contains(stage)) return false;
    const QString side = workflow.value(QStringLiteral("side")).toString();
    if (side != QLatin1String("red") && side != QLatin1String("blue")) return false;
    for (const QString& field : {QStringLiteral("createdAt"), QStringLiteral("updatedAt")}) {
        if (workflow.contains(field) && !validFiniteNumber(workflow.value(field), 0.0)) {
            return false;
        }
    }
    if (workflow.contains(QStringLiteral("createdAt"))
        && workflow.contains(QStringLiteral("updatedAt"))
        && workflow.value(QStringLiteral("createdAt")).toDouble()
               > workflow.value(QStringLiteral("updatedAt")).toDouble()) {
        return false;
    }
    return !workflow.value(QStringLiteral("taskId")).toString().isEmpty();
}

bool validVmfWorkflowMap(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject workflows = value.toObject();
    static const QSet<QString> sides{QStringLiteral("red"), QStringLiteral("blue")};
    if (!hasOnlyFields(workflows, sides)) return false;
    for (const QString& side : sides) {
        if (!validVmfWorkflow(workflows.value(side))) return false;
    }
    return true;
}

bool validVmfAutomation(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject automation = value.toObject();
    static const QSet<QString> fields{
        QStringLiteral("enabled"), QStringLiteral("humanStagesRemainManual"),
        QStringLiteral("timeoutTakeover")};
    if (!hasOnlyFields(automation, fields)) return false;
    for (const QString& field : fields) {
        if (automation.contains(field) && !automation.value(field).isBool()) return false;
    }
    return true;
}

bool validDemoState(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject state = value.toObject();
    static const QSet<QString> phases{
        QStringLiteral("target-report"), QStringLiteral("route-planning"),
        QStringLiteral("guidance-command"), QStringLiteral("ground-guidance"),
        QStringLiteral("destruction-confirmation"), QStringLiteral("return")};
    static const QSet<QString> statuses{
        QStringLiteral("active"), QStringLiteral("paused"), QStringLiteral("completed")};
    if (state.value(QStringLiteral("schemaVersion")).toInt() != 1
        || state.value(QStringLiteral("profile")).toString()
            != QLatin1String("vmf-demo-v2")
        || !validNonNegativeInteger(state.value(QStringLiteral("generation")))
        || state.value(QStringLiteral("generation")).toInteger() <= 0
        || !validNonNegativeInteger(state.value(QStringLiteral("revision")))
        || state.value(QStringLiteral("revision")).toInteger() <= 0
        || !phases.contains(state.value(QStringLiteral("phase")).toString())
        || !statuses.contains(state.value(QStringLiteral("status")).toString())
        || !validOptionalString(state, QStringLiteral("substep"), MaxIdentifierLength)
        || !validOptionalString(state, QStringLiteral("activeSeat"), MaxSeatIdLength)
        || !validOptionalString(state, QStringLiteral("expectedAction"), MaxActionLength)
        || !state.value(QStringLiteral("phases")).isArray()
        || state.value(QStringLiteral("phases")).toArray().size() != 6
        || !state.value(QStringLiteral("targetState")).isObject()) {
        return false;
    }
    return !state.contains(QStringLiteral("traces"))
        || (state.value(QStringLiteral("traces")).isArray()
            && state.value(QStringLiteral("traces")).toArray().size() <= 200);
}

bool validObserverRoomState(const QJsonObject& roomState) {
    static const QSet<QString> allowed{
        QStringLiteral("phase"), QStringLiteral("roomId"), QStringLiteral("roomName"),
        QStringLiteral("roomStatus"), QStringLiteral("roomMode"),
        QStringLiteral("aiDifficulty"), QStringLiteral("aiEngine"),
        QStringLiteral("configVersion"),
        QStringLiteral("running"), QStringLiteral("simTime"), QStringLiteral("speed"),
        QStringLiteral("scenarioRevision"), QStringLiteral("stateRevision"),
        QStringLiteral("observer"), QStringLiteral("vmfWorkflows"),
        QStringLiteral("protocolProfile"), QStringLiteral("operationMode"),
        QStringLiteral("participantSide"), QStringLiteral("fixedTargetSide"),
        QStringLiteral("scenarioEditable"), QStringLiteral("vmfAutomation"),
        QStringLiteral("vmfTasks"), QStringLiteral("demoState")};
    return hasOnlyFields(roomState, allowed)
        && roomState.value(QStringLiteral("observer")).isBool()
        && roomState.value(QStringLiteral("observer")).toBool()
        && (!roomState.contains(QStringLiteral("vmfWorkflows"))
            || roomState.value(QStringLiteral("vmfWorkflows")).isObject())
        && (!roomState.contains(QStringLiteral("protocolProfile"))
            || (roomState.value(QStringLiteral("protocolProfile")).isString()
                && (roomState.value(QStringLiteral("protocolProfile")).toString()
                        == QLatin1String("native")
                    || roomState.value(QStringLiteral("protocolProfile")).toString()
                        == QLatin1String("vmf-guided-strike-v1")
                    || roomState.value(QStringLiteral("protocolProfile")).toString()
                        == QLatin1String("vmf-demo-v2"))))
        && (!roomState.contains(QStringLiteral("operationMode"))
            || (roomState.value(QStringLiteral("operationMode")).isString()
                && (roomState.value(QStringLiteral("operationMode")).toString()
                        == QLatin1String("standard")
                    || roomState.value(QStringLiteral("operationMode")).toString()
                        == QLatin1String("vmf-single-side"))))
        && (!roomState.contains(QStringLiteral("participantSide"))
            || (roomState.value(QStringLiteral("participantSide")).isString()
                && (roomState.value(QStringLiteral("participantSide")).toString().isEmpty()
                    || roomState.value(QStringLiteral("participantSide")).toString()
                        == QLatin1String("red"))))
        && (!roomState.contains(QStringLiteral("fixedTargetSide"))
            || (roomState.value(QStringLiteral("fixedTargetSide")).isString()
                && (roomState.value(QStringLiteral("fixedTargetSide")).toString().isEmpty()
                    || roomState.value(QStringLiteral("fixedTargetSide")).toString()
                        == QLatin1String("blue"))))
        && (!roomState.contains(QStringLiteral("scenarioEditable"))
            || roomState.value(QStringLiteral("scenarioEditable")).isBool())
        && (!roomState.contains(QStringLiteral("vmfAutomation"))
            || validVmfAutomation(roomState.value(QStringLiteral("vmfAutomation"))))
        && (!roomState.contains(QStringLiteral("vmfTasks"))
            || roomState.value(QStringLiteral("vmfTasks")).isObject())
        && (!roomState.contains(QStringLiteral("demoState"))
            || validDemoState(roomState.value(QStringLiteral("demoState"))));
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
        QStringLiteral("scenario"), QStringLiteral("units"), QStringLiteral("projectiles"),
        QStringLiteral("roomState"), QStringLiteral("observerTrajectories"),
        QStringLiteral("intelState"),
        // Older servers included an empty participant map-mark collection in
        // observer projections. Accept that inert legacy shape so a client
        // upgrade does not turn an observer join into a fatal protocol error.
        QStringLiteral("mapMarks")};
    return hasOnlyFields(payload, allowed) && validObserverRoomState(roomState)
        && validObserverScenario(payload.value(QStringLiteral("scenario")))
        && validObserverUnits(payload.value(QStringLiteral("units")))
        && validProjectedProjectiles(payload, true)
        && (!payload.contains(QStringLiteral("observerTrajectories"))
            || validObserverTrajectories(payload.value(QStringLiteral("observerTrajectories"))))
        && (!payload.contains(QStringLiteral("mapMarks"))
            || (payload.value(QStringLiteral("mapMarks")).isArray()
                && payload.value(QStringLiteral("mapMarks")).toArray().isEmpty()));
}

bool validObserverDelta(const QJsonObject& payload, const QJsonObject& roomState) {
    static const QSet<QString> allowed{
        QStringLiteral("schemaVersion"), QStringLiteral("baseStateRevision"),
        QStringLiteral("stateRevision"), QStringLiteral("scenarioRevision"),
        QStringLiteral("units"), QStringLiteral("changedUnitIds"),
        QStringLiteral("projectiles"), QStringLiteral("roomState"),
        QStringLiteral("observerTrajectoryDelta"),
        QStringLiteral("mapMarks")};
    return hasOnlyFields(payload, allowed) && validObserverRoomState(roomState)
        && validObserverUnits(payload.value(QStringLiteral("units")))
        && (!payload.contains(QStringLiteral("changedUnitIds"))
            || validChangedUnitIdList(payload.value(QStringLiteral("changedUnitIds"))))
        && (!payload.contains(QStringLiteral("observerTrajectoryDelta"))
            || validObserverTrajectoryDelta(payload.value(QStringLiteral("observerTrajectoryDelta"))))
        && (!payload.contains(QStringLiteral("projectiles"))
            || validProjectileArrayShape(payload.value(QStringLiteral("projectiles")), true))
        && (!payload.contains(QStringLiteral("mapMarks"))
            || (payload.value(QStringLiteral("mapMarks")).isArray()
                && payload.value(QStringLiteral("mapMarks")).toArray().isEmpty()));
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
            && !validNonNegativeInteger(roomState.value(QStringLiteral("configVersion"))))
        || (roomState.contains(QStringLiteral("roomDescription"))
            && !validString(roomState.value(QStringLiteral("roomDescription")),
                            MaxRoomDescriptionLength, true))
        || (roomState.contains(QStringLiteral("scenarioId"))
            && !validString(roomState.value(QStringLiteral("scenarioId")), 128))
        || (roomState.contains(QStringLiteral("seatLimits"))
            && !validRoomSeatLimits(roomState.value(QStringLiteral("seatLimits"))))
        || (roomState.contains(QStringLiteral("seatParameters"))
            && !validRoomSeatParameters(roomState.value(QStringLiteral("seatParameters"))))
        || (roomState.contains(QStringLiteral("vmfWorkflow"))
            && !validVmfWorkflow(roomState.value(QStringLiteral("vmfWorkflow"))))
        || (roomState.contains(QStringLiteral("vmfWorkflows"))
            && !validVmfWorkflowMap(roomState.value(QStringLiteral("vmfWorkflows"))))
        || (roomState.contains(QStringLiteral("protocolProfile"))
            && (!validString(roomState.value(QStringLiteral("protocolProfile")), 64)
                || (roomState.value(QStringLiteral("protocolProfile")).toString()
                    != QLatin1String("native")
                    && roomState.value(QStringLiteral("protocolProfile")).toString()
                       != QLatin1String("vmf-guided-strike-v1")
                    && roomState.value(QStringLiteral("protocolProfile")).toString()
                       != QLatin1String("vmf-demo-v2"))))
        || (roomState.contains(QStringLiteral("operationMode"))
            && (!roomState.value(QStringLiteral("operationMode")).isString()
                || (roomState.value(QStringLiteral("operationMode")).toString()
                        != QLatin1String("standard")
                && roomState.value(QStringLiteral("operationMode")).toString()
                        != QLatin1String("vmf-single-side"))))
        || (roomState.contains(QStringLiteral("participantSide"))
            && (!roomState.value(QStringLiteral("participantSide")).isString()
                || (roomState.value(QStringLiteral("participantSide")).toString()
                        != QLatin1String("red")
                    && !roomState.value(QStringLiteral("participantSide"))
                            .toString().isEmpty())))
        || (roomState.contains(QStringLiteral("fixedTargetSide"))
            && (!roomState.value(QStringLiteral("fixedTargetSide")).isString()
                || (roomState.value(QStringLiteral("fixedTargetSide")).toString()
                        != QLatin1String("blue")
                    && !roomState.value(QStringLiteral("fixedTargetSide"))
                            .toString().isEmpty())))
        || (roomState.contains(QStringLiteral("scenarioEditable"))
            && !roomState.value(QStringLiteral("scenarioEditable")).isBool())
        || (roomState.contains(QStringLiteral("vmfAutomation"))
            && !validVmfAutomation(roomState.value(QStringLiteral("vmfAutomation"))))
        || (roomState.contains(QStringLiteral("vmfTasks"))
            && !roomState.value(QStringLiteral("vmfTasks")).isObject())
        || (roomState.contains(QStringLiteral("demoState"))
            && !validDemoState(roomState.value(QStringLiteral("demoState"))))) {
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
    projection->roomDescription = roomState.value(QStringLiteral("roomDescription")).toString();
    projection->scenarioId = roomState.value(QStringLiteral("scenarioId")).toString(
        QStringLiteral("default"));
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
    projection->seatLimits = roomState.value(QStringLiteral("seatLimits")).toObject();
    projection->seatParameters = roomState.value(QStringLiteral("seatParameters")).toObject();
    projection->vmfWorkflow = roomState.value(QStringLiteral("vmfWorkflow")).toObject();
    projection->vmfWorkflows = roomState.value(QStringLiteral("vmfWorkflows")).toObject();
    projection->protocolProfile = roomState.value(QStringLiteral("protocolProfile"))
                                      .toString(QStringLiteral("native"));
    projection->operationMode = roomState.value(QStringLiteral("operationMode"))
                                    .toString(QStringLiteral("standard"));
    projection->participantSide = roomState.value(QStringLiteral("participantSide")).toString();
    projection->fixedTargetSide = roomState.value(QStringLiteral("fixedTargetSide")).toString();
    projection->scenarioEditable = roomState.value(QStringLiteral("scenarioEditable")).toBool();
    projection->vmfAutomation = roomState.value(QStringLiteral("vmfAutomation")).toObject();
    projection->vmfTasks = roomState.value(QStringLiteral("vmfTasks")).toObject();
    projection->demoState = roomState.value(QStringLiteral("demoState")).toObject();
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

ValidationResult validateSnapshotState(const QJsonObject& payload, int schemaVersion) {
    const auto invalid = [](const QString& message) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"), message);
    };
    if (!isSupportedWireVersion(schemaVersion, schemaVersion)
        || !validNonNegativeInteger(payload.value(QStringLiteral("schemaVersion")))
        || payload.value(QStringLiteral("schemaVersion")).toInteger() != schemaVersion
        || !validNonNegativeInteger(payload.value(QStringLiteral("stateRevision")))
        || payload.value(QStringLiteral("stateRevision")).toInteger() <= 0
        || !payload.value(QStringLiteral("scenario")).isObject()
        || !payload.value(QStringLiteral("units")).isArray()
        || !validRuntimeActionFields(payload.value(QStringLiteral("units")))
        || !payload.value(QStringLiteral("projectiles")).isArray()
        || !payload.value(QStringLiteral("roomState")).isObject()
        || (payload.contains(QStringLiteral("messages"))
            && !payload.value(QStringLiteral("messages")).isArray())
        || (payload.contains(QStringLiteral("mapMarks"))
            && !payload.value(QStringLiteral("mapMarks")).isArray())) {
        return invalid(QStringLiteral("完整快照结构无效"));
    }

    const QJsonObject roomState = payload.value(QStringLiteral("roomState")).toObject();
    const ValidationResult lifecycle = projectSnapshot(payload, nullptr);
    if (!lifecycle.valid) return lifecycle;
    const bool observer = roomState.value(QStringLiteral("observer")).toBool(false);
    if (payload.contains(QStringLiteral("intelState"))) {
        if (schemaVersion < IntelSchemaVersion || observer
            || !validateIntelState(payload.value(QStringLiteral("intelState")).toObject()).valid) {
            return invalid(QStringLiteral("情报台账投影无效"));
        }
    }
    if (!validProjectedProjectiles(payload, observer)) {
        return invalid(QStringLiteral("完整快照导弹状态无效"));
    }
    if (observer && !validObserverSnapshot(payload, roomState)) {
        return invalid(QStringLiteral("观察员快照结构无效"));
    }
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
    if (!validIdentifier(payload.value(QStringLiteral("intelId")))
        || !validIdentifier(payload.value(QStringLiteral("senderSeatId")))
        || !validOptionalIdentifier(payload, QStringLiteral("targetId"), true)
        || !validOptionalString(payload, QStringLiteral("sharedAt"), 64)
        || !validOptionalString(payload, QStringLiteral("note"), 1024)) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                         QStringLiteral("情报共享消息结构无效"));
    }
    if (projection) {
        projection->senderSeatId = payload.value(QStringLiteral("senderSeatId")).toString();
        projection->intelId = payload.value(QStringLiteral("intelId")).toString();
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

ValidationResult projectVmfTaskResult(const QJsonObject& payload,
                                      VmfTaskResultProjection* projection) {
    const QString status = payload.value(QStringLiteral("status")).toString();
    if ((status != QLatin1String("accepted") && status != QLatin1String("blocked")
         && status != QLatin1String("rejected"))
        || !validNonNegativeInteger(payload.value(QStringLiteral("taskRevision")))
        || !payload.value(QStringLiteral("messageIds")).isArray()
        || payload.value(QStringLiteral("messageIds")).toArray().size() > 16
        || !validString(payload.value(QStringLiteral("code")), MaxIdentifierLength)
        || (payload.contains(QStringLiteral("retryable"))
            && !payload.value(QStringLiteral("retryable")).isBool())) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                         QStringLiteral("VMF 任务回执结构无效"));
    }
    QStringList messageIds;
    for (const QJsonValue& value : payload.value(QStringLiteral("messageIds")).toArray()) {
        if (!validIdentifier(value)) {
            return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"),
                                             QStringLiteral("VMF 任务消息 ID 无效"));
        }
        messageIds.append(value.toString());
    }
    if (projection) {
        projection->status = status;
        projection->taskRevision = payload.value(QStringLiteral("taskRevision")).toInteger();
        projection->messageIds = messageIds;
        projection->code = payload.value(QStringLiteral("code")).toString();
        projection->retryable = payload.value(QStringLiteral("retryable")).toBool();
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
                                    {QStringLiteral("controllerType"), seat.controllerType},
                                    {QStringLiteral("controlMode"), seat.controlMode},
                                    {QStringLiteral("claimable"), seat.claimable},
                                    {QStringLiteral("sourceUnitId"), seat.sourceUnitId}});
    }
    return variants;
}

bool isKnownClientMessageType(const QString& type) {
    return clientTypes().contains(type);
}

bool isKnownServerMessageType(const QString& type) {
    return serverTypes().contains(type);
}

bool isSupportedWireVersion(int protocolVersion, int schemaVersion) {
    return (protocolVersion == Version && schemaVersion == SchemaVersion)
        || (protocolVersion == PreviousVersion && schemaVersion == PreviousSchemaVersion)
        || (protocolVersion == OlderVersion && schemaVersion == OlderSchemaVersion)
        || (protocolVersion == LegacyVersion && schemaVersion == LegacySchemaVersion);
}

ValidationResult validateClientEnvelopeInternal(const QJsonObject& envelope,
                                                bool allowCompatibleVersion) {
    ValidationResult complexity = validateComplexity(envelope);
    if (!complexity.valid) return complexity;
    ValidationResult result = validateCommon(envelope, false, allowCompatibleVersion);
    if (!result.valid) return result;
    const QJsonValue messageId = envelope.value(QStringLiteral("messageId"));
    if (!messageId.isString() || messageId.toString().isEmpty()
        || messageId.toString().size() > MaxIdentifierLength) {
        return ValidationResult::failure(QStringLiteral("INVALID_ENVELOPE"),
                                         QStringLiteral("消息 ID 缺失或过长"));
    }
    const ValidationResult payloadValidation = validateClientPayloadForVersion(
        envelope.value(QStringLiteral("type")).toString(),
        envelope.value(QStringLiteral("payload")).toObject(),
        envelope.value(QStringLiteral("schemaVersion")).toInteger());
    if (!payloadValidation.valid) return payloadValidation;
    if (envelope.value(QStringLiteral("type")) == QLatin1String("vmfTaskCommand")
        && envelope.value(QStringLiteral("payload")).toObject()
               .value(QStringLiteral("requestId")) != messageId) {
        return ValidationResult::failure(
            QStringLiteral("INVALID_ENVELOPE"),
            QStringLiteral("严格 VMF requestId 必须与 envelope messageId 一致"));
    }
    if ((envelope.value(QStringLiteral("type")) == QLatin1String("demoAction")
         || envelope.value(QStringLiteral("type")) == QLatin1String("demoControl"))
        && envelope.value(QStringLiteral("payload")).toObject()
               .value(QStringLiteral("requestId")) != messageId) {
        return ValidationResult::failure(
            QStringLiteral("INVALID_ENVELOPE"),
            QStringLiteral("演示模式 requestId 必须与 envelope messageId 一致"));
    }
    return ValidationResult::success();
}

ValidationResult validateServerEnvelopeInternal(const QJsonObject& envelope,
                                                bool allowCompatibleVersion) {
    ValidationResult complexity = validateComplexity(envelope);
    if (!complexity.valid) return complexity;
    ValidationResult result = validateCommon(envelope, true, allowCompatibleVersion);
    if (!result.valid) return result;
    const QJsonValue sequence = envelope.value(QStringLiteral("sequence"));
    if (!validNonNegativeInteger(sequence) || sequence.toInteger() <= 0) {
        return ValidationResult::failure(QStringLiteral("INVALID_ENVELOPE"),
                                         QStringLiteral("服务器消息序号无效"));
    }
    return validateServerPayloadForVersion(
        envelope.value(QStringLiteral("type")).toString(),
        envelope.value(QStringLiteral("payload")).toObject(),
        envelope.value(QStringLiteral("schemaVersion")).toInteger());
}

ValidationResult validateClientEnvelope(const QJsonObject& envelope) {
    return validateClientEnvelopeInternal(envelope, false);
}

ValidationResult validateServerEnvelope(const QJsonObject& envelope) {
    return validateServerEnvelopeInternal(envelope, false);
}

ValidationResult validateClientEnvelopeForVersion(const QJsonObject& envelope) {
    return validateClientEnvelopeInternal(envelope, true);
}

ValidationResult validateServerEnvelopeForVersion(const QJsonObject& envelope) {
    return validateServerEnvelopeInternal(envelope, true);
}

ValidationResult validateClientPayloadForVersion(const QString& type,
                                                 const QJsonObject& payload,
                                                 int schemaVersion) {
    auto invalid = [](const QString& message) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"), message);
    };
    if ((type == QLatin1String("vmfTaskCommand") || type == QLatin1String("demoAction")
         || type == QLatin1String("demoControl")) && schemaVersion != SchemaVersion) {
        return invalid(QStringLiteral("严格 VMF 任务需要协议 schema 8"));
    }
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
            QStringLiteral("setRoe"), QStringLiteral("activateCountermeasure"),
            QStringLiteral("activateScan"), QStringLiteral("attemptFieldRepair"),
            QStringLiteral("cancelService"), QStringLiteral("updateRoomConfig")};
        if (!commandActions.contains(action)) return invalid(QStringLiteral("未知命令操作"));
        auto validIds = [&args](std::initializer_list<QString> fields) {
            for (const QString& field : fields) {
                if (!validIdentifier(args.value(field))) return false;
            }
            return true;
        };
        if (action == QLatin1String("updateRoomConfig")) {
            static const QSet<QString> fields{
                QStringLiteral("expectedConfigVersion"), QStringLiteral("name"),
                QStringLiteral("description"), QStringLiteral("scenarioId"),
                QStringLiteral("seatLimits"), QStringLiteral("seatParameters"),
                QStringLiteral("protocolProfile")};
            if (!hasOnlyFields(args, fields)
                || !validNonNegativeInteger(args.value(QStringLiteral("expectedConfigVersion")))
                || args.value(QStringLiteral("expectedConfigVersion")).toInteger() <= 0
                || !validString(args.value(QStringLiteral("name")), MaxRoomNameLength)
                || !validOptionalString(args, QStringLiteral("description"),
                                        MaxRoomDescriptionLength, true)
                || !validString(args.value(QStringLiteral("scenarioId")), 128)
                || (args.contains(QStringLiteral("protocolProfile"))
                    && (args.value(QStringLiteral("protocolProfile")).toString()
                            != QLatin1String("native")
                        && args.value(QStringLiteral("protocolProfile")).toString()
                            != QLatin1String("vmf-guided-strike-v1")
                        && args.value(QStringLiteral("protocolProfile")).toString()
                            != QLatin1String("vmf-demo-v2")))
                || (args.contains(QStringLiteral("seatLimits"))
                    && !validRoomSeatLimits(args.value(QStringLiteral("seatLimits")), true))
                || !validRoomSeatParameters(args.value(QStringLiteral("seatParameters")))) {
                return invalid(QStringLiteral("房间配置结构无效"));
            }
        } else if ((action == QLatin1String("assignTarget")
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
             || action == QLatin1String("activateCountermeasure")
             || action == QLatin1String("activateScan")
             || action == QLatin1String("attemptFieldRepair")
             || action == QLatin1String("cancelService")
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
        if (schemaVersion == LegacySchemaVersion) {
            if (!validIdentifier(payload.value(QStringLiteral("targetId")))
                || !payload.value(QStringLiteral("recipientSeatIds")).isArray()
                || payload.value(QStringLiteral("recipientSeatIds")).toArray().isEmpty()
                || !validOptionalString(payload, QStringLiteral("note"),
                                        LegacyShareIntelNoteLength)) {
                return invalid(QStringLiteral("情报共享目标或接收战位无效"));
            }
            for (const QJsonValue& value
                 : payload.value(QStringLiteral("recipientSeatIds")).toArray()) {
                if (!validIdentifier(value)) return invalid(QStringLiteral("接收战位 ID 无效"));
            }
        } else {
            const ValidationResult intel = validateIntelShareRequest(payload);
            if (!intel.valid) return intel;
        }
    } else if (type == QLatin1String("createIntelReport")) {
        if (schemaVersion == LegacySchemaVersion) {
            return invalid(QStringLiteral("当前协议版本不支持人工情报"));
        }
        static const QSet<QString> fields{QStringLiteral("position"), QStringLiteral("type"),
                                          QStringLiteral("title"), QStringLiteral("note")};
        if (!hasOnlyFields(payload, fields) || payload.size() > fields.size()) {
            return invalid(QStringLiteral("人工情报字段无效"));
        }
        if (!payload.value(QStringLiteral("position")).isObject()
            || !validPoint(payload.value(QStringLiteral("position")))
            || !validString(payload.value(QStringLiteral("type")), MaxIdentifierLength)
            || !validOptionalString(payload, QStringLiteral("title"), MaxIntelTitleLength)
            || !validOptionalString(payload, QStringLiteral("note"), MaxIntelNoteLength)) {
            return invalid(QStringLiteral("人工情报报告结构无效"));
        }
    } else if (type == QLatin1String("requestIntelHistory")) {
        if (schemaVersion == LegacySchemaVersion) {
            return invalid(QStringLiteral("当前协议版本不支持情报历史"));
        }
        return validateIntelHistoryQuery(payload);
    } else if (type == QLatin1String("setObserverTrajectories")
               || type == QLatin1String("setObserverTrails")) {
        if (payload.size() != 1
            || !validObserverUnitIdList(payload.value(QStringLiteral("unitIds")))) {
            return invalid(QStringLiteral("旁观轨迹选择无效，最多选择 8 个单位"));
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
    } else if (type == QLatin1String("vmfMessage")) {
        static const QSet<QString> fields{
            QStringLiteral("traceId"), QStringLiteral("correlationId"),
            QStringLiteral("vmfMessage"), QStringLiteral("wireFormat"),
            QStringLiteral("wireBytes"), QStringLiteral("wireBitLength"),
            QStringLiteral("senderUnitId"), QStringLiteral("receiverUnitId"),
            QStringLiteral("messageType"), QStringLiteral("payload"),
            QStringLiteral("requiresAck"), QStringLiteral("retryCount"),
            QStringLiteral("fieldCount")};
        if (!hasOnlyFields(payload, fields)
            || !validString(payload.value(QStringLiteral("traceId")), MaxIdentifierLength)
            || !validOptionalString(payload, QStringLiteral("correlationId"), MaxIdentifierLength)
            || payload.value(QStringLiteral("wireFormat")).toString()
                   != QLatin1String("vmf-design-v1")
            || !validString(payload.value(QStringLiteral("vmfMessage")), MaxIdentifierLength)
            || (payload.value(QStringLiteral("vmfMessage")).toString()
                    != QLatin1String("NetworkMonitoring")
                && payload.value(QStringLiteral("vmfMessage")).toString()
                    != QLatin1String("Land Route")
                && payload.value(QStringLiteral("vmfMessage")).toString()
                    != QLatin1String("Target Report"))
            || !validVmfWire(payload)
            || !validIdentifier(payload.value(QStringLiteral("senderUnitId")))) {
            return invalid(QStringLiteral("VMF 消息载荷或发送单元无效"));
        }
        const QJsonValue receiver = payload.value(QStringLiteral("receiverUnitId"));
        if (!(receiver.isString() && (receiver.toString() == QLatin1String("*")
                                      || validIdentifier(receiver)))) {
            return invalid(QStringLiteral("VMF 接收单元无效"));
        }
        static const QSet<QString> messageTypes{
            QStringLiteral("PositionReport"), QStringLiteral("TargetDetect"),
            QStringLiteral("TargetReport"), QStringLiteral("TargetTrack"),
            QStringLiteral("TargetDestroyed"), QStringLiteral("UnitOrder"),
            QStringLiteral("AttackOrder"), QStringLiteral("StrikePlan"),
            QStringLiteral("FlightPlan"), QStringLiteral("Guidance"),
            QStringLiteral("GroundGuideOrder"), QStringLiteral("GroundAttackConfirm"),
            QStringLiteral("Withdraw"), QStringLiteral("WithdrawOrder"),
            QStringLiteral("CommCheck"), QStringLiteral("EngagementReport"),
            QStringLiteral("SharedDetect"), QStringLiteral("Pursue"),
            QStringLiteral("Halt"), QStringLiteral("CancelEngagement"),
            QStringLiteral("SetRulesOfEngagement"), QStringLiteral("IdentityReport"),
            QStringLiteral("GroundTargetReport"), QStringLiteral("RouteAcceptance"),
            QStringLiteral("AttackReadyReport"), QStringLiteral("AttackAuthorization"),
            QStringLiteral("BattleDamageReport"), QStringLiteral("DamageAssessmentConfirm")};
        if (!messageTypes.contains(payload.value(QStringLiteral("messageType")).toString())
            || (payload.contains(QStringLiteral("payload"))
                && !payload.value(QStringLiteral("payload")).isObject())
            || (payload.contains(QStringLiteral("requiresAck"))
                && !payload.value(QStringLiteral("requiresAck")).isBool())
            || (payload.contains(QStringLiteral("retryCount"))
                && (!validNonNegativeInteger(payload.value(QStringLiteral("retryCount")))
                    || payload.value(QStringLiteral("retryCount")).toInteger() > 16))
            || (payload.contains(QStringLiteral("fieldCount"))
                && (!validNonNegativeInteger(payload.value(QStringLiteral("fieldCount")))
                    || payload.value(QStringLiteral("fieldCount")).toInteger() > 4096))) {
            return invalid(QStringLiteral("VMF 消息类型或附加字段无效"));
        }
    } else if (type == QLatin1String("vmfTaskCommand")) {
        if (!validVmfTaskCommand(payload)) {
            return invalid(QStringLiteral("严格 VMF 任务命令结构无效"));
        }
    } else if (type == QLatin1String("demoAction")) {
        static const QSet<QString> fields{
            QStringLiteral("requestId"), QStringLiteral("actionId"),
            QStringLiteral("expectedRevision"), QStringLiteral("seat"),
            QStringLiteral("action"), QStringLiteral("phase"),
            QStringLiteral("inputMode"), QStringLiteral("payload")};
        static const QSet<QString> actions{
            QStringLiteral("reportTarget"), QStringLiteral("planRoute"),
            QStringLiteral("acceptRoute"), QStringLiteral("issueGuidance"),
            QStringLiteral("acknowledgeGuidance"), QStringLiteral("confirmGroundGuidance"),
            QStringLiteral("reportDamage"), QStringLiteral("confirmDestroyed"),
            QStringLiteral("orderReturn"), QStringLiteral("confirmReturned")};
        const QString inputMode = payload.value(QStringLiteral("inputMode")).toString();
        const QJsonObject data = payload.value(QStringLiteral("payload")).toObject();
        if (!hasOnlyFields(payload, fields)
            || !validIdentifier(payload.value(QStringLiteral("requestId")))
            || !validIdentifier(payload.value(QStringLiteral("actionId")))
            || !validNonNegativeInteger(payload.value(QStringLiteral("expectedRevision")))
            || payload.value(QStringLiteral("expectedRevision")).toInteger() <= 0
            || !validOptionalString(payload, QStringLiteral("seat"), MaxSeatIdLength)
            || !actions.contains(payload.value(QStringLiteral("action")).toString())
            || !validString(payload.value(QStringLiteral("phase")), MaxIdentifierLength)
            || (inputMode != QLatin1String("template") && inputMode != QLatin1String("xml"))
            || !payload.value(QStringLiteral("payload")).isObject()
            || (inputMode == QLatin1String("xml")
                && !validString(data.value(QStringLiteral("xml")), MaxVmfWireBytes))) {
            return invalid(QStringLiteral("演示模式动作结构无效"));
        }
    } else if (type == QLatin1String("demoControl")) {
        static const QSet<QString> fields{QStringLiteral("requestId"),
                                          QStringLiteral("expectedRevision"),
                                          QStringLiteral("action"),
                                          QStringLiteral("payload")};
        static const QSet<QString> actions{QStringLiteral("reset"), QStringLiteral("jump"),
                                           QStringLiteral("pause"), QStringLiteral("resume"),
                                           QStringLiteral("setTargetScript")};
        if (!hasOnlyFields(payload, fields)
            || !validIdentifier(payload.value(QStringLiteral("requestId")))
            || !validNonNegativeInteger(payload.value(QStringLiteral("expectedRevision")))
            || payload.value(QStringLiteral("expectedRevision")).toInteger() <= 0
            || !actions.contains(payload.value(QStringLiteral("action")).toString())
            || !payload.value(QStringLiteral("payload")).isObject()) {
            return invalid(QStringLiteral("演示模式导演控制结构无效"));
        }
    }
    return ValidationResult::success();
}

ValidationResult validateServerPayloadForVersion(const QString& type,
                                                 const QJsonObject& payload,
                                                 int schemaVersion) {
    auto invalid = [](const QString& message) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"), message);
    };
    if ((type == QLatin1String("vmfTaskResult") || type == QLatin1String("vmfTrace")
         || type == QLatin1String("demoState") || type == QLatin1String("demoTrace")
         || type == QLatin1String("demoResult") || type == QLatin1String("demoError"))
        && schemaVersion != SchemaVersion) {
        return invalid(QStringLiteral("严格 VMF 服务器消息需要协议 schema 8"));
    }
    if (type == QLatin1String("welcome")) {
        if (!validAccountText(payload.value(QStringLiteral("username")))
            || !validAccountText(payload.value(QStringLiteral("displayName")), true)) {
            return invalid(QStringLiteral("欢迎消息中的账号身份无效"));
        }
        if (payload.contains(QStringLiteral("role"))) {
            const QString role = payload.value(QStringLiteral("role")).toString();
            static const QSet<QString> fixedRoles{
                QStringLiteral("player"), QStringLiteral("room_admin"),
                QStringLiteral("editor"), QStringLiteral("director"),
                QStringLiteral("red"), QStringLiteral("blue"),
                QStringLiteral("observer")};
            if (!validString(payload.value(QStringLiteral("role")), MaxIdentifierLength)
                || (!fixedRoles.contains(role) && !validSeatIdentifier(payload.value(QStringLiteral("role"))))) {
                return invalid(QStringLiteral("欢迎消息中的账号角色无效"));
            }
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
        const ValidationResult snapshot = validateSnapshotState(payload, schemaVersion);
        if (!snapshot.valid) return snapshot;
    } else if (type == QLatin1String("delta")) {
        if (!validNonNegativeInteger(payload.value(QStringLiteral("schemaVersion")))
            || payload.value(QStringLiteral("schemaVersion")).toInteger() != schemaVersion
            || !validNonNegativeInteger(payload.value(QStringLiteral("baseStateRevision")))
            || !validNonNegativeInteger(payload.value(QStringLiteral("stateRevision")))
            || payload.value(QStringLiteral("stateRevision")).toInteger()
                   <= payload.value(QStringLiteral("baseStateRevision")).toInteger()
            || !validNonNegativeInteger(payload.value(QStringLiteral("scenarioRevision")))
            || !payload.value(QStringLiteral("units")).isArray()
            || !validRuntimeActionFields(payload.value(QStringLiteral("units")))
            || (payload.contains(QStringLiteral("changedUnitIds"))
                && !validChangedUnitIdList(payload.value(QStringLiteral("changedUnitIds"))))
            || (payload.contains(QStringLiteral("projectiles"))
                && !validProjectileArrayShape(payload.value(QStringLiteral("projectiles")),
                                              false))
            || !payload.value(QStringLiteral("roomState")).isObject()) {
            return invalid(QStringLiteral("状态增量结构无效"));
        }
        if (schemaVersion == LegacySchemaVersion
            && (payload.contains(QStringLiteral("intelState"))
                || payload.contains(QStringLiteral("intelDelta")))) {
            return invalid(QStringLiteral("旧版状态增量包含未知情报字段"));
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
        if (payload.contains(QStringLiteral("intelDelta"))) {
            const QJsonObject intelDelta = payload.value(QStringLiteral("intelDelta")).toObject();
            if (intelDelta.isEmpty()
                || !validNonNegativeInteger(intelDelta.value(QStringLiteral("baseRevision")))
                || !validNonNegativeInteger(intelDelta.value(QStringLiteral("revision")))
                || intelDelta.value(QStringLiteral("revision")).toInteger()
                       < intelDelta.value(QStringLiteral("baseRevision")).toInteger()
                || !intelDelta.value(QStringLiteral("upserts")).isArray()
                || !intelDelta.value(QStringLiteral("archivedIntelIds")).isArray()
                || !intelDelta.value(QStringLiteral("deletedIntelIds")).isArray()
                || !intelDelta.value(QStringLiteral("shareTargets")).isArray()) {
                return invalid(QStringLiteral("情报状态增量结构无效"));
            }
            if (intelDelta.value(QStringLiteral("revision")).toInteger()
                    == intelDelta.value(QStringLiteral("baseRevision")).toInteger()
                && (!intelDelta.value(QStringLiteral("upserts")).toArray().isEmpty()
                    || !intelDelta.value(QStringLiteral("archivedIntelIds")).toArray().isEmpty()
                    || !intelDelta.value(QStringLiteral("deletedIntelIds")).toArray().isEmpty())) {
                return invalid(QStringLiteral("相同情报版本不能包含记录变更"));
            }
            QSet<QString> upsertIds;
            QJsonArray projectedRecords;
            for (const QJsonValue& value : intelDelta.value(QStringLiteral("upserts")).toArray()) {
                if (!value.isObject()) return invalid(QStringLiteral("情报增量项目必须是对象"));
                IntelContact contact;
                const ValidationResult contactValidation = fromJson(value.toObject(), &contact);
                if (!contactValidation.valid || upsertIds.contains(contact.intelId)) {
                    return invalid(QStringLiteral("情报增量项目无效或重复"));
                }
                upsertIds.insert(contact.intelId);
                projectedRecords.append(contact.toJson());
            }
            auto validIntelIdList = [](const QJsonValue& value, const QSet<QString>& forbidden) {
                if (!value.isArray() || value.toArray().size() > MaxIntelRecords) return false;
                QSet<QString> ids;
                for (const QJsonValue& item : value.toArray()) {
                    if (!validIdentifier(item) || ids.contains(item.toString())
                        || forbidden.contains(item.toString())) return false;
                    ids.insert(item.toString());
                }
                return true;
            };
            if (!validIntelIdList(intelDelta.value(QStringLiteral("archivedIntelIds")), {})
                || !validIntelIdList(intelDelta.value(QStringLiteral("deletedIntelIds")), upsertIds)
                || !validateIntelState(QJsonObject{
                       {QStringLiteral("revision"), intelDelta.value(QStringLiteral("revision"))},
                       {QStringLiteral("records"), projectedRecords},
                       {QStringLiteral("shareTargets"), intelDelta.value(QStringLiteral("shareTargets"))}}).valid) {
                return invalid(QStringLiteral("情报状态增量项目无效"));
            }
            for (const QJsonValue& value : intelDelta.value(QStringLiteral("archivedIntelIds")).toArray()) {
                const QJsonObject record = projectedRecords.isEmpty()
                    ? QJsonObject{} : [&]() {
                          for (const QJsonValue& item : projectedRecords) {
                              if (item.toObject().value(QStringLiteral("intelId")) == value) {
                                  return item.toObject();
                              }
                          }
                          return QJsonObject{};
                      }();
                if (record.isEmpty() || record.value(QStringLiteral("freshness"))
                                       != QLatin1String("archived")) {
                    return invalid(QStringLiteral("情报归档增量缺少归档记录"));
                }
            }
        }
    } else if (type == QLatin1String("commandResult")) {
        return projectCommandResult(payload, nullptr);
    } else if (type == QLatin1String("vmfTaskResult")) {
        return projectVmfTaskResult(payload, nullptr);
    } else if (type == QLatin1String("vmfTrace")) {
        if (!validIdentifier(payload.value(QStringLiteral("traceId")))
            || !validIdentifier(payload.value(QStringLiteral("taskId")))) {
            return invalid(QStringLiteral("VMF trace 标识无效"));
        }
    } else if (type == QLatin1String("demoState")) {
        if (!validDemoState(payload)) return invalid(QStringLiteral("演示状态无效"));
    } else if (type == QLatin1String("demoTrace")) {
        if (!validIdentifier(payload.value(QStringLiteral("traceId")))
            || !validIdentifier(payload.value(QStringLiteral("actionId")))
            || !validString(payload.value(QStringLiteral("canonicalXml")), MaxVmfWireBytes)
            || !validString(payload.value(QStringLiteral("decodedXml")), MaxVmfWireBytes)
            || !payload.value(QStringLiteral("fields")).isArray()
            || payload.value(QStringLiteral("fields")).toArray().size() > 4096
            || !validNonNegativeInteger(payload.value(QStringLiteral("wireBitLength")))) {
            return invalid(QStringLiteral("演示 VMF trace 无效"));
        }
    } else if (type == QLatin1String("demoResult")) {
        const QString status = payload.value(QStringLiteral("status")).toString();
        if (!validIdentifier(payload.value(QStringLiteral("requestId")))
            || (status != QLatin1String("accepted") && status != QLatin1String("duplicate")
                && status != QLatin1String("paused"))
            || !validString(payload.value(QStringLiteral("code")), MaxIdentifierLength)
            || !validNonNegativeInteger(payload.value(QStringLiteral("revision")))
            || !validDemoState(payload.value(QStringLiteral("state")))) {
            return invalid(QStringLiteral("演示动作结果无效"));
        }
    } else if (type == QLatin1String("demoError")) {
        if (!validString(payload.value(QStringLiteral("code")), MaxIdentifierLength)
            || !validString(payload.value(QStringLiteral("message")), 1024)
            || !validOptionalString(payload, QStringLiteral("requestId"), MaxIdentifierLength)) {
            return invalid(QStringLiteral("演示错误消息无效"));
        }
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
            if (!validString(room.value(QStringLiteral("roomId")), MaxIdentifierLength)
                || !validString(room.value(QStringLiteral("name")), MaxRoomNameLength)
                || !validOptionalString(room, QStringLiteral("description"),
                                        MaxRoomDescriptionLength, true)
                || (room.contains(QStringLiteral("seatLimits"))
                    && !validRoomSeatLimits(room.value(QStringLiteral("seatLimits"))))
                || (room.contains(QStringLiteral("seatParameters"))
                    && !validRoomSeatParameters(room.value(QStringLiteral("seatParameters"))))
                || (mode != QLatin1String("pvp") && mode != QLatin1String("pve"))
                || (difficulty != QLatin1String("easy")
                    && difficulty != QLatin1String("normal")
                    && difficulty != QLatin1String("hard"))
                || (room.contains(QStringLiteral("configVersion"))
                    && !validNonNegativeInteger(room.value(QStringLiteral("configVersion"))))
                || (room.contains(QStringLiteral("protocolProfile"))
                    && (room.value(QStringLiteral("protocolProfile")).toString()
                            != QLatin1String("native")
                        && room.value(QStringLiteral("protocolProfile")).toString()
                            != QLatin1String("vmf-guided-strike-v1")
                        && room.value(QStringLiteral("protocolProfile")).toString()
                            != QLatin1String("vmf-demo-v2")))) {
                return invalid(QStringLiteral("房间模式配置无效"));
            }
        }
    } else if (type == QLatin1String("seatState")) {
        return projectSeatDirectory(payload, nullptr);
    } else if (type == QLatin1String("deploymentPrompt")) {
        return projectDeploymentPrompt(payload, nullptr);
    } else if (type == QLatin1String("intelShare")) {
        if (schemaVersion == LegacySchemaVersion) {
            if (!validIdentifier(payload.value(QStringLiteral("senderSeatId")))
                || !validIdentifier(payload.value(QStringLiteral("targetId")))
                || !validOptionalString(payload, QStringLiteral("sharedAt"), MaxIntelTimestampLength)
                || !validOptionalString(payload, QStringLiteral("note"),
                                        LegacyShareIntelNoteLength)) {
                return invalid(QStringLiteral("旧版情报共享消息结构无效"));
            }
            return ValidationResult::success();
        }
        return projectIntelShare(payload, nullptr);
    } else if (type == QLatin1String("intelHistoryPage")) {
        if (schemaVersion == LegacySchemaVersion) {
            return invalid(QStringLiteral("当前协议版本不支持情报历史"));
        }
        return validateIntelHistoryPage(payload);
    } else if (type == QLatin1String("vmfEvent")) {
        static const QSet<QString> fields{
            QStringLiteral("kind"), QStringLiteral("messageId"),
            QStringLiteral("traceId"), QStringLiteral("correlationId"),
            QStringLiteral("vmfMessage"), QStringLiteral("wireFormat"),
            QStringLiteral("wireBitLength"), QStringLiteral("senderUnitId"),
            QStringLiteral("receiverUnitId"), QStringLiteral("messageType"),
            QStringLiteral("validated"), QStringLiteral("fieldCount"),
            QStringLiteral("retryCount"), QStringLiteral("acked"),
            QStringLiteral("catalogId"), QStringLiteral("trigger"),
            QStringLiteral("informationValue"),
            QStringLiteral("senderSide"), QStringLiteral("receiverSide"),
            QStringLiteral("summary"), QStringLiteral("workflow"),
            QStringLiteral("wireBytes"), QStringLiteral("fields"),
            QStringLiteral("canonicalXml"), QStringLiteral("decodedXml"),
            QStringLiteral("roundTripEqual"), QStringLiteral("ackState")};
        if (!hasOnlyFields(payload, fields)
            || !validString(payload.value(QStringLiteral("kind")), MaxIdentifierLength)
            || !validIdentifier(payload.value(QStringLiteral("messageId")))
            || !validOptionalString(payload, QStringLiteral("traceId"), MaxIdentifierLength)
            || !validOptionalString(payload, QStringLiteral("correlationId"), MaxIdentifierLength)
            || !validString(payload.value(QStringLiteral("vmfMessage")), MaxIdentifierLength)
            || payload.value(QStringLiteral("wireFormat")).toString()
                   != QLatin1String("vmf-design-v1")
            || !validNonNegativeInteger(payload.value(QStringLiteral("wireBitLength")))
            || payload.value(QStringLiteral("wireBitLength")).toInteger() <= 0
            || !validIdentifier(payload.value(QStringLiteral("senderUnitId")))
            || !(payload.value(QStringLiteral("receiverUnitId")).isString())
            || !validString(payload.value(QStringLiteral("messageType")), MaxIdentifierLength)
            || !payload.value(QStringLiteral("validated")).isBool()
            || !validNonNegativeInteger(payload.value(QStringLiteral("fieldCount")))
            || payload.value(QStringLiteral("fieldCount")).toInteger() > 4096
            || (payload.contains(QStringLiteral("retryCount"))
                && !validNonNegativeInteger(payload.value(QStringLiteral("retryCount"))))
            || (payload.contains(QStringLiteral("acked"))
                && !payload.value(QStringLiteral("acked")).isBool())
            || (payload.contains(QStringLiteral("catalogId"))
                && !validString(payload.value(QStringLiteral("catalogId")), MaxIdentifierLength))
            || (payload.contains(QStringLiteral("trigger"))
                && !validString(payload.value(QStringLiteral("trigger")), MaxIdentifierLength))
            || (payload.contains(QStringLiteral("informationValue"))
                && !payload.value(QStringLiteral("informationValue")).isObject())
            || !validOptionalString(payload, QStringLiteral("senderSide"), MaxIdentifierLength)
            || !validOptionalString(payload, QStringLiteral("receiverSide"), MaxIdentifierLength)
            || (payload.contains(QStringLiteral("workflow"))
                && !validVmfWorkflow(payload.value(QStringLiteral("workflow"))))
            || (payload.contains(QStringLiteral("wireBytes"))
                && !payload.value(QStringLiteral("wireBytes")).isString())
            || (payload.contains(QStringLiteral("fields"))
                && !payload.value(QStringLiteral("fields")).isArray())
            || (payload.contains(QStringLiteral("roundTripEqual"))
                && !payload.value(QStringLiteral("roundTripEqual")).isBool())
            || !validOptionalString(payload, QStringLiteral("summary"), 256)) {
            return invalid(QStringLiteral("VMF 事件摘要结构无效"));
        }
    }
    return ValidationResult::success();
}

ValidationResult validateClientPayload(const QString& type, const QJsonObject& payload) {
    return validateClientPayloadForVersion(type, payload, SchemaVersion);
}

ValidationResult validateServerPayload(const QString& type, const QJsonObject& payload) {
    return validateServerPayloadForVersion(type, payload, SchemaVersion);
}

QJsonObject makeClientEnvelope(const QString& type, const QString& messageId,
                               const QJsonObject& payload) {
    return makeClientEnvelopeForVersion(type, messageId, payload, Version, SchemaVersion);
}

QJsonObject makeClientEnvelopeForVersion(const QString& type, const QString& messageId,
                                         const QJsonObject& payload,
                                         int protocolVersion, int schemaVersion) {
    return {{QStringLiteral("protocolVersion"), protocolVersion},
            {QStringLiteral("schemaVersion"), schemaVersion},
            {QStringLiteral("type"), type},
            {QStringLiteral("messageId"), messageId},
            {QStringLiteral("payload"), payload}};
}

QJsonObject makeServerEnvelope(const QString& type, quint64 sequence,
                               const QJsonObject& payload) {
    return makeServerEnvelopeForVersion(type, sequence, payload, Version, SchemaVersion);
}

QJsonObject makeServerEnvelopeForVersion(const QString& type, quint64 sequence,
                                         const QJsonObject& payload,
                                         int protocolVersion, int schemaVersion) {
    return {{QStringLiteral("protocolVersion"), protocolVersion},
            {QStringLiteral("schemaVersion"), schemaVersion},
            {QStringLiteral("type"), type},
            {QStringLiteral("sequence"), static_cast<qint64>(sequence)},
            {QStringLiteral("sentAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("payload"), payload}};
}

} // namespace gbr::Protocol
