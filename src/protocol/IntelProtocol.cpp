#include "IntelProtocol.h"

#include <QDateTime>
#include <QHash>
#include <QJsonValue>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace gbr::Protocol {

namespace {

ValidationResult invalid(const QString& message) {
    return ValidationResult::failure(QStringLiteral("INVALID_PAYLOAD"), message);
}

bool hasOnlyFields(const QJsonObject& object, const QSet<QString>& allowed) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowed.contains(it.key())) return false;
    }
    return true;
}

bool validString(const QJsonValue& value, int maximumLength, bool allowEmpty = false) {
    return value.isString() && value.toString().size() <= maximumLength
        && (allowEmpty || !value.toString().trimmed().isEmpty());
}

bool validIdentifier(const QJsonValue& value, bool allowEmpty = false) {
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (allowEmpty && text.isEmpty()) return true;
    if (text.isEmpty() || text.size() > MaxIdentifierLength) return false;
    for (const QChar character : text) {
        if (character.isSpace() || character.isNull() || !character.isPrint()) return false;
    }
    return true;
}

bool validSafeInteger(const QJsonValue& value, qint64 minimum = 0) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= static_cast<double>(minimum)
        && number <= static_cast<double>(MaxSafeJsonInteger)
        && std::floor(number) == number;
}

bool validFiniteNumber(const QJsonValue& value, double minimum = -HUGE_VAL,
                       double maximum = HUGE_VAL) {
    return value.isDouble() && std::isfinite(value.toDouble())
        && value.toDouble() >= minimum && value.toDouble() <= maximum;
}

bool validTimestamp(const QJsonValue& value, bool allowEmpty = false) {
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (allowEmpty && text.isEmpty()) return true;
    if (text.isEmpty() || text.size() > MaxIntelTimestampLength) return false;
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid()) parsed = QDateTime::fromString(text, Qt::ISODate);
    return parsed.isValid() && parsed.offsetFromUtc() == 0;
}

QDateTime parseTimestamp(const QString& text) {
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid()) parsed = QDateTime::fromString(text, Qt::ISODate);
    return parsed;
}

bool validOptionalTimestamp(const QJsonObject& object, const QString& field) {
    return !object.contains(field) || validTimestamp(object.value(field), true);
}

bool validPosition(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject position = value.toObject();
    static const QSet<QString> allowed{
        QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("alt"),
        QStringLiteral("uncertaintyRadius")};
    if (!hasOnlyFields(position, allowed)
        || !validFiniteNumber(position.value(QStringLiteral("x")), 0.0)
        || !validFiniteNumber(position.value(QStringLiteral("y")), 0.0)
        || (position.contains(QStringLiteral("alt"))
            && !validFiniteNumber(position.value(QStringLiteral("alt"))))
        || (position.contains(QStringLiteral("uncertaintyRadius"))
            && !validFiniteNumber(position.value(QStringLiteral("uncertaintyRadius")), 0.0))) {
        return false;
    }
    return true;
}

bool validKnownAttributes(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject attributes = value.toObject();
    if (attributes.size() > MaxIntelKnownAttributes) return false;
    for (auto it = attributes.constBegin(); it != attributes.constEnd(); ++it) {
        if (it.key().isEmpty() || it.key().size() > MaxIdentifierLength) return false;
        const QJsonValue field = it.value();
        if (field.isUndefined() || field.isNull() || field.isArray() || field.isObject()) {
            return false;
        }
        if (field.isString() && field.toString().size() > MaxIntelAttributeValueLength) {
            return false;
        }
        if (field.isDouble() && !std::isfinite(field.toDouble())) return false;
    }
    return true;
}

bool validFreshness(const QJsonValue& value, bool allowEmpty = false) {
    if (!value.isString()) return false;
    const QString freshness = value.toString();
    return (allowEmpty && freshness.isEmpty()) || freshness == QLatin1String("live")
        || freshness == QLatin1String("stale") || freshness == QLatin1String("archived");
}

bool validIntelType(const QJsonValue& value, bool allowEmpty = false) {
    if (!value.isString()) return false;
    const QString type = value.toString();
    return (allowEmpty && type.isEmpty()) || type == QLatin1String("sensorContact")
        || type == QLatin1String("manualReport");
}

bool validHistoryEventType(const QJsonValue& value) {
    if (!value.isString()) return false;
    const QString type = value.toString();
    return type == QLatin1String("discovered") || type == QLatin1String("updated")
        || type == QLatin1String("shared") || type == QLatin1String("received")
        || type == QLatin1String("freshnessChanged") || type == QLatin1String("archived")
        || type == QLatin1String("deleted");
}

