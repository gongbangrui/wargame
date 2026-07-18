#include "SessionGateway.h"

#include "PersistenceStore.h"
#include "ServerConfig.h"
#include "ServerMetrics.h"
#include "SimulationRoom.h"
#include "protocol/Protocol.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketProtocol>
#include <algorithm>
#include <cmath>
#include <vector>

namespace gbr {

namespace {

QString newMessageId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool finiteNumber(const QJsonValue& value) {
    return value.isDouble() && std::isfinite(value.toDouble());
}

bool validId(const QJsonValue& value) {
    const QString id = value.toString();
    return value.isString() && !id.isEmpty() && id.size() <= ProtocolLimits::MaxIdLength;
}

bool hasOnlyFields(const QJsonObject& object, const QSet<QString>& allowed) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowed.contains(it.key())) return false;
    }
    return true;
}

bool validNonnegativeInteger(const QJsonValue& value) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number && number >= 0.0;
}

QMap<QString, QJsonObject> unitsById(const QJsonArray& units) {
    QMap<QString, QJsonObject> result;
    for (const auto& value : units) {
        const QJsonObject unit = value.toObject();
        result.insert(unit.value(QStringLiteral("id")).toString(), unit);
    }
    return result;
}

QJsonObject buildStateDelta(const QJsonObject& previous, const QJsonObject& current) {
    const auto oldUnits = unitsById(previous.value(QStringLiteral("units")).toArray());
    const auto newUnits = unitsById(current.value(QStringLiteral("units")).toArray());
    QJsonArray upsertUnits;
    QJsonArray removeFields;
    QJsonArray removeUnitIds;
    for (auto it = newUnits.constBegin(); it != newUnits.constEnd(); ++it) {
        const auto old = oldUnits.constFind(it.key());
        if (old == oldUnits.constEnd() || old.value() != it.value()) {
            if (old == oldUnits.constEnd()) {
                upsertUnits.append(it.value());
                continue;
            }
            QJsonObject patch{{QStringLiteral("id"), it.key()}};
            for (auto field = it.value().constBegin(); field != it.value().constEnd(); ++field) {
                if (field.key() != QLatin1String("id")
                    && old.value().value(field.key()) != field.value()) {
                    patch.insert(field.key(), field.value());
                }
            }
            upsertUnits.append(patch);
        }

        if (old == oldUnits.constEnd()) continue;
        QJsonArray fields;
        for (auto field = old.value().constBegin(); field != old.value().constEnd(); ++field) {
            if (field.key() != QLatin1String("id") && !it.value().contains(field.key())) {
                fields.append(field.key());
            }
        }
        if (!fields.isEmpty()) {
            removeFields.append(QJsonObject{{QStringLiteral("id"), it.key()},
                                            {QStringLiteral("fields"), fields}});
        }
    }
    for (auto it = oldUnits.constBegin(); it != oldUnits.constEnd(); ++it) {
        if (!newUnits.contains(it.key())) removeUnitIds.append(it.key());
    }
    QJsonObject delta{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("simTime"), current.value(QStringLiteral("simTime"))},
        {QStringLiteral("running"), current.value(QStringLiteral("running"))},
        {QStringLiteral("readyForSim"), current.value(QStringLiteral("readyForSim"))},
        {QStringLiteral("cpIssues"), current.value(QStringLiteral("cpIssues"))},
        {QStringLiteral("upsertUnits"), upsertUnits},
        {QStringLiteral("removeFields"), removeFields},
        {QStringLiteral("removeUnitIds"), removeUnitIds},
    };
    if (previous.value(QStringLiteral("messages"))
        != current.value(QStringLiteral("messages"))) {
        delta.insert(QStringLiteral("messages"), current.value(QStringLiteral("messages")));
    }
    if (previous.value(QStringLiteral("map")) != current.value(QStringLiteral("map"))) {
        delta.insert(QStringLiteral("map"), current.value(QStringLiteral("map")));
    }
    if (previous.value(QStringLiteral("lobby")) != current.value(QStringLiteral("lobby"))) {
        delta.insert(QStringLiteral("lobby"), current.value(QStringLiteral("lobby")));
    }
    if (previous.value(QStringLiteral("chatMessages"))
        != current.value(QStringLiteral("chatMessages"))) {
        delta.insert(QStringLiteral("chatMessages"), current.value(QStringLiteral("chatMessages")));
    }
    return delta;
}

