#include "OllamaPlanningContract.h"

#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace gbr {

namespace {

QJsonArray strings(const QStringList& values) {
    QJsonArray output;
    for (const QString& value : values) output.append(value);
    return output;
}

QStringList sortedUnique(QStringList values) {
    values.removeDuplicates();
    values.sort();
    return values;
}

bool movementAction(const QString& action) {
    return action == QLatin1String("search") || action == QLatin1String("patrol")
        || action == QLatin1String("guard") || action == QLatin1String("jam");
}

QJsonObject metadataProperty(const QString& type, const QJsonValue& value) {
    return QJsonObject{{QStringLiteral("type"), type},
                       {QStringLiteral("const"), value}};
}

}

QStringList OllamaPlanningContract::actionsFor(const OllamaSeatConstraint& seat) {
    QStringList actions{QStringLiteral("defend"), QStringLiteral("patrol"),
                        QStringLiteral("withdraw")};
    if (seat.kind == QLatin1String("attackuav") && !seat.visibleTargetIds.isEmpty()) {
        actions.append(QStringLiteral("attack"));
    } else if (seat.kind == QLatin1String("reconuav")) {
        actions.append(QStringLiteral("search"));
    } else if (seat.kind == QLatin1String("jammeruav")) {
        actions.append(QStringLiteral("jam"));
    } else if (seat.kind == QLatin1String("groundscout")) {
        actions.append(QStringLiteral("guard"));
    }
    return sortedUnique(actions);
}

QJsonArray OllamaPlanningContract::messagesFor(const OllamaPlanRequest& request) {
    const QJsonObject metadata{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("requestId"), request.requestId},
        {QStringLiteral("matchGeneration"), static_cast<qint64>(request.matchGeneration)},
        {QStringLiteral("sourceStateRevision"),
         static_cast<qint64>(request.sourceStateRevision)},
        {QStringLiteral("planningGeneration"),
         static_cast<qint64>(request.planningGeneration)},
        {QStringLiteral("validUntil"), request.validUntil}};
    QList<OllamaSeatConstraint> seats = request.mobileSeats;
    std::sort(seats.begin(), seats.end(), [](const OllamaSeatConstraint& left,
                                             const OllamaSeatConstraint& right) {
        return left.seatId < right.seatId;
    });
    QJsonArray mobileSeats;
    for (const OllamaSeatConstraint& seat : seats) {
        mobileSeats.append(QJsonObject{
            {QStringLiteral("seatId"), seat.seatId},
            {QStringLiteral("unitId"), seat.unitId},
            {QStringLiteral("kind"), seat.kind},
            {QStringLiteral("position"),
             QJsonObject{{QStringLiteral("x"), seat.x}, {QStringLiteral("y"), seat.y}}},
            {QStringLiteral("actions"), strings(actionsFor(seat))},
            {QStringLiteral("visibleTargetIds"),
             strings(sortedUnique(seat.visibleTargetIds))}});
    }
    const QJsonObject userContent{
        {QStringLiteral("metadata"), metadata},
        {QStringLiteral("map"),
         QJsonObject{{QStringLiteral("widthMeters"), request.mapWidth},
                     {QStringLiteral("heightMeters"), request.mapHeight}}},
        {QStringLiteral("mobileSeats"), mobileSeats},
        {QStringLiteral("projectedState"), request.projection}};
    return QJsonArray{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                    {QStringLiteral("content"),
                     QStringLiteral("Return one valid AiPlanV1 objective for every mobile seat. "
                                    "Use only the supplied actions, targets, coordinates, and metadata.")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"),
                     QString::fromUtf8(QJsonDocument(userContent)
                                           .toJson(QJsonDocument::Compact))}}};
}

