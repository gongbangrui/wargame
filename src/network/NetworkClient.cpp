#include "NetworkClient.h"

#include "protocol/Protocol.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace gbr {

NetworkClient::NetworkClient(QObject* parent) : QObject(parent) {
    m_monotonic.start();
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &NetworkClient::openWebSocket);
    m_connectTimer.setSingleShot(true);
    connect(&m_connectTimer, &QTimer::timeout, this, [this]() {
        if (m_socket.state() == QAbstractSocket::ConnectingState) {
            m_diagnosticState = QStringLiteral("error");
            m_diagnosticMessage = QStringLiteral("推演服务器连接超时，正在重试");
            publishDiagnostics();
            m_socket.abort();
        }
    });
    m_authTimer.setSingleShot(true);
    connect(&m_authTimer, &QTimer::timeout, this, [this]() {
        if (!m_authenticated && m_socket.state() == QAbstractSocket::ConnectedState) {
            m_diagnosticState = QStringLiteral("error");
            m_diagnosticMessage = QStringLiteral("推演服务器认证超时，正在重试");
            publishDiagnostics();
            m_socket.close();
        }
    });
    connect(&m_socket, &QWebSocket::connected, this, &NetworkClient::onWebSocketConnected);
    connect(&m_socket, &QWebSocket::disconnected, this, &NetworkClient::onWebSocketDisconnected);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &NetworkClient::onTextMessage);
    m_latencyTimer.setInterval(10000);
    connect(&m_latencyTimer, &QTimer::timeout, this, &NetworkClient::sendLatencyProbe);
    m_commandTimer.setInterval(1000);
    connect(&m_commandTimer, &QTimer::timeout, this, &NetworkClient::processPendingCommands);
    m_commandTimer.start();
    connect(&m_socket, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                if (!m_manualClose) setState(QStringLiteral("error"), m_socket.errorString());
            });
}

void NetworkClient::publishDiagnostics() {
    emit diagnosticsChanged(m_diagnosticState, m_diagnosticMessage,
                            m_accountLatencyMs, m_gameLatencyMs);
}

