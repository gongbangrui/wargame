#include "ClientStateStore.h"

#include "protocol/Protocol.h"
#include "protocol/StateDelta.h"

#include <QJsonArray>

namespace gbr {

void ClientStateStore::reset() {
    m_lastSequence = 0;
    m_snapshot = {};
    m_lifecycle = {};
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
    QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    const QJsonObject roomState = payload.value(QStringLiteral("roomState")).toObject();
    if ((type == QLatin1String("snapshot") || type == QLatin1String("delta"))
        && roomState.value(QStringLiteral("observer")).toBool()
        && payload.value(QStringLiteral("mapMarks")).isArray()
        && payload.value(QStringLiteral("mapMarks")).toArray().isEmpty()) {
        // Keep the internal observer state on the current strict shape after
        // accepting the empty legacy compatibility field at the boundary.
        payload.remove(QStringLiteral("mapMarks"));
    }
    if (sequence <= m_lastSequence) return {Disposition::Ignored, type, payload, {}, {}};

    if (type == QLatin1String("snapshot")) {
        Protocol::SnapshotProjection projection;
        const Protocol::ValidationResult projectionResult =
            Protocol::projectSnapshot(payload, &projection);
        if (!projectionResult.valid) {
            return {Disposition::Fatal, {}, {}, projectionResult.code, projectionResult.message};
        }
        m_snapshot = payload;
        m_lifecycle = projection.lifecycle;
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

        const QJsonObject previousSnapshot = m_snapshot;
        const Protocol::RoomLifecycleProjection previousLifecycle = m_lifecycle;
        QJsonObject candidateSnapshot = m_snapshot;
        QString error;
        if (!StateDelta::apply(candidateSnapshot, payload, &error)) {
            m_snapshot = previousSnapshot;
            m_lifecycle = previousLifecycle;
            m_waitingForResync = true;
            return {Disposition::ResyncRequired, type, payload,
                    QStringLiteral("DELTA_REJECTED"), error};
        }
        Protocol::SnapshotProjection projection;
        const Protocol::ValidationResult projectionResult =
            Protocol::projectSnapshot(candidateSnapshot, &projection);
        if (!projectionResult.valid) {
            m_snapshot = previousSnapshot;
            m_lifecycle = previousLifecycle;
            m_waitingForResync = true;
            return {Disposition::ResyncRequired, type, payload,
                    QStringLiteral("STATE_PROJECTION_REJECTED"), projectionResult.message};
        }
        m_snapshot = candidateSnapshot;
        m_lifecycle = projection.lifecycle;
        m_lastSequence = sequence;
        return {Disposition::DeltaApplied, type, payload, {}, {}};
    }

    m_lastSequence = sequence;
    return {Disposition::Accepted, type, payload, {}, {}};
}

} // namespace gbr