bool validatePoints(const QJsonArray& points, bool includeTime, int maxPoints) {
    if (points.size() > maxPoints) return false;
    for (const auto& value : points) {
        const QJsonObject point = value.toObject();
        if (!value.isObject() || point.size() != (includeTime ? 3 : 2)
            || !finiteNumber(point.value(QStringLiteral("x")))
            || !finiteNumber(point.value(QStringLiteral("y")))
            || (includeTime && !finiteNumber(point.value(QStringLiteral("time"))))) return false;
    }
    return true;
}

bool validateCommandArgs(const QString& action, const QJsonObject& args, QString* error) {
    QSet<QString> allowed;
    bool valid = true;
    if (action == QLatin1String(CommandAction::AssignTarget)
        || action == QLatin1String(CommandAction::EngageTarget)
        || action == QLatin1String(CommandAction::Pursue)) {
        allowed = {QStringLiteral("attackerId"), QStringLiteral("targetId")};
        valid = validId(args.value(QStringLiteral("attackerId")))
            && validId(args.value(QStringLiteral("targetId")));
    } else if (action == QLatin1String(CommandAction::SetFlightPlan)) {
        allowed = {QStringLiteral("attackerId"), QStringLiteral("waypoints")};
        valid = validId(args.value(QStringLiteral("attackerId")))
            && args.value(QStringLiteral("waypoints")).isArray()
            && !args.value(QStringLiteral("waypoints")).toArray().isEmpty()
            && validatePoints(args.value(QStringLiteral("waypoints")).toArray(), false, 256);
    } else if (action == QLatin1String(CommandAction::MoveTo)) {
        allowed = {QStringLiteral("unitId"), QStringLiteral("pos")};
        const QJsonObject pos = args.value(QStringLiteral("pos")).toObject();
        valid = validId(args.value(QStringLiteral("unitId")))
            && args.value(QStringLiteral("pos")).isObject()
            && pos.size() == 2
            && finiteNumber(pos.value(QStringLiteral("x")))
            && finiteNumber(pos.value(QStringLiteral("y")));
    } else if (action == QLatin1String(CommandAction::Withdraw)
               || action == QLatin1String(CommandAction::Halt)) {
        allowed = {QStringLiteral("unitId")};
        valid = validId(args.value(QStringLiteral("unitId")));
    } else if (action == QLatin1String(CommandAction::SetSpeed)) {
        allowed = {QStringLiteral("unitId"), QStringLiteral("speed")};
        valid = validId(args.value(QStringLiteral("unitId")))
            && finiteNumber(args.value(QStringLiteral("speed")));
    } else if (action == QLatin1String(CommandAction::GuideAttack)) {
        allowed = {QStringLiteral("guideId"), QStringLiteral("attackerId"),
                   QStringLiteral("targetId"), QStringLiteral("targetPos")};
        const QJsonObject pos = args.value(QStringLiteral("targetPos")).toObject();
        valid = validId(args.value(QStringLiteral("guideId")))
            && validId(args.value(QStringLiteral("attackerId")))
            && validId(args.value(QStringLiteral("targetId")))
            && args.value(QStringLiteral("targetPos")).isObject()
            && pos.size() == 2
            && finiteNumber(pos.value(QStringLiteral("x")))
            && finiteNumber(pos.value(QStringLiteral("y")));
    } else if (action == QLatin1String(CommandAction::SetSchedule)) {
        allowed = {QStringLiteral("unitId"), QStringLiteral("schedule")};
        valid = validId(args.value(QStringLiteral("unitId")))
            && args.value(QStringLiteral("schedule")).isArray()
            && validatePoints(args.value(QStringLiteral("schedule")).toArray(), true, 4096);
    } else if (action == QLatin1String("setRunning")) {
        allowed = {QStringLiteral("running")};
        valid = args.value(QStringLiteral("running")).isBool();
    } else if (action == QLatin1String("setSimulationSpeed")) {
        allowed = {QStringLiteral("speed")};
        valid = finiteNumber(args.value(QStringLiteral("speed")));
    } else if (action == QLatin1String("stepOnce")) {
        valid = args.isEmpty();
    } else if (action == QLatin1String("resetSimulation")) {
        valid = args.isEmpty();
    } else if (action == QLatin1String("replaceScenario")) {
        allowed = {QStringLiteral("scenario")};
        valid = args.value(QStringLiteral("scenario")).isObject();
    } else if (action == QLatin1String("setSideReady")) {
        allowed = {QStringLiteral("ready")};
        valid = args.value(QStringLiteral("ready")).isBool();
    } else if (action == QLatin1String("sendChat")) {
        allowed = {QStringLiteral("text")};
        const QString text = args.value(QStringLiteral("text")).toString();
        valid = args.value(QStringLiteral("text")).isString()
            && !text.trimmed().isEmpty() && text.size() <= 500;
    } else {
        // Unknown actions are passed to the room so the stable UNKNOWN_ACTION
        // result is returned, but they may not smuggle arbitrary data.
        valid = args.isEmpty();
    }
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        if (!allowed.contains(it.key())) valid = false;
    }
    if (!valid && error) *error = QStringLiteral("命令参数字段、类型或数量无效");
    return valid;
}

} // namespace

