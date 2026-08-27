#include "VmfRuntimeState.h"

#include <QSet>
#include <QJsonDocument>

#include <cmath>

namespace gbr::vmf {

namespace {

constexpr int kMaxIdentifierLength = 128;
constexpr int kMaxTraceStringLength = 128;

bool validString(const QJsonValue& value, int maximum, bool allowEmpty = false) {
    return value.isString() && value.toString().size() <= maximum
        && (allowEmpty || !value.toString().trimmed().isEmpty());
}

bool validFiniteNonNegative(const QJsonValue& value) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= 0.0;
}

bool validSide(const QJsonValue& value) {
    return value.toString() == QLatin1String("red")
        || value.toString() == QLatin1String("blue");
}

bool validateTask(const QJsonValue& value, QSet<QString>* ids, QString* error) {
    if (!value.isObject()) {
        if (error) *error = QStringLiteral("VMF 活动任务必须是对象");
        return false;
    }
    const QJsonObject task = value.toObject();
    for (const QString& field : {QStringLiteral("taskId"), QStringLiteral("stage"),
                                 QStringLiteral("side"), QStringLiteral("targetId"),
                                 QStringLiteral("attackerId"), QStringLiteral("guideId"),
                                 QStringLiteral("correlationId")}) {
        if (!validString(task.value(field), kMaxIdentifierLength, true)) {
            if (error) *error = QStringLiteral("VMF 活动任务字段无效: %1").arg(field);
            return false;
        }
    }
    const QString taskId = task.value(QStringLiteral("taskId")).toString();
    if (taskId.isEmpty() || !validSide(task.value(QStringLiteral("side")))
        || !validFiniteNonNegative(task.value(QStringLiteral("updatedAt")))) {
        if (error) *error = QStringLiteral("VMF 活动任务身份、阵营或时间无效");
        return false;
    }
    if (task.contains(QStringLiteral("createdAt"))
        && !validFiniteNonNegative(task.value(QStringLiteral("createdAt")))) {
        if (error) *error = QStringLiteral("VMF 活动任务创建时间无效");
        return false;
    }
    const double created = task.value(QStringLiteral("createdAt"))
                               .toDouble(task.value(QStringLiteral("updatedAt")).toDouble());
    if (created > task.value(QStringLiteral("updatedAt")).toDouble()) {
        if (error) *error = QStringLiteral("VMF 活动任务时间顺序无效");
        return false;
    }
    if (ids && ids->contains(taskId)) {
        if (error) *error = QStringLiteral("VMF 活动任务 ID 重复: %1").arg(taskId);
        return false;
    }
    if (ids) ids->insert(taskId);
    return true;
}

bool validateAck(const QJsonValue& value, QSet<QString>* ids, QString* error) {
    if (!value.isObject()) {
        if (error) *error = QStringLiteral("VMF pending ACK 必须是对象");
        return false;
    }
    const QJsonObject ack = value.toObject();
    for (const QString& field : {QStringLiteral("messageId"), QStringLiteral("type"),
                                 QStringLiteral("traceId"),
                                 QStringLiteral("correlationId"), QStringLiteral("sender"),
                                 QStringLiteral("receiver")}) {
        if (!validString(ack.value(field), kMaxIdentifierLength, true)) {
            if (error) *error = QStringLiteral("VMF pending ACK 字段无效: %1").arg(field);
            return false;
        }
    }
    const QString messageId = ack.value(QStringLiteral("messageId")).toString();
    const QJsonValue retries = ack.value(QStringLiteral("retries"));
    if (messageId.isEmpty()
        || ack.value(QStringLiteral("type")).toString().trimmed().isEmpty()
        || !validFiniteNonNegative(ack.value(QStringLiteral("sentAt")))
        || !retries.isDouble() || retries.toDouble() < 0.0
        || retries.toDouble() > 16.0 || std::floor(retries.toDouble()) != retries.toDouble()) {
        if (error) *error = QStringLiteral("VMF pending ACK 时间或重试次数无效");
        return false;
    }
    if (ack.contains(QStringLiteral("retryCount"))) {
        const QJsonValue retryCount = ack.value(QStringLiteral("retryCount"));
        if (!validFiniteNonNegative(retryCount) || retryCount.toDouble() > 16.0
            || std::floor(retryCount.toDouble()) != retryCount.toDouble()) {
            if (error) *error = QStringLiteral("VMF pending ACK retryCount 无效");
            return false;
        }
    }
    if (ack.contains(QStringLiteral("requiresAck"))
        && !ack.value(QStringLiteral("requiresAck")).isBool()) {
        if (error) *error = QStringLiteral("VMF pending ACK requiresAck 无效");
        return false;
    }
    if (ack.contains(QStringLiteral("automaticAck"))
        && !ack.value(QStringLiteral("automaticAck")).isBool()) {
        if (error) *error = QStringLiteral("VMF pending ACK automaticAck 无效");
        return false;
    }
    if (ack.contains(QStringLiteral("payload"))) {
        const QJsonValue payload = ack.value(QStringLiteral("payload"));
        if (!payload.isObject()
            || QJsonDocument(payload.toObject()).toJson(QJsonDocument::Compact).size() > 64 * 1024) {
            if (error) *error = QStringLiteral("VMF pending ACK payload 无效或过大");
            return false;
        }
    }
    if (ack.contains(QStringLiteral("vmfMessage"))
        && !validString(ack.value(QStringLiteral("vmfMessage")), kMaxIdentifierLength)) {
        if (error) *error = QStringLiteral("VMF pending ACK 消息名无效");
        return false;
    }
    if (ack.contains(QStringLiteral("wireFormat"))) {
        const QString format = ack.value(QStringLiteral("wireFormat")).toString();
        if (format != QLatin1String("native") && format != QLatin1String("vmf-design-v1")) {
            if (error) *error = QStringLiteral("VMF pending ACK wireFormat 无效");
            return false;
        }
    }
    if (ack.contains(QStringLiteral("wireBytes"))) {
        const QJsonValue encoded = ack.value(QStringLiteral("wireBytes"));
        if (!encoded.isString() || encoded.toString().size() > (8 * 1024 * 1024 * 4 / 3 + 4)) {
            if (error) *error = QStringLiteral("VMF pending ACK wireBytes 无效或过大");
            return false;
        }
        const QByteArray bytes = QByteArray::fromBase64(encoded.toString().toLatin1());
        if (bytes.isEmpty() || bytes.toBase64() != encoded.toString().toLatin1()) {
            if (error) *error = QStringLiteral("VMF pending ACK wireBytes 不是有效 Base64");
            return false;
        }
        if (!ack.contains(QStringLiteral("wireBitLength"))
            || !validFiniteNonNegative(ack.value(QStringLiteral("wireBitLength")))
            || ack.value(QStringLiteral("wireBitLength")).toDouble() <= 0.0
            || std::floor(ack.value(QStringLiteral("wireBitLength")).toDouble())
                   != ack.value(QStringLiteral("wireBitLength")).toDouble()
            || ack.value(QStringLiteral("wireBitLength")).toDouble()
                   > static_cast<double>(bytes.size() * 8)) {
            if (error) *error = QStringLiteral("VMF pending ACK wireBitLength 无效");
            return false;
        }
        const int remainder = static_cast<int>(
            ack.value(QStringLiteral("wireBitLength")).toDouble()) % 8;
        if (remainder != 0) {
            const unsigned char unusedMask = static_cast<unsigned char>(
                (1U << (8 - remainder)) - 1U);
            if ((static_cast<unsigned char>(bytes.at(bytes.size() - 1)) & unusedMask) != 0U) {
                if (error) *error = QStringLiteral("VMF pending ACK padding 位无效");
                return false;
            }
        }
    } else if (ack.contains(QStringLiteral("wireBitLength"))) {
        if (error) *error = QStringLiteral("VMF pending ACK 缺少 wireBytes");
        return false;
    }
    if (ids && ids->contains(messageId)) {
        if (error) *error = QStringLiteral("VMF pending ACK ID 重复: %1").arg(messageId);
        return false;
    }
    if (ids) ids->insert(messageId);
    return true;
}

bool validateTrace(const QJsonValue& value, QString* error) {
    if (!value.isObject()) {
        if (error) *error = QStringLiteral("VMF trace 摘要必须是对象");
        return false;
    }
    const QJsonObject trace = value.toObject();
    for (const QString& field : {QStringLiteral("messageId"), QStringLiteral("traceId"),
                                 QStringLiteral("correlationId"), QStringLiteral("sender"),
                                 QStringLiteral("receiver"), QStringLiteral("vmfMessage"),
                                 QStringLiteral("wireFormat")}) {
        if (trace.contains(field) && !validString(trace.value(field), kMaxTraceStringLength, true)) {
            if (error) *error = QStringLiteral("VMF trace 字段过长或类型错误: %1").arg(field);
            return false;
        }
    }
    if (trace.contains(QStringLiteral("wireBytes")) || trace.contains(QStringLiteral("xml"))
        || trace.contains(QStringLiteral("payload"))) {
        if (error) *error = QStringLiteral("VMF trace 不得持久化原始 wire/XML/payload");
        return false;
    }
    if (trace.contains(QStringLiteral("bitLength"))
        && (!trace.value(QStringLiteral("bitLength")).isDouble()
            || trace.value(QStringLiteral("bitLength")).toInteger() <= 0
            || trace.value(QStringLiteral("bitLength")).toInteger() > 16 * 1024 * 1024)) {
        if (error) *error = QStringLiteral("VMF trace 位长度无效");
        return false;
    }
    return true;
}

bool validateMessageIds(const QJsonArray& values, QString* error) {
    QSet<QString> ids;
    for (const QJsonValue& value : values) {
        if (!validString(value, kMaxIdentifierLength) || ids.contains(value.toString())) {
            if (error) *error = QStringLiteral("VMF 去重消息 ID 无效或重复");
            return false;
        }
        ids.insert(value.toString());
    }
    return true;
}

} // namespace

