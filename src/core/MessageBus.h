#pragma once

#include <QObject>
#include <QPointF>
#include <QPoint>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QTimer>
#include <functional>
#include <unordered_map>
#include <vector>

namespace gbr {

struct Message {
    enum class WireFormat { Native, VmfDesignV1 };
    enum class Type {
        PositionReport,        // 位置广播
        TargetDetect,          // 发现目标
        TargetReport,          // VMF/design target report
        TargetTrack,           // 跟踪目标
        TargetDestroyed,       // 目标摧毁确认
        UnitOrder,
        AttackOrder,           // 攻击命令
        StrikePlan,            // 指挥员确认的打击规划
        FlightPlan,            // 航路规划
        Guidance,              // 引导指令
        GroundGuideOrder,      // 指挥员命令地面分队引导
        GroundAttackConfirm,   // 地面分队人工确认攻击
        Ack,                   // 应答
        Withdraw,              // 撤离指令
        WithdrawOrder,         // 指挥员确认的撤离
        CommCheck,             // 通联检测
        EngagementReport,      // 交战报告
        SharedDetect,          // 共享侦察信息
        Pursue,                // 追击指令
        Halt,                  // 停止指令
        CancelEngagement,      // 取消交战，保留当前位置
        SetRulesOfEngagement,  // 交战规则
        IdentityReport,
        GroundTargetReport,
        RouteAcceptance,
        AttackReadyReport,
        AttackAuthorization,
        BattleDamageReport,
        DamageAssessmentConfirm,
    };

    static QString typeName(Type t) {
        switch (t) {
        case Type::PositionReport: return QStringLiteral("PositionReport");
        case Type::TargetDetect: return QStringLiteral("TargetDetect");
        case Type::TargetReport: return QStringLiteral("TargetReport");
        case Type::TargetTrack: return QStringLiteral("TargetTrack");
        case Type::TargetDestroyed: return QStringLiteral("TargetDestroyed");
        case Type::UnitOrder: return QStringLiteral("UnitOrder");
        case Type::AttackOrder: return QStringLiteral("AttackOrder");
        case Type::StrikePlan: return QStringLiteral("StrikePlan");
        case Type::FlightPlan: return QStringLiteral("FlightPlan");
        case Type::Guidance: return QStringLiteral("Guidance");
        case Type::GroundGuideOrder: return QStringLiteral("GroundGuideOrder");
        case Type::GroundAttackConfirm: return QStringLiteral("GroundAttackConfirm");
        case Type::Ack: return QStringLiteral("Ack");
        case Type::Withdraw: return QStringLiteral("Withdraw");
        case Type::WithdrawOrder: return QStringLiteral("WithdrawOrder");
        case Type::CommCheck: return QStringLiteral("CommCheck");
        case Type::EngagementReport: return QStringLiteral("EngagementReport");
        case Type::SharedDetect: return QStringLiteral("SharedDetect");
        case Type::Pursue: return QStringLiteral("Pursue");
        case Type::Halt: return QStringLiteral("Halt");
        case Type::CancelEngagement: return QStringLiteral("CancelEngagement");
        case Type::SetRulesOfEngagement: return QStringLiteral("SetRulesOfEngagement");
        case Type::IdentityReport: return QStringLiteral("IdentityReport");
        case Type::GroundTargetReport: return QStringLiteral("GroundTargetReport");
        case Type::RouteAcceptance: return QStringLiteral("RouteAcceptance");
        case Type::AttackReadyReport: return QStringLiteral("AttackReadyReport");
        case Type::AttackAuthorization: return QStringLiteral("AttackAuthorization");
        case Type::BattleDamageReport: return QStringLiteral("BattleDamageReport");
        case Type::DamageAssessmentConfirm: return QStringLiteral("DamageAssessmentConfirm");
        }
        return QStringLiteral("Unknown");
    }

    static bool parseTypeName(const QString& name, Type* output) {
        if (!output) return false;
        static const Type values[] = {
            Type::PositionReport, Type::TargetDetect, Type::TargetReport,
            Type::TargetTrack, Type::TargetDestroyed, Type::UnitOrder,
            Type::AttackOrder, Type::StrikePlan, Type::FlightPlan,
            Type::Guidance, Type::GroundGuideOrder, Type::GroundAttackConfirm,
            Type::Ack, Type::Withdraw, Type::WithdrawOrder, Type::CommCheck,
            Type::EngagementReport, Type::SharedDetect, Type::Pursue,
            Type::Halt, Type::CancelEngagement, Type::SetRulesOfEngagement,
            Type::IdentityReport, Type::GroundTargetReport, Type::RouteAcceptance,
            Type::AttackReadyReport, Type::AttackAuthorization,
            Type::BattleDamageReport, Type::DamageAssessmentConfirm};
        for (const Type value : values) {
            if (typeName(value) == name) {
                *output = value;
                return true;
            }
        }
        return false;
    }

