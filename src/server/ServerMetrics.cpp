#include "ServerMetrics.h"

#include <QDateTime>

namespace gbr {

QJsonObject ServerMetrics::health(const QString& version, bool ready) const {
    return {{QStringLiteral("status"), ready ? QStringLiteral("ok") : QStringLiteral("starting")},
            {QStringLiteral("version"), version},
            {QStringLiteral("ready"), ready},
            {QStringLiteral("uptimeSeconds"),
             startedAtMs > 0 ? (QDateTime::currentMSecsSinceEpoch() - startedAtMs) / 1000 : 0}};
}

QByteArray ServerMetrics::prometheus() const {
    QByteArray output;
    auto metric = [&output](const char* name, double value) {
        output += name;
        output += ' ';
        output += QByteArray::number(value, 'f', 3);
        output += '\n';
    };
    metric("wargame_connections", activeConnections);
    metric("wargame_authenticated_connections", authenticatedConnections);
    metric("wargame_commands_accepted_total", commandsAccepted);
    metric("wargame_commands_rejected_total", commandsRejected);
    metric("wargame_auth_failures_total", authFailures);
    metric("wargame_protocol_errors_total", protocolErrors);
    metric("wargame_rate_limited_total", rateLimited);
    metric("wargame_resync_requests_total", resyncRequests);
    metric("wargame_snapshots_sent_total", snapshotsSent);
    metric("wargame_deltas_sent_total", deltasSent);
    metric("wargame_bytes_sent_total", bytesSent);
    metric("wargame_bytes_received_total", bytesReceived);
    metric("wargame_slow_client_disconnects_total", slowClientDisconnects);
    metric("wargame_checkpoint_failures_total", checkpointFailures);
    metric("wargame_persistence_failures_total", persistenceFailures);
    metric("wargame_last_tick_duration_ms", lastTickDurationMs);
    return output;
}

} // namespace gbr
