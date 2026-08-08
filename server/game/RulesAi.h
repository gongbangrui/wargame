#pragma once

#include "AiPlan.h"

#include <QList>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>

namespace gbr {

struct AiDifficultyParameters {
    int unitDecisionIntervalMs = 1000;
    int commanderReplanIntervalMs = 45000;
    int maxTargets = 2;
    int coordinatedUnitsPerTarget = 2;
    double suboptimalRate = 0.08;
    int enhancedDecisionIntervalMs = 750;
    int enhancedReplanIntervalMs = 18000;
    int contactMemorySeconds = 25;
    int candidateRoutes = 6;
    int lookaheadSeconds = 45;
    int reactionDelayMs = 450;
};

struct AiObservedTarget {
    QString targetId;
    QString targetKind;
    double x = 0.0;
    double y = 0.0;
    double velocityX = 0.0;
    double velocityY = 0.0;
    double confidence = 0.0;
    double lastSeenAt = 0.0;
    bool visible = false;
    bool privileged = false;
    bool commandPost = false;
    double hp = 100.0;
    double maxHp = 100.0;

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& object, AiObservedTarget* target,
                         QString* error = nullptr);
};

struct AiObservedProjectile {
    QString projectileId;
    QString side;
    QString targetId;
    double x = 0.0;
    double y = 0.0;
    double headingRad = 0.0;
    double speed = 0.0;
    double age = 0.0;
    double lifetime = 0.0;
    bool active = true;
    double expectedDamage = 0.0;
};

struct AiSeatState {
    QString seatId;
    QString unitId;
    QString kind;
    QString targetId;
    QString targetKind;
    double x = 0.0;
    double y = 0.0;
    double targetX = 0.0;
    double targetY = 0.0;
    // Runtime speed is after damage effects. Commanded and cruise speeds stay
    // local to the rules executor and are never part of AiPlanV1's wire shape.
    double speed = 0.0;
    double commandedSpeed = 0.0;
    double cruiseSpeed = 0.0;
    double maxCommandedSpeed = 0.0;
    bool targetVisible = false;
    bool alive = true;
    bool movable = true;
    double hpRatio = 1.0;
    double sensorHealth = 1.0;
    double commsHealth = 1.0;
    double mobilityHealth = 1.0;
    double weaponHealth = 1.0;
    double detectRange = 0.0;
    double commRange = 0.0;
    int ammoRemaining = -1;
    int ammoCapacity = -1;
    double cooldownRemaining = 0.0;
    double minimumAttackRange = -1.0;
    double optimalAttackRange = -1.0;
    double maximumAttackRange = -1.0;
    double fuelRemaining = -1.0;
    double fuelCapacity = -1.0;
    double fuelBurnRate = 0.0;
    double estimatedFuelEndurance = -1.0;
    double lowestSubsystemHealth = 1.0;
    bool serviceEligible = false;
    bool serviceRequested = false;
    double serviceProgress = 0.0;
    bool commandPostAlive = false;
    double commandPostX = 0.0;
    double commandPostY = 0.0;
    bool countermeasureSupported = false;
    bool countermeasureAvailable = false;
    double countermeasureRange = 0.0;
    double countermeasureCooldownRemaining = 0.0;
    int countermeasureRemaining = 0;
    int countermeasureCapacity = 0;
    bool scanSupported = false;
    bool scanAvailable = false;
    double scanCooldownRemaining = 0.0;
    bool repairAvailable = false;
    double repairCooldownRemaining = 0.0;
    bool communicationAvailable = true;
    QList<AiObservedTarget> visibleTargets;
    QList<AiObservedProjectile> visibleProjectiles;
};

struct AiKnowledgeState {
    QList<AiSeatState> seats;
    QList<AiObservedTarget> contacts;
    double now = 0.0;
    double mapWidth = 20000.0;
    double mapHeight = 15000.0;
    QString phase = QStringLiteral("recon");
    bool commandPostThreat = false;
    bool commandPostAlive = true;
};

struct AiCommand {
    QString seatId;
    QString action;
    QVariantMap args;
    int priority = 0;
};

class RulesAi final {
public:
    static AiDifficultyParameters parameters(const QString& difficulty);
    static quint64 nextRandom(quint64* state);
    static AiPlanV1 makeCommanderPlan(const QList<AiSeatState>& seats,
                                       const QString& requestId,
                                       quint64 matchGeneration,
                                       quint64 sourceStateRevision,
                                       double validUntil,
                                       quint64* rngState,
                                       double suboptimalRate = 0.08,
                                       double mapWidth = 20000.0,
                                       double mapHeight = 15000.0,
                                       quint64 planningGeneration = 1);
    static AiPlanV1 makeStrategicPlan(const AiKnowledgeState& knowledge,
                                      const QString& requestId,
                                      quint64 matchGeneration,
                                      quint64 sourceStateRevision,
                                      double validUntil,
                                      quint64* rngState,
                                      const AiDifficultyParameters& parameters,
                                      quint64 planningGeneration = 1);
    static QList<AiCommand> commandsForPlan(const AiPlanV1& plan,
                                             const QList<AiSeatState>& seats,
                                             double now,
                                             double mapWidth = 20000.0,
                                             double mapHeight = 15000.0,
                                             const AiKnowledgeState* knowledge = nullptr);
};

}
