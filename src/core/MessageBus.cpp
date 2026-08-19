#include "MessageBus.h"

#include <QDateTime>
#include <QJsonArray>
#include <QDebug>
#include <QQueue>
#include <QSet>
#include <cmath>
#include <algorithm>

namespace gbr {

MessageBus::MessageBus(QObject* parent) : QObject(parent) {}

void MessageBus::subscribe(const QString& unitId, Handler h) {
    m_handlers[unitId].push_back(std::move(h));
    // Subscription establishes the unit's communication identity. Position
    // updates from an arbitrary id must not silently create registrations.
    m_units.try_emplace(unitId);
}

void MessageBus::unsubscribe(const QString& unitId) {
    m_handlers.erase(unitId);
}

void MessageBus::unregisterUnit(const QString& unitId) {
    m_handlers.erase(unitId);
    m_units.erase(unitId);
}

void MessageBus::updateUnitPosition(const QString& unitId, const QPointF& pos, double commRange, const QString& side) {
    auto it = m_units.find(unitId);
    if (it == m_units.end()) {
        qDebug() << "[bus] ignoring position update for unknown unit" << unitId;
        return;
    }
    auto& r = it->second;
    r.pos = pos;
    r.commRange = std::max(0.0, commRange);
    if (!side.isEmpty()) r.side = side;
}

void MessageBus::setUnitActive(const QString& unitId, bool active) {
    auto it = m_units.find(unitId);
    if (it == m_units.end()) return;
    it->second.active = active;
}

void MessageBus::setUnitCommandPost(const QString& unitId, bool isCp) {
    auto it = m_units.find(unitId);
    if (it == m_units.end()) return;
    it->second.isCp = isCp;
}

void MessageBus::updateUnitSide(const QString& unitId, const QString& side) {
    auto it = m_units.find(unitId);
    if (it == m_units.end()) return;
    it->second.side = side;
}

bool MessageBus::isRegistered(const QString& unitId) const {
    return m_units.find(unitId) != m_units.end();
}

QString MessageBus::unitSide(const QString& unitId) const {
    auto it = m_units.find(unitId);
    return it == m_units.end() ? QString() : it->second.side;
}

bool MessageBus::canCommunicate(const QString& aId, const QString& bId) const {
    auto a = m_units.find(aId);
    auto b = m_units.find(bId);
    if (a == m_units.end() || b == m_units.end()) return false;
    if (!a->second.active || !b->second.active) return false;
    if (a->second.side.isEmpty() || b->second.side.isEmpty()
        || a->second.side != b->second.side) return false;
    if (aId == bId) return true;

    // Communication is a directed, sender-range-limited graph. A message may
    // cross friendly relay units, matching the authoritative server
    // projection instead of granting the command post a hidden global link.
    QQueue<QString> pending;
    QSet<QString> visited;
    pending.enqueue(aId);
    visited.insert(aId);
    while (!pending.isEmpty()) {
        const QString currentId = pending.dequeue();
        const auto current = m_units.find(currentId);
        if (current == m_units.end() || !current->second.active) continue;
        for (const auto& [candidateId, candidate] : m_units) {
            if (visited.contains(candidateId) || !candidate.active
                || candidate.side != a->second.side) continue;
            const double dx = current->second.pos.x() - candidate.pos.x();
            const double dy = current->second.pos.y() - candidate.pos.y();
            if (std::hypot(dx, dy) > std::max(0.0, current->second.commRange)) continue;
            if (candidateId == bId) return true;
            visited.insert(candidateId);
            pending.enqueue(candidateId);
        }
    }
    return false;
}

bool MessageBus::send(const Message& msg) {
    Message m = msg;
    if (!m.timestamp.isValid()) m.timestamp = QDateTime::currentDateTimeUtc();
    if (m.id.isEmpty()) m.id = QStringLiteral("m_%1").arg(++m_seq);

    // The simulation chooses the communication profile once per scenario.
    // Encoding happens before ACK bookkeeping and delivery so retries repeat
    // the exact validated envelope rather than re-encoding mutable state.
    if (m_vmfEncoder && !m.vmfEncoded && m.type != Message::Type::Ack) {
        Message encoded;
        QString encodingError;
        if (!m_vmfEncoder(m, &encoded, &encodingError)) {
            emit vmfEncodingFailed(m.id, encodingError);
            return false;
        }
        encoded.vmfEncoded = true;
        m = std::move(encoded);
    }
    if (m_automaticAck && m.requiresAck) m.automaticAck = true;
    m.acked = !m.requiresAck;

    if (m.requiresAck && m.type != Message::Type::Ack) {
        PendingAck pending;
        pending.message = m;
        pending.sentAt = m_simulationTime;
        pending.retries = m.retryCount;
        m_pendingAcks.insert(m.id, std::move(pending));
    }

    if (m.type == Message::Type::Ack) {
        const QString inReplyTo = m.payload.value(QStringLiteral("inReplyTo")).toString();
        if (!inReplyTo.isEmpty()) {
            auto pending = m_pendingAcks.find(inReplyTo);
            if (pending != m_pendingAcks.end()) {
                const int retries = pending->retries;
                m_pendingAcks.erase(pending);
                emit ackStateChanged(inReplyTo, true, retries, QStringLiteral("ack"));
            }
        }
    }

    dispatch(m);
    return true;
}

void MessageBus::dispatch(const Message& msg) {
    Message m = msg;
    if (m.automaticAck && m_seenAutomaticMessages.contains(m.id)) {
        // A retry or duplicate VMF envelope is acknowledged again but must not
        // mutate domain state twice.
        if (m.receiver == "*") {
            std::vector<QString> recipients;
            recipients.reserve(m_handlers.size());
            for (const auto& [uid, _] : m_handlers) recipients.push_back(uid);
            for (const auto& uid : recipients) {
                if (uid != m.sender && canCommunicate(m.sender, uid)) maybeAutoAck(m, uid);
            }
        } else if (!m.receiver.isEmpty() && canCommunicate(m.sender, m.receiver)) {
            maybeAutoAck(m, m.receiver);
        }
        return;
    }
    QJsonObject posted = m.toJson();
    posted["senderSide"] = unitSide(m.sender);
    posted["receiverSide"] = unitSide(m.receiver);
    posted["simulationTime"] = m_simulationTime;
    emit messagePosted(posted);

    if (m.receiver == "*") {
        // Handlers may synchronously unregister units. Snapshot ids so those
        // callbacks cannot invalidate the map iteration.
        std::vector<QString> recipients;
        recipients.reserve(m_handlers.size());
        for (const auto& [uid, _] : m_handlers) recipients.push_back(uid);
        std::vector<QString> reachable;
        reachable.reserve(recipients.size());
        for (const auto& uid : recipients) {
            if (uid != m.sender && canCommunicate(m.sender, uid)) reachable.push_back(uid);
        }
        if (m.automaticAck && !reachable.empty()) m_seenAutomaticMessages.insert(m.id);
        for (const auto& uid : reachable) {
            deliver(m, uid);
            maybeAutoAck(m, uid);
        }
    } else if (m.receiver.isEmpty()) {
        qDebug() << "[bus] dropping message with empty receiver" << m.sender;
    } else {
        if (canCommunicate(m.sender, m.receiver)) {
            if (m.automaticAck) m_seenAutomaticMessages.insert(m.id);
            deliver(m, m.receiver);
            maybeAutoAck(m, m.receiver);
        } else {
            qDebug() << "[bus]" << m.sender << "->" << m.receiver << "NO COMM";
        }
    }
}

void MessageBus::maybeAutoAck(const Message& msg, const QString& recipientId) {
    if (!msg.requiresAck || !msg.automaticAck || msg.type == Message::Type::Ack) return;
    Message ack;
    ack.type = Message::Type::Ack;
    ack.sender = recipientId;
    ack.receiver = msg.sender;
    ack.automaticAck = false;
    ack.traceId = msg.traceId;
    ack.correlationId = msg.id;
    ack.payload.insert(QStringLiteral("inReplyTo"), msg.id);
    send(ack);
}

void MessageBus::setSimulationTime(double seconds) {
    if (!std::isfinite(seconds)) return;
    m_simulationTime = std::max(0.0, seconds);
}

void MessageBus::advanceSimulationTime(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0) return;
    m_simulationTime += seconds;

