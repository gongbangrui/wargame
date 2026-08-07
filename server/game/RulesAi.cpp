#include "RulesAi.h"

#include "core/SimulationEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <QHash>
#include <QJsonValue>

namespace {

constexpr double kDefaultMapWidth = 20000.0;
constexpr double kDefaultMapHeight = 15000.0;
constexpr double kMinimumCommandDisplacement = 50.0;
constexpr double kSpeedChangeThreshold = 0.5;
constexpr double kSpeedSettlingSeconds = 8.0;

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

double distance(double ax, double ay, double bx, double by) {
    return std::sqrt(distance2(ax, ay, bx, by));
}

bool hasVisibleTarget(const gbr::AiSeatState& seat) {
    return seat.targetVisible && !seat.targetId.isEmpty();
}

bool hasVisibleTargetPosition(const gbr::AiSeatState& seat) {
    return hasVisibleTarget(seat) && std::isfinite(seat.targetX)
        && std::isfinite(seat.targetY);
}

const gbr::AiObservedTarget* visibleTarget(const gbr::AiSeatState& seat,
                                           const QString& targetId) {
    for (const gbr::AiObservedTarget& target : seat.visibleTargets) {
        if (target.targetId == targetId && target.visible) return &target;
    }
    return nullptr;
}

bool targetsRedCommandPost(const gbr::AiSeatState& seat) {
    return hasVisibleTarget(seat)
        && (seat.targetId == QLatin1String("red_cp")
            || seat.targetKind == QLatin1String("commandpost"));
}

int planningTargetRank(const gbr::AiSeatState& seat) {
    if (seat.kind == QLatin1String("attackuav") && targetsRedCommandPost(seat)) return 0;
    if (targetsRedCommandPost(seat)) return 1;
    if (seat.kind == QLatin1String("attackuav") && hasVisibleTarget(seat)) return 2;
    return 3;
}

QPair<double, double> advanceTowardLiveTarget(const gbr::AiSeatState& seat,
                                               double mapWidth, double mapHeight) {
    return {clampCoordinate(seat.x + (seat.targetX - seat.x) * 0.60, mapWidth),
            clampCoordinate(seat.y + (seat.targetY - seat.y) * 0.60, mapHeight)};
}

double preferredSpeed(const gbr::AiSeatState& seat, double destinationX,
                      double destinationY) {
    const double cruise = std::clamp(
        std::isfinite(seat.cruiseSpeed) && seat.cruiseSpeed > 0.0
            ? seat.cruiseSpeed
            : (std::isfinite(seat.commandedSpeed) && seat.commandedSpeed > 0.0
                   ? seat.commandedSpeed : seat.speed),
        0.0, gbr::SimulationEngine::kMaximumCommandedUnitSpeedMps);
    if (cruise <= 0.0) return 0.0;
    const double distance = std::sqrt(distance2(destinationX, destinationY, seat.x, seat.y));
    const double minimum = std::min(cruise, std::max(1.0, cruise * 0.10));
    return std::clamp(distance / kSpeedSettlingSeconds, minimum, cruise);
}

void appendSpeedCommand(QList<gbr::AiCommand>* commands, const gbr::AiSeatState& seat,
                        double destinationX, double destinationY, int priority) {
    if (!commands) return;
    const double desired = preferredSpeed(seat, destinationX, destinationY);
    const double current = std::isfinite(seat.commandedSpeed) && seat.commandedSpeed > 0.0
        ? seat.commandedSpeed : seat.speed;
    if (desired <= 0.0 || !std::isfinite(current)
        || std::abs(desired - current) < kSpeedChangeThreshold) {
        return;
    }
    commands->append(gbr::AiCommand{seat.seatId, QStringLiteral("setSpeed"),
                                    {{QStringLiteral("unitId"), seat.unitId},
                                     {QStringLiteral("speed"), desired}}, priority});
}

double targetValue(const gbr::AiObservedTarget& target) {
    if (target.commandPost || target.targetKind == QLatin1String("commandpost")) return 150.0;
    if (target.targetKind == QLatin1String("attackuav")) return 115.0;
    if (target.targetKind == QLatin1String("jammeruav")) return 102.0;
    if (target.targetKind == QLatin1String("reconuav")) return 95.0;
    if (target.targetKind == QLatin1String("groundscout")) return 82.0;
    return 70.0;
}

int seatRoleRank(const gbr::AiSeatState& seat) {
    if (seat.kind == QLatin1String("attackuav")) return 0;
    if (seat.kind == QLatin1String("groundscout")) return 1;
    if (seat.kind == QLatin1String("jammeruav")) return 2;
    if (seat.kind == QLatin1String("reconuav")) return 3;
    return 4;
}

QPair<double, double> clampPoint(double x, double y, double width, double height) {
    return {clampCoordinate(x, width), clampCoordinate(y, height)};
}

QPair<double, double> deterministicRegion(const gbr::AiSeatState& seat,
                                          const gbr::AiObservedTarget* contact,
                                          const gbr::AiKnowledgeState& knowledge,
                                          const gbr::AiDifficultyParameters& parameters,
                                          quint64 planningGeneration,
                                          int candidateIndex) {
    double x = knowledge.mapWidth * 0.5;
    double y = knowledge.mapHeight * 0.5;
    if (contact) {
        const double lookahead = std::max(0, parameters.lookaheadSeconds);
        x = contact->x + contact->velocityX * lookahead;
        y = contact->y + contact->velocityY * lookahead;
    } else {
        const quint64 hash = static_cast<quint64>(seat.seatId.size() * 131)
            + planningGeneration * 17ULL + static_cast<quint64>(seatRoleRank(seat) * 97);
        const double fraction = static_cast<double>(hash % 1000ULL) / 1000.0;
        x = knowledge.mapWidth * (0.18 + fraction * 0.64);
        y = knowledge.mapHeight * (0.18 + (1.0 - fraction) * 0.64);
    }
    const double radius = std::max(200.0, std::min(knowledge.mapWidth, knowledge.mapHeight)
                                   * (0.025 + 0.012 * std::max(0, candidateIndex)));
    const quint64 seed = static_cast<quint64>(seat.seatId.size() * 977)
        + planningGeneration * 7919ULL + static_cast<quint64>(candidateIndex * 101);
    const double angle = static_cast<double>(seed % 6283ULL) / 1000.0;
    if (contact) {
        // Offset scouting/jamming routes so several units do not stack on the
        // same point and expose the whole group to one threat sector.
        x += std::cos(angle) * radius;
        y += std::sin(angle) * radius;
    }
    return clampPoint(x, y, knowledge.mapWidth, knowledge.mapHeight);
}

bool needsWithdrawal(const gbr::AiSeatState& seat) {
    if (seat.kind != QLatin1String("attackuav")) return false;
    if (seat.ammoRemaining == 0 || seat.weaponHealth < 0.25 || seat.hpRatio < 0.30) return true;
    return seat.fuelCapacity > 0.0 && seat.fuelRemaining >= 0.0
        && seat.fuelRemaining / seat.fuelCapacity < 0.16;
}

bool targetIsFresh(const gbr::AiObservedTarget& target, double now, int memorySeconds) {
    return target.targetId.size() <= 64 && std::isfinite(target.x) && std::isfinite(target.y)
        && std::isfinite(target.lastSeenAt) && target.lastSeenAt <= now + 1e-6
        && now - target.lastSeenAt <= std::max(1, memorySeconds)
        && target.confidence > 0.05;
}

}

