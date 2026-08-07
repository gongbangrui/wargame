#include "RulesAi.h"

#include "core/SimulationEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <QHash>
#include <QJsonValue>
#include <QSet>

namespace {

constexpr double kDefaultMapWidth = 20000.0;
constexpr double kDefaultMapHeight = 15000.0;
constexpr double kMinimumCommandDisplacement = 50.0;
constexpr double kSpeedChangeThreshold = 0.5;
constexpr double kSpeedSettlingSeconds = 8.0;
constexpr double kImmediateThreatSeconds = 8.0;
constexpr double kFuelReturnSafetyFactor = 1.25;
constexpr double kFuelServiceRatio = 0.40;

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

QPair<double, double> optimalFiringPosition(const gbr::AiSeatState& seat,
                                            double targetX, double targetY,
                                            double mapWidth, double mapHeight) {
    const double dx = seat.x - targetX;
    const double dy = seat.y - targetY;
    const double currentDistance = std::hypot(dx, dy);
    if (!std::isfinite(currentDistance) || currentDistance <= 1e-9) {
        return clampPoint(seat.x, seat.y, mapWidth, mapHeight);
    }

    double desiredDistance = seat.optimalAttackRange;
    if (!std::isfinite(desiredDistance) || desiredDistance <= 0.0) {
        desiredDistance = seat.maximumAttackRange;
    }
    if (!std::isfinite(desiredDistance) || desiredDistance <= 0.0) {
        return clampPoint(seat.x, seat.y, mapWidth, mapHeight);
    }
    if (std::isfinite(seat.minimumAttackRange) && seat.minimumAttackRange >= 0.0) {
        desiredDistance = std::max(desiredDistance, seat.minimumAttackRange);
    }
    if (std::isfinite(seat.maximumAttackRange) && seat.maximumAttackRange > 0.0) {
        desiredDistance = std::min(desiredDistance, seat.maximumAttackRange);
    }

    // Keep the attacker on the line through the target, at the configured
    // firing distance. This works both when closing in and when backing out
    // of the minimum range envelope.
    const double scale = desiredDistance / currentDistance;
    return clampPoint(targetX + dx * scale, targetY + dy * scale,
                      mapWidth, mapHeight);
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

double nominalCruiseFuelBurn(const gbr::AiSeatState& seat) {
    if (seat.kind == QLatin1String("attackuav")) return 4.0;
    if (seat.kind == QLatin1String("reconuav")) return 3.5;
    if (seat.kind == QLatin1String("jammeruav")) return 4.5;
    if (seat.kind == QLatin1String("groundscout")) return 3.0;
    return 0.0;
}

bool limitedCountermeasureDepleted(const gbr::AiSeatState& seat) {
    return seat.countermeasureSupported && seat.countermeasureCapacity > 0
        && seat.countermeasureRemaining <= 0;
}

bool needsService(const gbr::AiSeatState& seat) {
    const bool lowFuel = seat.fuelCapacity > 0.0 && seat.fuelRemaining >= 0.0
        && seat.fuelRemaining / seat.fuelCapacity <= kFuelServiceRatio;
    return seat.ammoRemaining == 0 || limitedCountermeasureDepleted(seat)
        || seat.hpRatio < 0.40 || seat.lowestSubsystemHealth < 0.35 || lowFuel;
}

double fuelRequiredToCommandPost(const gbr::AiSeatState& seat) {
    if (!seat.commandPostAlive || seat.fuelCapacity <= 0.0 || seat.fuelRemaining < 0.0) {
        return 0.0;
    }
    const double cruise = std::max(1.0, std::isfinite(seat.cruiseSpeed)
        && seat.cruiseSpeed > 0.0 ? seat.cruiseSpeed : seat.speed);
    const double burnRate = std::max(
        std::max(0.0, seat.fuelBurnRate), nominalCruiseFuelBurn(seat));
    const double travelSeconds = distance(seat.x, seat.y,
                                          seat.commandPostX, seat.commandPostY) / cruise;
    return travelSeconds * burnRate * kFuelReturnSafetyFactor;
}

bool fuelRequiresReturn(const gbr::AiSeatState& seat) {
    return seat.fuelCapacity > 0.0 && seat.fuelRemaining >= 0.0
        && seat.fuelRemaining <= fuelRequiredToCommandPost(seat) + 1e-9;
}

struct ThreatEstimate {
    const gbr::AiObservedProjectile* projectile = nullptr;
    double eta = std::numeric_limits<double>::infinity();
    double distance = std::numeric_limits<double>::infinity();
};

ThreatEstimate nearestIncomingThreat(const gbr::AiSeatState& seat) {
    ThreatEstimate nearest;
    for (const gbr::AiObservedProjectile& projectile : seat.visibleProjectiles) {
        if (!projectile.active || projectile.side == QLatin1String("blue")
            || !std::isfinite(projectile.x) || !std::isfinite(projectile.y)
            || !std::isfinite(projectile.headingRad) || !std::isfinite(projectile.speed)
            || projectile.speed <= 1e-9) {
            continue;
        }
        const bool explicitlyIncoming = projectile.targetId == seat.unitId;
        if (!projectile.targetId.isEmpty() && !explicitlyIncoming) continue;

        const double dx = seat.x - projectile.x;
        const double dy = seat.y - projectile.y;
        const double range = std::hypot(dx, dy);
        double eta = range / projectile.speed;
        if (!explicitlyIncoming) {
            const double velocityX = std::cos(projectile.headingRad) * projectile.speed;
            const double velocityY = std::sin(projectile.headingRad) * projectile.speed;
            const double speedSquared = projectile.speed * projectile.speed;
            eta = (dx * velocityX + dy * velocityY) / speedSquared;
            if (eta < 0.0) continue;
            const double closestX = projectile.x + velocityX * eta;
            const double closestY = projectile.y + velocityY * eta;
            if (distance(closestX, closestY, seat.x, seat.y)
                > gbr::SimulationEngine::kProjectileCollisionRadiusMeters) {
                continue;
            }
        }
        const double remainingLife = std::max(0.0, projectile.lifetime - projectile.age);
        if (!std::isfinite(eta) || eta < 0.0 || eta > remainingLife + 1e-9) continue;
        if (eta < nearest.eta - 1e-9
            || (std::abs(eta - nearest.eta) <= 1e-9
                && (!nearest.projectile
                    || projectile.projectileId < nearest.projectile->projectileId))) {
            nearest = ThreatEstimate{&projectile, eta, range};
        }
    }
    return nearest;
}

QPair<double, double> evasionDestination(const gbr::AiSeatState& seat,
                                         const gbr::AiObservedProjectile& projectile,
                                         double mapWidth, double mapHeight) {
    const double evadeDistance = std::clamp(
        std::max(1.0, seat.cruiseSpeed) * kImmediateThreatSeconds, 600.0, 1800.0);
    const double perpendicularX = -std::sin(projectile.headingRad);
    const double perpendicularY = std::cos(projectile.headingRad);
    const QPair<double, double> left = clampPoint(
        seat.x + perpendicularX * evadeDistance,
        seat.y + perpendicularY * evadeDistance, mapWidth, mapHeight);
    const QPair<double, double> right = clampPoint(
        seat.x - perpendicularX * evadeDistance,
        seat.y - perpendicularY * evadeDistance, mapWidth, mapHeight);
    const double leftDisplacement = distance2(seat.x, seat.y, left.first, left.second);
    const double rightDisplacement = distance2(seat.x, seat.y, right.first, right.second);
    if (std::abs(leftDisplacement - rightDisplacement) > 1e-9) {
        return leftDisplacement > rightDisplacement ? left : right;
    }
    return (stableHash(seat.unitId + projectile.projectileId) & 1ULL) == 0 ? left : right;
}

bool hasFreshIntel(const gbr::AiKnowledgeState& knowledge, double now) {
    for (const gbr::AiObservedTarget& contact : knowledge.contacts) {
        if (contact.confidence <= 0.05 || !std::isfinite(contact.lastSeenAt)
            || contact.lastSeenAt > now + 1e-9) {
            continue;
        }
        if (now - contact.lastSeenAt <= kImmediateThreatSeconds) return true;
    }
    for (const gbr::AiSeatState& seat : knowledge.seats) {
        if (!seat.visibleTargets.isEmpty()) return true;
    }
    return false;
}

double friendlyInFlightExpectedDamage(const QList<gbr::AiSeatState>& seats,
                                      const QString& targetId) {
    if (targetId.isEmpty()) return 0.0;
    QSet<QString> counted;
    double expectedDamage = 0.0;
    for (const gbr::AiSeatState& seat : seats) {
        for (const gbr::AiObservedProjectile& projectile : seat.visibleProjectiles) {
            if (projectile.active && projectile.side == QLatin1String("blue")
                && projectile.targetId == targetId
                && !counted.contains(projectile.projectileId)) {
                counted.insert(projectile.projectileId);
                expectedDamage += projectile.expectedDamage > 0.0
                    ? projectile.expectedDamage : 68.4;
            }
        }
    }
    return expectedDamage;
}

QList<gbr::AiCommand> tacticalCommands(const gbr::AiKnowledgeState& knowledge,
                                       const QList<gbr::AiSeatState>& sourceSeats,
                                       double now, double mapWidth, double mapHeight,
                                       QSet<QString>* overriddenSeats) {
    QList<gbr::AiCommand> commands;
    QList<gbr::AiSeatState> seats = sourceSeats;
    std::sort(seats.begin(), seats.end(), [](const gbr::AiSeatState& left,
                                             const gbr::AiSeatState& right) {
        return left.seatId < right.seatId;
    });
    const bool staleIntel = !hasFreshIntel(knowledge, now);
    for (const gbr::AiSeatState& seat : seats) {
        if (!seat.alive || seat.unitId.isEmpty()
            || !seat.seatId.startsWith(QLatin1String("blue_"))) {
            continue;
        }
        const ThreatEstimate threat = nearestIncomingThreat(seat);
        const bool immediateThreat = threat.projectile
            && threat.eta <= kImmediateThreatSeconds + 1e-9;
        if (immediateThreat) {
            if (overriddenSeats) overriddenSeats->insert(seat.seatId);
            if (seat.countermeasureAvailable
                && threat.distance <= seat.countermeasureRange + 1e-9) {
                commands.append(gbr::AiCommand{
                    seat.seatId, QStringLiteral("activateCountermeasure"),
                    {{QStringLiteral("unitId"), seat.unitId}}, 1200});
            }
            if (seat.movable && seat.communicationAvailable && seat.fuelRemaining > 1e-9) {
                if (seat.serviceRequested) {
                    commands.append(gbr::AiCommand{
                        seat.seatId, QStringLiteral("cancelService"),
                        {{QStringLiteral("unitId"), seat.unitId}}, 1195});
                }
                const auto destination = evasionDestination(
                    seat, *threat.projectile, mapWidth, mapHeight);
                const double cruise = std::clamp(seat.cruiseSpeed, 0.0,
                    gbr::SimulationEngine::kMaximumCommandedUnitSpeedMps);
                if (cruise > 0.0
                    && std::abs(cruise - seat.commandedSpeed) >= kSpeedChangeThreshold) {
                    commands.append(gbr::AiCommand{
                        seat.seatId, QStringLiteral("setSpeed"),
                        {{QStringLiteral("unitId"), seat.unitId},
                         {QStringLiteral("speed"), cruise}}, 1190});
                }
                commands.append(gbr::AiCommand{
                    seat.seatId, QStringLiteral("moveTo"),
                    {{QStringLiteral("unitId"), seat.unitId},
                     {QStringLiteral("pos"),
                      QVariantMap{{QStringLiteral("x"), destination.first},
                                  {QStringLiteral("y"), destination.second}}}}, 1185});
            }
            continue;
        }

        if (seat.serviceRequested) {
            if (overriddenSeats) overriddenSeats->insert(seat.seatId);
            continue;
        }

        const bool serviceNeeded = needsService(seat);
        const bool returnNeeded = serviceNeeded || fuelRequiresReturn(seat);
        if (seat.serviceEligible && serviceNeeded) {
            commands.append(gbr::AiCommand{
                seat.seatId, QStringLiteral("service"),
                {{QStringLiteral("unitId"), seat.unitId}}, 1100});
            if (overriddenSeats) overriddenSeats->insert(seat.seatId);
            continue;
        }
        if (returnNeeded) {
            if (overriddenSeats) overriddenSeats->insert(seat.seatId);
            if (seat.commandPostAlive && seat.movable
                && seat.communicationAvailable && seat.fuelRemaining > 1e-9) {
                commands.append(gbr::AiCommand{
                    seat.seatId, QStringLiteral("withdraw"),
                    {{QStringLiteral("unitId"), seat.unitId},
                     {QStringLiteral("pos"),
                      QVariantMap{{QStringLiteral("x"), seat.commandPostX},
                                  {QStringLiteral("y"), seat.commandPostY}}}}, 1050});
            }
            continue;
        }

        if (seat.repairAvailable && seat.lowestSubsystemHealth < 0.70) {
            commands.append(gbr::AiCommand{
                seat.seatId, QStringLiteral("attemptFieldRepair"),
                {{QStringLiteral("unitId"), seat.unitId}}, 800});
        }
        if (seat.scanAvailable && (staleIntel || knowledge.commandPostThreat)) {
            commands.append(gbr::AiCommand{
                seat.seatId, QStringLiteral("activateScan"),
                {{QStringLiteral("unitId"), seat.unitId}}, 790});
        }
    }
    return commands;
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
            {QStringLiteral("hp"), hp},
            {QStringLiteral("maxHp"), maxHp},
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
    const double hp = object.value(QStringLiteral("hp")).toDouble(100.0);
    const double maxHp = object.value(QStringLiteral("maxHp")).toDouble(100.0);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(vx) || !std::isfinite(vy)
        || !std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0
        || !std::isfinite(lastSeenAt) || lastSeenAt < 0.0
        || !std::isfinite(hp) || !std::isfinite(maxHp)
        || hp < 0.0 || maxHp <= 0.0 || hp > maxHp) {
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
        target->hp = hp;
        target->maxHp = maxHp;
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
                                          double mapHeight,
                                          const AiKnowledgeState* knowledge) {
    QList<AiCommand> commands;
    if (!std::isfinite(mapWidth) || mapWidth <= 0.0) mapWidth = kDefaultMapWidth;
    if (!std::isfinite(mapHeight) || mapHeight <= 0.0) mapHeight = kDefaultMapHeight;
    QSet<QString> tacticallyOverridden;
    if (knowledge) {
        commands = tacticalCommands(*knowledge, seats, now, mapWidth, mapHeight,
                                    &tacticallyOverridden);
    }
    QList<AiObjectiveV1> objectives = plan.objectives;
    std::stable_sort(objectives.begin(), objectives.end(), [](const AiObjectiveV1& left,
                                                               const AiObjectiveV1& right) {
        if (left.priority != right.priority) return left.priority > right.priority;
        return left.seatId < right.seatId;
    });
    for (const AiObjectiveV1& objective : objectives) {
        if (objective.validUntil < now) continue;
        if (!objective.seatId.startsWith(QLatin1String("blue_"))) continue;
        if (tacticallyOverridden.contains(objective.seatId)) continue;
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
            const double remainingHp = liveTarget ? liveTarget->hp : 100.0;
            if (friendlyInFlightExpectedDamage(seats, targetId)
                >= std::max(0.0, remainingHp) - 1e-9) {
                continue;
            }
            const double targetX = liveTarget ? liveTarget->x : it->targetX;
            const double targetY = liveTarget ? liveTarget->y : it->targetY;
            const bool validTargetPosition = std::isfinite(targetX)
                && std::isfinite(targetY);
            const bool hasFiringEnvelope = validTargetPosition
                && std::isfinite(it->optimalAttackRange)
                && it->optimalAttackRange > 0.0;
            if (hasFiringEnvelope) {
                const double targetDistance = distance(it->x, it->y, targetX, targetY);
                const double minimumRange = std::isfinite(it->minimumAttackRange)
                    ? std::max(0.0, it->minimumAttackRange) : 0.0;
                const double optimalRange = it->optimalAttackRange;
                if (std::abs(targetDistance - optimalRange) > kMinimumCommandDisplacement
                    || targetDistance < minimumRange - kMinimumCommandDisplacement) {
                    const auto firingPosition = optimalFiringPosition(
                        *it, targetX, targetY, mapWidth, mapHeight);
                    if (distance2(firingPosition.first, firingPosition.second,
                                  it->x, it->y)
                        >= kMinimumCommandDisplacement * kMinimumCommandDisplacement) {
                        command.action = QStringLiteral("moveTo");
                        command.args = {{QStringLiteral("unitId"), it->unitId},
                                        {QStringLiteral("attackTargetId"), targetId},
                                        {QStringLiteral("pos"),
                                         QVariantMap{{QStringLiteral("x"), firingPosition.first},
                                                     {QStringLiteral("y"), firingPosition.second}}}};
                        commands.append(command);
                        continue;
                    }
                }
            } else if (liveTarget) {
                appendSpeedCommand(&commands, *it, targetX, targetY,
                                   objective.priority);
            } else if (hasVisibleTargetPosition(*it)) {
                appendSpeedCommand(&commands, *it, targetX, targetY,
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
