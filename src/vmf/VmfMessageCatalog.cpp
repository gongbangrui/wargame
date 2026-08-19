#include "VmfMessageCatalog.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>

namespace gbr::vmf {

namespace {

QStringList stringArray(const QJsonValue& value, bool* valid) {
    QStringList result;
    if (!value.isArray()) {
        if (valid) *valid = false;
        return result;
    }
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isString() || item.toString().trimmed().isEmpty()
            || item.toString().size() > 256) {
            if (valid) *valid = false;
            return {};
        }
        result.append(item.toString().trimmed());
    }
    if (valid) *valid = true;
    return result;
}

bool requiredString(const QJsonObject& object, const QString& key, QString* output) {
    const QJsonValue value = object.value(key);
    if (!value.isString() || value.toString().trimmed().isEmpty()
        || value.toString().size() > 256) return false;
    if (output) *output = value.toString().trimmed();
    return true;
}

bool roleAllowed(const QStringList& roles, const QString& role) {
    if (role.isEmpty() || roles.isEmpty()) return true;
    return roles.contains(role) || roles.contains(QStringLiteral("any"));
}

bool payloadHasRoute(const QJsonObject& payload) {
    const QJsonArray waypoints = payload.value(QStringLiteral("waypoints")).toArray();
    if (!waypoints.isEmpty()) return true;
    const bool hasPoint = payload.contains(QStringLiteral("x"))
        && payload.contains(QStringLiteral("y"));
    const bool hasHome = payload.contains(QStringLiteral("homeX"))
        && payload.contains(QStringLiteral("homeY"));
    return hasPoint || hasHome;
}

bool payloadConditionMatches(const QString& condition, const QJsonObject& payload) {
    if (condition.isEmpty() || condition == QLatin1String("any")) return true;
    if (condition == QLatin1String("route")) return payloadHasRoute(payload);
    if (condition == QLatin1String("no-route")) return !payloadHasRoute(payload);
    return false;
}

void addFallback(QVector<MessageCatalogEntry>* entries, const QString& id,
                 const QStringList& domains, const QString& vmf,
                 const QStringList& senders, const QStringList& receivers,
                 const QString& trigger, const QStringList& nextStages,
                 bool requiresAck, const QString& level, int score,
                 const QStringList& fields, const QString& condition = {}) {
    if (!entries) return;
    MessageCatalogEntry entry;
    entry.catalogId = id;
    entry.domainTypes = domains;
    entry.vmfMessage = vmf;
    entry.senderRoles = senders;
    entry.receiverRoles = receivers;
    entry.trigger = trigger;
    entry.nextStages = nextStages;
    entry.payloadCondition = condition;
    entry.requiresAck = requiresAck;
    entry.automaticAck = true;
    entry.repeatable = !requiresAck;
    entry.informationValue = InformationValue{level, score, fields};
    entries->append(std::move(entry));
}

