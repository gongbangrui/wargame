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
        QStringLiteral("ping")};
    return types;
}

const QSet<QString>& serverTypes() {
    static const QSet<QString> types{
        QStringLiteral("welcome"), QStringLiteral("snapshot"), QStringLiteral("delta"),
        QStringLiteral("commandResult"), QStringLiteral("event"), QStringLiteral("chat"),
        QStringLiteral("pong"), QStringLiteral("error")};
    return types;
}

bool validIdentifier(const QJsonValue& value) {
    return value.isString() && !value.toString().isEmpty()
        && value.toString().size() <= MaxIdentifierLength;
}

bool validString(const QJsonValue& value, int maximumLength, bool allowEmpty = false) {
    return value.isString() && value.toString().size() <= maximumLength
        && (allowEmpty || !value.toString().trimmed().isEmpty());
}

bool validNonNegativeInteger(const QJsonValue& value) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= 0.0 && std::floor(number) == number;
}

bool validPoint(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject point = value.toObject();
    return point.value(QStringLiteral("x")).isDouble()
        && point.value(QStringLiteral("y")).isDouble();
}

bool validPointArray(const QJsonValue& value, bool requiresTime) {
    if (!value.isArray()) return false;
    for (const QJsonValue& item : value.toArray()) {
        if (!validPoint(item)) return false;
        if (requiresTime && !item.toObject().value(QStringLiteral("time")).isDouble()) {
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
    if (!protocolVersion.isDouble() || protocolVersion.toInt() != Version) {
        return ValidationResult::failure(QStringLiteral("PROTOCOL_MISMATCH"),
                                         QStringLiteral("协议版本不兼容"));
    }
    const QJsonValue schemaVersion = envelope.value(QStringLiteral("schemaVersion"));
    if (!schemaVersion.isDouble() || schemaVersion.toInt() != SchemaVersion) {
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
            || !payload.value(QStringLiteral("args")).isObject()) {
            return invalid(QStringLiteral("命令操作或参数无效"));
        }
        const QString action = payload.value(QStringLiteral("action")).toString();
        const QJsonObject args = payload.value(QStringLiteral("args")).toObject();
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
                || !validPointArray(args.value(QStringLiteral("waypoints")), false))) {
            return invalid(QStringLiteral("航路命令参数类型无效"));
        }
        if (action == QLatin1String("moveTo")
            && (!validIds({QStringLiteral("unitId")})
                || !validPoint(args.value(QStringLiteral("pos"))))) {
            return invalid(QStringLiteral("移动命令参数类型无效"));
        }
        if ((action == QLatin1String("withdraw") || action == QLatin1String("halt"))
            && !validIds({QStringLiteral("unitId")})) {
            return invalid(QStringLiteral("单元 ID 无效"));
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
    }
    return ValidationResult::success();
}

ValidationResult validateServerPayload(const QString& type, const QJsonObject& payload) {
    auto invalid = [](const QString& message) {
        return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"), message);
    };
    if (type == QLatin1String("welcome")) {
        static const QSet<QString> roles{QStringLiteral("director"), QStringLiteral("editor"),
                                         QStringLiteral("red"), QStringLiteral("blue")};
        if (!validString(payload.value(QStringLiteral("username")), 128)
            || !validString(payload.value(QStringLiteral("displayName")), 128, true)
            || !roles.contains(payload.value(QStringLiteral("role")).toString())) {
            return invalid(QStringLiteral("欢迎消息中的账号身份无效"));
        }
    } else if (type == QLatin1String("snapshot")) {
        if (payload.value(QStringLiteral("schemaVersion")).toInt() != SchemaVersion
            || !payload.value(QStringLiteral("stateRevision")).isDouble()
            || payload.value(QStringLiteral("stateRevision")).toInteger() <= 0
            || !payload.value(QStringLiteral("scenario")).isObject()
            || !payload.value(QStringLiteral("units")).isArray()
            || !payload.value(QStringLiteral("messages")).isArray()
            || !payload.value(QStringLiteral("roomState")).isObject()) {
            return invalid(QStringLiteral("完整快照结构无效"));
        }
    } else if (type == QLatin1String("delta")) {
        if (payload.value(QStringLiteral("schemaVersion")).toInt() != SchemaVersion
            || !payload.value(QStringLiteral("baseStateRevision")).isDouble()
            || !payload.value(QStringLiteral("stateRevision")).isDouble()
            || !payload.value(QStringLiteral("scenarioRevision")).isDouble()
            || !payload.value(QStringLiteral("units")).isArray()
            || !payload.value(QStringLiteral("roomState")).isObject()) {
            return invalid(QStringLiteral("状态增量结构无效"));
        }
    } else if (type == QLatin1String("commandResult")) {
        if (!validIdentifier(payload.value(QStringLiteral("commandId")))
            || !payload.value(QStringLiteral("accepted")).isBool()
            || !validString(payload.value(QStringLiteral("code")), MaxIdentifierLength)
            || !validString(payload.value(QStringLiteral("message")), 1024, true)) {
            return invalid(QStringLiteral("命令回执结构无效"));
        }
    } else if (type == QLatin1String("error")) {
        if (!validString(payload.value(QStringLiteral("code")), MaxIdentifierLength)
            || !validString(payload.value(QStringLiteral("message")), 1024)) {
            return invalid(QStringLiteral("错误消息结构无效"));
        }
    } else if (type == QLatin1String("chat")) {
        if (!validString(payload.value(QStringLiteral("text")), MaxChatLength)) {
            return invalid(QStringLiteral("聊天消息结构无效"));
        }
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