    const auto pendingIds = m_pendingAcks.keys();
    for (const QString& id : pendingIds) {
        auto pending = m_pendingAcks.find(id);
        if (pending == m_pendingAcks.end()) continue;
        if (m_simulationTime - pending->sentAt + 1e-9 < m_ackTimeoutSeconds) continue;
        if (pending->retries < m_maxAckRetries) {
            const int retryCount = ++pending->retries;
            pending->sentAt = m_simulationTime;
            Message retry = pending->message;
            retry.retryCount = retryCount;
            retry.acked = false;
            dispatch(retry);
            // dispatch() may synchronously receive an automatic ACK and erase
            // the pending entry, so do not dereference the iterator here.
            emit ackStateChanged(id, false, retryCount, QStringLiteral("retry"));
        } else {
            const int retries = pending->retries;
            m_pendingAcks.erase(pending);
            emit ackStateChanged(id, false, retries, QStringLiteral("timeout"));
        }
    }
}

void MessageBus::setAckPolicy(double timeoutSeconds, int maxRetries, bool automaticAck) {
    if (std::isfinite(timeoutSeconds) && timeoutSeconds > 0.0) {
        m_ackTimeoutSeconds = timeoutSeconds;
    }
    m_maxAckRetries = std::clamp(maxRetries, 0, 16);
    m_automaticAck = automaticAck;
}

