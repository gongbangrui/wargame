#pragma once

#include "Geo.h"
#include "MessageBus.h"
#include "Scenario.h"
#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <vector>
#include <memory>

namespace gbr {

enum class Side { Red, Blue };

inline QString sideName(Side s) {
    return s == Side::Red ? QStringLiteral("red") : QStringLiteral("blue");
}

inline Side sideFromName(const QString& n) {
    return n == QStringLiteral("red") ? Side::Red : Side::Blue;
}

enum class UnitKind {
    CommandPost,
    ReconUAV,
    AttackUAV,
    GroundScout,
};

inline QString kindName(UnitKind k) {
    switch (k) {
    case UnitKind::CommandPost: return QStringLiteral("commandpost");
    case UnitKind::ReconUAV: return QStringLiteral("reconuav");
    case UnitKind::AttackUAV: return QStringLiteral("attackuav");
    case UnitKind::GroundScout: return QStringLiteral("groundscout");
    }
    return QStringLiteral("unknown");
}

inline UnitKind kindFromName(const QString& n) {
    if (n == QStringLiteral("commandpost")) return UnitKind::CommandPost;
    if (n == QStringLiteral("reconuav")) return UnitKind::ReconUAV;
    if (n == QStringLiteral("attackuav")) return UnitKind::AttackUAV;
    if (n == QStringLiteral("groundscout")) return UnitKind::GroundScout;
    return UnitKind::CommandPost;
}

class UnitBase : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(QString callsign READ callsign WRITE setCallsign NOTIFY callsignChanged)
    Q_PROPERTY(QString side READ sideStr NOTIFY sideChanged)
    Q_PROPERTY(QString kind READ kindStr CONSTANT)
    Q_PROPERTY(QVariantList position READ position NOTIFY positionChanged)
    Q_PROPERTY(QJsonObject perception READ perceptionJson NOTIFY perceptionChanged)
    Q_PROPERTY(QJsonObject sharedKnowledge READ sharedKnowledgeJson NOTIFY sharedKnowledgeChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(double detectRange READ detectRange WRITE setDetectRange NOTIFY paramsChanged)
    Q_PROPERTY(double attackRange READ attackRange WRITE setAttackRange NOTIFY paramsChanged)
    Q_PROPERTY(double commRange READ commRange WRITE setCommRange NOTIFY paramsChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY paramsChanged)
    Q_PROPERTY(double maxHp READ maxHp WRITE setMaxHp NOTIFY paramsChanged)
    Q_PROPERTY(double hp READ hp NOTIFY hpChanged)
    Q_PROPERTY(bool alive READ alive NOTIFY hpChanged)
    Q_PROPERTY(bool movable READ movable CONSTANT)
public:
    struct Params {
        double detectRange = 5000.0;
        double attackRange = 1500.0;
        double commRange = 20000.0;
        double speed = 50.0;
        double maxHp = 100.0;
        GeoPos pos;
    };

    UnitBase(const QString& id, UnitKind kind, Side side, MessageBus* bus, QObject* parent = nullptr);
    virtual ~UnitBase();

    void setParams(const Params& p);
    void setPosition(const GeoPos& pos);
    virtual void onTick(double simSeconds) = 0;

    QString id() const { return m_id; }
    QString callsign() const { return m_callsign; }
    void setCallsign(const QString& s);
    QString sideStr() const { return sideName(m_side); }
    QString kindStr() const { return kindName(m_kind); }
    UnitKind kind() const { return m_kind; }
    Side side() const { return m_side; }
    bool movable() const { return m_kind != UnitKind::CommandPost; }

    QVariantList position() const;
    QJsonObject perceptionJson() const;
    QJsonObject sharedKnowledgeJson() const;
    QString statusText() const { return m_status; }
    void setStatus(const QString& s);

    double detectRange() const { return m_params.detectRange; }
    double attackRange() const { return m_params.attackRange; }
    double commRange() const { return m_params.commRange; }
    double speed() const { return m_params.speed; }
    double maxHp() const { return m_params.maxHp; }
    double hp() const { return m_hp; }
    bool alive() const { return m_hp > 0.0; }

    void setDetectRange(double v) { m_params.detectRange = v; emit paramsChanged(); }
    void setAttackRange(double v) { m_params.attackRange = v; emit paramsChanged(); }
    void setCommRange(double v) { m_params.commRange = v; emit paramsChanged(); }
    void setSpeed(double v) { m_params.speed = v; emit paramsChanged(); }
    void setMaxHp(double v) { m_params.maxHp = v; emit paramsChanged(); }
    void setHp(double v) { if (m_hp != v) { m_hp = v; emit hpChanged(); } }

    void handleMessage(const Message& m);
    bool hasActiveWaypoints() const { return m_hasActiveWaypoints; }
    void setHasActiveWaypoints(bool v) { m_hasActiveWaypoints = v; }

    bool canDetect(const GeoPos& pos) const;
    GeoPos pos() const { return m_params.pos; }
    const Params& params() const { return m_params; }

    const std::vector<SchedulePoint>& schedule() const { return m_schedule; }
    void setSchedule(const std::vector<SchedulePoint>& s);

    static std::unique_ptr<UnitBase> create(const QString& id, UnitKind kind, Side side, MessageBus* bus, QObject* parent = nullptr);

signals:
    void callsignChanged();
    void sideChanged();
    void positionChanged();
    void perceptionChanged();
    void sharedKnowledgeChanged();
    void statusChanged();
    void paramsChanged();
    void hpChanged();
    void notifyEvent(const QString& title, const QString& body, const QString& level);
    void strikeOrderRequested(const QString& targetId);
    void routeChangeRequested();
    void attackProgress(double distanceToTarget, double attackRange);

protected:
    virtual void onMessage(const Message& m);

    void send(const Message& m) { if (m_bus) m_bus->send(m); }
    void rememberShared(const QString& key, const QJsonValue& v);

    QString m_id;
    QString m_callsign;
    UnitKind m_kind;
    Side m_side;
    Params m_params;
    double m_hp = 100.0;
    QString m_status;
    QJsonObject m_sharedKnowledge;
    MessageBus* m_bus = nullptr;
    std::vector<SchedulePoint> m_schedule;
    bool m_hasActiveWaypoints = false;
};

} // namespace gbr
