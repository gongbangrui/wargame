#pragma once

#include "ITransport.h"
#include <QObject>
#include <memory>

namespace gbr {

/// @brief In-process transport wrapping the existing MessageBus.
/// @details Zero behavioural difference from the pre-refactor MessageBus:
/// same in-memory pub/sub, same canCommunicate rules, same CP bypass.
/// All methods forward to the held MessageBus. This is the default transport
/// when running single-process; networking layers would be TcpTransport,
/// UdpTransport, etc.
class LocalTransport : public QObject, public ITransport {
    Q_OBJECT
public:
    explicit LocalTransport(QObject* parent = nullptr);
    ~LocalTransport() override = default;

    // ITransport interface
    void send(const Message& msg) override;
    void subscribe(const QString& unitId, MessageBus::Handler h) override;
    void unsubscribe(const QString& unitId) override;
    void unregisterUnit(const QString& unitId) override;
    bool canCommunicate(const QString& aId, const QString& bId) const override;
    void updateUnitPosition(const QString& unitId, const QPointF& pos,
                            double commRange, const QString& side = QString()) override;
    void setUnitCommandPost(const QString& unitId, bool isCp) override;
    void updateUnitSide(const QString& unitId, const QString& side) override;
    bool isRegistered(const QString& unitId) const override;
    QString unitSide(const QString& unitId) const override;
    void setMessageSink(Sink sink) override;
    MessageBus* bus() const override { return m_bus.get(); }
    bool isLocal() const override { return true; }

private:
    std::unique_ptr<MessageBus> m_bus;
    Sink m_sink;
};

} // namespace gbr