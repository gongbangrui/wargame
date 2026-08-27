#pragma once

#include "VmfCodec.h"
#include "VmfMessageCatalog.h"
#include "core/MessageBus.h"
#include "core/Geo.h"

#include <QObject>
#include <functional>
#include <memory>
#include <optional>

namespace gbr::vmf {

/// Bridges a domain Message and a validated VMF design-profile envelope.
/// Encoding and decoding are deliberately performed before the message is
/// handed to MessageBus, so a malformed XML/bit stream can never enter the
/// simulation through the VMF path.
class VmfMessageGateway final : public QObject {
    Q_OBJECT
public:
    using CoordinateResolver = std::function<std::optional<GeoCoord>(double x, double y)>;

    explicit VmfMessageGateway(MessageBus* bus,
                               std::shared_ptr<const DictionarySet> dictionaries,
                               std::shared_ptr<const VmfMessageCatalog> catalog = {},
                               QObject* parent = nullptr);

    /// Resolve simulation logical metres through the active GIS alignment.
    /// An empty resolver deliberately keeps the documented bounded local-grid
    /// fallback, which is useful for headless fixtures without map metadata.
    void setCoordinateResolver(CoordinateResolver resolver) {
        m_coordinateResolver = std::move(resolver);
    }
    void setAutomaticAckEnabled(bool enabled) { m_automaticAckEnabled = enabled; }
    bool automaticAckEnabled() const { return m_automaticAckEnabled; }

    bool send(const QString& messageName, const MessageXml& xml, Message message,
              EncodedMessage* encoded = nullptr, QString* error = nullptr);
    /// Map a domain message to the design profile and return a fully encoded
    /// message.  MessageBus uses this callback before ACK bookkeeping.
    bool prepareDomainMessage(const Message& input, Message* output,
                              QString* error = nullptr,
                              QJsonObject* trace = nullptr) const;
    /// Validate and encode operator-supplied XML with the same dictionaries and
    /// catalog used by template-generated runtime messages.
    bool prepareXmlMessage(const QString& messageName, const QByteArray& xml,
                           const Message& input, Message* output,
                           QJsonObject* trace = nullptr,
                           QString* error = nullptr) const;
    bool decode(const Message& message, DecodedMessage* decoded,
                QList<Diagnostic>* diagnostics = nullptr) const;

    const VmfMessageCatalog* catalog() const { return m_catalog.get(); }
    bool validateCatalog(const Message& message, const QString& senderRole = {},
                         const QString& receiverRole = {},
                         QString* error = nullptr) const;
    QJsonObject catalogSummary(const Message& message) const;

    /// Returns the design-profile message selected for a domain message.  The
    /// selection is deterministic so an inbound envelope can be checked
    /// against its declared domain type before it reaches MessageBus.
    static QString messageNameForType(Message::Type type,
                                      const QJsonObject& payload = {});
    static bool isMessageNameCompatible(const Message& message);

    QJsonArray recentTraceSummaries() const { return m_recentTraces; }
    bool restoreTraceSummaries(const QJsonArray& traces, QString* error = nullptr);
    static QJsonObject traceSummary(const Message& message, const EncodedMessage& encoded);

signals:
    void diagnostic(const QString& code, const QString& path, const QString& message);
    void vmfDelivered(const QJsonObject& trace);

private:
    static bool domainMessageXml(const Message& message, MessageXml* xml,
                                 QString* messageName,
                                 const CoordinateResolver& coordinateResolver,
                                 QString* error);
    void rememberTrace(const QJsonObject& trace);

    MessageBus* m_bus = nullptr;
    std::shared_ptr<const DictionarySet> m_dictionaries;
    std::shared_ptr<const VmfMessageCatalog> m_catalog;
    CoordinateResolver m_coordinateResolver;
    bool m_automaticAckEnabled = true;
    QJsonArray m_recentTraces;
    quint64 m_traceSequence = 0;
};

} // namespace gbr::vmf
