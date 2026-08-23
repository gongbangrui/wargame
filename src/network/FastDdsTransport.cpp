#include "FastDdsTransport.h"

#include "core/LocalTransport.h"
#include "protocol/Protocol.h"
#include "protocol/dds/WargameEnvelope.h"

#include <QDebug>
#include <QDateTime>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QMutexLocker>
#include <QHostAddress>
#include <QAbstractSocket>

#include <sstream>
#include <string>
#include <vector>

#ifdef WARGAME_HAS_FASTDDS
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#ifdef WARGAME_FASTDDS_LEGACY_API
#include <fastdds/rtps/common/Locator.h>
#include <fastrtps/utils/IPLocator.h>
#else
#include <fastdds/rtps/common/Locator.hpp>
#include <fastdds/utils/IPLocator.hpp>
#endif
#ifdef WARGAME_FASTDDS_LEGACY_API
#include <WargameEnvelopePubSubTypes.h>
#elif __has_include(<WargameEnvelopePubSubTypes.hpp>)
#include <WargameEnvelopePubSubTypes.hpp>
#else
#include <WargameEnvelopePubSubTypes.h>
#endif
#endif

namespace gbr {

#ifdef WARGAME_HAS_FASTDDS
#ifdef WARGAME_FASTDDS_LEGACY_API
namespace fastdds_rtps = eprosima::fastrtps::rtps;
#else
namespace fastdds_rtps = eprosima::fastdds::rtps;
#endif
namespace {

using DdsType = gbr::protocol::dds::WargameEnvelope;
#ifdef WARGAME_FASTDDS_LEGACY_API
using DdsReturnCode = eprosima::fastrtps::types::ReturnCode_t;
#else
using DdsReturnCode = eprosima::fastdds::dds::ReturnCode_t;
#endif
using namespace eprosima::fastdds::dds;

bool ddsReturnCodeOk(DdsReturnCode code) {
#ifdef WARGAME_FASTDDS_LEGACY_API
    return code == DdsReturnCode::RETCODE_OK;
#else
    return code == eprosima::fastdds::dds::RETCODE_OK;
#endif
}

class ReaderListener final : public DataReaderListener {
public:
    explicit ReaderListener(FastDdsTransport* owner) : m_owner(owner) {}

