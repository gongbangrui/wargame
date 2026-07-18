#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantList>
#include <QVariantMap>

namespace gbr {

class SimulationEngine;

class ISessionAdapter : public QObject {
    Q_OBJECT
public:
    explicit ISessionAdapter(QObject* parent = nullptr) : QObject(parent) {}
    ~ISessionAdapter() override = default;

    virtual bool isRemote() const = 0;
    virtual bool connected() const = 0;
    virtual QString connectionState() const {
        return isRemote() ? QStringLiteral("disconnected") : QStringLiteral("local");
    }
    virtual QString role() const { return {}; }
    virtual QString side() const { return {}; }
    virtual SimulationEngine* localEngine() const { return nullptr; }
    virtual double simTime() const = 0;
    virtual bool running() const = 0;
    virtual bool readyForSim() const = 0;
    virtual QString cpIssues() const = 0;
    virtual QString lastError() const = 0;
    virtual QVariantList units() const = 0;
    virtual QVariantList messages() const = 0;
    virtual QVariantMap lobby() const { return {}; }
    virtual QVariantList chatMessages() const { return {}; }
    virtual QJsonObject mapInfo() const = 0;
    virtual QJsonArray allUnits() const = 0;
    virtual QJsonObject unitAt(const QString& id) const = 0;
    virtual QJsonObject scenarioJson() const = 0;
    virtual QVariantMap command(const QString& action, const QVariantMap& args) = 0;
    virtual void setRunning(bool running) = 0;
    virtual void setSpeed(double speed) = 0;
    virtual void stepOnce() = 0;

signals:
    void simTimeChanged();
    void runningChanged();
    void unitsChanged();
    void messagesChanged();
    void mapChanged();
    void readyForSimChanged();
    void lobbyChanged();
    void chatMessagesChanged();
    void errorOccurred(const QString& message);
    void eventPosted(const QString& title, const QString& body,
                     const QString& level, const QString& sourceUnitId);
    void targetDestroyedVisual(const QString& unitId, double x, double y);
    void unitDestroyed(const QString& unitId);
    void simulationEnded(const QString& winner, const QString& loser);
    void connectionStatusChanged(const QString& message);
    void connectionStateChanged();
    void connectedChanged();
    void commandResultReceived(const QString& commandId, const QVariantMap& result);
};

} // namespace gbr