bool validPropagationSources(const QJsonValue& value) {
    if (!value.isArray() || value.toArray().size() > MaxIntelPropagationSources) return false;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isObject()) return false;
        const QJsonObject source = item.toObject();
        static const QSet<QString> allowed{
            QStringLiteral("sourceSeatId"), QStringLiteral("sourceIntelId"),
            QStringLiteral("sharedAt")};
        if (!hasOnlyFields(source, allowed)
            || !validIdentifier(source.value(QStringLiteral("sourceSeatId")))
            || (source.contains(QStringLiteral("sourceIntelId"))
                && !validIdentifier(source.value(QStringLiteral("sourceIntelId"))))
            || !validTimestamp(source.value(QStringLiteral("sharedAt")))) {
            return false;
        }
    }
    return true;
}

bool validSeatIdList(const QJsonValue& value, bool allowEmpty = true) {
    if (!value.isArray() || value.toArray().size() > MaxIntelShareTargets
        || (!allowEmpty && value.toArray().isEmpty())) {
        return false;
    }
    QSet<QString> ids;
    for (const QJsonValue& item : value.toArray()) {
        if (!validIdentifier(item) || ids.contains(item.toString())) return false;
        ids.insert(item.toString());
    }
    return true;
}

QJsonArray stringArray(const QStringList& values) {
    QJsonArray result;
    for (const QString& value : values) result.append(value);
    return result;
}

QStringList stringList(const QJsonArray& values) {
    QStringList result;
    result.reserve(values.size());
    for (const QJsonValue& value : values) result.append(value.toString());
    return result;
}

template <typename T>
ValidationResult assignIfValid(const ValidationResult& validation, const T& candidate, T* output) {
    if (!validation.valid) return validation;
    if (output) *output = candidate;
    return validation;
}

QJsonObject contactToJson(const IntelContact& value) {
    QJsonObject object{{QStringLiteral("intelId"), value.intelId},
                       {QStringLiteral("type"), value.type},
                       {QStringLiteral("knownAttributes"), value.knownAttributes},
                       {QStringLiteral("lastPosition"), value.lastPosition},
                       {QStringLiteral("sourceSeatId"), value.sourceSeatId},
                       {QStringLiteral("firstDiscoveredAt"), value.firstDiscoveredAt},
                       {QStringLiteral("lastObservedAt"), value.lastObservedAt},
                       {QStringLiteral("receivedAt"), value.receivedAt},
                       {QStringLiteral("confidence"), value.confidence},
                       {QStringLiteral("freshness"), value.freshness},
                       {QStringLiteral("note"), value.note},
                       {QStringLiteral("propagationSources"), value.propagationSources},
                       {QStringLiteral("actionable"), value.actionable}};
    if (!value.targetId.isEmpty()) object[QStringLiteral("targetId")] = value.targetId;
    if (!value.sourceUnitId.isEmpty()) object[QStringLiteral("sourceUnitId")] = value.sourceUnitId;
    return object;
}

QJsonObject historyToJson(const IntelHistoryEntry& value) {
    QJsonObject object{{QStringLiteral("historyId"), value.historyId},
                       {QStringLiteral("intelId"), value.intelId},
                       {QStringLiteral("eventType"), value.eventType},
                       {QStringLiteral("occurredAt"), value.occurredAt},
                       {QStringLiteral("sourceSeatId"), value.sourceSeatId},
                       {QStringLiteral("note"), value.note},
                       {QStringLiteral("confidence"), value.confidence}};
    if (!value.sourceUnitId.isEmpty()) object[QStringLiteral("sourceUnitId")] = value.sourceUnitId;
    if (!value.recipientSeatId.isEmpty()) {
        object[QStringLiteral("recipientSeatId")] = value.recipientSeatId;
    }
    if (!value.freshness.isEmpty()) object[QStringLiteral("freshness")] = value.freshness;
    if (!value.position.isEmpty()) object[QStringLiteral("position")] = value.position;
    if (!value.knownAttributes.isEmpty()) {
        object[QStringLiteral("knownAttributes")] = value.knownAttributes;
    }
    if (!value.propagationSource.isEmpty()) {
        object[QStringLiteral("propagationSource")] = value.propagationSource;
    }
    if (!value.targetId.isEmpty()) object[QStringLiteral("targetId")] = value.targetId;
    return object;
}

} // namespace

