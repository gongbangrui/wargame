#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

namespace gbr {

class SimulationEngine;

class StateProjector final {
public:
    static QString sideForRole(const QString& role);
    static bool canEditSide(const QString& role, const QString& side);
    static bool canControlSide(const QString& role, const QString& side);
    static QSet<QString> visibleUnitIds(const SimulationEngine& engine, const QString& role);
    static QJsonArray filteredMessages(const SimulationEngine& engine, const QString& role);
    static QJsonObject projectEvent(const SimulationEngine& engine, const QString& role,
                                    const QJsonObject& event);
    static QJsonObject snapshotFor(const SimulationEngine& engine, const QString& role,
                                   quint64 stateRevision, const QJsonObject& roomState);
};

} // namespace gbr
