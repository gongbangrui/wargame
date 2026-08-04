#pragma once

#include "core/Scenario.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace gbr {

struct RoomCheckpoint {
    Scenario scenario;
    Scenario runInitialScenario;
    QJsonArray runtimeUnits;
    QJsonArray commandHistory;
    QJsonArray mapMarks;
    QJsonObject authoritativeRoom;
    QString phase = QStringLiteral("preparing");
    bool redReady = false;
    bool blueReady = false;
    bool running = false;
    double simTime = 0.0;
    double speed = 1.0;
    quint64 scenarioRevision = 1;
    quint64 stateRevision = 1;
    quint64 eventSequence = 0;
};

class RoomPersistence final {
public:
    RoomPersistence(QString checkpointPath, QString eventLogPath,
                    QString dataDir = QString());

    bool saveCheckpoint(const RoomCheckpoint& checkpoint, QString* error = nullptr) const;
    bool loadCheckpoint(RoomCheckpoint* checkpoint, QString* error = nullptr) const;

    bool appendEvent(quint64 sequence, const QString& kind, const QJsonObject& payload,
                     QString* error = nullptr,
                     bool* checkpointRequired = nullptr) const;
    QJsonArray eventsAfter(quint64 sequence, QString* error = nullptr) const;

    QString checkpointPath() const { return m_checkpointPath; }
    QString eventLogPath() const { return m_eventLogPath; }
    QString configurationError() const { return m_configurationError; }

    static QString resolvePathWithinRoot(const QString& path, const QString& dataDir,
                                         QString* error = nullptr);

private:
    bool loadCheckpointFile(const QString& path, RoomCheckpoint* checkpoint,
                            QString* error) const;

    QString m_checkpointPath;
    QString m_eventLogPath;
    QString m_configurationError;
};

} // namespace gbr
