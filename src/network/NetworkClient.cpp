#include "NetworkClient.h"

#include "protocol/Protocol.h"

#include <QJsonDocument>
#include <QEventLoop>
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
    connect(&m_commandTimer, &QTimer::timeout, this, &NetworkClient::processPendingIntelRequests);
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
    m_protocolVersion = Protocol::Version;
    m_schemaVersion = Protocol::SchemaVersion;
    m_legacyFallbackAttempted = false;
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
    m_loginReply = reply;
    const QString loginAccountServer = m_accountServer;
    QTimer::singleShot(10000, reply, [reply]() {
        if (reply->isRunning()) {
            reply->setProperty("loginTimedOut", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, loginAccountServer]() {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const QString networkError = reply->errorString();
        if (m_loginReply == reply) m_loginReply = nullptr;
        reply->deleteLater();
        if (statusCode != 200 || !document.isObject()) {
            if (generation != m_loginGeneration) return;
            QString message = document.object().value(QStringLiteral("detail")).toString();
            if (reply->property("loginTimedOut").toBool()) message = QStringLiteral("连接账号服务器超时");
            if (message == QLatin1String("USER_ALREADY_ONLINE")) {
                message = QStringLiteral("该账号已在另一客户端登录，请先退出原客户端后再试");
            }
            if (message.isEmpty()) message = networkError;
            setState(QStringLiteral("error"), message);
            emit fatalError(message);
            return;
        }
        const QJsonObject response = document.object();
        const QString responseToken = response.value(QStringLiteral("token")).toString();
        if (generation != m_loginGeneration) {
            if (statusCode == 200 && !responseToken.isEmpty() && !loginAccountServer.isEmpty()) {
                QNetworkRequest request{QUrl(loginAccountServer + QStringLiteral("/api/client/logout"))};
                request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + responseToken.toUtf8());
                request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
                QNetworkReply* cleanupReply = m_network.post(request, QByteArrayLiteral("{}"));
                QTimer::singleShot(5000, cleanupReply, [cleanupReply]() {
                    if (cleanupReply->isRunning()) cleanupReply->abort();
                });
                connect(cleanupReply, &QNetworkReply::finished, cleanupReply, &QObject::deleteLater);
            }
            return;
        }
        m_loginTokenCandidate = responseToken;
        m_token = m_loginTokenCandidate;
        m_webSocketUrl = QUrl(response.value(QStringLiteral("gameWebSocketUrl")).toString());
        const QString webSocketScheme = m_webSocketUrl.scheme().toLower();
        if (m_token.isEmpty() || !m_webSocketUrl.isValid() || m_webSocketUrl.host().isEmpty()
            || (webSocketScheme != QLatin1String("ws") && webSocketScheme != QLatin1String("wss"))) {
            const QString message = QStringLiteral("服务器返回的联网配置无效");
            setState(QStringLiteral("error"), message);
            emit fatalError(message);
            return;
        }
        if (m_manualClose) return;
        m_reconnectAttempt = 0;
        openWebSocket();
    });
}

