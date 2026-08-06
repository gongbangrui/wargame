#pragma once

#include "AiPlan.h"

#include <QList>
#include <QString>
#include <QVariantMap>

namespace gbr {

struct AiDifficultyParameters {
    int unitDecisionIntervalMs = 2000;
    int commanderReplanIntervalMs = 30000;
    int maxTargets = 2;
    int coordinatedUnitsPerTarget = 2;
    double suboptimalRate = 0.08;
};

struct AiSeatState {
    QString seatId;
    QString unitId;
    QString kind;
    QString targetId;
    double x = 0.0;
    double y = 0.0;
    double targetX = 0.0;
    double targetY = 0.0;
    bool targetVisible = false;
    bool alive = true;
    bool movable = true;
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
    static QList<AiCommand> commandsForPlan(const AiPlanV1& plan,
                                             const QList<AiSeatState>& seats,
                                             double now);
};

}