QJsonObject IntelContact::toJson() const { return Protocol::toJson(*this); }

ValidationResult IntelContact::fromJson(const QJsonObject& object, IntelContact* contact) {
    return Protocol::fromJson(object, contact);
}

QJsonObject IntelHistoryEntry::toJson() const { return Protocol::toJson(*this); }

ValidationResult IntelHistoryEntry::fromJson(const QJsonObject& object, IntelHistoryEntry* entry) {
    return Protocol::fromJson(object, entry);
}

QJsonObject IntelState::toJson() const { return Protocol::toJson(*this); }

ValidationResult IntelState::fromJson(const QJsonObject& object, IntelState* state) {
    return Protocol::fromJson(object, state);
}

QJsonObject IntelShareRequest::toJson() const { return Protocol::toJson(*this); }

ValidationResult IntelShareRequest::fromJson(const QJsonObject& object, IntelShareRequest* request) {
    return Protocol::fromJson(object, request);
}

QJsonObject IntelHistoryQuery::toJson() const { return Protocol::toJson(*this); }

ValidationResult IntelHistoryQuery::fromJson(const QJsonObject& object, IntelHistoryQuery* query) {
    return Protocol::fromJson(object, query);
}

QJsonObject IntelHistoryPage::toJson() const { return Protocol::toJson(*this); }

ValidationResult IntelHistoryPage::fromJson(const QJsonObject& object, IntelHistoryPage* page) {
    return Protocol::fromJson(object, page);
}

ValidationResult validateIntelContact(const QJsonObject& object) {
    static const QSet<QString> allowed{
        QStringLiteral("intelId"), QStringLiteral("type"), QStringLiteral("targetId"),
        QStringLiteral("knownAttributes"), QStringLiteral("lastPosition"),
        QStringLiteral("sourceSeatId"), QStringLiteral("sourceUnitId"),
        QStringLiteral("firstDiscoveredAt"), QStringLiteral("lastObservedAt"),
        QStringLiteral("receivedAt"), QStringLiteral("confidence"),
        QStringLiteral("freshness"), QStringLiteral("note"),
        QStringLiteral("propagationSources"), QStringLiteral("actionable")};
    if (!hasOnlyFields(object, allowed)
        || !validIdentifier(object.value(QStringLiteral("intelId")))
        || !validIntelType(object.value(QStringLiteral("type")))
        || !validKnownAttributes(object.value(QStringLiteral("knownAttributes")))
        || !validPosition(object.value(QStringLiteral("lastPosition")))
        || !validIdentifier(object.value(QStringLiteral("sourceSeatId")))
        || !validTimestamp(object.value(QStringLiteral("firstDiscoveredAt")))
        || !validTimestamp(object.value(QStringLiteral("lastObservedAt")))
        || !validTimestamp(object.value(QStringLiteral("receivedAt")))
        || !validFiniteNumber(object.value(QStringLiteral("confidence")), 0.0, 100.0)
        || !validFreshness(object.value(QStringLiteral("freshness")))
        || !validString(object.value(QStringLiteral("note")), MaxIntelNoteLength, true)
        || !validPropagationSources(object.value(QStringLiteral("propagationSources")))
        || !object.value(QStringLiteral("actionable")).isBool()) {
        return invalid(QStringLiteral("情报记录结构无效"));
    }
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("sensorContact")) {
        if (!validIdentifier(object.value(QStringLiteral("targetId")))
            || !validIdentifier(object.value(QStringLiteral("sourceUnitId")))) {
            return invalid(QStringLiteral("传感器情报缺少服务器投影的目标或来源单位"));
        }
    } else if (object.contains(QStringLiteral("targetId"))
               || object.contains(QStringLiteral("sourceUnitId"))) {
        return invalid(QStringLiteral("人工位置报告不能绑定目标或来源单位"));
    }
    const QDateTime first = parseTimestamp(
        object.value(QStringLiteral("firstDiscoveredAt")).toString());
    const QDateTime observed = parseTimestamp(
        object.value(QStringLiteral("lastObservedAt")).toString());
    const QDateTime received = parseTimestamp(
        object.value(QStringLiteral("receivedAt")).toString());
    if (first.isValid() && observed.isValid() && first > observed) {
        return invalid(QStringLiteral("情报首次发现时间晚于最后观测时间"));
    }
    if (observed.isValid() && received.isValid() && observed > received) {
        return invalid(QStringLiteral("情报接收时间早于最后观测时间"));
    }
    if (object.value(QStringLiteral("freshness")).toString() != QLatin1String("live")
        && object.value(QStringLiteral("actionable")).toBool()) {
        return invalid(QStringLiteral("失联或归档情报不能标记为可行动"));
    }
    return ValidationResult::success();
}