QJsonObject OllamaPlanningContract::schemaFor(const OllamaPlanRequest& request) {
    QStringList seatIds;
    QStringList targetIds;
    QStringList actions;
    for (const OllamaSeatConstraint& seat : request.mobileSeats) {
        seatIds.append(seat.seatId);
        targetIds.append(seat.visibleTargetIds);
        actions.append(actionsFor(seat));
    }
    seatIds = sortedUnique(seatIds);
    targetIds = sortedUnique(targetIds);
    actions = sortedUnique(actions);
    const QJsonObject region{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("x"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
                          {QStringLiteral("minimum"), 0.0},
                          {QStringLiteral("maximum"), request.mapWidth}}},
             {QStringLiteral("y"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
                          {QStringLiteral("minimum"), 0.0},
                          {QStringLiteral("maximum"), request.mapHeight}}}}},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("x"), QStringLiteral("y")}}};
    QJsonObject objectiveProperties{
        {QStringLiteral("action"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                     {QStringLiteral("enum"), strings(actions)}}},
        {QStringLiteral("priority"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                     {QStringLiteral("minimum"), 0},
                     {QStringLiteral("maximum"), 100}}},
        {QStringLiteral("seatId"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                     {QStringLiteral("enum"), strings(seatIds)}}},
        {QStringLiteral("region"), region},
        {QStringLiteral("validUntil"),
         metadataProperty(QStringLiteral("number"), request.validUntil)}};
    if (!targetIds.isEmpty()) {
        objectiveProperties.insert(
            QStringLiteral("targetId"),
            QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                        {QStringLiteral("enum"), strings(targetIds)}});
    }
    const QJsonObject objective{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"), objectiveProperties},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("action"), QStringLiteral("priority"),
                    QStringLiteral("seatId"), QStringLiteral("validUntil")}}};
    QJsonArray exactSeatConstraints;
    for (const QString& seatId : seatIds) {
        exactSeatConstraints.append(QJsonObject{
            {QStringLiteral("contains"),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                         {QStringLiteral("properties"),
                          QJsonObject{{QStringLiteral("seatId"),
                                       QJsonObject{{QStringLiteral("const"), seatId}}}}},
                         {QStringLiteral("required"), QJsonArray{QStringLiteral("seatId")}}}},
            {QStringLiteral("minContains"), 1},
            {QStringLiteral("maxContains"), 1}});
    }
    const QJsonObject objectives{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("minItems"), seatIds.size()},
        {QStringLiteral("maxItems"), seatIds.size()},
        {QStringLiteral("uniqueItems"), true},
        {QStringLiteral("items"), objective},
        {QStringLiteral("allOf"), exactSeatConstraints}};
    const QJsonObject properties{
        {QStringLiteral("schemaVersion"), metadataProperty(QStringLiteral("integer"), 1)},
        {QStringLiteral("requestId"),
         metadataProperty(QStringLiteral("string"), request.requestId)},
        {QStringLiteral("matchGeneration"),
         metadataProperty(QStringLiteral("integer"),
                          static_cast<qint64>(request.matchGeneration))},
        {QStringLiteral("sourceStateRevision"),
         metadataProperty(QStringLiteral("integer"),
                          static_cast<qint64>(request.sourceStateRevision))},
        {QStringLiteral("planningGeneration"),
         metadataProperty(QStringLiteral("integer"),
                          static_cast<qint64>(request.planningGeneration))},
        {QStringLiteral("objectives"), objectives}};
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"), properties},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("schemaVersion"), QStringLiteral("requestId"),
                    QStringLiteral("matchGeneration"), QStringLiteral("sourceStateRevision"),
                    QStringLiteral("planningGeneration"), QStringLiteral("objectives")}}};
}

