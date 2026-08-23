#pragma once

#include "core/ITransport.h"

#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <memory>

namespace gbr {

/**
 * Fast DDS data-plane adapter.
 *
 * The domain model only depends on ITransport, so the local simulation keeps
 * using LocalTransport unchanged. When Fast DDS is available at configure
 * time this class is the boundary for DomainParticipant/Topic creation; the
 * fallback implementation deliberately preserves the same MessageBus rules
 * so development builds without the vendor SDK remain usable.
 */
class FastDdsTransport final : public QObject, public ITransport {
    Q_OBJECT
public:
    struct Options {
        QString domain = QStringLiteral("wargame");
        QString roomId;
        QString seatId;
        QString participantName = QStringLiteral("wargame-client");
        QString sessionTicket;
        quint32 domainId = 0;
        QString transportMode = QStringLiteral("disabled");
        QStringList staticPeers;
        int discoveryTimeoutMs = 5000;
    };

    explicit FastDdsTransport(QObject* parent = nullptr);
    FastDdsTransport(const Options& options, QObject* parent = nullptr);
    ~FastDdsTransport() override;

    bool start(QString* error = nullptr);
    bool start(const Options& options, QString* error = nullptr);
    void stop();
    bool running() const { return m_running; }
    QString backendName() const;
    Options options() const { return m_options; }

    void publishJson(const QString& topic, const QJsonObject& payload);
    void subscribeJson(const QString& topic, std::function<void(const QJsonObject&)> handler);
    void dispatchJson(const QString& topic, const QJsonObject& payload);
    // Called by the DDS listener to suppress the writer's own loopback sample.
    bool consumeLocalPublication(const QString& messageId);

    bool send(const Message& msg) override;
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
    MessageBus* bus() const override;
    bool isLocal() const override { return false; }

    QString sessionTicket() const { return m_options.sessionTicket; }

signals:
    void jsonPublished(const QString& topic, const QJsonObject& payload);

private:
    Options m_options;
    std::unique_ptr<class LocalTransport> m_compatBus;
    QHash<QString, QList<std::function<void(const QJsonObject&)>>> m_jsonHandlers;
    mutable QMutex m_jsonHandlersMutex;
    bool m_running = false;
    void* m_participant = nullptr;
    void* m_typeSupport = nullptr;
    void* m_topic = nullptr;
    void* m_bestEffortTopic = nullptr;
    void* m_publisher = nullptr;
    void* m_subscriber = nullptr;
    void* m_writer = nullptr;
    void* m_bestEffortWriter = nullptr;
    void* m_reader = nullptr;
    void* m_bestEffortReader = nullptr;
    void* m_readerListener = nullptr;
    void* m_bestEffortReaderListener = nullptr;
    quint64 m_publishSequence = 0;
    QSet<QString> m_localPublicationIds;
    mutable QMutex m_localPublicationMutex;

};

} // namespace gbr
