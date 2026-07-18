#pragma once

#include <QObject>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantList>

namespace gbr {

class ClientStateStore : public QObject {
    Q_OBJECT
public:
    explicit ClientStateStore(QObject* parent = nullptr);

    bool applySnapshot(const QJsonObject& snapshot, QString* error = nullptr);
    bool applyDelta(const QJsonObject& delta, QString* error = nullptr);
    bool advanceSequence(qint64 sequence, QString* error = nullptr);
    void resetSequence();
    void clear();

    double simTime() const { return m_simTime; }
    bool running() const { return m_running; }
    bool readyForSim() const { return m_readyForSim; }
    QString cpIssues() const { return m_cpIssues; }
    QString lastError() const { return m_lastError; }
    QJsonObject mapInfo() const { return m_map; }
    QJsonArray allUnits() const { return m_units; }
    QVariantList unitsForView() const { return m_units.toVariantList(); }
    QVariantList messages() const { return m_messages.toVariantList(); }
    QVariantMap lobby() const { return m_lobby.toVariantMap(); }
    QVariantList chatMessages() const { return m_chatMessages.toVariantList(); }
    QJsonObject unitAt(const QString& id) const { return m_unitsById.value(id); }
    QJsonObject scenarioJson() const { return m_scenario; }
    qint64 scenarioRevision() const { return m_scenarioRevision; }
    qint64 serverTick() const { return m_serverTick; }
    qint64 lastSequence() const { return m_lastSequence; }
    QString role() const { return m_role; }
    QString side() const { return m_side; }
    void setIdentity(const QString& role, const QString& side);
    void setError(const QString& error);

signals:
    void simTimeChanged();
    void runningChanged();
    void unitsChanged();
    void messagesChanged();
    void mapChanged();
    void readyForSimChanged();
    void lobbyChanged();
    void chatMessagesChanged();
    void errorChanged(const QString& message);
    void identityChanged();

private:
    void rebuildIndex();

    double m_simTime = 0.0;
    bool m_running = false;
    bool m_readyForSim = false;
    QString m_cpIssues;
    QString m_lastError;
    QJsonObject m_map;
    QJsonArray m_units;
    QJsonArray m_messages;
    QJsonObject m_lobby;
    QJsonArray m_chatMessages;
    QJsonObject m_scenario;
    QHash<QString, QJsonObject> m_unitsById;
    qint64 m_scenarioRevision = 0;
    qint64 m_serverTick = 0;
    qint64 m_lastSequence = 0;
    QString m_role;
    QString m_side;
};

} // namespace gbr