SessionGateway::SessionGateway(const ServerConfig& config, AuthPolicy* auth,
                               PersistenceStore* persistence, SimulationRoom* room,
                               ServerMetrics* metrics, QObject* parent)
    : QObject(parent),
      m_config(config),
      m_auth(auth),
      m_persistence(persistence),
      m_room(room),
      m_metrics(metrics),
      m_server(QStringLiteral("wargame_server"), QWebSocketServer::NonSecureMode, this) {
    m_server.setMaxPendingConnections(config.maxConnections);
    connect(&m_server, &QWebSocketServer::newConnection,
            this, &SessionGateway::onNewConnection);
    m_snapshotTimer.setInterval(config.snapshotIntervalMs);
    connect(&m_snapshotTimer, &QTimer::timeout,
            this, &SessionGateway::broadcastStateUpdates);
    m_heartbeatTimer.setInterval(15000);
    connect(&m_heartbeatTimer, &QTimer::timeout,
            this, &SessionGateway::heartbeat);
}

SessionGateway::~SessionGateway() {
    m_snapshotTimer.stop();
    m_heartbeatTimer.stop();
    m_server.close();

    // QObject deletes child sockets after members have been destroyed. Disconnect
    // first so their teardown cannot access the already-destroyed connection map.
    for (const auto& [socket, connection] : m_connections) {
        QObject::disconnect(socket, nullptr, this, nullptr);
        socket->abort();
        if (m_metrics) {
            if (connection.authenticated) --m_metrics->authenticatedConnections;
            --m_metrics->activeConnections;
        }
    }
    m_connections.clear();
}

bool SessionGateway::listen(QString* error) {
    if (!m_server.listen(m_config.listenAddress, m_config.port)) {
        if (error) *error = m_server.errorString();
        return false;
    }
    m_snapshotTimer.start();
    m_heartbeatTimer.start();
    return true;
}

