#pragma once

#include "AiPlan.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace gbr {

struct OllamaSeatConstraint {
    QString seatId;
    QString unitId;
    QString kind;
    double x = 0.0;
    double y = 0.0;
    QStringList visibleTargetIds;
};

struct OllamaPlanRequest {
    QJsonObject projection;
    QString requestId;
    quint64 matchGeneration = 0;
    quint64 sourceStateRevision = 0;
    quint64 planningGeneration = 0;
    double validUntil = 0.0;
    double mapWidth = 20000.0;
    double mapHeight = 15000.0;
    QList<OllamaSeatConstraint> mobileSeats;
};

struct OllamaModelSelection {
    QString resolvedModel;
    QString failureClass;
};

class OllamaPlanningContract final {
public:
    static QJsonArray messagesFor(const OllamaPlanRequest& request);
    static QJsonObject schemaFor(const OllamaPlanRequest& request);
    static OllamaModelSelection selectModel(const QString& configuredModel,
                                             const QJsonArray& models);
    static QString validatePlan(const AiPlanV1& plan,
                                const OllamaPlanRequest& request);
    static QStringList actionsFor(const OllamaSeatConstraint& seat);
};

}