void NetworkClient::close(bool waitForLogout) {
    m_manualClose = true;
    if (waitForLogout && m_loginReply && m_loginReply->isRunning()) {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(m_loginReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(1500);
        loop.exec();
        if (m_loginReply && m_loginReply->isRunning()) m_loginReply->abort();
    }
    ++m_loginGeneration;
    m_reconnectTimer.stop();
    m_latencyTimer.stop();
    m_connectTimer.stop();
    m_authTimer.stop();
    m_pingPending = false;
    m_gameLatencyMs = -1;
    QString logoutToken = m_token.isEmpty() ? m_loginTokenCandidate : m_token;
    if (waitForLogout && logoutToken.isEmpty() && m_loginReply && !m_loginReply->isRunning()) {
        const QJsonDocument document = QJsonDocument::fromJson(m_loginReply->readAll());
        logoutToken = document.object().value(QStringLiteral("token")).toString();
    }
    if (!logoutToken.isEmpty() && !m_accountServer.isEmpty() && !m_logoutReply) {
        QNetworkRequest request{QUrl(m_accountServer + QStringLiteral("/api/client/logout"))};
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + logoutToken.toUtf8());
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QNetworkReply* reply = m_network.post(request, QByteArrayLiteral("{}"));
        m_logoutReply = reply;
        QTimer::singleShot(5000, reply, [reply]() {
            if (reply->isRunning()) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            if (m_logoutReply == reply) m_logoutReply = nullptr;
            reply->deleteLater();
        });
    }
    if (waitForLogout && m_logoutReply && m_logoutReply->isRunning()) {
        const QPointer<QNetworkReply> pendingLogout = m_logoutReply;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(pendingLogout, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(3000);
        loop.exec();
        if (pendingLogout && pendingLogout->isRunning()) pendingLogout->abort();
    }
    m_token.clear();
    m_loginTokenCandidate.clear();
    m_ddsTicket.clear();
    m_dataPlane.stop();
    m_stateStore.reset();
    m_authenticated = false;
    m_identityPublished = false;
    m_currentSeatId.clear();
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
    QJsonObject auth{{QStringLiteral("token"), m_token}};
    // The v4 server contract did not require reconnect cursors. Omitting the
    // optional fields keeps auth compatible with strict older validators.
    if (m_schemaVersion != Protocol::LegacySchemaVersion) {
        auth.insert(QStringLiteral("resumeSequence"), static_cast<qint64>(resumeSequence));
        auth.insert(QStringLiteral("resumeStateRevision"), resumeStateRevision);
    }
    sendEnvelope(QStringLiteral("auth"), auth);
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
    if (!wasAuthenticated && m_protocolVersion != Protocol::LegacyVersion
        && !m_legacyFallbackAttempted) {
        fallbackToLegacyProtocol();
    }
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
    if (text.size() > Protocol::MaxServerMessageBytes / 3) {
        const QString message = QStringLiteral("推演服务器返回的消息超过允许大小");
        m_manualClose = true;
        m_token.clear();
        m_socket.close();
        setState(QStringLiteral("error"), message);
        emit fatalError(message);
        return;
    }
    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() > Protocol::MaxServerMessageBytes) {
        const QString message = QStringLiteral("推演服务器返回的消息超过允许大小");
        m_manualClose = true;
        m_token.clear();
        m_socket.close();
        setState(QStringLiteral("error"), message);
        emit fatalError(message);
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(utf8, &parseError);
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
    const QJsonObject envelope = document.object();
    const int incomingProtocol = envelope.value(QStringLiteral("protocolVersion")).toInt();
    const int incomingSchema = envelope.value(QStringLiteral("schemaVersion")).toInt();
    if (Protocol::isSupportedWireVersion(incomingProtocol, incomingSchema)) {
        if (m_authenticated
            && (incomingProtocol != m_protocolVersion || incomingSchema != m_schemaVersion)) {
            reconnectWithWireVersion(
                incomingProtocol, incomingSchema,
                QStringLiteral("服务器在同一连接内切换了协议版本，正在重新协商"));
            return;
        }
        m_protocolVersion = incomingProtocol;
        m_schemaVersion = incomingSchema;
        m_legacyFallbackAttempted = incomingProtocol == Protocol::LegacyVersion;
    }
    const ClientStateStore::Result result = m_stateStore.applyEnvelope(envelope);
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
        m_ddsTicket = payload.value(QStringLiteral("ddsTicket")).toString();
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
                               m_welcomePayload.value(QStringLiteral("seatId")).toString(),
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
        retransmitPendingIntelRequests();
    } else if (type == QLatin1String("delta")) {
        QStringList changedUnitIds;
        for (const QJsonValue& value
             : payload.value(QStringLiteral("changedUnitIds")).toArray()) {
            changedUnitIds.append(value.toString());
        }
        emit deltaSnapshotReceived(m_stateStore.snapshot(), changedUnitIds);
    } else if (type == QLatin1String("chat")) {
        emit chatReceived(payload);
    } else if (type == QLatin1String("roomDirectory")) {
        emit roomDirectoryReceived(payload.value(QStringLiteral("rooms")).toArray());
    } else if (type == QLatin1String("seatState")) {
        emit seatStateReceived(payload);
        m_currentSeatId = payload.value(QStringLiteral("yourSeatId")).toString();
        m_dataPlane.stop();
    } else if (type == QLatin1String("deploymentPrompt")) {
        emit deploymentPromptReceived(payload);
    } else if (type == QLatin1String("intelShare")) {
        emit intelShareReceived(payload);
    } else if (type == QLatin1String("intelHistoryPage")) {
        emit intelHistoryPageReceived(payload);
    } else if (type == QLatin1String("vmfEvent")) {
        emit vmfEventReceived(payload);
    } else if (type == QLatin1String("vmfTaskResult")) {
        emit vmfTaskResultReceived(payload);
    } else if (type == QLatin1String("vmfTrace")) {
        emit vmfTraceReceived(payload);
    } else if (type == QLatin1String("demoState")) {
        emit demoStateReceived(payload);
    } else if (type == QLatin1String("demoTrace")) {
        emit demoTraceReceived(payload);
    } else if (type == QLatin1String("demoResult")) {
        emit demoResultReceived(payload);
    } else if (type == QLatin1String("demoError")) {
        emit demoErrorReceived(payload);
        emit commandRejected(payload.value(QStringLiteral("message")).toString());
    } else if (type == QLatin1String("event")) {
        Protocol::TransferEventProjection transfer;
        if (Protocol::projectTransferEvent(payload, &transfer).valid) {
            emit transferEventReceived(payload);
        }
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
        const QString requestId = payload.value(QStringLiteral("requestId")).toString();
        if (!m_authenticated && (code == QLatin1String("PROTOCOL_MISMATCH")
                                 || code == QLatin1String("SCHEMA_MISMATCH"))) {
            fallbackToLegacyProtocol();
            return;
        }
        if (!requestId.isEmpty() && m_pendingCommands.contains(requestId)) {
            const PendingCommand pending = m_pendingCommands.take(requestId);
            emit commandStatusChanged(requestId, pending.action, QStringLiteral("rejected"),
                                      code, message);
            emit commandRejected(message);
            return;
        }
        if (!requestId.isEmpty() && m_pendingIntelRequests.contains(requestId)) {
            const PendingIntelRequest pending = m_pendingIntelRequests.take(requestId);
            emit commandStatusChanged(requestId, pending.action, QStringLiteral("rejected"),
                                      code, message);
            emit commandRejected(message);
            return;
        }
        if (code == QLatin1String("KICKED_BY_ADMIN")
            || code == QLatin1String("USER_KICKED_OFFLINE")) {
            const QString token = m_token;
            const bool hadSession = m_authenticated;
            m_manualClose = true;
            if (!token.isEmpty() && !m_accountServer.isEmpty()) {
                QNetworkRequest request{QUrl(m_accountServer + QStringLiteral("/api/client/logout"))};
                request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + token.toUtf8());
                request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
                QNetworkReply* reply = m_network.post(request, QByteArrayLiteral("{}"));
                QTimer::singleShot(5000, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
                connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
            }
            m_token.clear();
            m_dataPlane.stop();
            m_socket.close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("已被管理员移出房间"));
            setState(QStringLiteral("error"), message);
            if (hadSession) emit authenticationLost(message);
            else emit fatalError(message);
        } else if (code == QLatin1String("SESSION_REVOKED") || code == QLatin1String("INVALID_TOKEN")) {
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
        Protocol::CommandResultProjection commandResult;
        const Protocol::ValidationResult validation =
            Protocol::projectCommandResult(payload, &commandResult);
        if (!validation.valid) return;
        if (!m_pendingCommands.contains(commandResult.commandId)) {
            if (!m_pendingIntelRequests.contains(commandResult.commandId)) return;
            const PendingIntelRequest pending = m_pendingIntelRequests.take(commandResult.commandId);
            emit commandStatusChanged(commandResult.commandId, pending.action,
                                      commandResult.accepted ? QStringLiteral("accepted")
                                                             : QStringLiteral("rejected"),
                                      commandResult.code, commandResult.message);
            if (!commandResult.accepted) emit commandRejected(commandResult.message);
            return;
        }
        const PendingCommand pending = m_pendingCommands.take(commandResult.commandId);
        emit commandStatusChanged(commandResult.commandId, pending.action,
                                  commandResult.accepted ? QStringLiteral("accepted")
                                                         : QStringLiteral("rejected"),
                                  commandResult.code, commandResult.message);
        if (!commandResult.accepted) emit commandRejected(commandResult.message);
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

void NetworkClient::fallbackToLegacyProtocol() {
    if (m_protocolVersion == Protocol::Version) {
        reconnectWithWireVersion(Protocol::PreviousVersion, Protocol::PreviousSchemaVersion,
                                 QStringLiteral("服务器不支持当前协议，正在切换 v7 兼容模式"));
    } else if (m_protocolVersion == Protocol::PreviousVersion) {
        reconnectWithWireVersion(Protocol::OlderVersion, Protocol::OlderSchemaVersion,
                                 QStringLiteral("服务器版本较旧，正在切换 v6 兼容模式"));
    } else if (m_protocolVersion == Protocol::OlderVersion) {
        reconnectWithWireVersion(Protocol::LegacyVersion, Protocol::LegacySchemaVersion,
                                 QStringLiteral("服务器版本较旧，正在切换 v4 兼容模式"));
    }
}

void NetworkClient::reconnectWithWireVersion(int protocolVersion, int schemaVersion,
                                             const QString& message) {
    if (!Protocol::isSupportedWireVersion(protocolVersion, schemaVersion)) return;
    m_protocolVersion = protocolVersion;
    m_schemaVersion = schemaVersion;
    m_legacyFallbackAttempted = protocolVersion == Protocol::LegacyVersion;
    m_stateStore.reset();
    m_authTimer.stop();
    m_identityPublished = false;
    m_welcomePayload = {};
    m_diagnosticState = QStringLiteral("checking");
    m_diagnosticMessage = message;
    publishDiagnostics();
    if (m_socket.state() != QAbstractSocket::UnconnectedState) m_socket.abort();
}

void NetworkClient::setState(const QString& state, const QString& message) {
    m_state = state;
    emit stateChanged(state, message);
}

bool NetworkClient::sendEnvelope(const QString& type, const QJsonObject& payload) {
    return sendEnvelope(type, payload, QString());
}

bool NetworkClient::sendEnvelope(const QString& type, const QJsonObject& payload,
                                 const QString& messageId) {
    if (m_socket.state() != QAbstractSocket::ConnectedState) return false;
    const QJsonObject envelope = Protocol::makeClientEnvelopeForVersion(
        type, messageId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : messageId,
        payload, m_protocolVersion, m_schemaVersion);
    const QByteArray encoded = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    const Protocol::ValidationResult validation = Protocol::validateClientEnvelopeForVersion(
        envelope);
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
    if (!m_authenticated || m_socket.state() != QAbstractSocket::ConnectedState) {
        emit commandRejected(QStringLiteral("联网会话尚未建立"));
        return;
    }
    if (m_pendingCommands.size() >= 128) {
        emit commandRejected(QStringLiteral("待确认命令过多，请等待服务器响应"));
        return;
    }
    const QString commandId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject jsonArgs = QJsonObject::fromVariantMap(args);
    // During the initial sync the authoritative revision is not available
    // yet. Validate the command shape with a temporary positive revision; the
    // actual payload is built from the current snapshot when it is sent.
    const qint64 validationRevision = std::max<qint64>(1, m_stateStore.stateRevision());
    const QJsonObject payload{{QStringLiteral("commandId"), commandId},
                              {QStringLiteral("action"), action},
                              {QStringLiteral("stateRevision"), validationRevision},
                              {QStringLiteral("args"), jsonArgs}};
    const Protocol::ValidationResult validation =
        Protocol::validateClientPayload(QStringLiteral("command"), payload);
    if (!validation.valid) {
        emit commandStatusChanged(commandId, action, QStringLiteral("rejected"),
                                  validation.code, validation.message);
        emit commandRejected(validation.message);
        return;
    }
    m_pendingCommands.insert(commandId,
                             PendingCommand{action, jsonArgs, QStringLiteral("command")});
    emit commandStatusChanged(commandId, action, QStringLiteral("queued"), {},
                              m_stateStore.waitingForSnapshot() || m_stateStore.waitingForResync()
                                  ? QStringLiteral("状态同步完成后发送命令")
                                  : QStringLiteral("命令已进入发送队列"));
    sendPendingCommand(commandId, false);
}

void NetworkClient::sendUnitOrder(const QString& unitId, const QString& text) {
    sendCommand(QStringLiteral("unitOrder"),
                QVariantMap{{QStringLiteral("unitId"), unitId},
                            {QStringLiteral("text"), text.trimmed()}});
}

void NetworkClient::sendPendingCommand(const QString& commandId, bool retry) {
    if (!m_pendingCommands.contains(commandId) || !m_authenticated
        || state() != QLatin1String("connected")) {
        return;
    }
    PendingCommand& pending = m_pendingCommands[commandId];
    QJsonObject payload = pending.args;
    if (pending.wireType == QLatin1String("command")) {
        if (m_stateStore.waitingForSnapshot() || m_stateStore.waitingForResync()
            || m_stateStore.stateRevision() <= 0) {
            return;
        }
        payload = QJsonObject{{QStringLiteral("commandId"), commandId},
                              {QStringLiteral("action"), pending.action},
                              {QStringLiteral("stateRevision"), m_stateStore.stateRevision()},
                              {QStringLiteral("args"), pending.args}};
    }
    if (!sendEnvelope(pending.wireType, payload, commandId)) return;
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

void NetworkClient::sendPendingIntelRequest(const QString& requestId, bool retry) {
    if (!m_pendingIntelRequests.contains(requestId) || !m_authenticated
        || state() != QLatin1String("connected") || m_stateStore.waitingForSnapshot()
        || m_stateStore.waitingForResync()) {
        return;
    }
    PendingIntelRequest& pending = m_pendingIntelRequests[requestId];
    if (!sendEnvelope(pending.type, pending.payload, requestId)) return;
    pending.lastSentAtMs = m_monotonic.elapsed();
    ++pending.attempts;
    emit commandStatusChanged(requestId, pending.action,
                              retry ? QStringLiteral("retrying") : QStringLiteral("pending"),
                              {}, retry ? QStringLiteral("正在重新确认情报请求")
                                        : QStringLiteral("情报请求已提交，等待服务器确认"));
}

void NetworkClient::retransmitPendingIntelRequests() {
    const QStringList requestIds = m_pendingIntelRequests.keys();
    for (const QString& requestId : requestIds) {
        sendPendingIntelRequest(requestId,
                                m_pendingIntelRequests.value(requestId).attempts > 0);
    }
}

void NetworkClient::processPendingCommands() {
    if (m_pendingCommands.isEmpty() || !m_authenticated
        || state() != QLatin1String("connected")) {
        return;
    }
    const qint64 now = m_monotonic.elapsed();
    const QStringList commandIds = m_pendingCommands.keys();
    for (const QString& commandId : commandIds) {
        if (!m_pendingCommands.contains(commandId)) continue;
        PendingCommand& pending = m_pendingCommands[commandId];
        if (pending.wireType == QLatin1String("command")
            && (m_stateStore.waitingForSnapshot() || m_stateStore.waitingForResync())) {
            continue;
        }
        pending.onlineWaitMs += m_commandTimer.interval();
        const bool leavingRoom = pending.action == QLatin1String("leaveRoom");
        const int timeoutMs = leavingRoom ? 15000 : 30000;
        if (pending.onlineWaitMs >= timeoutMs) {
            const PendingCommand timedOut = m_pendingCommands.take(commandId);
            const QString message = leavingRoom
                ? QStringLiteral("退出请求未获服务器确认，当前房间状态保持不变")
                : QStringLiteral("命令结果暂时未知，请以同步后的战场状态为准");
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

void NetworkClient::processPendingIntelRequests() {
    if (m_pendingIntelRequests.isEmpty() || !m_authenticated
        || state() != QLatin1String("connected") || m_stateStore.waitingForSnapshot()
        || m_stateStore.waitingForResync()) {
        return;
    }
    const qint64 now = m_monotonic.elapsed();
    const QStringList requestIds = m_pendingIntelRequests.keys();
    for (const QString& requestId : requestIds) {
        if (!m_pendingIntelRequests.contains(requestId)) continue;
        PendingIntelRequest& pending = m_pendingIntelRequests[requestId];
        pending.onlineWaitMs += m_commandTimer.interval();
        if (pending.onlineWaitMs >= 30000) {
            const PendingIntelRequest timedOut = m_pendingIntelRequests.take(requestId);
            const QString message = QStringLiteral("情报请求结果暂时未知，请以同步后的台账为准");
            emit commandStatusChanged(requestId, timedOut.action, QStringLiteral("unknown"),
                                      QStringLiteral("CLIENT_TIMEOUT"), message);
            emit commandRejected(message);
            continue;
        }
        if (pending.lastSentAtMs < 0 || now - pending.lastSentAtMs >= 5000) {
            sendPendingIntelRequest(requestId, pending.attempts > 0);
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
    const auto pendingIntel = m_pendingIntelRequests;
    m_pendingIntelRequests.clear();
    for (auto it = pendingIntel.cbegin(); it != pendingIntel.cend(); ++it) {
        emit commandStatusChanged(it.key(), it.value().action, status,
                                  QStringLiteral("CLIENT_CANCELED"), message);
    }
}

void NetworkClient::sendControl(const QString& action, double speed) {
    if (!m_authenticated || state() != QLatin1String("connected")
        || m_stateStore.waitingForSnapshot() || m_stateStore.waitingForResync()) {
        emit commandRejected(QStringLiteral("推演状态尚未同步，暂时不能控制推演进程"));
        return;
    }
    QJsonObject payload{{QStringLiteral("action"), action}};
    if (speed >= 0.0) payload[QStringLiteral("speed")] = speed;
    sendEnvelope(QStringLiteral("control"), payload);
}

void NetworkClient::sendReady(bool ready) {
    if (!m_authenticated || state() != QLatin1String("connected")
        || m_stateStore.waitingForSnapshot() || m_stateStore.waitingForResync()) {
        emit commandRejected(QStringLiteral("推演状态尚未同步，暂时不能提交就绪状态"));
        return;
    }
    sendEnvelope(QStringLiteral("seatReady"), QJsonObject{{QStringLiteral("ready"), ready}});
}

void NetworkClient::sendSimple(const QString& type, const QJsonObject& payload) {
    if (!m_authenticated || m_socket.state() != QAbstractSocket::ConnectedState) {
        emit commandRejected(QStringLiteral("联网会话尚未建立"));
        return;
    }
    sendEnvelope(type, payload);
}

void NetworkClient::requestRooms() { sendSimple(QStringLiteral("roomList"), QJsonObject{}); }

void NetworkClient::joinRoom(const QString& roomId) {
    sendSimple(QStringLiteral("joinRoom"), QJsonObject{{QStringLiteral("roomId"), roomId}});
}

void NetworkClient::observeRoom(const QString& roomId) {
    sendSimple(QStringLiteral("joinRoom"),
               QJsonObject{{QStringLiteral("roomId"), roomId},
                           {QStringLiteral("asObserver"), true}});
}

void NetworkClient::claimSeat(const QString& seatId) {
    sendSimple(QStringLiteral("claimSeat"), QJsonObject{{QStringLiteral("seatId"), seatId}});
}

void NetworkClient::approveSeatTransfer(const QString& seatId, qint64 userId,
                                        qint64 requestedRevision) {
    sendSimple(QStringLiteral("claimSeat"),
               QJsonObject{{QStringLiteral("seatId"), seatId},
                           {QStringLiteral("approveUserId"), userId},
                           {QStringLiteral("requestedRevision"), requestedRevision}});
}

void NetworkClient::rejectSeatTransfer(const QString& seatId, qint64 userId,
                                       qint64 requestedRevision) {
    sendSimple(QStringLiteral("claimSeat"),
               QJsonObject{{QStringLiteral("seatId"), seatId},
                           {QStringLiteral("rejectUserId"), userId},
                           {QStringLiteral("requestedRevision"), requestedRevision}});
}

void NetworkClient::leaveRoom() {
    const QString commandId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto reject = [this, commandId](const QString& code, const QString& message) {
        emit commandStatusChanged(commandId, QStringLiteral("leaveRoom"),
                                  QStringLiteral("rejected"), code, message);
        emit commandRejected(message);
    };
    if (!m_authenticated || m_socket.state() != QAbstractSocket::ConnectedState) {
        reject(QStringLiteral("CLIENT_NOT_CONNECTED"),
               QStringLiteral("联网会话尚未建立"));
        return;
    }
    if (m_pendingCommands.size() >= 128) {
        reject(QStringLiteral("CLIENT_BACKPRESSURE"),
               QStringLiteral("待确认命令过多，请等待服务器响应"));
        return;
    }
    m_pendingCommands.insert(
        commandId,
        PendingCommand{QStringLiteral("leaveRoom"), {}, QStringLiteral("leaveRoom")});
    emit commandStatusChanged(commandId, QStringLiteral("leaveRoom"),
                              QStringLiteral("queued"), {},
                              QStringLiteral("退出请求已进入发送队列"));
    sendPendingCommand(commandId, false);
}

void NetworkClient::sendSeatReady(bool ready) {
    sendSimple(QStringLiteral("seatReady"), QJsonObject{{QStringLiteral("ready"), ready}});
}

void NetworkClient::sendDeployment(const QString& unitId, const QVariantMap& position) {
    sendSimple(QStringLiteral("deployment"), QJsonObject{{QStringLiteral("unitId"), unitId},
                                                          {QStringLiteral("position"), QJsonObject::fromVariantMap(position)}});
}

void NetworkClient::requestRedeploy() { sendSimple(QStringLiteral("requestRedeploy"), QJsonObject{}); }

void NetworkClient::redeploy(const QString& seatId) {
    sendSimple(QStringLiteral("redeploy"),
               QJsonObject{{QStringLiteral("seatId"), seatId}});
}

void NetworkClient::sendUnitName(const QString& unitName) {
    sendSimple(QStringLiteral("setUnitName"), QJsonObject{{QStringLiteral("unitName"), unitName}});
}

QString NetworkClient::sendIntelRequest(const QString& type, const QString& action,
                                        const QJsonObject& payload) {
    if (!m_authenticated || m_socket.state() != QAbstractSocket::ConnectedState) {
        emit commandRejected(QStringLiteral("联网会话尚未建立"));
        return {};
    }
    if (m_schemaVersion == Protocol::LegacySchemaVersion) {
        emit commandRejected(QStringLiteral("当前推演服务器版本不支持新版情报台账"));
        return {};
    }
    if (m_pendingIntelRequests.size() >= 128) {
        emit commandRejected(QStringLiteral("待确认情报请求过多，请等待服务器响应"));
        return {};
    }
    const Protocol::ValidationResult validation =
        Protocol::validateClientPayloadForVersion(type, payload, m_schemaVersion);
    if (!validation.valid) {
        emit commandRejected(validation.message);
        return {};
    }
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pendingIntelRequests.insert(requestId,
                                  PendingIntelRequest{type, action, payload});
    sendPendingIntelRequest(requestId, false);
    return requestId;
}

QString NetworkClient::shareIntel(const QString& intelId, const QStringList& recipientSeatIds,
                                  const QString& note) {
    QJsonArray recipients;
    for (const QString& seatId : recipientSeatIds) recipients.append(seatId);
    return sendIntelRequest(QStringLiteral("shareIntel"), QStringLiteral("shareIntel"),
                            QJsonObject{{QStringLiteral("intelId"), intelId},
                                        {QStringLiteral("recipientSeatIds"), recipients},
                                        {QStringLiteral("note"), note}});
}

QString NetworkClient::createIntelReport(const QVariantMap& position, const QString& type,
                                         const QString& title, const QString& note) {
    return sendIntelRequest(QStringLiteral("createIntelReport"),
                            QStringLiteral("createIntelReport"),
                            QJsonObject{{QStringLiteral("position"), QJsonObject::fromVariantMap(position)},
                                        {QStringLiteral("type"), type},
                                        {QStringLiteral("title"), title},
                                        {QStringLiteral("note"), note}});
}

QString NetworkClient::requestIntelHistory(const QVariantMap& query) {
    return sendIntelRequest(QStringLiteral("requestIntelHistory"),
                            QStringLiteral("requestIntelHistory"),
                            QJsonObject::fromVariantMap(query));
}

QString NetworkClient::sendVmfMessage(const QJsonObject& message) {
    const QString requestId = QStringLiteral("vmf-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!m_authenticated || m_socket.state() != QAbstractSocket::ConnectedState) {
        emit commandRejected(QStringLiteral("联网会话尚未建立"));
        return {};
    }
    if (!sendEnvelope(QString::fromLatin1(Protocol::VmfMessage), message, requestId)) {
        return {};
    }
    return requestId;
}

QString NetworkClient::sendVmfTaskCommand(const QJsonObject& command) {
    const QString requestId = command.value(QStringLiteral("requestId")).toString().isEmpty()
        ? QStringLiteral("vmf-task-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
        : command.value(QStringLiteral("requestId")).toString();
    QJsonObject payload = command;
    payload.insert(QStringLiteral("requestId"), requestId);
    if (!sendEnvelope(QString::fromLatin1(Protocol::VmfTaskCommandMessage), payload, requestId)) {
        return {};
    }
    return requestId;
}

QString NetworkClient::sendDemoAction(const QJsonObject& command) {
    const QString requestId = command.value(QStringLiteral("requestId")).toString().isEmpty()
        ? QStringLiteral("demo-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
        : command.value(QStringLiteral("requestId")).toString();
    QJsonObject payload = command;
    payload.insert(QStringLiteral("requestId"), requestId);
    if (!sendEnvelope(QString::fromLatin1(Protocol::DemoActionMessage), payload, requestId)) {
        return {};
    }
    return requestId;
}

QString NetworkClient::sendDemoControl(const QJsonObject& command) {
    const QString requestId = command.value(QStringLiteral("requestId")).toString().isEmpty()
        ? QStringLiteral("demo-control-%1")
              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
        : command.value(QStringLiteral("requestId")).toString();
    QJsonObject payload = command;
    payload.insert(QStringLiteral("requestId"), requestId);
    if (!sendEnvelope(QString::fromLatin1(Protocol::DemoControlMessage), payload, requestId)) {
        return {};
    }
    return requestId;
}

void NetworkClient::cancelIntelHistoryRequests() {
    const QStringList requestIds = m_pendingIntelRequests.keys();
    for (const QString& requestId : requestIds) {
        if (!m_pendingIntelRequests.contains(requestId)
            || m_pendingIntelRequests.value(requestId).type
                   != QLatin1String("requestIntelHistory")) {
            continue;
        }
        const PendingIntelRequest pending = m_pendingIntelRequests.take(requestId);
        emit commandStatusChanged(requestId, pending.action, QStringLiteral("canceled"),
                                  QStringLiteral("CLIENT_CANCELED"),
                                  QStringLiteral("情报历史查询已取消"));
    }
}

void NetworkClient::sendMapMark(const QVariantMap& position, const QString& label,
                                const QStringList& recipientSeatIds) {
    QJsonArray recipients;
    for (const QString& seatId : recipientSeatIds) recipients.append(seatId);
    sendSimple(QStringLiteral("mapMark"),
               QJsonObject{{QStringLiteral("position"), QJsonObject::fromVariantMap(position)},
                           {QStringLiteral("label"), label},
                           {QStringLiteral("recipientSeatIds"), recipients}});
}

void NetworkClient::setObserverTrajectories(const QStringList& unitIds) {
    QJsonArray ids;
    for (const QString& unitId : unitIds) ids.append(unitId);
    sendSimple(QString::fromLatin1(Protocol::SetObserverTrajectoriesMessage),
               QJsonObject{{QStringLiteral("unitIds"), ids}});
}

void NetworkClient::sendChat(const QString& text, const QStringList& recipientSeatIds) {
    if (!m_authenticated || state() != QLatin1String("connected")
        || m_stateStore.waitingForSnapshot() || m_stateStore.waitingForResync()) {
        emit commandRejected(QStringLiteral("推演状态尚未同步，暂时不能发送消息"));
        return;
    }
    QJsonArray recipients;
    for (const QString& seatId : recipientSeatIds) recipients.append(seatId);
    sendEnvelope(QStringLiteral("chat"),
                 QJsonObject{{QStringLiteral("text"), text},
                             {QStringLiteral("recipientSeatIds"), recipients}});
}

void NetworkClient::sendScenarioUpsert(const QJsonObject& unit) {
    if (!m_authenticated || state() != QLatin1String("connected")
        || m_stateStore.waitingForSnapshot() || m_stateStore.waitingForResync()) {
        emit commandRejected(QStringLiteral("推演状态尚未同步，暂时不能修改阵容"));
        return;
    }
    sendEnvelope(QStringLiteral("scenarioUpsert"), QJsonObject{{QStringLiteral("unit"), unit}});
}

void NetworkClient::sendScenarioRemove(const QString& unitId) {
    if (!m_authenticated || state() != QLatin1String("connected")
        || m_stateStore.waitingForSnapshot() || m_stateStore.waitingForResync()) {
        emit commandRejected(QStringLiteral("推演状态尚未同步，暂时不能修改阵容"));
        return;
    }
    sendEnvelope(QStringLiteral("scenarioRemove"), QJsonObject{{QStringLiteral("unitId"), unitId}});
}

void NetworkClient::sendScenarioReplace(const QJsonObject& scenario) {
    if (!m_authenticated || state() != QLatin1String("connected")
        || m_stateStore.waitingForSnapshot() || m_stateStore.waitingForResync()) {
        emit commandRejected(QStringLiteral("推演状态尚未同步，暂时不能替换场景"));
        return;
    }
    sendEnvelope(QStringLiteral("scenarioReplace"), QJsonObject{{QStringLiteral("scenario"), scenario}});
}

} // namespace gbr