ValidationResult validateIntelHistoryEntry(const QJsonObject& object) {
    static const QSet<QString> allowed{
        QStringLiteral("historyId"), QStringLiteral("intelId"),
        QStringLiteral("eventType"), QStringLiteral("occurredAt"),
        QStringLiteral("sourceSeatId"), QStringLiteral("sourceUnitId"),
        QStringLiteral("recipientSeatId"), QStringLiteral("note"),
        QStringLiteral("freshness"), QStringLiteral("confidence"),
        QStringLiteral("position"), QStringLiteral("knownAttributes"),
        QStringLiteral("propagationSource"), QStringLiteral("targetId")};
    if (!hasOnlyFields(object, allowed)
        || !validIdentifier(object.value(QStringLiteral("historyId")))
        || !validIdentifier(object.value(QStringLiteral("intelId")))
        || !validHistoryEventType(object.value(QStringLiteral("eventType")))
        || !validTimestamp(object.value(QStringLiteral("occurredAt")))
        || !validIdentifier(object.value(QStringLiteral("sourceSeatId")))
        || (object.contains(QStringLiteral("sourceUnitId"))
            && !validIdentifier(object.value(QStringLiteral("sourceUnitId"))))
        || (object.contains(QStringLiteral("recipientSeatId"))
            && !validIdentifier(object.value(QStringLiteral("recipientSeatId"))))
        || !validString(object.value(QStringLiteral("note")), MaxIntelNoteLength, true)
        || (object.contains(QStringLiteral("freshness"))
            && !validFreshness(object.value(QStringLiteral("freshness"))))
        || !validFiniteNumber(object.value(QStringLiteral("confidence")), 0.0, 100.0)
        || (object.contains(QStringLiteral("position"))
            && !validPosition(object.value(QStringLiteral("position"))))
        || (object.contains(QStringLiteral("knownAttributes"))
            && !validKnownAttributes(object.value(QStringLiteral("knownAttributes"))))
        || (object.contains(QStringLiteral("propagationSource"))
            && !validIdentifier(object.value(QStringLiteral("propagationSource"))))
        || (object.contains(QStringLiteral("targetId"))
            && !validIdentifier(object.value(QStringLiteral("targetId"))))) {
        return invalid(QStringLiteral("情报历史记录结构无效"));
    }
    return ValidationResult::success();
}

ValidationResult validateIntelState(const QJsonObject& object) {
    static const QSet<QString> allowed{
        QStringLiteral("revision"), QStringLiteral("records"), QStringLiteral("shareTargets")};
    if (!hasOnlyFields(object, allowed)
        || !validSafeInteger(object.value(QStringLiteral("revision")))
        || !object.value(QStringLiteral("records")).isArray()
        || object.value(QStringLiteral("records")).toArray().size() > MaxIntelRecords
        || !validSeatIdList(object.value(QStringLiteral("shareTargets")))) {
        return invalid(QStringLiteral("情报台账基线结构无效"));
    }
    QSet<QString> ids;
    for (const QJsonValue& item : object.value(QStringLiteral("records")).toArray()) {
        if (!item.isObject()) return invalid(QStringLiteral("情报台账项目必须是对象"));
        const QJsonObject record = item.toObject();
        const ValidationResult validation = validateIntelContact(record);
        if (!validation.valid) return validation;
        const QString id = record.value(QStringLiteral("intelId")).toString();
        if (ids.contains(id)) return invalid(QStringLiteral("情报台账包含重复 ID"));
        ids.insert(id);
    }
    return ValidationResult::success();
}

ValidationResult validateIntelShareRequest(const QJsonObject& object) {
    static const QSet<QString> allowed{
        QStringLiteral("intelId"), QStringLiteral("recipientSeatIds"), QStringLiteral("note")};
    if (!hasOnlyFields(object, allowed)
        || !validIdentifier(object.value(QStringLiteral("intelId")))
        || !validSeatIdList(object.value(QStringLiteral("recipientSeatIds")), false)
        || (object.contains(QStringLiteral("note"))
            && !validString(object.value(QStringLiteral("note")), MaxIntelNoteLength, true))) {
        return invalid(QStringLiteral("情报共享请求结构无效"));
    }
    return ValidationResult::success();
}