QJsonObject RuntimeState::toJson() const {
    return QJsonObject{{QStringLiteral("schemaVersion"), SchemaVersion},
                       {QStringLiteral("profile"), profileId},
                       {QStringLiteral("activeTasks"), activeTasks},
                       {QStringLiteral("pendingAcks"), pendingAcks},
                       {QStringLiteral("seenMessageIds"), seenMessageIds},
                       {QStringLiteral("traceSummaries"), traceSummaries}};
}

bool RuntimeState::validate(QString* error) const {
    if (error) error->clear();
    if (profileId != QString::fromLatin1(ProfileId)
        && profileId != QLatin1String("vmf-demo-v2")) {
        if (error) *error = QStringLiteral("VMF 运行时状态 profile 不兼容");
        return false;
    }
    if (activeTasks.size() > MaxActiveTasks || pendingAcks.size() > MaxPendingAcks
        || seenMessageIds.size() > MaxSeenMessageIds
        || traceSummaries.size() > MaxTraceSummaries) {
        if (error) *error = QStringLiteral("VMF 运行时状态超过数量上限");
        return false;
    }
    QSet<QString> taskIds;
    for (const QJsonValue& value : activeTasks) {
        if (!validateTask(value, &taskIds, error)) return false;
    }
    QSet<QString> ackIds;
    for (const QJsonValue& value : pendingAcks) {
        if (!validateAck(value, &ackIds, error)) return false;
    }
    if (!validateMessageIds(seenMessageIds, error)) return false;
    for (const QJsonValue& value : traceSummaries) {
        if (!validateTrace(value, error)) return false;
    }
    return true;
}