void SessionGateway::close() {
    m_snapshotTimer.stop();
    m_heartbeatTimer.stop();
    std::vector<QWebSocket*> sockets;
    sockets.reserve(m_connections.size());
    for (const auto& [socket, connection] : m_connections) {
        Q_UNUSED(connection);
        sockets.push_back(socket);
    }
    for (QWebSocket* socket : sockets) {
        socket->close(QWebSocketProtocol::CloseCodeGoingAway, QStringLiteral("服务器关闭"));
    }
    m_server.close();
}

void SessionGateway::onNewConnection() {
    while (QWebSocket* socket = m_server.nextPendingConnection()) {
        if (socket->requestUrl().path() != QLatin1String("/ws")) {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("WebSocket 路径必须是 /ws"));
            socket->deleteLater();
            continue;
        }
        if (static_cast<int>(m_connections.size()) >= m_config.maxConnections) {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("连接数已满"));
            socket->deleteLater();
            continue;
        }
        socket->setParent(this);
        socket->setMaxAllowedIncomingFrameSize(static_cast<quint64>(m_config.maxPacketBytes));
        socket->setMaxAllowedIncomingMessageSize(static_cast<quint64>(m_config.maxPacketBytes));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        Connection connection;
        connection.connectedAtMs = now;
        connection.lastSeenMs = now;
        connection.packetTokens = 200.0;
        connection.lastPacketRefillMs = now;
        connection.commandTokens = m_config.commandBurst;
        connection.lastTokenRefillMs = now;
        m_connections.emplace(socket, std::move(connection));
        ++m_metrics->activeConnections;
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString& message) { onTextMessage(socket, message); });
        connect(socket, &QWebSocket::binaryMessageReceived, this,
                [this, socket](const QByteArray&) {
                    auto it = m_connections.find(socket);
                    if (it != m_connections.end()) {
                        recordProtocolError(socket, it->second,
                                            QStringLiteral("BINARY_NOT_SUPPORTED"),
                                            QStringLiteral("首版协议只接受 JSON 文本消息"), true);
                    }
                });
        connect(socket, &QWebSocket::disconnected, this,
                [this, socket] { onDisconnected(socket); });
    }
}

void SessionGateway::onDisconnected(QWebSocket* socket) {
    auto it = m_connections.find(socket);
    if (it == m_connections.end()) return;
    if (it->second.authenticated) --m_metrics->authenticatedConnections;
    --m_metrics->activeConnections;
    m_connections.erase(it);
    socket->deleteLater();
}

void SessionGateway::onTextMessage(QWebSocket* socket, const QString& message) {
    auto it = m_connections.find(socket);
    if (it == m_connections.end()
        || it->second.closing
        || socket->state() != QAbstractSocket::ConnectedState) return;
    Connection& connection = it->second;
    const QByteArray bytes = message.toUtf8();
    m_metrics->bytesReceived += bytes.size();
    connection.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
    if (!consumePacketToken(connection)) {
        ++m_metrics->rateLimited;
        sendError(socket, connection, QStringLiteral("PACKET_RATE_LIMITED"),
                  QStringLiteral("消息频率超过连接限制"), true);
        return;
    }
    const ProtocolParseResult parsed = ProtocolCodec::parse(bytes, m_config.maxPacketBytes);
    if (!parsed.accepted) {
        recordProtocolError(socket, connection, parsed.code, parsed.message);
        return;
    }
    if (!connection.authenticated) {
        handleHello(socket, connection, parsed.envelope);
    } else {
        handleAuthenticated(socket, connection, parsed.envelope);
    }
}