ValidationResult validateIntelHistoryQuery(const QJsonObject& object) {
    static const QSet<QString> allowed{
        QStringLiteral("cursor"), QStringLiteral("pageSize"), QStringLiteral("target"),
        QStringLiteral("type"), QStringLiteral("freshness"), QStringLiteral("sourceSeatId"),
        QStringLiteral("from"), QStringLiteral("to")};
    if (!hasOnlyFields(object, allowed)
        || (object.contains(QStringLiteral("cursor"))
            && !validString(object.value(QStringLiteral("cursor")), MaxIntelCursorLength, true))
        || !validSafeInteger(object.value(QStringLiteral("pageSize")), 1)
        || object.value(QStringLiteral("pageSize")).toInteger() > MaxIntelHistoryPageSize
        || (object.contains(QStringLiteral("target"))
            && !validString(object.value(QStringLiteral("target")), MaxIntelSearchLength, true))
        || (object.contains(QStringLiteral("type"))
            && !validIntelType(object.value(QStringLiteral("type")), true))
        || (object.contains(QStringLiteral("freshness"))
            && !validFreshness(object.value(QStringLiteral("freshness")), true))
        || (object.contains(QStringLiteral("sourceSeatId"))
            && !validIdentifier(object.value(QStringLiteral("sourceSeatId")), true))
        || !validOptionalTimestamp(object, QStringLiteral("from"))
        || !validOptionalTimestamp(object, QStringLiteral("to"))) {
        return invalid(QStringLiteral("情报历史查询结构无效"));
    }
    const QDateTime from = parseTimestamp(object.value(QStringLiteral("from")).toString());
    const QDateTime to = parseTimestamp(object.value(QStringLiteral("to")).toString());
    if (from.isValid() && to.isValid() && from > to) {
        return invalid(QStringLiteral("情报历史查询的结束时间早于开始时间"));
    }
    if (object.contains(QStringLiteral("cursor"))
        && !object.value(QStringLiteral("cursor")).toString().isEmpty()) {
        bool cursorOk = false;
        const qint64 cursor = object.value(QStringLiteral("cursor")).toString().toLongLong(&cursorOk);
        if (!cursorOk || cursor < 0 || cursor > MaxSafeJsonInteger) {
            return invalid(QStringLiteral("情报历史游标无效"));
        }
    }
    return ValidationResult::success();
}

ValidationResult validateIntelHistoryPage(const QJsonObject& object) {
    static const QSet<QString> allowed{
        QStringLiteral("entries"), QStringLiteral("nextCursor"),
        QStringLiteral("hasMore"), QStringLiteral("revision")};
    if (!hasOnlyFields(object, allowed)
        || !object.value(QStringLiteral("entries")).isArray()
        || object.value(QStringLiteral("entries")).toArray().size() > MaxIntelHistoryPageSize
        || !validString(object.value(QStringLiteral("nextCursor")), MaxIntelCursorLength, true)
        || !object.value(QStringLiteral("hasMore")).isBool()
        || !validSafeInteger(object.value(QStringLiteral("revision")))) {
        return invalid(QStringLiteral("情报历史分页结构无效"));
    }
    if (object.value(QStringLiteral("hasMore")).toBool()
        && object.value(QStringLiteral("nextCursor")).toString().isEmpty()) {
        return invalid(QStringLiteral("情报历史存在后续页但缺少游标"));
    }
    QSet<QString> ids;
    for (const QJsonValue& item : object.value(QStringLiteral("entries")).toArray()) {
        if (!item.isObject()) return invalid(QStringLiteral("情报历史分页项目必须是对象"));
        const QJsonObject entry = item.toObject();
        const ValidationResult validation = validateIntelHistoryEntry(entry);
        if (!validation.valid) return validation;
        const QString id = entry.value(QStringLiteral("historyId")).toString();
        if (ids.contains(id)) return invalid(QStringLiteral("情报历史分页包含重复 ID"));
        ids.insert(id);
    }
    return ValidationResult::success();
}

QJsonObject toJson(const IntelContact& value) { return contactToJson(value); }

QJsonObject toJson(const IntelHistoryEntry& value) { return historyToJson(value); }

QJsonObject toJson(const IntelState& value) {
    QJsonArray records;
    for (const IntelContact& record : value.records) records.append(toJson(record));
    return {{QStringLiteral("revision"), value.revision},
            {QStringLiteral("records"), records},
            {QStringLiteral("shareTargets"), stringArray(value.shareTargets)}};
}

