#pragma once

#include "../core/UnitBase.h"
#include "../core/Geo.h"
#include <QVariantList>
#include <vector>

namespace gbr {

class AttackUAV : public UnitBase {
    Q_OBJECT
    Q_PROPERTY(QString targetId READ targetId NOTIFY targetChanged)
    Q_PROPERTY(double distanceToTarget READ distanceToTarget NOTIFY targetChanged)
    Q_PROPERTY(bool armed READ armed NOTIFY armedChanged)
public:
    explicit AttackUAV(const QString& id, Side side, MessageBus* bus, QObject* parent = nullptr);

    void onTick(double dt) override;

    QString targetId() const { return m_targetId; }
    double distanceToTarget() const;
    bool armed() const { return m_armed; }

    void setOtherPositions(const std::vector<std::pair<QString, GeoPos>>& others);
    void fireOnTarget(const QString& targetId);

protected:
    void onMessage(const Message& m) override;

signals:
    void targetChanged();
    void armedChanged();

private:
    void stepMotion(double dt);
    void stepCombat(double dt);

    QString m_targetId;
    QVariantList m_waypoints; // QPointF 列表
    int m_wpIdx = 0;
    bool m_armed = false;
    std::vector<std::pair<QString, GeoPos>> m_others;
    double m_cooldown = 0.0;
};

} // namespace gbr