void NetworkClient::diagnoseServer(const QString& accountServer) {
    const QUrl base = normalizeAccountServer(accountServer);
    if (!base.isValid() || base.host().isEmpty()
        || (base.scheme().toLower() != QLatin1String("http")
            && base.scheme().toLower() != QLatin1String("https"))) {
        m_diagnosticState = QStringLiteral("error");
        m_diagnosticMessage = QStringLiteral("账号服务器地址无效，请使用 http:// 或 https:// 地址");
        m_accountLatencyMs = -1;
        publishDiagnostics();
        return;
    }

    const quint64 generation = ++m_diagnosticGeneration;
    QElapsedTimer timer;
    timer.start();
    m_diagnosticState = QStringLiteral("checking");
    m_diagnosticMessage = QStringLiteral("正在检测账号服务器");
    m_accountLatencyMs = -1;
    publishDiagnostics();

    QNetworkRequest request{QUrl(base.toString(QUrl::RemovePath | QUrl::RemoveQuery
                                               | QUrl::RemoveFragment)
                                 + QStringLiteral("/api/health"))};
    request.setRawHeader("Accept", "application/json");
    QNetworkReply* reply = m_network.get(request);
    QTimer::singleShot(5000, reply, [reply]() {
        if (reply->isRunning()) {
            reply->setProperty("diagnosticTimedOut", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, timer]() mutable {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const bool timedOut = reply->property("diagnosticTimedOut").toBool();
        const QString networkError = reply->errorString();
        reply->deleteLater();
        if (generation != m_diagnosticGeneration) return;
        if (statusCode == 200 && document.isObject()
            && document.object().value(QStringLiteral("status")).toString() == QLatin1String("ok")) {
            m_diagnosticState = QStringLiteral("healthy");
            m_accountLatencyMs = static_cast<int>(timer.elapsed());
            m_diagnosticMessage = QStringLiteral("账号服务器可达，身份服务正常");
        } else {
            m_diagnosticState = QStringLiteral("error");
            m_accountLatencyMs = -1;
            m_diagnosticMessage = timedOut
                ? QStringLiteral("账号服务器检测超时，请检查地址、防火墙或网络")
                : QStringLiteral("账号服务器不可用：%1").arg(networkError.isEmpty()
                    ? QStringLiteral("HTTP %1").arg(statusCode) : networkError);
        }
        publishDiagnostics();
    });
}

QUrl NetworkClient::normalizeAccountServer(const QString& input) const {
    QString value = input.trimmed();
    if (!value.contains(QStringLiteral("://"))) value.prepend(QStringLiteral("http://"));
    while (value.endsWith(QLatin1Char('/'))) value.chop(1);
    return QUrl(value);
}

void NetworkClient::login(const QString& accountServer, const QString& username,
                          const QString& password) {
    close();
    m_manualClose = false;
    const quint64 generation = ++m_loginGeneration;
    const QUrl base = normalizeAccountServer(accountServer);
    if (!base.isValid() || base.host().isEmpty()
        || (base.scheme().toLower() != QLatin1String("http")
            && base.scheme().toLower() != QLatin1String("https"))) {
        const QString message = QStringLiteral("服务器地址无效");
        setState(QStringLiteral("error"), message);
        emit fatalError(message);
        return;
    }
    m_accountServer = base.toString(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    diagnoseServer(m_accountServer);
    QNetworkRequest request{QUrl(m_accountServer + QStringLiteral("/api/client/login"))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QJsonObject body{{QStringLiteral("username"), username.trimmed()},
                           {QStringLiteral("password"), password}};
    setState(QStringLiteral("loggingIn"), QStringLiteral("正在验证账号"));
    QNetworkReply* reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QTimer::singleShot(10000, reply, [reply]() {
        if (reply->isRunning()) {
            reply->setProperty("loginTimedOut", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation]() {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const QString networkError = reply->errorString();
        reply->deleteLater();
        if (m_manualClose || generation != m_loginGeneration) return;
        if (statusCode != 200 || !document.isObject()) {
            QString message = document.object().value(QStringLiteral("detail")).toString();
            if (reply->property("loginTimedOut").toBool()) message = QStringLiteral("连接账号服务器超时");
            if (message.isEmpty()) message = networkError;
            setState(QStringLiteral("error"), message);
            emit fatalError(message);
            return;
        }
        const QJsonObject response = document.object();
        m_token = response.value(QStringLiteral("token")).toString();
        m_webSocketUrl = QUrl(response.value(QStringLiteral("gameWebSocketUrl")).toString());
        const QString webSocketScheme = m_webSocketUrl.scheme().toLower();
        if (m_token.isEmpty() || !m_webSocketUrl.isValid() || m_webSocketUrl.host().isEmpty()
            || (webSocketScheme != QLatin1String("ws") && webSocketScheme != QLatin1String("wss"))) {
            const QString message = QStringLiteral("服务器返回的联网配置无效");
            setState(QStringLiteral("error"), message);
            emit fatalError(message);
            return;
        }
        m_reconnectAttempt = 0;
        openWebSocket();
    });
}

void NetworkClient::close() {
    ++m_loginGeneration;
    m_manualClose = true;
    m_reconnectTimer.stop();
    m_latencyTimer.stop();
    m_connectTimer.stop();
    m_authTimer.stop();
    m_pingPending = false;
    m_gameLatencyMs = -1;
    if (!m_token.isEmpty() && !m_accountServer.isEmpty()) {
        QNetworkRequest request{QUrl(m_accountServer + QStringLiteral("/api/client/logout"))};
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + m_token.toUtf8());
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QNetworkReply* reply = m_network.post(request, QByteArrayLiteral("{}"));
        QTimer::singleShot(5000, reply, [reply]() {
            if (reply->isRunning()) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
    m_token.clear();
    m_stateStore.reset();
    m_authenticated = false;
    m_identityPublished = false;
    m_welcomePayload = {};
    clearPendingCommands(QStringLiteral("canceled"), QStringLiteral("联网会话已关闭"));
    if (m_socket.state() != QAbstractSocket::UnconnectedState) m_socket.abort();
    setState(QStringLiteral("disconnected"), QStringLiteral("未连接"));
}

void NetworkClient::openWebSocket() {
    if (m_manualClose || m_token.isEmpty()) return;
    setState(m_reconnectAttempt == 0 ? QStringLiteral("connecting") : QStringLiteral("reconnecting"),
             m_reconnectAttempt == 0 ? QStringLiteral("正在连接推演服务器") : QStringLiteral("正在重新连接推演服务器"));
    m_socket.open(m_webSocketUrl);
    m_connectTimer.start(8000);
}

void NetworkClient::onWebSocketConnected() {
    m_connectTimer.stop();
    const quint64 resumeSequence = m_stateStore.lastSequence();
    const qint64 resumeStateRevision = m_stateStore.stateRevision();
    m_stateStore.beginConnection();
    m_identityPublished = false;
    m_welcomePayload = {};
    setState(QStringLiteral("authenticating"), QStringLiteral("正在进入推演室"));
    sendEnvelope(QStringLiteral("auth"),
                 QJsonObject{{QStringLiteral("token"), m_token},
                             {QStringLiteral("resumeSequence"),
                              static_cast<qint64>(resumeSequence)},
                             {QStringLiteral("resumeStateRevision"), resumeStateRevision}});
    m_authTimer.start(8000);
}

void NetworkClient::onWebSocketDisconnected() {
    m_connectTimer.stop();
    m_authTimer.stop();
    m_latencyTimer.stop();
    m_pingPending = false;
    m_gameLatencyMs = -1;
    const bool wasAuthenticated = m_authenticated;
    m_authenticated = false;
    if (wasAuthenticated) {
        m_diagnosticMessage = QStringLiteral("推演服务器连接已断开，正在尝试恢复");
        publishDiagnostics();
    }
    if (m_manualClose || m_token.isEmpty()) return;
    scheduleReconnect();
}

void NetworkClient::sendLatencyProbe() {
    if (!m_authenticated || m_socket.state() != QAbstractSocket::ConnectedState || m_pingPending) return;
    m_pingPending = true;
    m_pingSentAtMs = m_monotonic.elapsed();
    sendEnvelope(QStringLiteral("ping"), QJsonObject{});
    QTimer::singleShot(5000, this, [this, sentAt = m_pingSentAtMs]() {
        if (m_pingPending && m_pingSentAtMs == sentAt) {
            m_pingPending = false;
            m_gameLatencyMs = -1;
            m_diagnosticMessage = QStringLiteral("推演服务器延迟检测超时");
            publishDiagnostics();
        }
    });
}

void NetworkClient::scheduleReconnect() {
    static const int delays[] = {1000, 2000, 4000, 8000, 15000, 30000};
    const int index = std::min(m_reconnectAttempt, 5);
    const int baseDelay = delays[index];
    const int jitter = QRandomGenerator::global()->bounded(baseDelay / 5 + 1);
    const int delay = baseDelay + jitter;
    ++m_reconnectAttempt;
    setState(QStringLiteral("reconnecting"),
             QStringLiteral("连接已中断，%1 秒后重试").arg((delay + 999) / 1000));
    m_reconnectTimer.start(delay);
}

void NetworkClient::onTextMessage(const QString& text) {
    if (text.toUtf8().size() > Protocol::MaxServerMessageBytes) {
        const QString message = QStringLiteral("推演服务器返回的消息超过允许大小");
        m_manualClose = true;
        m_token.clear();
        m_socket.close();
        setState(QStringLiteral("error"), message);
        emit fatalError(message);
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (!document.isObject()) {
        const QString message = QStringLiteral("推演服务器返回了无效协议消息");
        m_manualClose = true;
        m_token.clear();
        m_diagnosticState = QStringLiteral("error");
        m_diagnosticMessage = message;
        publishDiagnostics();
        setState(QStringLiteral("error"), message);
        m_socket.close();
        emit fatalError(message);
        return;
    }
    const ClientStateStore::Result result = m_stateStore.applyEnvelope(document.object());
    if (result.disposition == ClientStateStore::Disposition::Fatal) {
        const QString message = result.message.isEmpty()
            ? QStringLiteral("服务器返回了无效协议消息") : result.message;
        m_manualClose = true;
        m_token.clear();
        m_socket.close();
        setState(QStringLiteral("error"), message);
        emit fatalError(message);
        return;
    }
    if (result.disposition == ClientStateStore::Disposition::Ignored) return;
    if (result.disposition == ClientStateStore::Disposition::ResyncRequired) {
        m_diagnosticState = QStringLiteral("error");
        m_diagnosticMessage = QStringLiteral("状态流不连续，正在重新同步: %1")
                                  .arg(result.message);
        publishDiagnostics();
        requestResync();
        return;
    }
    const QString& type = result.type;
    const QJsonObject& payload = result.payload;
    if (type == QLatin1String("welcome")) {
        m_authTimer.stop();
        m_reconnectAttempt = 0;
        m_authenticated = true;
        m_welcomePayload = payload;
        setState(QStringLiteral("synchronizing"), QStringLiteral("身份已确认，正在同步推演状态"));
        emit chatHistoryReceived(payload.value(QStringLiteral("chatHistory")).toArray());
    } else if (type == QLatin1String("snapshot")) {
        if (!m_authenticated) {
            const QString message = QStringLiteral("服务器在认证完成前发送了状态快照");
            m_manualClose = true;
            m_token.clear();
            m_socket.close();
            setState(QStringLiteral("error"), message);
            emit fatalError(message);
            return;
        }
        if (!m_identityPublished) {
            emit authenticated(m_welcomePayload.value(QStringLiteral("username")).toString(),
                               m_welcomePayload.value(QStringLiteral("displayName")).toString(),
                               m_welcomePayload.value(QStringLiteral("role")).toString(),
                               m_accountServer);
            m_identityPublished = true;
        }
        setState(QStringLiteral("connected"), QStringLiteral("已连接并同步"));
        m_diagnosticState = QStringLiteral("healthy");
        m_diagnosticMessage = QStringLiteral("账号服务与推演服务器连接正常");
        m_latencyTimer.start();
        sendLatencyProbe();
        publishDiagnostics();
        emit snapshotReceived(m_stateStore.snapshot());
        retransmitPendingCommands();
    } else if (type == QLatin1String("delta")) {
        emit snapshotReceived(m_stateStore.snapshot());
    } else if (type == QLatin1String("chat")) {
        emit chatReceived(payload);
    } else if (type == QLatin1String("event")) {
        emit eventReceived(payload);
    } else if (type == QLatin1String("pong")) {
        if (m_pingPending) {
            m_gameLatencyMs = static_cast<int>(m_monotonic.elapsed() - m_pingSentAtMs);
            m_pingPending = false;
            m_diagnosticState = QStringLiteral("healthy");
            m_diagnosticMessage = QStringLiteral("账号服务与推演服务器连接正常");
            publishDiagnostics();
        }
    } else if (type == QLatin1String("error")) {
        const QString message = payload.value(QStringLiteral("message")).toString(QStringLiteral("服务器拒绝了请求"));
        const QString code = payload.value(QStringLiteral("code")).toString();
        if (code == QLatin1String("SESSION_REVOKED") || code == QLatin1String("INVALID_TOKEN")) {
            const bool hadSession = m_authenticated;
            m_manualClose = true;
            m_token.clear();
            m_socket.close();
            setState(QStringLiteral("error"), message);
            if (hadSession) emit authenticationLost(message);
            else emit fatalError(message);
        } else {
            emit commandRejected(message);
        }
    } else if (type == QLatin1String("commandResult")) {
        const QString commandId = payload.value(QStringLiteral("commandId")).toString();
        if (!m_pendingCommands.contains(commandId)) return;
        const PendingCommand pending = m_pendingCommands.take(commandId);
        const bool accepted = payload.value(QStringLiteral("accepted")).toBool();
        const QString message = payload.value(QStringLiteral("message")).toString();
        emit commandStatusChanged(commandId, pending.action,
                                  accepted ? QStringLiteral("accepted")
                                           : QStringLiteral("rejected"),
                                  payload.value(QStringLiteral("code")).toString(), message);
        if (!accepted) emit commandRejected(message);
    }
}

void NetworkClient::requestResync() {
    if (m_socket.state() != QAbstractSocket::ConnectedState) return;
    sendEnvelope(QStringLiteral("resyncRequest"),
                 QJsonObject{{QStringLiteral("lastSequence"),
                              static_cast<qint64>(m_stateStore.lastSequence())},
                             {QStringLiteral("stateRevision"),
                              m_stateStore.stateRevision()}});
}

void NetworkClient::setState(const QString& state, const QString& message) {
    m_state = state;
    emit stateChanged(state, message);
}

bool NetworkClient::sendEnvelope(const QString& type, const QJsonObject& payload) {
    if (m_socket.state() != QAbstractSocket::ConnectedState) return false;
    const QJsonObject envelope = Protocol::makeClientEnvelope(
        type, QUuid::createUuid().toString(QUuid::WithoutBraces), payload);
    const QByteArray encoded = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    const Protocol::ValidationResult validation = Protocol::validateClientEnvelope(envelope);
    if (!validation.valid || encoded.size() > Protocol::MaxMessageBytes) {
        const QString message = validation.valid
            ? QStringLiteral("待发送消息超过 256 KiB") : validation.message;
        emit commandRejected(message);
        return false;
    }
    m_socket.sendTextMessage(QString::fromUtf8(encoded));
    return true;
}

void NetworkClient::sendCommand(const QString& action, const QVariantMap& args) {
    if (!m_authenticated || state() != QLatin1String("connected")
        || m_stateStore.waitingForSnapshot() || m_stateStore.waitingForResync()) {
        emit commandRejected(QStringLiteral("推演状态尚未同步，暂时不能下达命令"));
        return;
    }
    if (m_pendingCommands.size() >= 128) {
        emit commandRejected(QStringLiteral("待确认命令过多，请等待服务器响应"));
        return;
    }
    const QString commandId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject jsonArgs = QJsonObject::fromVariantMap(args);
    const QJsonObject payload{{QStringLiteral("commandId"), commandId},
                              {QStringLiteral("action"), action},
                              {QStringLiteral("args"), jsonArgs}};
    const Protocol::ValidationResult validation =
        Protocol::validateClientPayload(QStringLiteral("command"), payload);
    if (!validation.valid) {
        emit commandStatusChanged(commandId, action, QStringLiteral("rejected"),
                                  validation.code, validation.message);
        emit commandRejected(validation.message);
        return;
    }
    m_pendingCommands.insert(commandId, PendingCommand{action, jsonArgs});
    emit commandStatusChanged(commandId, action, QStringLiteral("queued"), {},
                              QStringLiteral("命令已进入发送队列"));
    sendPendingCommand(commandId, false);
}

void NetworkClient::sendPendingCommand(const QString& commandId, bool retry) {
    if (!m_pendingCommands.contains(commandId) || !m_authenticated
        || state() != QLatin1String("connected") || m_stateStore.waitingForSnapshot()
        || m_stateStore.waitingForResync()) {
        return;
    }
    PendingCommand& pending = m_pendingCommands[commandId];
    const QJsonObject payload{{QStringLiteral("commandId"), commandId},
                              {QStringLiteral("action"), pending.action},
                              {QStringLiteral("args"), pending.args}};
    if (!sendEnvelope(QStringLiteral("command"), payload)) return;
    pending.lastSentAtMs = m_monotonic.elapsed();
    ++pending.attempts;
    emit commandStatusChanged(commandId, pending.action,
                              retry ? QStringLiteral("retrying")
                                    : QStringLiteral("pending"),
                              {}, retry ? QStringLiteral("正在重新确认命令结果")
                                        : QStringLiteral("命令已发送，等待服务器确认"));
}

void NetworkClient::retransmitPendingCommands() {
    const QStringList commandIds = m_pendingCommands.keys();
    for (const QString& commandId : commandIds) {
        sendPendingCommand(commandId, m_pendingCommands.value(commandId).attempts > 0);
    }
}

void NetworkClient::processPendingCommands() {
    if (m_pendingCommands.isEmpty() || !m_authenticated
        || state() != QLatin1String("connected") || m_stateStore.waitingForResync()) {
        return;
    }
    const qint64 now = m_monotonic.elapsed();
    const QStringList commandIds = m_pendingCommands.keys();
    for (const QString& commandId : commandIds) {
        if (!m_pendingCommands.contains(commandId)) continue;
        PendingCommand& pending = m_pendingCommands[commandId];
        pending.onlineWaitMs += m_commandTimer.interval();
        if (pending.onlineWaitMs >= 30000) {
            const PendingCommand timedOut = m_pendingCommands.take(commandId);
            const QString message = QStringLiteral("命令结果暂时未知，请以同步后的战场状态为准");
            emit commandStatusChanged(commandId, timedOut.action, QStringLiteral("unknown"),
                                      QStringLiteral("CLIENT_TIMEOUT"), message);
            emit commandRejected(message);
            continue;
        }
        if (pending.lastSentAtMs < 0 || now - pending.lastSentAtMs >= 5000) {
            sendPendingCommand(commandId, pending.attempts > 0);
        }
    }
}

void NetworkClient::clearPendingCommands(const QString& status, const QString& message) {
    const auto pending = m_pendingCommands;
    m_pendingCommands.clear();
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        emit commandStatusChanged(it.key(), it.value().action, status,
                                  QStringLiteral("CLIENT_CANCELED"), message);
    }
}

void NetworkClient::sendControl(const QString& action, double speed) {
    if (!m_authenticated || state() != QLatin1String("connected")) {
        emit commandRejected(QStringLiteral("当前未连接，不能控制推演进程"));
        return;
    }
    QJsonObject payload{{QStringLiteral("action"), action}};
    if (speed >= 0.0) payload[QStringLiteral("speed")] = speed;
    sendEnvelope(QStringLiteral("control"), payload);
}

void NetworkClient::sendReady(bool ready) {
    if (!m_authenticated || state() != QLatin1String("connected")) {
        emit commandRejected(QStringLiteral("当前未连接，不能提交就绪状态"));
        return;
    }
    sendEnvelope(QStringLiteral("setReady"), QJsonObject{{QStringLiteral("ready"), ready}});
}

void NetworkClient::sendChat(const QString& text) {
    if (!m_authenticated || state() != QLatin1String("connected")) {
        emit commandRejected(QStringLiteral("当前未连接，不能发送消息"));
        return;
    }
    sendEnvelope(QStringLiteral("chat"), QJsonObject{{QStringLiteral("text"), text}});
}

void NetworkClient::sendScenarioUpsert(const QJsonObject& unit) {
    if (!m_authenticated || state() != QLatin1String("connected")) {
        emit commandRejected(QStringLiteral("当前未连接，不能修改阵容"));
        return;
    }
    sendEnvelope(QStringLiteral("scenarioUpsert"), QJsonObject{{QStringLiteral("unit"), unit}});
}

void NetworkClient::sendScenarioRemove(const QString& unitId) {
    if (!m_authenticated || state() != QLatin1String("connected")) {
        emit commandRejected(QStringLiteral("当前未连接，不能修改阵容"));
        return;
    }
    sendEnvelope(QStringLiteral("scenarioRemove"), QJsonObject{{QStringLiteral("unitId"), unitId}});
}

void NetworkClient::sendScenarioReplace(const QJsonObject& scenario) {
    if (!m_authenticated || state() != QLatin1String("connected")) {
        emit commandRejected(QStringLiteral("当前未连接，不能替换场景"));
        return;
    }
    sendEnvelope(QStringLiteral("scenarioReplace"), QJsonObject{{QStringLiteral("scenario"), scenario}});
}

} // namespace gbr