QJsonObject toJson(const IntelShareRequest& value) {
    return {{QStringLiteral("intelId"), value.intelId},
            {QStringLiteral("recipientSeatIds"), stringArray(value.recipientSeatIds)},
            {QStringLiteral("note"), value.note}};
}

QJsonObject toJson(const IntelHistoryQuery& value) {
    QJsonObject object{{QStringLiteral("pageSize"), value.pageSize}};
    if (!value.cursor.isEmpty()) object[QStringLiteral("cursor")] = value.cursor;
    if (!value.target.isEmpty()) object[QStringLiteral("target")] = value.target;
    if (!value.type.isEmpty()) object[QStringLiteral("type")] = value.type;
    if (!value.freshness.isEmpty()) object[QStringLiteral("freshness")] = value.freshness;
    if (!value.sourceSeatId.isEmpty()) {
        object[QStringLiteral("sourceSeatId")] = value.sourceSeatId;
    }
    if (!value.from.isEmpty()) object[QStringLiteral("from")] = value.from;
    if (!value.to.isEmpty()) object[QStringLiteral("to")] = value.to;
    return object;
}

QJsonObject toJson(const IntelHistoryPage& value) {
    QJsonArray entries;
    for (const IntelHistoryEntry& entry : value.entries) entries.append(toJson(entry));
    return {{QStringLiteral("entries"), entries},
            {QStringLiteral("nextCursor"), value.nextCursor},
            {QStringLiteral("hasMore"), value.hasMore},
            {QStringLiteral("revision"), value.revision}};
}

ValidationResult fromJson(const QJsonObject& object, IntelContact* value) {
    const ValidationResult validation = validateIntelContact(object);
    IntelContact candidate;
    candidate.intelId = object.value(QStringLiteral("intelId")).toString();
    candidate.type = object.value(QStringLiteral("type")).toString();
    candidate.targetId = object.value(QStringLiteral("targetId")).toString();
    candidate.knownAttributes = object.value(QStringLiteral("knownAttributes")).toObject();
    candidate.lastPosition = object.value(QStringLiteral("lastPosition")).toObject();
    candidate.sourceSeatId = object.value(QStringLiteral("sourceSeatId")).toString();
    candidate.sourceUnitId = object.value(QStringLiteral("sourceUnitId")).toString();
    candidate.firstDiscoveredAt = object.value(QStringLiteral("firstDiscoveredAt")).toString();
    candidate.lastObservedAt = object.value(QStringLiteral("lastObservedAt")).toString();
    candidate.receivedAt = object.value(QStringLiteral("receivedAt")).toString();
    candidate.confidence = object.value(QStringLiteral("confidence")).toDouble();
    candidate.freshness = object.value(QStringLiteral("freshness")).toString();
    candidate.note = object.value(QStringLiteral("note")).toString();
    candidate.propagationSources = object.value(QStringLiteral("propagationSources")).toArray();
    candidate.actionable = object.value(QStringLiteral("actionable")).toBool();
    return assignIfValid(validation, candidate, value);
}

ValidationResult fromJson(const QJsonObject& object, IntelHistoryEntry* value) {
    const ValidationResult validation = validateIntelHistoryEntry(object);
    IntelHistoryEntry candidate;
    candidate.historyId = object.value(QStringLiteral("historyId")).toString();
    candidate.intelId = object.value(QStringLiteral("intelId")).toString();
    candidate.eventType = object.value(QStringLiteral("eventType")).toString();
    candidate.occurredAt = object.value(QStringLiteral("occurredAt")).toString();
    candidate.sourceSeatId = object.value(QStringLiteral("sourceSeatId")).toString();
    candidate.sourceUnitId = object.value(QStringLiteral("sourceUnitId")).toString();
    candidate.recipientSeatId = object.value(QStringLiteral("recipientSeatId")).toString();
    candidate.note = object.value(QStringLiteral("note")).toString();
    candidate.freshness = object.value(QStringLiteral("freshness")).toString();
    candidate.confidence = object.value(QStringLiteral("confidence")).toDouble();
    candidate.position = object.value(QStringLiteral("position")).toObject();
    candidate.knownAttributes = object.value(QStringLiteral("knownAttributes")).toObject();
    candidate.propagationSource = object.value(QStringLiteral("propagationSource")).toString();
    candidate.targetId = object.value(QStringLiteral("targetId")).toString();
    return assignIfValid(validation, candidate, value);
}

