#pragma once

#include "MessageBus.h"
#include <QObject>
#include <QString>
#include <QPointF>
#include <QJsonObject>
#include <functional>
#include <memory>

namespace gbr {

/// @brief 推演域消息传输抽象。
/// @details 默认实现为进程内发布订阅的 `LocalTransport`。它表达战场通信，
/// 不代表客户端与服务器之间的 WebSocket 连接。
class ITransport {
public:
    virtual ~ITransport() = default;

    /// @brief Send a message through the transport.
    /// @details 消息交给推演域处理器，并保留 MessageBus 的通信距离规则。
    virtual void send(const Message& msg) = 0;

    /// @brief Subscribe a local handler to receive messages addressed to @p unitId.
    /// @details Implementations must be safe to call before start().
    virtual void subscribe(const QString& unitId, MessageBus::Handler h) = 0;

    /// @brief Drop all subscriptions for @p unitId.
    virtual void unsubscribe(const QString& unitId) = 0;

    /// @brief Drop both subscriptions and comm-state for @p unitId.
    virtual void unregisterUnit(const QString& unitId) = 0;

    /// @brief Check whether @p aId can communicate with @p bId.
    virtual bool canCommunicate(const QString& aId, const QString& bId) const = 0;

    /// @brief Update a unit's position, comm range, and side.
    virtual void updateUnitPosition(const QString& unitId, const QPointF& pos,
                                    double commRange, const QString& side = QString()) = 0;

    /// @brief Mark a unit as a command post (bypasses range checks).
    virtual void setUnitCommandPost(const QString& unitId, bool isCp) = 0;

    /// @brief Update a unit's side (used when scenario changes side).
    virtual void updateUnitSide(const QString& unitId, const QString& side) = 0;

    /// @brief Query whether a unit is registered.
    virtual bool isRegistered(const QString& unitId) const = 0;

    /// @brief Return the registered side for @p unitId, or empty if unknown.
    virtual QString unitSide(const QString& unitId) const = 0;

    /// @brief Set the sink that will receive every emitted message as JSON.
    /// @details Used by MessageBus to forward posted messages for the
    /// `messagePosted` signal (and the recent-message cache). The transport
    /// calls this once per emission; the sink should be cheap (no IO).
    using Sink = std::function<void(const QJsonObject&)>;
    virtual void setMessageSink(Sink sink) = 0;

    /// @brief 返回维护推演域通信状态的 MessageBus。
    virtual MessageBus* bus() const = 0;

    /// @brief 是否为进程内实现。
    virtual bool isLocal() const = 0;
};

} // namespace gbr
