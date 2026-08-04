#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QJsonObject>
#include <QSet>
#include <QMutex>
#include <functional>

namespace gbr {

class FastDdsNode final : public QObject {
    Q_OBJECT
public:
    using TicketValidator = std::function<bool(const QString& seatId, const QString& ticket)>;
    explicit FastDdsNode(QObject* parent = nullptr);
    ~FastDdsNode() override;

    bool start(const QString& roomId, quint32 domainId, QString* error = nullptr);
    void stop();
    bool running() const { return m_running; }
    QString backendName() const;
    QString roomId() const { return m_roomId; }
    quint32 domainId() const { return m_domainId; }
    QStringList topicNames() const { return m_topics; }
    void publishJson(const QString& topic, const QJsonObject& payload);
    void dispatchJson(const QString& topic, const QJsonObject& payload);
    void setTicketValidator(TicketValidator validator);
    bool isSeatAuthenticated(const QString& seatId) const;
    bool consumeLocalPublication(const QString& messageId);

signals:
    void transportWarning(const QString& message);
    void envelopeReceived(const QString& topic, const QJsonObject& payload);
    void ddsSeatAuthenticated(const QString& seatId);
    void transportSecurityWarning(const QString& message);

private:
    bool m_running = false;
    QString m_roomId;
    quint32 m_domainId = 0;
    QStringList m_topics;
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
    QSet<QString> m_authenticatedSeats;
    QSet<QString> m_localPublicationIds;
    mutable QMutex m_localPublicationMutex;
    TicketValidator m_ticketValidator;

};

} // namespace gbr