void SessionGateway::handleHello(QWebSocket* socket, Connection& connection,
                                 const QJsonObject& envelope) {
    if (envelope.value(QStringLiteral("type")).toString()
        != QLatin1String(ProtocolType::Hello)) {
        recordProtocolError(socket, connection, QStringLiteral("HELLO_REQUIRED"),
                            QStringLiteral("认证前必须先发送 hello"), true);
        return;
    }
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    static const QSet<QString> helloFields{
        QStringLiteral("token"), QStringLiteral("username"), QStringLiteral("password"),
        QStringLiteral("resumeSequence"),
    };
    if (!hasOnlyFields(payload, helloFields)
        || (payload.contains(QStringLiteral("token"))
            && !payload.value(QStringLiteral("token")).isString())
        || (payload.contains(QStringLiteral("username"))
            && !payload.value(QStringLiteral("username")).isString())
        || (payload.contains(QStringLiteral("password"))
            && !payload.value(QStringLiteral("password")).isString())
        || (payload.contains(QStringLiteral("resumeSequence"))
            && !validNonnegativeInteger(payload.value(QStringLiteral("resumeSequence"))))) {
        recordProtocolError(socket, connection, QStringLiteral("INVALID_HELLO"),
                            QStringLiteral("hello payload 结构无效"));
        return;
    }
    QString error;
    SessionIdentity identity;
    if (payload.value(QStringLiteral("token")).isString()
        && !payload.value(QStringLiteral("token")).toString().isEmpty()) {
        identity = m_auth->authenticate(payload.value(QStringLiteral("token")).toString(), &error);
    } else if (payload.value(QStringLiteral("username")).isString()
               && payload.value(QStringLiteral("password")).isString()) {
        identity = m_auth->authenticatePassword(payload.value(QStringLiteral("username")).toString(),
                                                 payload.value(QStringLiteral("password")).toString(), &error);
    } else {
        error = QStringLiteral("必须提供 token 或账号密码");
    }
    if (!identity.isValid() || identity.roomId != m_room->roomId()) {
        ++m_metrics->authFailures;
        sendError(socket, connection, QStringLiteral("AUTH_FAILED"),
                  error.isEmpty() ? QStringLiteral("身份无效或房间不匹配") : error, true);
        return;
    }
    connection.identity = identity;
    connection.clientId = envelope.value(QStringLiteral("clientId")).toString();
    if (connection.clientId.isEmpty()) connection.clientId = newMessageId();
    connection.authenticated = true;
    ++m_metrics->authenticatedConnections;
    sendEnvelope(socket, connection, QString::fromLatin1(ProtocolType::Welcome),
                 {{QStringLiteral("userId"), identity.userId},
                  {QStringLiteral("role"), identity.role},
                  {QStringLiteral("side"), identity.side},
                  {QStringLiteral("roomId"), identity.roomId},
                  {QStringLiteral("heartbeatSeconds"), 15},
                  {QStringLiteral("protocolVersion"), ProtocolLimits::Version}});
    sendSnapshot(socket, connection);
}