OllamaModelSelection OllamaPlanningContract::selectModel(const QString& configuredModel,
                                                          const QJsonArray& models) {
    struct Candidate {
        QString name;
        qint64 size = std::numeric_limits<qint64>::max();
        bool compatible = false;
    };
    QList<Candidate> candidates;
    for (const QJsonValue& value : models) {
        if (!value.isObject()) continue;
        const QJsonObject model = value.toObject();
        const QString name = model.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) continue;
        const QJsonValue capabilities = model.value(QStringLiteral("capabilities"));
        const bool legacy = capabilities.isUndefined() || capabilities.isNull();
        const bool compatible = legacy || (capabilities.isArray()
            && capabilities.toArray().contains(QStringLiteral("completion")));
        const qint64 size = model.value(QStringLiteral("size")).toInteger(
            std::numeric_limits<qint64>::max());
        candidates.append(Candidate{name, size >= 0 ? size : std::numeric_limits<qint64>::max(),
                                    compatible});
    }
    if (configuredModel != QLatin1String("auto")) {
        const auto match = std::find_if(candidates.cbegin(), candidates.cend(),
                                        [&configuredModel](const Candidate& candidate) {
            return candidate.name == configuredModel;
        });
        if (match == candidates.cend()) return {{}, QStringLiteral("model_missing")};
        return match->compatible
            ? OllamaModelSelection{match->name, {}}
            : OllamaModelSelection{{}, QStringLiteral("model_incompatible")};
    }
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [](const Candidate& candidate) {
        return !candidate.compatible;
    }), candidates.end());
    if (candidates.isEmpty()) {
        return {{}, models.isEmpty() ? QStringLiteral("model_missing")
                                     : QStringLiteral("model_incompatible")};
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left,
                                                        const Candidate& right) {
        const bool leftPreferred = left.name == QLatin1String("qwen3.5:2b");
        const bool rightPreferred = right.name == QLatin1String("qwen3.5:2b");
        if (leftPreferred != rightPreferred) return leftPreferred;
        if (left.size != right.size) return left.size < right.size;
        return left.name < right.name;
    });
    return {candidates.first().name, {}};
}

QString OllamaPlanningContract::validatePlan(const AiPlanV1& plan,
                                              const OllamaPlanRequest& request) {
    if (plan.schemaVersion != 1 || plan.requestId != request.requestId
        || plan.matchGeneration != request.matchGeneration
        || plan.sourceStateRevision != request.sourceStateRevision
        || plan.planningGeneration != request.planningGeneration) {
        return QStringLiteral("stale_response");
    }
    if (plan.objectives.size() != request.mobileSeats.size()) {
        return QStringLiteral("semantic_objective_count");
    }
    QHash<QString, OllamaSeatConstraint> seats;
    for (const OllamaSeatConstraint& seat : request.mobileSeats) seats.insert(seat.seatId, seat);
    QSet<QString> visited;
    for (const AiObjectiveV1& objective : plan.objectives) {
        if (!seats.contains(objective.seatId)) return QStringLiteral("semantic_unknown_seat");
        if (visited.contains(objective.seatId)) return QStringLiteral("semantic_duplicate_seat");
        visited.insert(objective.seatId);
        const OllamaSeatConstraint seat = seats.value(objective.seatId);
        if (!actionsFor(seat).contains(objective.action)) {
            return QStringLiteral("semantic_action_not_allowed");
        }
        if (std::abs(objective.validUntil - request.validUntil) > 1e-6) {
            return QStringLiteral("semantic_expiry_invalid");
        }
        if (objective.action == QLatin1String("attack")) {
            if (objective.targetId.isEmpty()) return QStringLiteral("semantic_target_required");
            if (!objective.region.isEmpty()) return QStringLiteral("semantic_region_forbidden");
            if (!seat.visibleTargetIds.contains(objective.targetId)) {
                return QStringLiteral("semantic_target_not_visible");
            }
            continue;
        }
        if (!objective.targetId.isEmpty()) return QStringLiteral("semantic_target_forbidden");
        if (movementAction(objective.action)) {
            if (objective.region.isEmpty()) return QStringLiteral("semantic_region_required");
            const double x = objective.region.value(QStringLiteral("x")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            const double y = objective.region.value(QStringLiteral("y")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || y < 0.0
                || x > request.mapWidth || y > request.mapHeight) {
                return QStringLiteral("semantic_region_out_of_bounds");
            }
            const double minimum = std::max(
                50.0, std::min(request.mapWidth, request.mapHeight) * 0.01);
            const double dx = x - seat.x;
            const double dy = y - seat.y;
            if (dx * dx + dy * dy < minimum * minimum) {
                return QStringLiteral("semantic_region_too_close");
            }
        } else if (!objective.region.isEmpty()) {
            return QStringLiteral("semantic_region_forbidden");
        }
    }
    return {};
}

}
