#pragma once

#include "AuthPolicy.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace gbr {

class SimulationEngine;

class VisibleStateProjector {
public:
    static QJsonObject project(const SimulationEngine& engine,
                               const SessionIdentity& identity,
                               qint64 scenarioRevision,
                               qint64 serverTick,
                               qint64 lastSequence);
    static QSet<QString> visibleEnemyIds(const SimulationEngine& engine,
                                         const QString& friendlySide);
    static QJsonObject sanitizeDetectedEnemy(const QJsonObject& unit);

private:
    static QSet<QString> visibleEnemyIds(const QJsonArray& units, const QString& friendlySide);
    static QJsonObject sanitizeFriendlyUnit(const QJsonObject& unit);
    static QJsonArray projectMessages(const SimulationEngine& engine,
                                      const SessionIdentity& identity,
                                      const QSet<QString>& visibleIds);
};

} // namespace gbr