void SessionGateway::handleAuthenticated(QWebSocket* socket, Connection& connection,
                                         const QJsonObject& envelope) {
    const QString type = envelope.value(QStringLiteral("type")).toString();
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    if (type == QLatin1String(ProtocolType::Command)) {
        handleCommand(socket, connection, envelope);
    } else if (type == QLatin1String(ProtocolType::ResyncRequest)) {
        static const QSet<QString> resyncFields{QStringLiteral("lastSequence")};
        if (payload.size() != 1 || !hasOnlyFields(payload, resyncFields)
            || !validNonnegativeInteger(payload.value(QStringLiteral("lastSequence")))) {
            recordProtocolError(socket, connection, QStringLiteral("INVALID_RESYNC"),
                                QStringLiteral("resyncRequest payload 结构无效"));
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (connection.lastResyncMs > 0 && now - connection.lastResyncMs < 1000) {
            ++m_metrics->rateLimited;
            sendError(socket, connection, QStringLiteral("RESYNC_RATE_LIMITED"),
                      QStringLiteral("完整重同步请求过于频繁"));
            return;
        }
        connection.lastResyncMs = now;
        ++m_metrics->resyncRequests;
        sendSnapshot(socket, connection);
    } else if (type == QLatin1String(ProtocolType::Ping)) {
        if (!payload.isEmpty()) {
            recordProtocolError(socket, connection, QStringLiteral("INVALID_PING"),
                                QStringLiteral("ping payload 必须为空"));
            return;
        }
        sendEnvelope(socket, connection, QString::fromLatin1(ProtocolType::Pong), {});
    } else if (type == QLatin1String(ProtocolType::Pong)) {
        if (!payload.isEmpty()) {
            recordProtocolError(socket, connection, QStringLiteral("INVALID_PONG"),
                                QStringLiteral("pong payload 必须为空"));
            return;
        }
        connection.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
    } else {
        recordProtocolError(socket, connection, QStringLiteral("CLIENT_TYPE_NOT_ALLOWED"),
                            QStringLiteral("客户端不能发送该消息类型"));
    }
}

bool SessionGateway::consumeCommandToken(Connection& connection) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double elapsedSeconds = std::max<qint64>(0, now - connection.lastTokenRefillMs) / 1000.0;
    connection.commandTokens = std::min<double>(
        m_config.commandBurst,
        connection.commandTokens + elapsedSeconds * m_config.commandRatePerSecond);
    connection.lastTokenRefillMs = now;
    if (connection.commandTokens < 1.0) return false;
    connection.commandTokens -= 1.0;
    return true;
}

bool SessionGateway::consumePacketToken(Connection& connection) {
    constexpr double packetRatePerSecond = 100.0;
    constexpr double packetBurst = 200.0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double elapsedSeconds = std::max<qint64>(
        0, now - connection.lastPacketRefillMs) / 1000.0;
    connection.packetTokens = std::min(
        packetBurst, connection.packetTokens + elapsedSeconds * packetRatePerSecond);
    connection.lastPacketRefillMs = now;
    if (connection.packetTokens < 1.0) return false;
    connection.packetTokens -= 1.0;
    return true;
}

void SessionGateway::handleCommand(QWebSocket* socket, Connection& connection,
                                   const QJsonObject& envelope) {
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    const QString commandId = payload.value(QStringLiteral("commandId")).toString();
    const QString action = payload.value(QStringLiteral("action")).toString();
    static const QSet<QString> commandFields{
        QStringLiteral("commandId"), QStringLiteral("action"), QStringLiteral("args"),
    };
    if (payload.size() != 3 || !hasOnlyFields(payload, commandFields)
        || !payload.value(QStringLiteral("commandId")).isString()
        || !payload.value(QStringLiteral("action")).isString()
        || commandId.isEmpty() || commandId.size() > ProtocolLimits::MaxIdLength
        || action.isEmpty() || action.size() > ProtocolLimits::MaxActionLength
        || !payload.value(QStringLiteral("args")).isObject()) {
        recordProtocolError(socket, connection, QStringLiteral("INVALID_COMMAND"),
                            QStringLiteral("commandId、action 或 args 无效"));
        return;
    }

    QJsonObject previous = m_persistence->commandResult(
        connection.identity.roomId, connection.identity.userId, commandId);
    if (!previous.isEmpty()) {
        previous.insert(QStringLiteral("duplicate"), true);
        sendEnvelope(socket, connection, QString::fromLatin1(ProtocolType::CommandResult), previous);
        return;
    }

    auto persistAndSend = [&](const CommandResult& result) {
        QJsonObject response = result.toJson();
        response.insert(QStringLiteral("commandId"), commandId);
        QString persistenceError;
        if (!m_persistence->storeCommandResult(connection.identity.roomId,
                                               connection.identity.userId,
                                               commandId, response,
                                               &persistenceError)) {
            ++m_metrics->persistenceFailures;
        }
        persistenceError.clear();
        if (!m_persistence->appendAudit(connection.identity, commandId, action,
                                        result.code, m_room->serverTick(), {},
                                        &persistenceError)) {
            ++m_metrics->persistenceFailures;
        }
        if (result.accepted) ++m_metrics->commandsAccepted;
        else ++m_metrics->commandsRejected;
        sendEnvelope(socket, connection, QString::fromLatin1(ProtocolType::CommandResult), response);
    };

    if (!consumeCommandToken(connection)) {
        ++m_metrics->rateLimited;
        ++m_metrics->commandsRejected;
        const CommandResult result = CommandResult::rejected(
            CommandCode::RateLimited, QStringLiteral("命令频率超过限制"), m_room->serverTick());
        QJsonObject response = result.toJson();
        response.insert(QStringLiteral("commandId"), commandId);
        sendEnvelope(socket, connection, QString::fromLatin1(ProtocolType::CommandResult), response);
        return;
    }
    QString schemaError;
    if (!validateCommandArgs(action, payload.value(QStringLiteral("args")).toObject(),
                             &schemaError)) {
        const CommandResult result = CommandResult::rejected(
            CommandCode::InvalidArgument, schemaError, m_room->serverTick());
        persistAndSend(result);
        return;
    }
    const qint64 revision = envelope.value(QStringLiteral("scenarioRevision")).toInteger(-1);
    const CommandResult result = m_room->execute(
        connection.identity, action,
        payload.value(QStringLiteral("args")).toObject().toVariantMap(), revision);
    persistAndSend(result);
    if (result.code == QLatin1String(CommandCode::RevisionMismatch)) sendSnapshot(socket, connection);
}