    void on_data_available(DataReader* reader) override {
        DdsType sample;
        SampleInfo info;
        while (ddsReturnCodeOk(reader->take_next_sample(&sample, &info))) {
            if (!info.valid_data || !m_owner) continue;
            const QString messageId = QString::fromUtf8(sample.messageId().c_str());
            if (m_owner->consumeLocalPublication(messageId)) continue;
            const auto& payload = sample.payload();
            Protocol::Dds::WargameEnvelope envelope;
            envelope.protocolVersion = sample.protocolVersion();
            envelope.schemaVersion = sample.schemaVersion();
            envelope.messageType = QString::fromUtf8(sample.messageType().c_str());
            envelope.messageId = messageId;
            envelope.sequence = sample.sequenceNumber();
            envelope.stateRevision = sample.stateRevision();
            envelope.scenarioRevision = sample.scenarioRevision();
            envelope.serverTick = sample.serverTick();
            envelope.sentAt = QString::fromUtf8(sample.sentAt().c_str());
            envelope.payload = QByteArray(reinterpret_cast<const char*>(payload.data()),
                                          static_cast<qsizetype>(payload.size()));
            const auto decoded = Protocol::Dds::fromJson(Protocol::Dds::toJson(envelope));
            if (!decoded.valid) continue;
            const QByteArray bytes = decoded.envelope.payload;
            const QString messageType = decoded.envelope.messageType;
            QPointer<FastDdsTransport> owner(m_owner);
            QMetaObject::invokeMethod(m_owner, [owner, messageType, bytes]() {
                if (owner) {
                    const QJsonDocument document = QJsonDocument::fromJson(bytes);
                    if (document.isObject()) {
                        owner->dispatchJson(messageType, document.object());
                    }
                }
            }, Qt::QueuedConnection);
        }
    }

private:
    FastDdsTransport* m_owner = nullptr;
};

} // namespace
#endif

#ifdef WARGAME_HAS_FASTDDS
namespace {

bool parsePeerLocator(const QString& value, fastdds_rtps::Locator_t* locator) {
    if (!locator) return false;
    const QString text = value.trimmed();
    if (text.isEmpty()) return false;
    if (text.contains(QStringLiteral("["))) {
        std::istringstream stream(text.toStdString());
        stream >> *locator;
        if (stream.fail()) return false;
        stream >> std::ws;
        return stream.eof();
    }
    const int separator = text.lastIndexOf(QLatin1Char(':'));
    if (separator <= 0 || separator == text.size() - 1) return false;
    const QHostAddress address(text.left(separator));
    bool ok = false;
    const uint port = text.mid(separator + 1).toUInt(&ok);
    if (!ok || port > 65535 || address.protocol() != QAbstractSocket::IPv4Protocol) return false;
    *locator = fastdds_rtps::Locator_t{};
    locator->kind = LOCATOR_KIND_UDPv4;
    return fastdds_rtps::IPLocator::setIPv4(*locator, address.toString().toStdString())
        && (locator->port = port, true);
}

}
#endif

FastDdsTransport::FastDdsTransport(QObject* parent)
    : FastDdsTransport(Options{}, parent) {}

FastDdsTransport::FastDdsTransport(const Options& options, QObject* parent)
    : QObject(parent), m_options(options), m_compatBus(std::make_unique<LocalTransport>()) {}

FastDdsTransport::~FastDdsTransport() {
    stop();
}

bool FastDdsTransport::start(QString* error) {
    if (m_running) return true;
    if (m_options.roomId.trimmed().isEmpty() || m_options.seatId.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Fast DDS 会话必须指定房间和战位");
        return false;
    }
    const QString mode = m_options.transportMode.trimmed().toLower();
    if (mode == QLatin1String("disabled") || mode == QLatin1String("off")
        || mode == QLatin1String("none")) {
        if (error) *error = QStringLiteral("Fast DDS 按传输模式配置禁用");
        return false;
    }
    if (mode == QLatin1String("compatibility")) {
        if (error) *error = QStringLiteral("Fast DDS 已禁用：安全认证与加密配置尚未部署");
        return false;
    }
#ifdef WARGAME_HAS_FASTDDS
    using namespace eprosima::fastdds::dds;
    DomainParticipantQos qos = PARTICIPANT_QOS_DEFAULT;
    qos.name(m_options.participantName.toStdString());
    if (m_options.discoveryTimeoutMs > 0) {
        const int leaseSeconds = qBound(2, (m_options.discoveryTimeoutMs + 999) / 1000, 3600);
        qos.wire_protocol().builtin.discovery_config.leaseDuration.seconds = leaseSeconds;
        qos.wire_protocol().builtin.discovery_config.leaseDuration_announcementperiod.seconds =
            qBound(1, leaseSeconds / 3, leaseSeconds - 1);
    }
    for (const QString& peer : m_options.staticPeers) {
        fastdds_rtps::Locator_t locator;
        if (!parsePeerLocator(peer, &locator)) {
            if (error) *error = QStringLiteral("Fast DDS 静态 peer 地址无效: %1").arg(peer);
            return false;
        }
        qos.wire_protocol().builtin.initialPeersList.push_back(locator);
    }
    m_participant = DomainParticipantFactory::get_instance()->create_participant(
        m_options.domainId, qos);
    if (!m_participant) {
        if (error) *error = QStringLiteral("Fast DDS Participant 创建失败");
        return false;
    }

    auto fail = [this, error](const QString& message) {
        if (error) *error = message;
        stop();
        return false;
    };
    auto* participant = static_cast<DomainParticipant*>(m_participant);
    auto* typeSupport = new TypeSupport(new gbr::protocol::dds::WargameEnvelopePubSubType());
    if (!ddsReturnCodeOk(typeSupport->register_type(participant))) {
        delete typeSupport;
        return fail(QStringLiteral("Fast DDS envelope 类型注册失败"));
    }
    m_typeSupport = typeSupport;
    Topic* topic = participant->create_topic(QStringLiteral("WargameEnvelope").toStdString(),
                                             typeSupport->get_type_name(), TOPIC_QOS_DEFAULT);
    if (!topic) return fail(QStringLiteral("Fast DDS envelope Topic 创建失败"));
    m_topic = topic;
    Topic* bestEffortTopic = participant->create_topic(
        QStringLiteral("WargameEnvelopeBestEffort").toStdString(),
        typeSupport->get_type_name(), TOPIC_QOS_DEFAULT);
    if (!bestEffortTopic) return fail(QStringLiteral("Fast DDS best-effort Topic 创建失败"));
    m_bestEffortTopic = bestEffortTopic;

    PublisherQos publisherQos = PUBLISHER_QOS_DEFAULT;
    std::vector<std::string> partitions{
        QStringLiteral("room/%1").arg(m_options.roomId).toStdString()};
    publisherQos.partition().names(partitions);
    Publisher* publisher = participant->create_publisher(publisherQos);
    if (!publisher) return fail(QStringLiteral("Fast DDS Publisher 创建失败"));
    m_publisher = publisher;
    DataWriterQos writerQos = DATAWRITER_QOS_DEFAULT;
    writerQos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    writerQos.history().kind = KEEP_LAST_HISTORY_QOS;
    writerQos.history().depth = 8;
    writerQos.data_sharing().off();
    DataWriter* writer = publisher->create_datawriter(topic, writerQos);
    if (!writer) return fail(QStringLiteral("Fast DDS DataWriter 创建失败"));
    m_writer = writer;
    DataWriterQos bestEffortWriterQos = DATAWRITER_QOS_DEFAULT;
    bestEffortWriterQos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    bestEffortWriterQos.history().kind = KEEP_LAST_HISTORY_QOS;
    bestEffortWriterQos.history().depth = 8;
    bestEffortWriterQos.data_sharing().off();
    DataWriter* bestEffortWriter = publisher->create_datawriter(bestEffortTopic,
                                                                 bestEffortWriterQos);
    if (!bestEffortWriter) return fail(QStringLiteral("Fast DDS best-effort DataWriter 创建失败"));
    m_bestEffortWriter = bestEffortWriter;

    SubscriberQos subscriberQos = SUBSCRIBER_QOS_DEFAULT;
    subscriberQos.partition().names(partitions);
    Subscriber* subscriber = participant->create_subscriber(subscriberQos);
    if (!subscriber) return fail(QStringLiteral("Fast DDS Subscriber 创建失败"));
    m_subscriber = subscriber;
    DataReaderQos readerQos = DATAREADER_QOS_DEFAULT;
    readerQos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    readerQos.history().kind = KEEP_LAST_HISTORY_QOS;
    readerQos.history().depth = 8;
    readerQos.data_sharing().off();
    auto* listener = new ReaderListener(this);
    DataReader* reader = subscriber->create_datareader(topic, readerQos, listener);
    if (!reader) {
        delete listener;
        return fail(QStringLiteral("Fast DDS DataReader 创建失败"));
    }
    m_readerListener = listener;
    m_reader = reader;
    auto* bestEffortListener = new ReaderListener(this);
    DataReaderQos bestEffortReaderQos = DATAREADER_QOS_DEFAULT;
    bestEffortReaderQos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    bestEffortReaderQos.history().kind = KEEP_LAST_HISTORY_QOS;
    bestEffortReaderQos.history().depth = 8;
    bestEffortReaderQos.data_sharing().off();
    DataReader* bestEffortReader = subscriber->create_datareader(bestEffortTopic,
                                                                  bestEffortReaderQos,
                                                                  bestEffortListener);
    if (!bestEffortReader) {
        delete bestEffortListener;
        return fail(QStringLiteral("Fast DDS best-effort DataReader 创建失败"));
    }
    m_bestEffortReaderListener = bestEffortListener;
    m_bestEffortReader = bestEffortReader;
#else
    if (error) *error = QStringLiteral("Fast DDS 集成已禁用：缺少 SDK 或 fastddsgen");
    return false;
#endif
    m_running = true;
    if (!m_options.sessionTicket.isEmpty()) {
        publishJson(QStringLiteral("DdsHandshake"),
                    QJsonObject{{QStringLiteral("seatId"), m_options.seatId},
                                {QStringLiteral("ddsTicket"), m_options.sessionTicket}});
    }
    return true;
}

bool FastDdsTransport::start(const Options& options, QString* error) {
    stop();
    m_options = options;
    return start(error);
}

void FastDdsTransport::stop() {
#ifdef WARGAME_HAS_FASTDDS
    m_running = false;
    if (m_participant) {
        using namespace eprosima::fastdds::dds;
        auto* participant = static_cast<DomainParticipant*>(m_participant);
        participant->delete_contained_entities();
        delete static_cast<ReaderListener*>(m_readerListener);
        m_readerListener = nullptr;
        delete static_cast<ReaderListener*>(m_bestEffortReaderListener);
        m_bestEffortReaderListener = nullptr;
        delete static_cast<TypeSupport*>(m_typeSupport);
        m_typeSupport = nullptr;
        DomainParticipantFactory::get_instance()->delete_participant(participant);
        m_participant = nullptr;
    }
#endif
    m_running = false;
    m_typeSupport = nullptr;
    m_topic = nullptr;
    m_bestEffortTopic = nullptr;
    m_publisher = nullptr;
    m_subscriber = nullptr;
    m_writer = nullptr;
    m_bestEffortWriter = nullptr;
    m_reader = nullptr;
    m_bestEffortReader = nullptr;
    m_readerListener = nullptr;
    m_bestEffortReaderListener = nullptr;
    m_publishSequence = 0;
    {
        QMutexLocker publicationLocker(&m_localPublicationMutex);
        m_localPublicationIds.clear();
    }
    QMutexLocker locker(&m_jsonHandlersMutex);
    m_jsonHandlers.clear();
}

QString FastDdsTransport::backendName() const {
    if (m_running) {
        return QStringLiteral("Fast DDS 会话传输；WebSocket 权威数据面");
    }
    return QStringLiteral("Fast DDS disabled until authenticated encrypted transport is deployed; WebSocket authoritative data plane");
}

void FastDdsTransport::publishJson(const QString& topic, const QJsonObject& payload) {
    if (!m_running) return;
#ifdef WARGAME_HAS_FASTDDS
    DataWriter* writer = static_cast<DataWriter*>(m_writer);
    const bool bestEffort = topic == QLatin1String("Heartbeat")
        || topic == QLatin1String("heartbeat")
        || topic == QLatin1String("ChatMessage")
        || topic == QLatin1String("MapMark");
    if (bestEffort) writer = static_cast<DataWriter*>(m_bestEffortWriter);
    if (!writer) return;
    QJsonObject wirePayload = payload;
    if (!m_options.sessionTicket.isEmpty()) {
        wirePayload.insert(QStringLiteral("ddsTicket"), m_options.sessionTicket);
        wirePayload.insert(QStringLiteral("seatId"), m_options.seatId);
    }
    const QByteArray bytes = QJsonDocument(wirePayload).toJson(QJsonDocument::Compact);
    if (bytes.isEmpty() || bytes.size() > Protocol::Dds::MaxEnvelopePayloadBytes) {
        qWarning() << "Fast DDS envelope payload exceeds the configured limit" << topic
                   << bytes.size();
        return;
    }
    DdsType sample;
    sample.protocolVersion(Protocol::Version);
    sample.schemaVersion(Protocol::SchemaVersion);
    sample.messageType(topic.toStdString());
    const quint64 sequence = ++m_publishSequence;
    const QString messageId = QStringLiteral("dds-%1").arg(sequence);
    sample.messageId(messageId.toStdString());
    sample.sequenceNumber(sequence);
    sample.stateRevision(0);
    sample.scenarioRevision(0);
    sample.serverTick(0);
    sample.sentAt(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString());
    sample.payload().assign(reinterpret_cast<const uint8_t*>(bytes.constData()),
                            reinterpret_cast<const uint8_t*>(bytes.constData() + bytes.size()));
    {
        QMutexLocker publicationLocker(&m_localPublicationMutex);
        m_localPublicationIds.insert(messageId);
        while (m_localPublicationIds.size() > 2048) {
            m_localPublicationIds.remove(*m_localPublicationIds.cbegin());
        }
    }
    if (!ddsReturnCodeOk(writer->write(&sample))) {
        qWarning() << "Fast DDS envelope publish failed" << topic;
        return;
    }
#endif
    emit jsonPublished(topic, payload);
    QList<std::function<void(const QJsonObject&)>> handlers;
    {
        QMutexLocker locker(&m_jsonHandlersMutex);
        handlers = m_jsonHandlers.value(topic);
    }
    for (const auto& handler : handlers) {
        if (handler) handler(payload);
    }
}

bool FastDdsTransport::consumeLocalPublication(const QString& messageId) {
    if (messageId.isEmpty()) return false;
    QMutexLocker locker(&m_localPublicationMutex);
    const auto it = m_localPublicationIds.find(messageId);
    if (it == m_localPublicationIds.end()) return false;
    m_localPublicationIds.erase(it);
    return true;
}

void FastDdsTransport::dispatchJson(const QString& topic, const QJsonObject& payload) {
    if (!m_running) return;
    emit jsonPublished(topic, payload);
    QList<std::function<void(const QJsonObject&)>> handlers;
    {
        QMutexLocker locker(&m_jsonHandlersMutex);
        handlers = m_jsonHandlers.value(topic);
    }
    for (const auto& handler : handlers) {
        if (handler) handler(payload);
    }
}

void FastDdsTransport::subscribeJson(const QString& topic,
                                     std::function<void(const QJsonObject&)> handler) {
    if (topic.trimmed().isEmpty() || !handler) return;
    QMutexLocker locker(&m_jsonHandlersMutex);
    m_jsonHandlers[topic].append(std::move(handler));
}

bool FastDdsTransport::send(const Message& msg) { return m_compatBus->send(msg); }
void FastDdsTransport::subscribe(const QString& unitId, MessageBus::Handler h) { m_compatBus->subscribe(unitId, std::move(h)); }
void FastDdsTransport::unsubscribe(const QString& unitId) { m_compatBus->unsubscribe(unitId); }
void FastDdsTransport::unregisterUnit(const QString& unitId) { m_compatBus->unregisterUnit(unitId); }
bool FastDdsTransport::canCommunicate(const QString& aId, const QString& bId) const { return m_compatBus->canCommunicate(aId, bId); }
void FastDdsTransport::updateUnitPosition(const QString& unitId, const QPointF& pos, double commRange, const QString& side) { m_compatBus->updateUnitPosition(unitId, pos, commRange, side); }
void FastDdsTransport::setUnitCommandPost(const QString& unitId, bool isCp) { m_compatBus->setUnitCommandPost(unitId, isCp); }
void FastDdsTransport::updateUnitSide(const QString& unitId, const QString& side) { m_compatBus->updateUnitSide(unitId, side); }
bool FastDdsTransport::isRegistered(const QString& unitId) const { return m_compatBus->isRegistered(unitId); }
QString FastDdsTransport::unitSide(const QString& unitId) const { return m_compatBus->unitSide(unitId); }
void FastDdsTransport::setMessageSink(Sink sink) { m_compatBus->setMessageSink(std::move(sink)); }
MessageBus* FastDdsTransport::bus() const { return m_compatBus->bus(); }

} // namespace gbr