std::shared_ptr<VmfMessageCatalog> fallbackCatalog() {
    QVector<MessageCatalogEntry> entries;
    addFallback(&entries, QStringLiteral("47001"),
                {QStringLiteral("TargetDetect"), QStringLiteral("TargetReport")},
                QStringLiteral("Target Report"), {QStringLiteral("recon")},
                {QStringLiteral("commander")}, QStringLiteral("targetDetected"),
                {QStringLiteral("targetReported")}, true, QStringLiteral("high"), 90,
                {QStringLiteral("targetType"), QStringLiteral("targetQuantity"),
                 QStringLiteral("iff"), QStringLiteral("position"),
                 QStringLiteral("status"), QStringLiteral("time")});
    addFallback(&entries, QStringLiteral("47002"), {QStringLiteral("PositionReport")},
                QStringLiteral("Target Report"), {QStringLiteral("any")},
                {QStringLiteral("any")}, QStringLiteral("positionBroadcast"), {}, false,
                QStringLiteral("low"), 25, {QStringLiteral("position"), QStringLiteral("time")});
    addFallback(&entries, QStringLiteral("47003"), {QStringLiteral("TargetTrack")},
                QStringLiteral("Target Report"), {QStringLiteral("recon")},
                {QStringLiteral("commander")}, QStringLiteral("targetUpdated"), {}, true,
                QStringLiteral("high"), 80, {QStringLiteral("position"), QStringLiteral("status"),
                                              QStringLiteral("time")});
    addFallback(&entries, QStringLiteral("47004"), {QStringLiteral("TargetDestroyed")},
                QStringLiteral("Target Report"), {QStringLiteral("attack"), QStringLiteral("recon")},
                {QStringLiteral("commander")}, QStringLiteral("targetDestroyed"),
                {QStringLiteral("targetDestroyed")}, true, QStringLiteral("high"), 95,
                {QStringLiteral("targetId"), QStringLiteral("position"), QStringLiteral("status")});
    addFallback(&entries, QStringLiteral("47005"), {QStringLiteral("EngagementReport")},
                QStringLiteral("Target Report"), {QStringLiteral("attack")},
                {QStringLiteral("commander")}, QStringLiteral("engagementReported"), {}, true,
                QStringLiteral("medium"), 70, {QStringLiteral("targetId"), QStringLiteral("outcome")});
    addFallback(&entries, QStringLiteral("47006"), {QStringLiteral("StrikePlan"), QStringLiteral("FlightPlan")},
                QStringLiteral("Land Route"), {QStringLiteral("commander"), QStringLiteral("ground")},
                {QStringLiteral("attack")}, QStringLiteral("strikePlanned"),
                {QStringLiteral("strikeDispatched")}, true, QStringLiteral("high"), 88,
                {QStringLiteral("targetId"), QStringLiteral("route"), QStringLiteral("time")});
    addFallback(&entries, QStringLiteral("47007"), {QStringLiteral("AttackOrder")},
                QStringLiteral("Land Route"), {QStringLiteral("commander"), QStringLiteral("ground")},
                {QStringLiteral("attack")}, QStringLiteral("attackOrdered"),
                {QStringLiteral("strikeDispatched")}, true, QStringLiteral("high"), 86,
                {QStringLiteral("targetId"), QStringLiteral("route")}, QStringLiteral("route"));
    addFallback(&entries, QStringLiteral("47008"), {QStringLiteral("AttackOrder")},
                QStringLiteral("NetworkMonitoring"), {QStringLiteral("commander"), QStringLiteral("ground")},
                {QStringLiteral("attack")}, QStringLiteral("attackOrdered"),
                {QStringLiteral("strikeDispatched")}, true, QStringLiteral("high"), 86,
                {QStringLiteral("targetId")}, QStringLiteral("no-route"));
    addFallback(&entries, QStringLiteral("47009"), {QStringLiteral("GroundGuideOrder")},
                QStringLiteral("NetworkMonitoring"), {QStringLiteral("commander")},
                {QStringLiteral("ground")}, QStringLiteral("groundGuidanceRequested"),
                {QStringLiteral("groundGuidancePending")}, true, QStringLiteral("high"), 84,
                {QStringLiteral("targetId"), QStringLiteral("attackerId")});
    addFallback(&entries, QStringLiteral("47010"), {QStringLiteral("GroundAttackConfirm")},
                QStringLiteral("Land Route"), {QStringLiteral("ground")},
                {QStringLiteral("attack")}, QStringLiteral("groundAttackConfirmed"),
                {QStringLiteral("engaging")}, true, QStringLiteral("high"), 92,
                {QStringLiteral("targetId"), QStringLiteral("route")});
    addFallback(&entries, QStringLiteral("47011"), {QStringLiteral("Guidance")},
                QStringLiteral("Land Route"), {QStringLiteral("commander"), QStringLiteral("ground")},
                {QStringLiteral("attack")}, QStringLiteral("targetGuided"), {}, true,
                QStringLiteral("high"), 82, {QStringLiteral("targetId"), QStringLiteral("route")});
    addFallback(&entries, QStringLiteral("47012"),
                {QStringLiteral("Withdraw"), QStringLiteral("WithdrawOrder")},
                QStringLiteral("Land Route"), {QStringLiteral("commander")},
                {QStringLiteral("attack")}, QStringLiteral("withdrawOrdered"),
                {QStringLiteral("withdrawn")}, true, QStringLiteral("medium"), 75,
                {QStringLiteral("route"), QStringLiteral("time")});
    addFallback(&entries, QStringLiteral("47013"), {QStringLiteral("SharedDetect")},
                QStringLiteral("Target Report"), {QStringLiteral("recon")},
                {QStringLiteral("commander")}, QStringLiteral("targetShared"), {}, true,
                QStringLiteral("medium"), 65, {QStringLiteral("targetId"), QStringLiteral("position")});
    addFallback(&entries, QStringLiteral("47014"), {QStringLiteral("Ack")},
                QStringLiteral("NetworkMonitoring"), {QStringLiteral("any")},
                {QStringLiteral("any")}, QStringLiteral("acknowledged"), {}, false,
                QStringLiteral("low"), 15, {QStringLiteral("correlationId")});
    addFallback(&entries, QStringLiteral("47015"),
                {QStringLiteral("UnitOrder"), QStringLiteral("CommCheck"),
                 QStringLiteral("Pursue"), QStringLiteral("Halt"),
                 QStringLiteral("CancelEngagement"), QStringLiteral("SetRulesOfEngagement")},
                QStringLiteral("NetworkMonitoring"), {QStringLiteral("any")},
                {QStringLiteral("any")}, QStringLiteral("controlCommand"), {}, false,
                QStringLiteral("low"), 30, {QStringLiteral("command")});
    return VmfMessageCatalog::create(1, std::move(entries));
}