void SessionGateway::sendSnapshot(QWebSocket* socket, Connection& connection,
                                  const QJsonObject* projectedBase) {
    QJsonObject payload = projectedBase
        ? *projectedBase
        : m_room->projectedState(connection.identity, 0);
    payload.insert(QStringLiteral("lastSequence"), connection.sequence + 1);
    const QByteArray payloadBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (payloadBytes.size() > ProtocolLimits::MaxSnapshotBytes) {
        sendError(socket, connection, QStringLiteral("SNAPSHOT_TOO_LARGE"),
                  QStringLiteral("角色快照超过 8 MiB 限制"), true);
        return;
    }
    if (sendEnvelope(socket, connection, QString::fromLatin1(ProtocolType::Snapshot), payload)) {
        connection.lastProjectedState = payload;
        ++m_metrics->snapshotsSent;
    }
}

void SessionGateway::sendStateUpdate(QWebSocket* socket, Connection& connection,
                                     const QJsonObject& projectedBase) {
    if (connection.lastProjectedState.isEmpty()
        || connection.lastProjectedState.value(QStringLiteral("scenarioRevision")).toInteger(-1)
            != m_room->scenarioRevision()) {
        sendSnapshot(socket, connection, &projectedBase);
        return;
    }
    QJsonObject current = projectedBase;
    current.insert(QStringLiteral("lastSequence"), connection.sequence + 1);
    const QJsonObject delta = buildStateDelta(connection.lastProjectedState, current);
    const QByteArray deltaBytes = QJsonDocument(delta).toJson(QJsonDocument::Compact);
    const QByteArray snapshotBytes = QJsonDocument(current).toJson(QJsonDocument::Compact);
    if (deltaBytes.size() >= snapshotBytes.size()) {
        sendSnapshot(socket, connection, &projectedBase);
        return;
    }
    if (sendEnvelope(socket, connection, QString::fromLatin1(ProtocolType::Delta), delta)) {
        connection.lastProjectedState = current;
        ++m_metrics->deltasSent;
    }
}

