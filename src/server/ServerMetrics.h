#pragma once

#include <QJsonObject>
#include <QString>

namespace gbr {

struct ServerMetrics {
    qint64 startedAtMs = 0;
    qint64 activeConnections = 0;
    qint64 authenticatedConnections = 0;
    qint64 commandsAccepted = 0;
    qint64 commandsRejected = 0;
    qint64 authFailures = 0;
    qint64 protocolErrors = 0;
    qint64 rateLimited = 0;
    qint64 resyncRequests = 0;
    qint64 snapshotsSent = 0;
    qint64 deltasSent = 0;
    qint64 bytesSent = 0;
    qint64 bytesReceived = 0;
    qint64 slowClientDisconnects = 0;
    qint64 checkpointFailures = 0;
    qint64 persistenceFailures = 0;
    double lastTickDurationMs = 0.0;

    QJsonObject health(const QString& version, bool ready) const;
    QByteArray prometheus() const;
};

} // namespace gbr