std::shared_ptr<VmfMessageCatalog> parseDocument(const QJsonDocument& document,
                                                  QList<Diagnostic>* diagnostics) {
    auto fail = [diagnostics](const QString& code, const QString& path,
                              const QString& message) {
        if (diagnostics) diagnostics->append({Diagnostic::Severity::Error, code, path, message});
    };
    if (!document.isObject()) {
        fail(QStringLiteral("CATALOG_ROOT"), {}, QStringLiteral("消息目录根节点必须是对象"));
        return {};
    }
    const QJsonObject root = document.object();
    const QJsonValue version = root.value(QStringLiteral("catalogVersion"));
    if (!version.isDouble() || version.toInt() != 1) {
        fail(QStringLiteral("CATALOG_VERSION"), QStringLiteral("/catalogVersion"),
             QStringLiteral("只支持 catalogVersion=1"));
        return {};
    }
    const QJsonArray messages = root.value(QStringLiteral("messages")).toArray();
    if (messages.isEmpty() || messages.size() > 256) {
        fail(QStringLiteral("CATALOG_MESSAGES"), QStringLiteral("/messages"),
             QStringLiteral("messages 必须包含 1 到 256 条记录"));
        return {};
    }
    QVector<MessageCatalogEntry> entries;
    QSet<QString> keys;
    for (int index = 0; index < messages.size(); ++index) {
        const QJsonValue value = messages.at(index);
        const QString path = QStringLiteral("/messages/%1").arg(index);
        if (!value.isObject()) {
            fail(QStringLiteral("CATALOG_ENTRY"), path, QStringLiteral("目录记录必须是对象"));
            return {};
        }
        const QJsonObject object = value.toObject();
        MessageCatalogEntry entry;
        if (!requiredString(object, QStringLiteral("catalogId"), &entry.catalogId)
            || !requiredString(object, QStringLiteral("vmfMessage"), &entry.vmfMessage)
            || !requiredString(object, QStringLiteral("trigger"), &entry.trigger)) {
            fail(QStringLiteral("CATALOG_FIELD"), path,
                 QStringLiteral("catalogId/vmfMessage/trigger 字段无效"));
            return {};
        }
        bool valid = false;
        entry.domainTypes = stringArray(object.value(QStringLiteral("domainTypes")), &valid);
        if (!valid || entry.domainTypes.isEmpty()) {
            fail(QStringLiteral("CATALOG_DOMAIN"), path, QStringLiteral("domainTypes 无效"));
            return {};
        }
        entry.senderRoles = stringArray(object.value(QStringLiteral("senderRoles")), &valid);
        if (!valid || entry.senderRoles.isEmpty()) {
            fail(QStringLiteral("CATALOG_SENDER"), path, QStringLiteral("senderRoles 无效"));
            return {};
        }
        entry.receiverRoles = stringArray(object.value(QStringLiteral("receiverRoles")), &valid);
        if (!valid || entry.receiverRoles.isEmpty()) {
            fail(QStringLiteral("CATALOG_RECEIVER"), path, QStringLiteral("receiverRoles 无效"));
            return {};
        }
        entry.preconditions = stringArray(object.value(QStringLiteral("preconditions")), &valid);
        if (!valid) {
            fail(QStringLiteral("CATALOG_PRECONDITION"), path, QStringLiteral("preconditions 无效"));
            return {};
        }
        entry.nextStages = stringArray(object.value(QStringLiteral("nextStages")), &valid);
        if (!valid) {
            fail(QStringLiteral("CATALOG_STAGE"), path, QStringLiteral("nextStages 无效"));
            return {};
        }
        entry.payloadCondition = object.value(QStringLiteral("payloadCondition")).toString().trimmed();
        if (!entry.payloadCondition.isEmpty()
            && entry.payloadCondition != QLatin1String("any")
            && entry.payloadCondition != QLatin1String("route")
            && entry.payloadCondition != QLatin1String("no-route")) {
            fail(QStringLiteral("CATALOG_CONDITION"), path, QStringLiteral("payloadCondition 无效"));
            return {};
        }
        if (!object.value(QStringLiteral("requiresAck")).isBool()
            || !object.value(QStringLiteral("automaticAck")).isBool()
            || !object.value(QStringLiteral("repeatable")).isBool()) {
            fail(QStringLiteral("CATALOG_ACK"), path, QStringLiteral("ACK 字段必须是布尔值"));
            return {};
        }
        entry.requiresAck = object.value(QStringLiteral("requiresAck")).toBool();
        entry.automaticAck = object.value(QStringLiteral("automaticAck")).toBool();
        entry.repeatable = object.value(QStringLiteral("repeatable")).toBool();
        const QJsonObject info = object.value(QStringLiteral("informationValue")).toObject();
        if (!requiredString(info, QStringLiteral("level"), &entry.informationValue.level)
            || !info.value(QStringLiteral("score")).isDouble()
            || info.value(QStringLiteral("score")).toInt(-1) < 0
            || info.value(QStringLiteral("score")).toInt(-1) > 100) {
            fail(QStringLiteral("CATALOG_VALUE"), path, QStringLiteral("informationValue 无效"));
            return {};
        }
        entry.informationValue.score = info.value(QStringLiteral("score")).toInt();
        entry.informationValue.fields = stringArray(info.value(QStringLiteral("fields")), &valid);
        if (!valid) {
            fail(QStringLiteral("CATALOG_VALUE_FIELDS"), path,
                 QStringLiteral("informationValue.fields 无效"));
            return {};
        }
        for (const QString& domain : entry.domainTypes) {
            const QString key = domain + QLatin1Char('\x1f') + entry.vmfMessage
                + QLatin1Char('\x1f') + entry.payloadCondition;
            if (keys.contains(key)) {
                fail(QStringLiteral("CATALOG_DUPLICATE"), path,
                     QStringLiteral("domainType/vmfMessage/payloadCondition 重复"));
                return {};
            }
            keys.insert(key);
        }
        entries.append(std::move(entry));
    }
    return VmfMessageCatalog::create(1, std::move(entries));
}

} // namespace