namespace gbr {

QJsonObject AiObservedTarget::toJson() const {
    return {{QStringLiteral("targetId"), targetId},
            {QStringLiteral("targetKind"), targetKind},
            {QStringLiteral("x"), x},
            {QStringLiteral("y"), y},
            {QStringLiteral("velocityX"), velocityX},
            {QStringLiteral("velocityY"), velocityY},
            {QStringLiteral("confidence"), confidence},
            {QStringLiteral("lastSeenAt"), lastSeenAt},
            {QStringLiteral("visible"), visible},
            {QStringLiteral("privileged"), privileged},
            {QStringLiteral("commandPost"), commandPost}};
}

bool AiObservedTarget::fromJson(const QJsonObject& object, AiObservedTarget* target,
                                QString* error) {
    if (error) error->clear();
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QString id = object.value(QStringLiteral("targetId")).toString();
    if (id.isEmpty() || id.size() > 64 || !id.startsWith(QLatin1String("red_"))) {
        return fail(QStringLiteral("AI 接触 ID 无效"));
    }
    const double x = object.value(QStringLiteral("x")).toDouble(
        std::numeric_limits<double>::quiet_NaN());
    const double y = object.value(QStringLiteral("y")).toDouble(
        std::numeric_limits<double>::quiet_NaN());
    const double vx = object.value(QStringLiteral("velocityX")).toDouble(0.0);
    const double vy = object.value(QStringLiteral("velocityY")).toDouble(0.0);
    const double confidence = object.value(QStringLiteral("confidence")).toDouble(0.0);
    const double lastSeenAt = object.value(QStringLiteral("lastSeenAt")).toDouble(
        std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(vx) || !std::isfinite(vy)
        || !std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0
        || !std::isfinite(lastSeenAt) || lastSeenAt < 0.0) {
        return fail(QStringLiteral("AI 接触数值无效"));
    }
    if (target) {
        target->targetId = id;
        target->targetKind = object.value(QStringLiteral("targetKind")).toString();
        target->x = x;
        target->y = y;
        target->velocityX = vx;
        target->velocityY = vy;
        target->confidence = confidence;
        target->lastSeenAt = lastSeenAt;
        target->visible = object.value(QStringLiteral("visible")).toBool(false);
        target->privileged = object.value(QStringLiteral("privileged")).toBool(false);
        target->commandPost = object.value(QStringLiteral("commandPost")).toBool(false);
    }
    return true;
}

AiDifficultyParameters RulesAi::parameters(const QString& difficulty) {
    if (difficulty == QLatin1String("easy")) {
        return {2000, 90000, 1, 1, 0.20, 1500, 35000, 8, 2, 20, 850};
    }
    if (difficulty == QLatin1String("hard")) {
        return {500, 30000, 4, 4, 0.0, 500, 10000, 45, 12, 90, 150};
    }
    return {1000, 45000, 2, 2, 0.08, 750, 18000, 25, 6, 45, 450};
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
        const int leftRank = planningTargetRank(left);
        const int rightRank = planningTargetRank(right);
        if (leftRank != rightRank) return leftRank < rightRank;
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

AiPlanV1 RulesAi::makeStrategicPlan(const AiKnowledgeState& knowledge,
                                    const QString& requestId,
                                    quint64 matchGeneration,
                                    quint64 sourceStateRevision,
                                    double validUntil,
                                    quint64* rngState,
                                    const AiDifficultyParameters& parameters,
                                    quint64 planningGeneration) {
    AiPlanV1 plan;
    plan.requestId = requestId;
    plan.matchGeneration = matchGeneration;
    plan.sourceStateRevision = sourceStateRevision;
    plan.planningGeneration = planningGeneration;

    const double width = std::isfinite(knowledge.mapWidth) && knowledge.mapWidth > 0.0
        ? knowledge.mapWidth : kDefaultMapWidth;
    const double height = std::isfinite(knowledge.mapHeight) && knowledge.mapHeight > 0.0
        ? knowledge.mapHeight : kDefaultMapHeight;
    const double now = std::isfinite(knowledge.now) && knowledge.now >= 0.0
        ? knowledge.now : 0.0;
    AiKnowledgeState normalizedKnowledge = knowledge;
    normalizedKnowledge.mapWidth = width;
    normalizedKnowledge.mapHeight = height;

    // Merge observations from the delayed commander memory and the current
    // per-unit sensor picture. A contact is never promoted to a fire mission
    // merely because it exists in memory; commandsForPlan still requires a
    // current visible target before it emits engageTarget.
    QHash<QString, AiObservedTarget> merged;
    for (const AiObservedTarget& contact : knowledge.contacts) {
        if (!targetIsFresh(contact, now, parameters.contactMemorySeconds)) continue;
        merged.insert(contact.targetId, contact);
    }
    for (const AiSeatState& seat : knowledge.seats) {
        for (const AiObservedTarget& observed : seat.visibleTargets) {
            if (observed.targetId.isEmpty() || !observed.visible) continue;
            AiObservedTarget current = observed;
            current.confidence = std::max(0.8, current.confidence);
            current.lastSeenAt = now;
            current.privileged = false;
            merged.insert(current.targetId, current);
        }
    }
    QList<AiObservedTarget> contacts = merged.values();
    std::sort(contacts.begin(), contacts.end(), [](const AiObservedTarget& left,
                                                   const AiObservedTarget& right) {
        const double leftScore = targetValue(left) * left.confidence;
        const double rightScore = targetValue(right) * right.confidence;
        if (std::abs(leftScore - rightScore) > 1e-9) return leftScore > rightScore;
        return left.targetId < right.targetId;
    });

    QList<AiSeatState> seats = knowledge.seats;
    std::sort(seats.begin(), seats.end(), [](const AiSeatState& left,
                                             const AiSeatState& right) {
        const int leftRank = seatRoleRank(left);
        const int rightRank = seatRoleRank(right);
        if (leftRank != rightRank) return leftRank < rightRank;
        return left.seatId < right.seatId;
    });
    seats.erase(std::remove_if(seats.begin(), seats.end(), [](const AiSeatState& seat) {
        return !seat.alive || !seat.movable || seat.seatId.isEmpty()
            || seat.unitId.isEmpty() || !seat.seatId.startsWith(QLatin1String("blue_"));
    }), seats.end());

    QHash<QString, int> allocations;
    const auto findContact = [&contacts, &allocations](const AiSeatState& seat,
                                                        bool visibleOnly) {
        int selected = -1;
        double selectedScore = -std::numeric_limits<double>::infinity();
        for (int index = 0; index < contacts.size(); ++index) {
            const AiObservedTarget& contact = contacts.at(index);
            if (visibleOnly && visibleTarget(seat, contact.targetId) == nullptr) continue;
            const double range = distance(seat.x, seat.y, contact.x, contact.y);
            double score = targetValue(contact) * std::clamp(contact.confidence, 0.05, 1.0)
                - range / 1000.0;
            score -= static_cast<double>(allocations.value(contact.targetId)) * 24.0;
            if (contact.privileged) score -= 12.0;
            if (seat.kind == QLatin1String("reconuav")
                && contact.targetKind == QLatin1String("reconuav")) score += 12.0;
            if (seat.kind == QLatin1String("jammeruav")
                && contact.targetKind == QLatin1String("jammeruav")) score += 18.0;
            if (score > selectedScore + 1e-9
                || (std::abs(score - selectedScore) <= 1e-9
                    && (selected < 0 || contact.targetId < contacts.at(selected).targetId))) {
                selected = index;
                selectedScore = score;
            }
        }
        return selected;
    };

    int priority = 100;
    for (const AiSeatState& seat : seats) {
        AiObjectiveV1 objective;
        objective.seatId = seat.seatId;
        objective.priority = std::max(0, priority--);
        objective.validUntil = std::max(now + 1.0, validUntil);

        if (!seat.communicationAvailable || needsWithdrawal(seat)) {
            objective.action = QStringLiteral("withdraw");
        } else if (seat.kind == QLatin1String("attackuav")) {
            const int visibleIndex = findContact(seat, true);
            if (visibleIndex >= 0 && seat.ammoRemaining != 0
                && seat.weaponHealth >= 0.25 && seat.cooldownRemaining <= 0.05) {
                const AiObservedTarget& target = contacts.at(visibleIndex);
                objective.action = QStringLiteral("attack");
                objective.targetId = target.targetId;
                ++allocations[target.targetId];
            } else {
                objective.action = QStringLiteral("search");
                const int memoryIndex = findContact(seat, false);
                const AiObservedTarget* contact = memoryIndex >= 0 ? &contacts.at(memoryIndex) : nullptr;
                const int candidate = std::max(0, parameters.candidateRoutes > 0
                                                   ? static_cast<int>(planningGeneration
                                                                      % parameters.candidateRoutes)
                                                   : 0);
                const auto point = deterministicRegion(seat, contact,
                                                        normalizedKnowledge, parameters,
                                                        planningGeneration, candidate);
                objective.region = QJsonObject{{QStringLiteral("x"), point.first},
                                               {QStringLiteral("y"), point.second}};
            }
        } else if (seat.kind == QLatin1String("reconuav")) {
            objective.action = QStringLiteral("search");
            const int memoryIndex = findContact(seat, false);
            const AiObservedTarget* contact = memoryIndex >= 0 ? &contacts.at(memoryIndex) : nullptr;
            const auto point = deterministicRegion(seat, contact, normalizedKnowledge, parameters,
                                                   planningGeneration,
                                                   std::max(0, parameters.candidateRoutes - 1));
            objective.region = QJsonObject{{QStringLiteral("x"), point.first},
                                           {QStringLiteral("y"), point.second}};
        } else if (seat.kind == QLatin1String("jammeruav")) {
            objective.action = QStringLiteral("jam");
            const int memoryIndex = findContact(seat, false);
            const AiObservedTarget* contact = memoryIndex >= 0 ? &contacts.at(memoryIndex) : nullptr;
            const auto point = deterministicRegion(seat, contact, normalizedKnowledge, parameters,
                                                   planningGeneration, 1);
            objective.region = QJsonObject{{QStringLiteral("x"), point.first},
                                           {QStringLiteral("y"), point.second}};
        } else if (seat.kind == QLatin1String("groundscout")) {
            objective.action = knowledge.commandPostThreat
                ? QStringLiteral("guard") : QStringLiteral("guard");
            const int memoryIndex = findContact(seat, false);
            const AiObservedTarget* contact = memoryIndex >= 0 ? &contacts.at(memoryIndex) : nullptr;
            const auto point = deterministicRegion(seat, contact, normalizedKnowledge, parameters,
                                                   planningGeneration, 0);
            objective.region = QJsonObject{{QStringLiteral("x"), point.first},
                                           {QStringLiteral("y"), point.second}};
        } else {
            objective.action = QStringLiteral("patrol");
            const auto point = deterministicRegion(seat, nullptr, normalizedKnowledge, parameters,
                                                   planningGeneration, 0);
            objective.region = QJsonObject{{QStringLiteral("x"), point.first},
                                           {QStringLiteral("y"), point.second}};
        }

        // Easy mode remains imperfect through route/target choice and a small
        // reaction delay, rather than repeatedly halting healthy units.
        if (rngState && objective.action != QLatin1String("attack")
            && parameters.suboptimalRate > 0.0
            && nextRandom(rngState) % 10000
                < static_cast<quint64>(std::clamp(parameters.suboptimalRate, 0.0, 1.0)
                                       * 10000.0)
            && !objective.region.isEmpty()) {
            const double x = objective.region.value(QStringLiteral("x")).toDouble();
            const double y = objective.region.value(QStringLiteral("y")).toDouble();
            const auto alternate = clampPoint(width - x, height - y, width, height);
            objective.region = QJsonObject{{QStringLiteral("x"), alternate.first},
                                           {QStringLiteral("y"), alternate.second}};
        }
        if (!objective.action.isEmpty()) plan.objectives.append(objective);
    }
    return plan;
}

QList<AiCommand> RulesAi::commandsForPlan(const AiPlanV1& plan,
                                          const QList<AiSeatState>& seats,
                                          double now,
                                          double mapWidth,
                                          double mapHeight) {
    QList<AiCommand> commands;
    if (!std::isfinite(mapWidth) || mapWidth <= 0.0) mapWidth = kDefaultMapWidth;
    if (!std::isfinite(mapHeight) || mapHeight <= 0.0) mapHeight = kDefaultMapHeight;
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
        if (needsWithdrawal(*it) && objective.action != QLatin1String("withdraw")) {
            command.action = QStringLiteral("withdraw");
            command.args = {{QStringLiteral("unitId"), it->unitId}};
            commands.append(command);
            continue;
        }
        const bool priorityAttack = it->kind == QLatin1String("attackuav")
            && targetsRedCommandPost(*it);
        if (priorityAttack || (objective.action == QLatin1String("attack")
                               && !objective.targetId.isEmpty())) {
            if (it->kind != QLatin1String("attackuav")) continue;
            const QString targetId = priorityAttack ? it->targetId : objective.targetId;
            if (targetId.isEmpty()) continue;
            const AiObservedTarget* liveTarget = visibleTarget(*it, targetId);
            const bool legacyVisible = hasVisibleTarget(*it) && it->targetId == targetId;
            if (!liveTarget && !legacyVisible) continue;
            if (liveTarget) {
                appendSpeedCommand(&commands, *it, liveTarget->x, liveTarget->y,
                                   objective.priority);
            } else if (hasVisibleTargetPosition(*it)) {
                appendSpeedCommand(&commands, *it, it->targetX, it->targetY,
                                   objective.priority);
            }
            command.action = QStringLiteral("engageTarget");
            command.args = {{QStringLiteral("attackerId"), it->unitId},
                            {QStringLiteral("targetId"), targetId}};
        } else if (objective.action == QLatin1String("search")
                   || objective.action == QLatin1String("patrol")
                   || objective.action == QLatin1String("guard")
                   || objective.action == QLatin1String("jam")) {
            QJsonObject region = objective.region;
            const AiObservedTarget* liveTarget = objective.targetId.isEmpty()
                ? nullptr : visibleTarget(*it, objective.targetId);
            if (liveTarget) {
                const double destinationX = clampCoordinate(
                    it->x + (liveTarget->x - it->x) * 0.60, mapWidth);
                const double destinationY = clampCoordinate(
                    it->y + (liveTarget->y - it->y) * 0.60, mapHeight);
                region = QJsonObject{{QStringLiteral("x"), destinationX},
                                     {QStringLiteral("y"), destinationY}};
            } else if (objective.targetId.isEmpty() && hasVisibleTargetPosition(*it)) {
                const QPair<double, double> destination = advanceTowardLiveTarget(
                    *it, mapWidth, mapHeight);
                region = QJsonObject{{QStringLiteral("x"), destination.first},
                                     {QStringLiteral("y"), destination.second}};
            }
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
            if (distance2(x, y, it->x, it->y)
                < kMinimumCommandDisplacement * kMinimumCommandDisplacement) {
                continue;
            }
            appendSpeedCommand(&commands, *it, x, y, objective.priority);
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