ValidationResult fromJson(const QJsonObject& object, IntelState* value) {
    const ValidationResult validation = validateIntelState(object);
    IntelState candidate;
    candidate.revision = object.value(QStringLiteral("revision")).toInteger();
    for (const QJsonValue& item : object.value(QStringLiteral("records")).toArray()) {
        IntelContact contact;
        const ValidationResult recordValidation = fromJson(item.toObject(), &contact);
        if (!recordValidation.valid) return recordValidation;
        candidate.records.append(contact);
    }
    candidate.shareTargets = stringList(object.value(QStringLiteral("shareTargets")).toArray());
    return assignIfValid(validation, candidate, value);
}

ValidationResult fromJson(const QJsonObject& object, IntelShareRequest* value) {
    const ValidationResult validation = validateIntelShareRequest(object);
    IntelShareRequest candidate;
    candidate.intelId = object.value(QStringLiteral("intelId")).toString();
    candidate.recipientSeatIds = stringList(
        object.value(QStringLiteral("recipientSeatIds")).toArray());
    candidate.note = object.value(QStringLiteral("note")).toString();
    return assignIfValid(validation, candidate, value);
}

ValidationResult fromJson(const QJsonObject& object, IntelHistoryQuery* value) {
    const ValidationResult validation = validateIntelHistoryQuery(object);
    IntelHistoryQuery candidate;
    candidate.cursor = object.value(QStringLiteral("cursor")).toString();
    candidate.pageSize = object.value(QStringLiteral("pageSize")).toInt();
    candidate.target = object.value(QStringLiteral("target")).toString();
    candidate.type = object.value(QStringLiteral("type")).toString();
    candidate.freshness = object.value(QStringLiteral("freshness")).toString();
    candidate.sourceSeatId = object.value(QStringLiteral("sourceSeatId")).toString();
    candidate.from = object.value(QStringLiteral("from")).toString();
    candidate.to = object.value(QStringLiteral("to")).toString();
    return assignIfValid(validation, candidate, value);
}

ValidationResult fromJson(const QJsonObject& object, IntelHistoryPage* value) {
    const ValidationResult validation = validateIntelHistoryPage(object);
    IntelHistoryPage candidate;
    for (const QJsonValue& item : object.value(QStringLiteral("entries")).toArray()) {
        IntelHistoryEntry entry;
        const ValidationResult entryValidation = fromJson(item.toObject(), &entry);
        if (!entryValidation.valid) return entryValidation;
        candidate.entries.append(entry);
    }
    candidate.nextCursor = object.value(QStringLiteral("nextCursor")).toString();
    candidate.hasMore = object.value(QStringLiteral("hasMore")).toBool();
    candidate.revision = object.value(QStringLiteral("revision")).toInteger();
    return assignIfValid(validation, candidate, value);
}

QJsonObject makeIntelDelta(const IntelState& before, const IntelState& after) {
    if (!validateIntelState(toJson(before)).valid || !validateIntelState(toJson(after)).valid
        || after.revision < before.revision) {
        return {};
    }
    QHash<QString, IntelContact> previous;
    QHash<QString, IntelContact> current;
    for (const IntelContact& contact : before.records) previous.insert(contact.intelId, contact);
    for (const IntelContact& contact : after.records) current.insert(contact.intelId, contact);
    QStringList currentIds = current.keys();
    currentIds.sort();
    QJsonArray upserts;
    QJsonArray archived;
    for (const QString& id : currentIds) {
        const IntelContact& contact = current.value(id);
        const bool changed = !previous.contains(id)
            || toJson(previous.value(id)) != toJson(contact);
        if (!changed) continue;
        if (contact.freshness == QLatin1String("archived") && previous.contains(id)
            && previous.value(id).freshness != QLatin1String("archived")) {
            archived.append(id);
        }
        upserts.append(toJson(contact));
    }
    QStringList previousIds = previous.keys();
    previousIds.sort();
    QJsonArray deleted;
    for (const QString& id : previousIds) {
        if (!current.contains(id)) deleted.append(id);
    }
    // Share targets are a projection of the current communication topology,
    // so they may change without advancing the durable intelligence ledger.
    // A same-revision delta is valid only for that projection-only change.
    if (after.revision == before.revision
        && (!upserts.isEmpty() || !deleted.isEmpty())) {
        return {};
    }
    if (after.revision == before.revision && before.shareTargets == after.shareTargets) {
        return {};
    }
    return {{QStringLiteral("baseRevision"), before.revision},
            {QStringLiteral("revision"), after.revision},
            {QStringLiteral("upserts"), upserts},
            {QStringLiteral("archivedIntelIds"), archived},
            {QStringLiteral("deletedIntelIds"), deleted},
            {QStringLiteral("shareTargets"), stringArray(after.shareTargets)}};
}