QJsonObject InformationValue::toJson() const {
    QJsonArray values;
    for (const QString& field : fields) values.append(field);
    return QJsonObject{{QStringLiteral("level"), level},
                       {QStringLiteral("score"), score},
                       {QStringLiteral("fields"), values}};
}

QJsonObject MessageCatalogEntry::toJson() const {
    QJsonArray domains;
    for (const QString& value : domainTypes) domains.append(value);
    QJsonArray senders;
    for (const QString& value : senderRoles) senders.append(value);
    QJsonArray receivers;
    for (const QString& value : receiverRoles) receivers.append(value);
    QJsonArray preconditionsJson;
    for (const QString& value : preconditions) preconditionsJson.append(value);
    QJsonArray stages;
    for (const QString& value : nextStages) stages.append(value);
    QJsonObject result{{QStringLiteral("catalogId"), catalogId},
                       {QStringLiteral("domainTypes"), domains},
                       {QStringLiteral("vmfMessage"), vmfMessage},
                       {QStringLiteral("senderRoles"), senders},
                       {QStringLiteral("receiverRoles"), receivers},
                       {QStringLiteral("trigger"), trigger},
                       {QStringLiteral("preconditions"), preconditionsJson},
                       {QStringLiteral("nextStages"), stages},
                       {QStringLiteral("requiresAck"), requiresAck},
                       {QStringLiteral("automaticAck"), automaticAck},
                       {QStringLiteral("repeatable"), repeatable},
                       {QStringLiteral("informationValue"), informationValue.toJson()}};
    if (!payloadCondition.isEmpty()) {
        result.insert(QStringLiteral("payloadCondition"), payloadCondition);
    }
    return result;
}

bool MessageCatalogEntry::matches(const QString& domainType,
                                  const QString& selectedVmfMessage,
                                  const QJsonObject& payload) const {
    return domainTypes.contains(domainType) && vmfMessage == selectedVmfMessage
        && payloadConditionMatches(payloadCondition, payload);
}

