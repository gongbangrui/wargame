#include "RulesAi.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kDefaultMapWidth = 20000.0;
constexpr double kDefaultMapHeight = 15000.0;

double clampCoordinate(double value, double extent) {
    if (!std::isfinite(value)) return 0.0;
    return std::clamp(value, 0.0, std::max(0.0, extent));
}

quint64 stableHash(const QString& value) {
    quint64 hash = 1469598103934665603ULL;
    for (const QChar character : value) {
        const ushort code = character.unicode();
        hash ^= static_cast<quint64>(code & 0xffU);
        hash *= 1099511628211ULL;
        hash ^= static_cast<quint64>((code >> 8U) & 0xffU);
        hash *= 1099511628211ULL;
    }
    return hash;
}

QList<QPair<double, double>> patrolRing(double width, double height) {
    const QList<double> xValues{width * 0.25, width * 0.50, width * 0.75};
    const QList<double> yValues{height * 0.25, height * 0.50, height * 0.75};
    QList<QPair<double, double>> points;
    points.reserve(8);
    points.append({xValues.at(0), yValues.at(0)});
    points.append({xValues.at(1), yValues.at(0)});
    points.append({xValues.at(2), yValues.at(0)});
    points.append({xValues.at(2), yValues.at(1)});
    points.append({xValues.at(2), yValues.at(2)});
    points.append({xValues.at(1), yValues.at(2)});
    points.append({xValues.at(0), yValues.at(2)});
    points.append({xValues.at(0), yValues.at(1)});
    return points;
}

double distance2(double ax, double ay, double bx, double by) {
    const double dx = ax - bx;
    const double dy = ay - by;
    return dx * dx + dy * dy;
}

}

namespace gbr {

AiDifficultyParameters RulesAi::parameters(const QString& difficulty) {
    if (difficulty == QLatin1String("easy")) {
        return {4000, 60000, 1, 1, 0.20};
    }
    if (difficulty == QLatin1String("hard")) {
        return {1000, 15000, 4, 4, 0.0};
    }
    return {2000, 30000, 2, 2, 0.08};
}

quint64 RulesAi::nextRandom(quint64* state) {
    if (!state) return 1;
    if (*state == 0) *state = 1;
    quint64 value = *state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value == 0 ? 1 : value;
    return *state;
}

AiPlanV1 RulesAi::makeCommanderPlan(const QList<AiSeatState>& seats,
                                    const QString& requestId,
                                    quint64 matchGeneration,
                                    quint64 sourceStateRevision,
                                    double validUntil,
                                    quint64* rngState,
                                    double suboptimalRate,
                                    double mapWidth,
                                    double mapHeight,
                                    quint64 planningGeneration) {
    AiPlanV1 plan;
    plan.requestId = requestId;
    plan.matchGeneration = matchGeneration;
    plan.sourceStateRevision = sourceStateRevision;
    plan.planningGeneration = planningGeneration;
    if (!std::isfinite(mapWidth) || mapWidth <= 0.0) mapWidth = kDefaultMapWidth;
    if (!std::isfinite(mapHeight) || mapHeight <= 0.0) mapHeight = kDefaultMapHeight;
    const double minimumDisplacement = std::max(50.0, std::min(mapWidth, mapHeight) * 0.01);
    const double minimumDisplacementSquared = minimumDisplacement * minimumDisplacement;
    const QList<QPair<double, double>> patrolPoints = patrolRing(mapWidth, mapHeight);
    QList<AiSeatState> candidates = seats;
    std::sort(candidates.begin(), candidates.end(), [](const AiSeatState& left,
                                                       const AiSeatState& right) {
        return left.seatId < right.seatId;
    });
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const AiSeatState& seat) {
        return !seat.alive || !seat.movable || seat.kind == QLatin1String("commandpost")
            || !seat.seatId.startsWith(QLatin1String("blue_"))
            || seat.seatId.isEmpty() || seat.unitId.isEmpty();
    }), candidates.end());
    int priority = 100;
    for (const AiSeatState& seat : candidates) {
        AiObjectiveV1 objective;
        objective.seatId = seat.seatId;
        objective.priority = priority--;
        objective.validUntil = validUntil;
        if (seat.targetVisible && !seat.targetId.isEmpty()
            && seat.kind == QLatin1String("attackuav")) {
            objective.action = QStringLiteral("attack");
            objective.targetId = seat.targetId;
        } else {
            if (seat.kind == QLatin1String("reconuav")) objective.action = QStringLiteral("search");
            else if (seat.kind == QLatin1String("jammeruav")) objective.action = QStringLiteral("jam");
            else if (seat.kind == QLatin1String("groundscout")) objective.action = QStringLiteral("guard");
            else objective.action = QStringLiteral("patrol");

            double desiredX = seat.x;
            double desiredY = seat.y;
            if (seat.targetVisible && !seat.targetId.isEmpty()) {
                desiredX = seat.x + (seat.targetX - seat.x) * 0.60;
                desiredY = seat.y + (seat.targetY - seat.y) * 0.60;
            }
            desiredX = clampCoordinate(desiredX, mapWidth);
            desiredY = clampCoordinate(desiredY, mapHeight);
            bool regionAvailable = true;
            if (!seat.targetVisible || distance2(desiredX, desiredY, seat.x, seat.y)
                    < minimumDisplacementSquared) {
                const quint64 start = (stableHash(seat.seatId)
                                        + planningGeneration) % static_cast<quint64>(patrolPoints.size());
                bool found = false;
                for (int offset = 0; offset < patrolPoints.size(); ++offset) {
                    const auto point = patrolPoints.at(static_cast<int>(
                        (start + static_cast<quint64>(offset))
                        % static_cast<quint64>(patrolPoints.size())));
                    const double candidateX = clampCoordinate(point.first, mapWidth);
                    const double candidateY = clampCoordinate(point.second, mapHeight);
                    if (distance2(candidateX, candidateY, seat.x, seat.y)
                            >= minimumDisplacementSquared) {
                        desiredX = candidateX;
                        desiredY = candidateY;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    regionAvailable = false;
                }
            }
            if (regionAvailable) {
                objective.region = QJsonObject{{QStringLiteral("x"), desiredX},
                                                {QStringLiteral("y"), desiredY}};
            }
        }
        if (rngState && objective.action != QLatin1String("attack")
            && (nextRandom(rngState) % 10000)
                < static_cast<quint64>(std::clamp(suboptimalRate, 0.0, 1.0) * 10000.0)
            && objective.action != QLatin1String("defend")) {
            objective.action = QStringLiteral("defend");
            objective.targetId.clear();
            objective.region = {};
        }
        plan.objectives.append(objective);
    }
    return plan;
}

