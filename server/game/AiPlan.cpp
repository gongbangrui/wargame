#include "AiPlan.h"

#include <QJsonArray>
#include <QSet>
#include <QJsonValue>

#include <cmath>

namespace gbr {

namespace {

constexpr int kMaxObjectives = 16;
constexpr int kMaxRequestIdLength = 128;
constexpr int kMaxSeatIdLength = 64;
constexpr int kMaxTargetIdLength = 64;

bool identifier(const QString& value, int maximum) {
    if (value.isEmpty() || value.size() > maximum) return false;
    for (const QChar character : value) {
        if (character.isLetterOrNumber() || character == QLatin1Char('_')
            || character == QLatin1Char(':') || character == QLatin1Char('-')) continue;
        return false;
    }
    return true;
}

bool exactKeys(const QJsonObject& object, const QSet<QString>& allowed) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!allowed.contains(it.key())) return false;
    }
    return true;
}

bool finiteNumber(const QJsonValue& value) {
    return value.isDouble() && std::isfinite(value.toDouble());
}

bool parseObjective(const QJsonValue& value, AiObjectiveV1* objective) {
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    if (!exactKeys(object, {QStringLiteral("action"), QStringLiteral("priority"),
                            QStringLiteral("seatId"), QStringLiteral("targetId"),
                            QStringLiteral("region"), QStringLiteral("validUntil")})
        || !object.value(QStringLiteral("action")).isString()
        || !isAiObjectiveAction(object.value(QStringLiteral("action")).toString())
        || !object.value(QStringLiteral("priority")).isDouble()
        || object.value(QStringLiteral("priority")).toInt() < 0
        || object.value(QStringLiteral("priority")).toInt() > 100
        || !object.value(QStringLiteral("seatId")).isString()) {
        return false;
    }
    const QString seatId = object.value(QStringLiteral("seatId")).toString();
    if (!seatId.startsWith(QLatin1String("blue_")) || !identifier(seatId, kMaxSeatIdLength)) {
        return false;
    }
    const QString action = object.value(QStringLiteral("action")).toString();
    const bool movementAction = action == QLatin1String("search")
        || action == QLatin1String("patrol") || action == QLatin1String("guard")
        || action == QLatin1String("jam");
    if (movementAction && (!object.contains(QStringLiteral("region"))
                           || !object.value(QStringLiteral("region")).isObject())) {
        return false;
    }
    const QString targetId = object.value(QStringLiteral("targetId")).toString();
    if (object.contains(QStringLiteral("targetId"))
        && !targetId.isEmpty() && !identifier(targetId, kMaxTargetIdLength)) return false;
    if (object.contains(QStringLiteral("region"))) {
        const QJsonObject region = object.value(QStringLiteral("region")).toObject();
        if (region.isEmpty() || !exactKeys(region, {QStringLiteral("x"), QStringLiteral("y")})) {
            return false;
        }
        if (!finiteNumber(region.value(QStringLiteral("x")))
            || !finiteNumber(region.value(QStringLiteral("y")))) return false;
    }
    const double validUntil = object.value(QStringLiteral("validUntil")).toDouble();
    if (!object.contains(QStringLiteral("validUntil")) || !finiteNumber(object.value(QStringLiteral("validUntil")))
        || validUntil < 0.0) return false;
    if (objective) {
        objective->action = object.value(QStringLiteral("action")).toString();
        objective->priority = object.value(QStringLiteral("priority")).toInt();
        objective->seatId = seatId;
        objective->targetId = targetId;
        objective->region = object.value(QStringLiteral("region")).toObject();
        objective->validUntil = validUntil;
    }
    return true;
}

}

bool isAiObjectiveAction(const QString& action) {
    return action == QLatin1String("defend") || action == QLatin1String("withdraw")
        || action == QLatin1String("search") || action == QLatin1String("attack")
        || action == QLatin1String("guard") || action == QLatin1String("jam")
        || action == QLatin1String("patrol");
}

QJsonObject AiPlanV1::toJson() const {
    QJsonArray objectivesJson;
    for (const AiObjectiveV1& objective : objectives) {
        QJsonObject item{{QStringLiteral("action"), objective.action},
                         {QStringLiteral("priority"), objective.priority},
                         {QStringLiteral("seatId"), objective.seatId},
                         {QStringLiteral("validUntil"), objective.validUntil}};
        if (!objective.targetId.isEmpty()) item[QStringLiteral("targetId")] = objective.targetId;
        if (!objective.region.isEmpty()) item[QStringLiteral("region")] = objective.region;
        objectivesJson.append(item);
    }
    QJsonObject result{{QStringLiteral("schemaVersion"), schemaVersion},
                       {QStringLiteral("requestId"), requestId},
                       {QStringLiteral("matchGeneration"), static_cast<qint64>(matchGeneration)},
                       {QStringLiteral("sourceStateRevision"),
                        static_cast<qint64>(sourceStateRevision)},
                       {QStringLiteral("objectives"), objectivesJson}};
    if (planningGeneration > 0) {
        result[QStringLiteral("planningGeneration")] = static_cast<qint64>(planningGeneration);
    }
    return result;
}

bool AiPlanV1::fromJson(const QJsonObject& object, AiPlanV1* plan, QString* error) {
    if (error) error->clear();
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!exactKeys(object, {QStringLiteral("schemaVersion"), QStringLiteral("requestId"),
                            QStringLiteral("matchGeneration"),
                            QStringLiteral("sourceStateRevision"),
                            QStringLiteral("planningGeneration"),
                            QStringLiteral("objectives")})
        || object.value(QStringLiteral("schemaVersion")).toInt() != 1
        || !object.value(QStringLiteral("requestId")).isString()
        || !identifier(object.value(QStringLiteral("requestId")).toString(), kMaxRequestIdLength)
        || !object.value(QStringLiteral("matchGeneration")).isDouble()
        || object.value(QStringLiteral("matchGeneration")).toInteger() <= 0
        || !object.value(QStringLiteral("sourceStateRevision")).isDouble()
        || object.value(QStringLiteral("sourceStateRevision")).toInteger() <= 0
        || (object.contains(QStringLiteral("planningGeneration"))
            && (!object.value(QStringLiteral("planningGeneration")).isDouble()
                || object.value(QStringLiteral("planningGeneration")).toInteger() <= 0))
        || !object.value(QStringLiteral("objectives")).isArray()) {
        return fail(QStringLiteral("invalid AiPlanV1 envelope"));
    }
    const QJsonArray values = object.value(QStringLiteral("objectives")).toArray();
    if (values.size() > kMaxObjectives) return fail(QStringLiteral("too many AI objectives"));
    QList<AiObjectiveV1> objectives;
    for (const QJsonValue& value : values) {
        AiObjectiveV1 objective;
        if (!parseObjective(value, &objective)) return fail(QStringLiteral("invalid AI objective"));
        objectives.append(objective);
    }
    if (plan) {
        plan->schemaVersion = 1;
        plan->requestId = object.value(QStringLiteral("requestId")).toString();
        plan->matchGeneration = static_cast<quint64>(
            object.value(QStringLiteral("matchGeneration")).toInteger());
        plan->sourceStateRevision = static_cast<quint64>(
            object.value(QStringLiteral("sourceStateRevision")).toInteger());
        plan->planningGeneration = static_cast<quint64>(
            object.value(QStringLiteral("planningGeneration")).toInteger());
        plan->objectives = objectives;
    }
    return true;
}

}