std::shared_ptr<const VmfMessageCatalog> VmfMessageCatalog::fromFile(
    const QString& path, QList<Diagnostic>* diagnostics) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (diagnostics) diagnostics->append({Diagnostic::Severity::Error,
                                              QStringLiteral("CATALOG_OPEN"), path,
                                              QStringLiteral("无法打开 VMF 消息目录")});
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (diagnostics) diagnostics->append({Diagnostic::Severity::Error,
                                              QStringLiteral("CATALOG_JSON"), path,
                                              parseError.errorString()});
        return {};
    }
    return parseDocument(document, diagnostics);
}

std::shared_ptr<const VmfMessageCatalog> VmfMessageCatalog::loadDesignV1(
    const QString& rootDirectory, QList<Diagnostic>* diagnostics) {
    const QString path = QDir(rootDirectory).filePath(QStringLiteral("message_catalog.json"));
    return fromFile(path, diagnostics);
}

std::shared_ptr<const VmfMessageCatalog> VmfMessageCatalog::designV1() {
#ifdef WARGAME_DESIGN_ROOT
    QList<Diagnostic> diagnostics;
    if (const auto loaded = loadDesignV1(QStringLiteral(WARGAME_DESIGN_ROOT), &diagnostics);
        loaded) return loaded;
#endif
    return fallbackCatalog();
}

std::shared_ptr<VmfMessageCatalog> VmfMessageCatalog::create(
    int version, QVector<MessageCatalogEntry> entries) {
    auto catalog = std::make_shared<VmfMessageCatalog>();
    catalog->m_version = version;
    catalog->m_entries = std::move(entries);
    return catalog;
}

std::optional<MessageCatalogEntry> VmfMessageCatalog::entryFor(
    const QString& domainType, const QString& vmfMessage, const QJsonObject& payload) const {
    for (const MessageCatalogEntry& entry : m_entries) {
        if (entry.matches(domainType, vmfMessage, payload)) return entry;
    }
    return std::nullopt;
}

std::optional<MessageCatalogEntry> VmfMessageCatalog::entryForDomain(
    const QString& domainType, const QJsonObject& payload) const {
    for (const MessageCatalogEntry& entry : m_entries) {
        if (entry.domainTypes.contains(domainType)
            && payloadConditionMatches(entry.payloadCondition, payload)) return entry;
    }
    return std::nullopt;
}

bool VmfMessageCatalog::validate(const QString& domainType, const QString& vmfMessage,
                                 const QJsonObject& payload, const QString& senderRole,
                                 const QString& receiverRole, QString* error) const {
    if (error) error->clear();
    const auto entry = entryFor(domainType, vmfMessage, payload);
    if (!entry.has_value()) {
        if (error) *error = QStringLiteral("VMF 消息目录没有匹配映射: %1 -> %2")
            .arg(domainType, vmfMessage);
        return false;
    }
    if (!roleAllowed(entry->senderRoles, senderRole)) {
        if (error) *error = QStringLiteral("VMF 发送角色不在消息目录允许范围内: %1")
            .arg(senderRole);
        return false;
    }
    if (!roleAllowed(entry->receiverRoles, receiverRole)) {
        if (error) *error = QStringLiteral("VMF 接收角色不在消息目录允许范围内: %1")
            .arg(receiverRole);
        return false;
    }
    return true;
}

QJsonObject VmfMessageCatalog::summaryFor(const QString& domainType,
                                          const QString& vmfMessage,
                                          const QJsonObject& payload) const {
    const auto entry = entryFor(domainType, vmfMessage, payload);
    if (!entry.has_value()) return {};
    return QJsonObject{{QStringLiteral("catalogId"), entry->catalogId},
                       {QStringLiteral("domainType"), domainType},
                       {QStringLiteral("vmfMessage"), entry->vmfMessage},
                       {QStringLiteral("senderRoles"), QJsonArray::fromStringList(entry->senderRoles)},
                       {QStringLiteral("receiverRoles"), QJsonArray::fromStringList(entry->receiverRoles)},
                       {QStringLiteral("trigger"), entry->trigger},
                       {QStringLiteral("nextStages"), QJsonArray::fromStringList(entry->nextStages)},
                       {QStringLiteral("requiresAck"), entry->requiresAck},
                       {QStringLiteral("automaticAck"), entry->automaticAck},
                       {QStringLiteral("repeatable"), entry->repeatable},
                       {QStringLiteral("informationValue"), entry->informationValue.toJson()}};
}

QJsonObject VmfMessageCatalog::toJson() const {
    QJsonArray messages;
    for (const MessageCatalogEntry& entry : m_entries) messages.append(entry.toJson());
    return QJsonObject{{QStringLiteral("catalogVersion"), m_version},
                       {QStringLiteral("messages"), messages}};
}

} // namespace gbr::vmf
