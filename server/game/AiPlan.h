#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

namespace gbr {

struct AiObjectiveV1 {
    QString action;
    int priority = 0;
    QString seatId;
    QString targetId;
    QJsonObject region;
    double validUntil = 0.0;
};

struct AiPlanV1 {
    int schemaVersion = 1;
    QString requestId;
    quint64 matchGeneration = 0;
    quint64 sourceStateRevision = 0;
    quint64 planningGeneration = 0;
    QList<AiObjectiveV1> objectives;

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& object, AiPlanV1* plan,
                         QString* error = nullptr);
};

bool isAiObjectiveAction(const QString& action);

}
