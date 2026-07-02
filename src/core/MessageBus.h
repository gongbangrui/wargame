#pragma once

#include <QObject>
#include <QPointF>
#include <QPoint>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QTimer>
#include <functional>
#include <unordered_map>
#include <vector>

namespace gbr {

struct Message {
    enum class Type {
        PositionReport,        // 位置广播
        TargetDetect,          // 发现目标
        TargetTrack,           // 跟踪目标
        TargetDestroyed,       // 目标摧毁确认
        AttackOrder,           // 攻击命令
        FlightPlan,            // 航路规划
        Guidance,              // 引导指令
        Ack,                   // 应答
        Withdraw,              // 撤离指令
        CommCheck,             // 通联检测
        EngagementReport,      // 交战报告
        Info,                  // 通用消息
    };

    static QString typeName(Type t) {
        switch (t) {
        case Type::PositionReport: return QStringLiteral("PositionReport");
        case Type::TargetDetect: return QStringLiteral("TargetDetect");
        case Type::TargetTrack: return QStringLiteral("TargetTrack");
        case Type::TargetDestroyed: return QStringLiteral("TargetDestroyed");
        case Type::AttackOrder: return QStringLiteral("AttackOrder");
        case Type::FlightPlan: return QStringLiteral("FlightPlan");
        case Type::Guidance: return QStringLiteral("Guidance");
        case Type::Ack: return QStringLiteral("Ack");
        case Type::Withdraw: return QStringLiteral("Withdraw");
        case Type::CommCheck: return QStringLiteral("CommCheck");
        case Type::EngagementReport: return QStringLiteral("EngagementReport");
        case Type::Info: return QStringLiteral("Info");
        }
        return QStringLiteral("Unknown");
    }

    QString id;
    Type type = Type::Info;
    QString sender;        // unit id
    QString receiver;      // unit id or "*" broadcast
    QDateTime timestamp;
    bool requiresAck = false;
    bool acked = false;
    QJsonObject payload;   // 自由格式：x,y,targetId,plan...

    QJsonObject toJson() const {
        QJsonObject o;
        o["id"] = id;
        o["type"] = typeName(type);
        o["sender"] = sender;
        o["receiver"] = receiver;
        o["time"] = timestamp.toString(Qt::ISODate);
        o["requiresAck"] = requiresAck;
        o["acked"] = acked;
        o["payload"] = payload;
        return o;
    }
};

class MessageBus : public QObject {
    Q_OBJECT
public:
    using Handler = std::function<void(const Message&)>;

    explicit MessageBus(QObject* parent = nullptr);

    void send(const Message& msg);

    void subscribe(const QString& unitId, Handler h);

    void unsubscribe(const QString& unitId);

    bool canCommunicate(const QString& aId, const QString& bId) const;

    void updateUnitPosition(const QString& unitId, const QPointF& pos, double commRange, const QString& side = QString());

    void updateUnitSide(const QString& unitId, const QString& side);

    bool isRegistered(const QString& unitId) const;
    QString unitSide(const QString& unitId) const;

signals:
    void messagePosted(const QJsonObject& msg);
    void unitStateChanged(const QString& unitId, const QJsonObject& snapshot);

private:
    struct Reg {
        QPointF pos;
        double commRange = 0.0;
        QString side; // 红/蓝
    };

    void deliver(const Message& msg, const QString& targetId);

    std::unordered_map<QString, std::vector<Handler>> m_handlers;
    std::unordered_map<QString, Reg> m_units;
    int m_seq = 0;
};

} // namespace gbr