void SessionGateway::broadcastStateUpdates() {
    std::vector<QWebSocket*> sockets;
    sockets.reserve(m_connections.size());
    for (const auto& [socket, connection] : m_connections) {
        if (connection.authenticated) sockets.push_back(socket);
    }
    QMap<QString, QJsonObject> projectedByAudience;
    for (QWebSocket* socket : sockets) {
        auto it = m_connections.find(socket);
        if (it != m_connections.end()
            && socket->state() == QAbstractSocket::ConnectedState) {
            Connection& connection = it->second;
            const QString audienceKey = connection.identity.role
                + QChar::Null + connection.identity.side;
            auto projected = projectedByAudience.constFind(audienceKey);
            if (projected == projectedByAudience.constEnd()) {
                projected = projectedByAudience.insert(
                    audienceKey, m_room->projectedState(connection.identity, 0));
            }
            sendStateUpdate(socket, connection, projected.value());
        }
    }
}

void SessionGateway::heartbeat() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_connections.begin(); it != m_connections.end();) {
        QWebSocket* socket = it->first;
        Connection& connection = it->second;
        ++it;
        if ((connection.authenticated && !connection.identity.isValid())
            || (!connection.authenticated && now - connection.connectedAtMs > 10000)
            || now - connection.lastSeenMs > 45000) {
            connection.closing = true;
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("身份失效、认证或心跳超时"));
            continue;
        }
        if (connection.authenticated) {
            sendEnvelope(socket, connection, QString::fromLatin1(ProtocolType::Ping), {});
        }
    }
}

bool SessionGateway::sendEnvelope(QWebSocket* socket, Connection& connection,
                                  const QString& type, const QJsonObject& payload,
                                  const QString& messageId) {
    if (!socket || connection.closing
        || socket->state() != QAbstractSocket::ConnectedState) return false;
    const qint64 nextSequence = connection.sequence + 1;
    const QJsonObject envelope = ProtocolCodec::envelope(
        type, payload, messageId.isEmpty() ? newMessageId() : messageId,
        m_room->roomId(), connection.clientId, nextSequence,
        m_room->scenarioRevision(), m_room->serverTick());
    const QByteArray bytes = ProtocolCodec::encode(envelope);
    const qint64 queuedBytes = socket->bytesToWrite();
    if (queuedBytes > 0
        && queuedBytes + bytes.size() > m_config.maxSendQueueBytes) {
        ++m_metrics->slowClientDisconnects;
        connection.closing = true;
        const QJsonObject errorEnvelope = ProtocolCodec::envelope(
            QString::fromLatin1(ProtocolType::Error),
            {{QStringLiteral("code"), QStringLiteral("SLOW_CLIENT")},
             {QStringLiteral("message"), QStringLiteral("客户端接收过慢")}},
            newMessageId(), m_room->roomId(), connection.clientId,
            nextSequence, m_room->scenarioRevision(), m_room->serverTick());
        const QByteArray errorBytes = ProtocolCodec::encode(errorEnvelope);
        if (socket->sendTextMessage(QString::fromUtf8(errorBytes)) >= 0) {
            connection.sequence = nextSequence;
            m_metrics->bytesSent += errorBytes.size();
        }
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                      QStringLiteral("客户端接收过慢"));
        return false;
    }
    if (socket->sendTextMessage(QString::fromUtf8(bytes)) < 0) return false;
    connection.sequence = nextSequence;
    m_metrics->bytesSent += bytes.size();
    return true;
}

void SessionGateway::sendError(QWebSocket* socket, Connection& connection,
                               const QString& code, const QString& message,
                               bool closeAfter) {
    sendEnvelope(socket, connection, QString::fromLatin1(ProtocolType::Error),
                 {{QStringLiteral("code"), code}, {QStringLiteral("message"), message}});
    if (closeAfter) {
        connection.closing = true;
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated, message.left(120));
    }
}

void SessionGateway::recordProtocolError(QWebSocket* socket, Connection& connection,
                                         const QString& code, const QString& message,
                                         bool closeAfter) {
    ++m_metrics->protocolErrors;
    ++connection.protocolErrors;
    sendError(socket, connection, code, message,
              closeAfter || connection.protocolErrors >= 3);
}

} // namespace gbr