bool RuntimeState::fromJson(const QJsonObject& object, RuntimeState* output, QString* error) {
    if (error) error->clear();
    if (!output) {
        if (error) *error = QStringLiteral("VMF 运行时状态输出参数为空");
        return false;
    }
    const QJsonValue version = object.value(QStringLiteral("schemaVersion"));
    if (!version.isDouble() || version.toInteger() != SchemaVersion) {
        if (error) *error = QStringLiteral("VMF 运行时状态版本不兼容");
        return false;
    }
    if (!object.value(QStringLiteral("profile")).isString()
        || (object.value(QStringLiteral("profile")).toString()
                != QString::fromLatin1(ProfileId)
            && object.value(QStringLiteral("profile")).toString()
                != QLatin1String("vmf-demo-v2"))) {
        if (error) *error = QStringLiteral("VMF 运行时状态 profile 不兼容");
        return false;
    }
    RuntimeState parsed;
    parsed.profileId = object.value(QStringLiteral("profile")).toString();
    for (const QString& field : {QStringLiteral("activeTasks"), QStringLiteral("pendingAcks"),
                                 QStringLiteral("seenMessageIds"),
                                 QStringLiteral("traceSummaries")}) {
        if (!object.value(field).isArray()) {
            if (error) *error = QStringLiteral("VMF 运行时状态字段不是数组: %1").arg(field);
            return false;
        }
    }
    parsed.activeTasks = object.value(QStringLiteral("activeTasks")).toArray();
    parsed.pendingAcks = object.value(QStringLiteral("pendingAcks")).toArray();
    parsed.seenMessageIds = object.value(QStringLiteral("seenMessageIds")).toArray();
    parsed.traceSummaries = object.value(QStringLiteral("traceSummaries")).toArray();
    if (!parsed.validate(error)) return false;
    *output = std::move(parsed);
    return true;
}

