#pragma once

#include "AuthPolicy.h"
#include "core/CommandResult.h"
#include "core/SimulationEngine.h"

#include <QObject>
#include <QTimer>

namespace gbr {

class PersistenceStore;
struct ServerConfig;

class SimulationRoom : public QObject {
    Q_OBJECT
public:
    SimulationRoom(const ServerConfig& config, PersistenceStore* persistence,
                   QObject* parent = nullptr);

    bool initialize(QString* error = nullptr);
    CommandResult execute(const SessionIdentity& identity, const QString& action,
                          const QVariantMap& args, qint64 clientRevision);
    bool checkpointNow(QString* error = nullptr);
    QJsonObject projectedState(const SessionIdentity& identity, qint64 lastSequence) const;

    SimulationEngine* engine() { return &m_engine; }
    const SimulationEngine* engine() const { return &m_engine; }
    QString roomId() const;
    qint64 scenarioRevision() const { return m_scenarioRevision; }
    qint64 serverTick() const;

signals:
    void checkpointFailed(const QString& message);

private:
    bool canControlUnit(const SessionIdentity& identity, const QString& unitId) const;
    bool isPreparationPhase() const;
    bool bothSidesReady() const { return m_redReady && m_blueReady; }
    void resetReadiness();
    CommandResult authorizeDomainCommand(const SessionIdentity& identity,
                                         const QString& action,
                                         const QVariantMap& args) const;
    CommandResult specialCommand(const SessionIdentity& identity,
                                 const QString& action,
                                 const QVariantMap& args);
    CommandResult rejected(const char* code, const QString& message) const;
    CommandResult accepted(const QString& message) const;

    const ServerConfig& m_config;
    PersistenceStore* m_persistence = nullptr;
    SimulationEngine m_engine;
    QTimer m_checkpointTimer;
    qint64 m_scenarioRevision = 1;
    bool m_redReady = false;
    bool m_blueReady = false;
    QJsonArray m_chatMessages;
    quint64 m_nextChatMessageId = 1;
};

} // namespace gbr
