#pragma once

#include "ISessionAdapter.h"

namespace gbr {

class SimulationEngine;

class LocalSessionAdapter : public ISessionAdapter {
    Q_OBJECT
public:
    explicit LocalSessionAdapter(SimulationEngine* engine, QObject* parent = nullptr);

    bool isRemote() const override { return false; }
    bool connected() const override { return true; }
    QString connectionState() const override { return QStringLiteral("local"); }
    SimulationEngine* localEngine() const override { return m_engine; }
    double simTime() const override;
    bool running() const override;
    bool readyForSim() const override;
    QString cpIssues() const override;
    QString lastError() const override;
    QVariantList units() const override;
    QVariantList messages() const override;
    QVariantMap lobby() const override;
    QVariantList chatMessages() const override { return {}; }
    QJsonObject mapInfo() const override;
    QJsonArray allUnits() const override;
    QJsonObject unitAt(const QString& id) const override;
    QJsonObject scenarioJson() const override;
    QVariantMap command(const QString& action, const QVariantMap& args) override;
    void setRunning(bool running) override;
    void setSpeed(double speed) override;
    void stepOnce() override;

private:
    SimulationEngine* m_engine = nullptr;
};

} // namespace gbr