    QString id;
    Type type = Type::PositionReport;
    QString sender;        // unit id
    QString receiver;      // unit id or "*" broadcast
    QDateTime timestamp;
    bool requiresAck = false;
    bool acked = false;
    bool automaticAck = false;
    // Internal loop-prevention marker. It is never serialized to clients.
    bool vmfEncoded = false;
    int retryCount = 0;
    QString traceId;
    QString correlationId;
    QString vmfMessage;
    WireFormat wireFormat = WireFormat::Native;
    QByteArray wireBytes;
    int wireBitLength = 0;
    QJsonObject payload;   // 自由格式：x,y,targetId,plan...

    static QString wireFormatName(WireFormat format) {
        return format == WireFormat::VmfDesignV1 ? QStringLiteral("vmf-design-v1")
                                                  : QStringLiteral("native");
    }

    QJsonObject toJson() const {
        QJsonObject o;
        o["id"] = id;
        o["type"] = typeName(type);
        o["sender"] = sender;
        o["receiver"] = receiver;
        o["time"] = timestamp.toString(Qt::ISODate);
        o["requiresAck"] = requiresAck;
        o["acked"] = acked;
        o["automaticAck"] = automaticAck;
        o["retryCount"] = retryCount;
        o["traceId"] = traceId;
        o["correlationId"] = correlationId;
        o["wireFormat"] = wireFormatName(wireFormat);
        if (!vmfMessage.isEmpty()) o["vmfMessage"] = vmfMessage;
        if (!wireBytes.isEmpty()) {
            o["wireBytes"] = QString::fromLatin1(wireBytes.toBase64());
            o["wireBitLength"] = wireBitLength;
        }
        o["payload"] = payload;
        return o;
    }
};

class MessageBus : public QObject {
    Q_OBJECT
public:
    using Handler = std::function<void(const Message&)>;
    using VmfEncoder = std::function<bool(const Message&, Message*, QString*)>;

    explicit MessageBus(QObject* parent = nullptr);

    /// Returns false when VMF encoding rejects the message before delivery.
    /// A message can still be accepted but undelivered when no communication
    /// path currently exists; that distinction is handled by ACK/retry state.
    bool send(const Message& msg);

    /// Advance ACK timers using simulation time.  A timeout at 3 seconds is
    /// retried twice and becomes a terminal failure at 9 seconds by default.
    void setSimulationTime(double seconds);
    double simulationTime() const { return m_simulationTime; }
    void advanceSimulationTime(double seconds);
    void setAckPolicy(double timeoutSeconds, int maxRetries, bool automaticAck = false);
    void setVmfEncoder(VmfEncoder encoder) { m_vmfEncoder = std::move(encoder); }
    bool hasVmfEncoder() const { return static_cast<bool>(m_vmfEncoder); }
    QList<Message> pendingAcks() const;
    QJsonArray pendingAckState() const;
    bool restorePendingAckState(const QJsonArray& state, QString* error = nullptr);
    QJsonArray automaticMessageState() const;
    bool restoreAutomaticMessageState(const QJsonArray& state, QString* error = nullptr);

    void subscribe(const QString& unitId, Handler h);

    void unsubscribe(const QString& unitId);

    /// Remove both message handlers and communication state for a unit.
    void unregisterUnit(const QString& unitId);

    bool canCommunicate(const QString& aId, const QString& bId) const;

    void updateUnitPosition(const QString& unitId, const QPointF& pos, double commRange, const QString& side = QString());
    /// Mark whether a registered unit is alive and may transmit or relay.
    /// Dead units remain registered so their identity can still be projected,
    /// but they must not participate in the communication graph.
    void setUnitActive(const QString& unitId, bool active);
    void setUnitCommandPost(const QString& unitId, bool isCp);

    void updateUnitSide(const QString& unitId, const QString& side);

    bool isRegistered(const QString& unitId) const;
    QString unitSide(const QString& unitId) const;

signals:
    void messagePosted(const QJsonObject& msg);
    void unitStateChanged(const QString& unitId, const QJsonObject& snapshot);
    void ackStateChanged(const QString& messageId, bool acknowledged,
                         int retryCount, const QString& reason);
    void vmfEncodingFailed(const QString& messageId, const QString& reason);

private:
    struct Reg {
        QPointF pos;
        double commRange = 0.0;
        QString side;
        bool isCp = false;
        bool active = true;
    };

    void deliver(const Message& msg, const QString& targetId);
    void dispatch(const Message& msg);
    void maybeAutoAck(const Message& msg, const QString& recipientId);

    struct PendingAck {
        Message message;
        double sentAt = 0.0;
        int retries = 0;
    };

    std::unordered_map<QString, std::vector<Handler>> m_handlers;
    std::unordered_map<QString, Reg> m_units;
    QHash<QString, PendingAck> m_pendingAcks;
    QSet<QString> m_seenAutomaticMessages;
    double m_simulationTime = 0.0;
    double m_ackTimeoutSeconds = 3.0;
    int m_maxAckRetries = 2;
    bool m_automaticAck = false;
    VmfEncoder m_vmfEncoder;
    int m_seq = 0;
};

} // namespace gbr
