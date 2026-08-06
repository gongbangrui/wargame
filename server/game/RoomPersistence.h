#pragma once

#include "AiPlan.h"
#include "core/Scenario.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace gbr {

struct AiCheckpointState {
    quint64 matchGeneration = 1;
    quint64 commandSequence = 0;
    quint64 planningGeneration = 0;
    quint64 rngState = 1;
    QString aiDifficulty = QStringLiteral("normal");
    QString providerMode = QStringLiteral("auto");
    QString providerModel = QStringLiteral("auto");
    double nextDecisionAt = 0.0;
    double nextReplanAt = 0.0;
    std::optional<AiPlanV1> currentPlan;
    int consecutiveFailures = 0;
    bool stickyRules = false;
    QString effectiveEngine = QStringLiteral("rules");
    QString lastFailureClass;
    quint64 providerRequests = 0;
    quint64 providerSuccesses = 0;
    quint64 providerFailures = 0;
    qint64 lastLatencyMs = 0;
    qint64 averageLatencyMs = 0;
};

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
    std::optional<AiCheckpointState> aiState;
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