bool RuntimeState::upsertTask(const QJsonObject& task, QString* error) {
    const QString taskId = task.value(QStringLiteral("taskId")).toString();
    if (taskId.isEmpty()) {
        if (error) *error = QStringLiteral("VMF 活动任务缺少 taskId");
        return false;
    }
    QJsonArray retained;
    for (const QJsonValue& value : activeTasks) {
        if (value.toObject().value(QStringLiteral("taskId")).toString() != taskId) {
            retained.append(value);
        }
    }
    retained.append(task);
    const QJsonArray previous = activeTasks;
    activeTasks = retained;
    if (!validate(error) || activeTasks.size() > MaxActiveTasks) {
        activeTasks = previous;
        if (error && error->isEmpty()) *error = QStringLiteral("VMF 活动任务数量超限");
        return false;
    }
    return true;
}

bool RuntimeState::removeTask(const QString& taskId) {
    bool removed = false;
    QJsonArray retained;
    for (const QJsonValue& value : activeTasks) {
        if (value.toObject().value(QStringLiteral("taskId")).toString() == taskId) {
            removed = true;
        } else {
            retained.append(value);
        }
    }
    activeTasks = retained;
    return removed;
}

bool RuntimeState::upsertPendingAck(const QJsonObject& ack, QString* error) {
    const QString messageId = ack.value(QStringLiteral("messageId")).toString();
    if (messageId.isEmpty()) {
        if (error) *error = QStringLiteral("VMF pending ACK 缺少 messageId");
        return false;
    }
    QJsonArray retained;
    for (const QJsonValue& value : pendingAcks) {
        if (value.toObject().value(QStringLiteral("messageId")).toString() != messageId) {
            retained.append(value);
        }
    }
    retained.append(ack);
    const QJsonArray previous = pendingAcks;
    pendingAcks = retained;
    if (!validate(error) || pendingAcks.size() > MaxPendingAcks) {
        pendingAcks = previous;
        if (error && error->isEmpty()) *error = QStringLiteral("VMF pending ACK 数量超限");
        return false;
    }
    return true;
}

bool RuntimeState::removePendingAck(const QString& messageId) {
    bool removed = false;
    QJsonArray retained;
    for (const QJsonValue& value : pendingAcks) {
        if (value.toObject().value(QStringLiteral("messageId")).toString() == messageId) {
            removed = true;
        } else {
            retained.append(value);
        }
    }
    pendingAcks = retained;
    return removed;
}

void RuntimeState::rememberMessageId(const QString& messageId) {
    if (messageId.trimmed().isEmpty()) return;
    QJsonArray retained;
    for (const QJsonValue& value : seenMessageIds) {
        if (value.toString() != messageId) retained.append(value);
    }
    retained.append(messageId);
    while (retained.size() > MaxSeenMessageIds) retained.removeFirst();
    seenMessageIds = retained;
}

void RuntimeState::appendTraceSummary(const QJsonObject& trace) {
    QJsonArray retained = traceSummaries;
    retained.append(trace);
    while (retained.size() > MaxTraceSummaries) retained.removeFirst();
    traceSummaries = retained;
}

} // namespace gbr::vmf