QList<Message> MessageBus::pendingAcks() const {
    QList<Message> result;
    result.reserve(m_pendingAcks.size());
    for (auto it = m_pendingAcks.cbegin(); it != m_pendingAcks.cend(); ++it) {
        result.append(it->message);
    }
    return result;
}

QJsonArray MessageBus::pendingAckState() const {
    QJsonArray result;
    for (auto it = m_pendingAcks.cbegin(); it != m_pendingAcks.cend(); ++it) {
        QJsonObject entry;
        entry.insert(QStringLiteral("message"), it->message.toJson());
        entry.insert(QStringLiteral("sentAt"), it->sentAt);
        entry.insert(QStringLiteral("retries"), it->retries);
        result.append(entry);
    }
    return result;
}

bool MessageBus::restorePendingAckState(const QJsonArray& state, QString* error) {
    if (error) error->clear();
    if (state.size() > 1024) {
        if (error) *error = QStringLiteral("pending ACK 状态过大");
        return false;
    }
    QHash<QString, PendingAck> restored;
    for (const QJsonValue& value : state) {
        if (!value.isObject()) {
            if (error) *error = QStringLiteral("pending ACK 条目必须是对象");
            return false;
        }
        const QJsonObject entry = value.toObject();
        const QJsonObject json = entry.value(QStringLiteral("message")).toObject();
        const QString id = json.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || id.size() > 128 || !entry.value(QStringLiteral("sentAt")).isDouble()
            || !std::isfinite(entry.value(QStringLiteral("sentAt")).toDouble())
            || entry.value(QStringLiteral("sentAt")).toDouble() < 0.0
            || !entry.value(QStringLiteral("retries")).isDouble()
            || !std::isfinite(entry.value(QStringLiteral("retries")).toDouble())
            || entry.value(QStringLiteral("retries")).toDouble() < 0.0
            || entry.value(QStringLiteral("retries")).toDouble() > 16.0
            || std::floor(entry.value(QStringLiteral("retries")).toDouble())
                   != entry.value(QStringLiteral("retries")).toDouble()
            || restored.contains(id)) {
            if (error) *error = QStringLiteral("pending ACK 条目字段无效");
            return false;
        }
        Message message;
        message.id = id;
        if (!Message::parseTypeName(json.value(QStringLiteral("type")).toString(), &message.type)) {
            if (error) *error = QStringLiteral("pending ACK 消息类型无效");
            return false;
        }
        message.sender = json.value(QStringLiteral("sender")).toString();
        message.receiver = json.value(QStringLiteral("receiver")).toString();
        message.requiresAck = json.value(QStringLiteral("requiresAck")).toBool();
        message.automaticAck = json.value(QStringLiteral("automaticAck")).toBool();
        message.retryCount = json.value(QStringLiteral("retryCount")).toInt();
        message.traceId = json.value(QStringLiteral("traceId")).toString();
        message.correlationId = json.value(QStringLiteral("correlationId")).toString();
        message.vmfMessage = json.value(QStringLiteral("vmfMessage")).toString();
        const QByteArray encodedText = json.value(QStringLiteral("wireBytes")).toString().toLatin1();
        const QByteArray encoded = QByteArray::fromBase64(encodedText);
        if (json.contains(QStringLiteral("wireBytes"))
            && (encoded.isEmpty() || encoded.toBase64() != encodedText)) {
            if (error) *error = QStringLiteral("pending ACK wireBytes 无效");
            return false;
        }
        message.wireBytes = encoded;
        message.wireBitLength = json.value(QStringLiteral("wireBitLength")).toInt();
        if (!message.wireBytes.isEmpty()
            && (message.wireBitLength <= 0
                || message.wireBitLength > message.wireBytes.size() * 8)) {
            if (error) *error = QStringLiteral("pending ACK 位长度无效");
            return false;
        }
        if (!message.wireBytes.isEmpty() && message.wireBitLength % 8 != 0) {
            const unsigned char unusedMask = static_cast<unsigned char>(
                (1U << (8 - (message.wireBitLength % 8))) - 1U);
            if ((static_cast<unsigned char>(message.wireBytes.at(message.wireBytes.size() - 1))
                 & unusedMask) != 0U) {
                if (error) *error = QStringLiteral("pending ACK padding 位无效");
                return false;
            }
        }
        message.acked = json.value(QStringLiteral("acked")).toBool(false);
        message.payload = json.value(QStringLiteral("payload")).toObject();
        const QString wireFormat = json.value(QStringLiteral("wireFormat")).toString();
        message.wireFormat = wireFormat == QLatin1String("vmf-design-v1")
            ? Message::WireFormat::VmfDesignV1 : Message::WireFormat::Native;
        message.vmfEncoded = message.wireFormat == Message::WireFormat::VmfDesignV1;
        PendingAck pending;
        pending.message = message;
        pending.sentAt = entry.value(QStringLiteral("sentAt")).toDouble();
        pending.retries = entry.value(QStringLiteral("retries")).toInt();
        if (pending.message.requiresAck == false || pending.message.type == Message::Type::Ack) {
            if (error) *error = QStringLiteral("pending ACK 消息必须需要 ACK 且不能是 ACK");
            return false;
        }
        restored.insert(id, std::move(pending));
    }
    m_pendingAcks = std::move(restored);
    return true;
}

QJsonArray MessageBus::automaticMessageState() const {
    QJsonArray result;
    for (const QString& id : m_seenAutomaticMessages) result.append(id);
    return result;
}

bool MessageBus::restoreAutomaticMessageState(const QJsonArray& state, QString* error) {
    if (error) error->clear();
    if (state.size() > 4096) {
        if (error) *error = QStringLiteral("自动 ACK 去重状态过大");
        return false;
    }
    QSet<QString> restored;
    for (const QJsonValue& value : state) {
        if (!value.isString() || value.toString().trimmed().isEmpty()
            || value.toString().size() > 128 || restored.contains(value.toString())) {
            if (error) *error = QStringLiteral("自动 ACK 去重状态无效");
            return false;
        }
        restored.insert(value.toString());
    }
    m_seenAutomaticMessages = std::move(restored);
    return true;
}

void MessageBus::deliver(const Message& msg, const QString& targetId) {
    auto it = m_handlers.find(targetId);
    if (it == m_handlers.end()) return;
    // A callback is allowed to unsubscribe itself. Invoke a stable snapshot
    // and let subscription changes take effect on the next message.
    const auto handlers = it->second;
    for (const auto& h : handlers) h(msg);
}

} // namespace gbr