ValidationResult applyIntelDelta(IntelState* state, const QJsonObject& delta) {
    if (!state) return invalid(QStringLiteral("情报增量缺少目标状态"));
    static const QSet<QString> allowed{
        QStringLiteral("baseRevision"), QStringLiteral("revision"),
        QStringLiteral("upserts"), QStringLiteral("archivedIntelIds"),
        QStringLiteral("deletedIntelIds"), QStringLiteral("shareTargets")};
    if (!hasOnlyFields(delta, allowed)
        || !validSafeInteger(delta.value(QStringLiteral("baseRevision")))
        || !validSafeInteger(delta.value(QStringLiteral("revision")))
        || delta.value(QStringLiteral("baseRevision")).toInteger() != state->revision
        || delta.value(QStringLiteral("revision")).toInteger() < state->revision
        || !delta.value(QStringLiteral("upserts")).isArray()
        || delta.value(QStringLiteral("upserts")).toArray().size() > MaxIntelRecords
        || !validSeatIdList(delta.value(QStringLiteral("shareTargets")))) {
        return invalid(QStringLiteral("情报增量结构或版本无效"));
    }
    const auto validIdArray = [](const QJsonValue& value) {
        if (!value.isArray() || value.toArray().size() > MaxIntelRecords) return false;
        QSet<QString> ids;
        for (const QJsonValue& item : value.toArray()) {
            if (!validIdentifier(item) || ids.contains(item.toString())) return false;
            ids.insert(item.toString());
        }
        return true;
    };
    if (!validIdArray(delta.value(QStringLiteral("archivedIntelIds")))
        || !validIdArray(delta.value(QStringLiteral("deletedIntelIds")))) {
        return invalid(QStringLiteral("情报归档或删除列表无效"));
    }
    if (delta.value(QStringLiteral("revision")).toInteger() == state->revision
        && (!delta.value(QStringLiteral("upserts")).toArray().isEmpty()
            || !delta.value(QStringLiteral("archivedIntelIds")).toArray().isEmpty()
            || !delta.value(QStringLiteral("deletedIntelIds")).toArray().isEmpty())) {
        return invalid(QStringLiteral("情报增量在相同版本中包含记录变更"));
    }
    IntelState candidate = *state;
    QHash<QString, IntelContact> records;
    for (const IntelContact& contact : candidate.records) records.insert(contact.intelId, contact);
    QSet<QString> upsertIds;
    for (const QJsonValue& item : delta.value(QStringLiteral("upserts")).toArray()) {
        if (!item.isObject()) return invalid(QStringLiteral("情报增量项目必须是对象"));
        IntelContact contact;
        const ValidationResult validation = fromJson(item.toObject(), &contact);
        if (!validation.valid || upsertIds.contains(contact.intelId)) {
            return invalid(QStringLiteral("情报增量包含无效或重复项目"));
        }
        upsertIds.insert(contact.intelId);
        records.insert(contact.intelId, contact);
    }
    for (const QJsonValue& item : delta.value(QStringLiteral("archivedIntelIds")).toArray()) {
        const QString id = item.toString();
        if (!records.contains(id) || !upsertIds.contains(id)
            || records.value(id).freshness != QLatin1String("archived")) {
            return invalid(QStringLiteral("情报归档操作缺少对应归档记录"));
        }
    }
    for (const QJsonValue& item : delta.value(QStringLiteral("deletedIntelIds")).toArray()) {
        const QString id = item.toString();
        if (upsertIds.contains(id) || !records.remove(id)) {
            return invalid(QStringLiteral("情报删除操作引用未知或同时更新的记录"));
        }
    }
    if (records.size() > MaxIntelRecords) return invalid(QStringLiteral("情报台账超过容量限制"));
    QStringList ids = records.keys();
    ids.sort();
    candidate.records.clear();
    for (const QString& id : ids) candidate.records.append(records.value(id));
    candidate.revision = delta.value(QStringLiteral("revision")).toInteger();
    candidate.shareTargets = stringList(delta.value(QStringLiteral("shareTargets")).toArray());
    const ValidationResult validation = validateIntelState(toJson(candidate));
    if (!validation.valid) return validation;
    *state = candidate;
    return ValidationResult::success();
}

} // namespace gbr::Protocol
