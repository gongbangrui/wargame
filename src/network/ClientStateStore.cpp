#include "ClientStateStore.h"

#include "protocol/Protocol.h"
#include "protocol/StateDelta.h"

namespace gbr {

void ClientStateStore::reset() {
    m_lastSequence = 0;
    m_snapshot = {};
    m_waitingForSnapshot = true;
    m_waitingForResync = false;
}

void ClientStateStore::beginConnection() {
    // Server envelope sequences are scoped to one WebSocket connection. Keep
    // the last rendered snapshot while reconnecting, but require a fresh
    // authoritative snapshot before accepting deltas on the new connection.
    m_lastSequence = 0;
    m_waitingForSnapshot = true;
    m_waitingForResync = false;
}

qint64 ClientStateStore::stateRevision() const {
    return m_snapshot.value(QStringLiteral("stateRevision")).toInteger();
}

ClientStateStore::Result ClientStateStore::applyEnvelope(const QJsonObject& envelope) {
    const Protocol::ValidationResult validation = Protocol::validateServerEnvelope(envelope);
    if (!validation.valid) {
        return {Disposition::Fatal, {}, {}, validation.code, validation.message};
    }

    const quint64 sequence = static_cast<quint64>(
        envelope.value(QStringLiteral("sequence")).toInteger());
    const QString type = envelope.value(QStringLiteral("type")).toString();
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    if (sequence <= m_lastSequence) return {Disposition::Ignored, type, payload, {}, {}};

    if (type == QLatin1String("snapshot")) {
        m_snapshot = payload;
        m_lastSequence = sequence;
        m_waitingForSnapshot = false;
        m_waitingForResync = false;
        return {Disposition::SnapshotApplied, type, payload, {}, {}};
    }

    const quint64 expected = m_lastSequence + 1;
    if (sequence != expected) {
        if (!m_waitingForResync) {
            m_waitingForResync = true;
            return {Disposition::ResyncRequired, type, payload,
                    QStringLiteral("SEQUENCE_GAP"),
                    QStringLiteral("服务器消息序号不连续")};
        }
        return {Disposition::Ignored, type, payload, {}, {}};
    }
    if (m_waitingForResync) return {Disposition::Ignored, type, payload, {}, {}};

    if (type == QLatin1String("delta")) {
        if (m_waitingForSnapshot) {
            m_waitingForResync = true;
            return {Disposition::ResyncRequired, type, payload,
                    QStringLiteral("SNAPSHOT_REQUIRED"),
                    QStringLiteral("尚未建立完整状态基线")};
        }
        QString error;
        if (!StateDelta::apply(m_snapshot, payload, &error)) {
            m_waitingForResync = true;
            return {Disposition::ResyncRequired, type, payload,
                    QStringLiteral("DELTA_REJECTED"), error};
        }
        m_lastSequence = sequence;
        return {Disposition::DeltaApplied, type, payload, {}, {}};
    }

    m_lastSequence = sequence;
    return {Disposition::Accepted, type, payload, {}, {}};
}

} // namespace gbr
