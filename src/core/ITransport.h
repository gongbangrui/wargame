#pragma once

#include "MessageBus.h"
#include <QObject>
#include <QString>
#include <QPointF>
#include <QJsonObject>
#include <functional>
#include <memory>

namespace gbr {

/// @brief Abstraction over the message transport layer.
/// @details Default implementation is `LocalTransport` (in-process publish/subscribe,
/// identical to the original MessageBus behaviour). Future networked transports
/// (TCP, UDP multicast) plug in by implementing this interface. A single
/// MessageBus can hold one transport; switching transports does NOT require
/// touching engine/unit code.
class ITransport {
public:
    virtual ~ITransport() = default;

    /// @brief Send a message through the transport.
    /// @details The transport is responsible for delivering to local handlers
    /// and (when networked) to remote peers. Implementations must NOT filter
    /// by comm range — that is the receiver's job via canCommunicate().
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

    /// @brief Underlying MessageBus for direct access (only LocalTransport provides one).
    /// @details Networked transports return nullptr — units that need to call MessageBus-only
    /// helpers must go through this interface instead. Required for the existing engine
    /// code that uses `m_bus->updateUnitPosition` etc. without going through transport.
    virtual MessageBus* bus() const = 0;

    /// @brief Convenience: returns true when this transport supports direct
    /// in-process MessageBus delivery (LocalTransport). Networked transports
    /// return false — their delivery path goes over the wire.
    virtual bool isLocal() const = 0;
};

} // namespace gbr