QList<AiCommand> RulesAi::commandsForPlan(const AiPlanV1& plan,
                                          const QList<AiSeatState>& seats,
                                          double now) {
    QList<AiCommand> commands;
    QList<AiObjectiveV1> objectives = plan.objectives;
    std::stable_sort(objectives.begin(), objectives.end(), [](const AiObjectiveV1& left,
                                                               const AiObjectiveV1& right) {
        if (left.priority != right.priority) return left.priority > right.priority;
        return left.seatId < right.seatId;
    });
    for (const AiObjectiveV1& objective : objectives) {
        if (objective.validUntil < now) continue;
        if (!objective.seatId.startsWith(QLatin1String("blue_"))) continue;
        const auto it = std::find_if(seats.cbegin(), seats.cend(),
                                     [&objective](const AiSeatState& seat) {
                                         return seat.seatId == objective.seatId;
                                     });
        if (it == seats.cend() || it->unitId.isEmpty() || !it->alive || !it->movable
            || it->kind == QLatin1String("commandpost")) {
            continue;
        }
        AiCommand command;
        command.seatId = it->seatId;
        command.priority = objective.priority;
        if (objective.action == QLatin1String("attack") && !objective.targetId.isEmpty()) {
            if (it->kind != QLatin1String("attackuav")) continue;
            command.action = QStringLiteral("engageTarget");
            command.args = {{QStringLiteral("attackerId"), it->unitId},
                            {QStringLiteral("targetId"), objective.targetId}};
        } else if (objective.action == QLatin1String("search")
                   || objective.action == QLatin1String("patrol")
                   || objective.action == QLatin1String("guard")
                   || objective.action == QLatin1String("jam")) {
            const QJsonObject region = objective.region;
            if (!region.contains(QStringLiteral("x")) || !region.contains(QStringLiteral("y"))
                || !region.value(QStringLiteral("x")).isDouble()
                || !region.value(QStringLiteral("y")).isDouble()) {
                continue;
            }
            const double x = region.value(QStringLiteral("x")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            const double y = region.value(QStringLiteral("y")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            if (distance2(x, y, it->x, it->y) < 2500.0) continue;
            command.action = QStringLiteral("moveTo");
            command.args = {{QStringLiteral("unitId"), it->unitId},
                            {QStringLiteral("pos"), QVariantMap{{QStringLiteral("x"), x},
                                                                   {QStringLiteral("y"), y}}}};
        } else if (objective.action == QLatin1String("withdraw")) {
            command.action = QStringLiteral("withdraw");
            command.args = {{QStringLiteral("unitId"), it->unitId}};
        } else if (objective.action == QLatin1String("defend")) {
            command.action = QStringLiteral("halt");
            command.args = {{QStringLiteral("unitId"), it->unitId}};
        } else {
            continue;
        }
        commands.append(command);
    }
    return commands;
}

}
