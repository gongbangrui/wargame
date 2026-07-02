#pragma once

#include "../core/UnitBase.h"
#include <QObject>
#include <QString>
#include <QJsonObject>
#include <unordered_map>

namespace gbr {

class CommandPost : public UnitBase {
    Q_OBJECT
public:
    explicit CommandPost(const QString& id, Side side, MessageBus* bus, QObject* parent = nullptr);

    void onTick(double dt) override;

    Q_INVOKABLE QJsonObject pendingTargets() const { return m_pending; }
    Q_INVOKABLE QStringList knownTargets() const;
    Q_INVOKABLE QJsonObject knownTarget(const QString& id) const;

    Q_INVOKABLE void orderStrike(const QString& attackerId, const QString& targetId);
    Q_INVOKABLE void orderWithdraw(const QString& unitId);
    Q_INVOKABLE void setFlightPlan(const QString& attackerId, const QVariantList& waypoints);

protected:
    void onMessage(const Message& m) override;

private:
    void rememberTarget(const QString& id, const QJsonObject& info);
    void sendAck(const Message& original);

    QJsonObject m_pending;            // targetId -> info
    QJsonObject m_targets;            // targetId -> info（已确认/已锁定）
    QJsonObject m_reports;            // unitId -> last target info
};

} // namespace gbr

