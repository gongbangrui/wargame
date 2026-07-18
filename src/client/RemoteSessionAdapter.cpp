#include "RemoteSessionAdapter.h"

#include "core/CommandResult.h"
#include "protocol/Protocol.h"

#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSet>
#include <QUuid>
#include <QWebSocketProtocol>

namespace gbr {

RemoteSessionAdapter::RemoteSessionAdapter(QObject* parent)
    : ISessionAdapter(parent), m_store(this) {
    m_clientId = newId();
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &RemoteSessionAdapter::openSocket);
    connect(&m_socket, &QWebSocket::connected, this, &RemoteSessionAdapter::onConnected);
    connect(&m_socket, &QWebSocket::disconnected, this, &RemoteSessionAdapter::onDisconnected);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &RemoteSessionAdapter::onTextMessage);
    connect(&m_store, &ClientStateStore::simTimeChanged, this, &ISessionAdapter::simTimeChanged);
    connect(&m_store, &ClientStateStore::runningChanged, this, &ISessionAdapter::runningChanged);
    connect(&m_store, &ClientStateStore::unitsChanged, this, &ISessionAdapter::unitsChanged);
    connect(&m_store, &ClientStateStore::messagesChanged, this, &ISessionAdapter::messagesChanged);
    connect(&m_store, &ClientStateStore::mapChanged, this, &ISessionAdapter::mapChanged);
    connect(&m_store, &ClientStateStore::readyForSimChanged, this, &ISessionAdapter::readyForSimChanged);
    connect(&m_store, &ClientStateStore::lobbyChanged, this, &ISessionAdapter::lobbyChanged);
    connect(&m_store, &ClientStateStore::chatMessagesChanged, this, &ISessionAdapter::chatMessagesChanged);
    connect(&m_store, &ClientStateStore::errorChanged, this, &ISessionAdapter::errorOccurred);
}

QString RemoteSessionAdapter::newId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void RemoteSessionAdapter::setConnectionState(const QString& state) {
    if (m_connectionState == state) return;
    m_connectionState = state;
    emit connectionStateChanged();
}

void RemoteSessionAdapter::connectToServer(const QUrl& url, const QString& token) {
    if (!url.isValid() || (url.scheme() != QLatin1String("ws") && url.scheme() != QLatin1String("wss"))) {
        m_store.setError(QStringLiteral("服务器地址必须是有效的 ws:// 或 wss:// URL"));
        return;
    }
    if (token.size() < 32) {
        m_store.setError(QStringLiteral("联网 token 格式无效"));
        return;
    }
    m_url = url;
    m_token = token.trimmed();
    m_username.clear();
    m_password.clear();
    m_pendingCommands.clear();
    m_manualDisconnect = false;
    m_reconnectAllowed = true;
    m_reconnectAttempt = 0;
    setConnectionState(QStringLiteral("connecting"));
    openSocket();
}

void RemoteSessionAdapter::connectToServerWithPassword(const QUrl& url, const QString& username,
                                                       const QString& password) {
    if (!url.isValid() || (url.scheme() != QLatin1String("ws") && url.scheme() != QLatin1String("wss"))) {
        m_store.setError(QStringLiteral("服务器地址必须是有效的 ws:// 或 wss:// URL"));
        return;
    }
    if (username.trimmed().isEmpty() || password.size() < 8) {
        m_store.setError(QStringLiteral("账号或密码格式无效"));
        return;
    }
    m_url = url;
    m_token.clear();
    m_username = username.trimmed();
    m_password = password;
    m_pendingCommands.clear();
    m_manualDisconnect = false;
    m_reconnectAllowed = true;
    m_reconnectAttempt = 0;
    setConnectionState(QStringLiteral("connecting"));
    openSocket();
}

void RemoteSessionAdapter::openSocket() {
    if (m_manualDisconnect || m_url.isEmpty()) return;
    setConnectionState(QStringLiteral("connecting"));
    emit connectionStatusChanged(QStringLiteral("正在连接 %1").arg(m_url.toString()));
    m_socket.open(m_url);
}

void RemoteSessionAdapter::disconnectFromServer() {
    m_manualDisconnect = true;
    m_reconnectTimer.stop();
    m_token.clear();
    m_password.clear();
    m_pendingCommands.clear();
    m_authenticated = false;
    m_socket.close();
    setConnectionState(QStringLiteral("disconnected"));
    emit connectedChanged();
    emit connectionStatusChanged(QStringLiteral("已断开服务器"));
}

void RemoteSessionAdapter::onConnected() {
    m_reconnectAttempt = 0;
    m_lastEnvelopeSequence = 0;
    m_store.resetSequence();
    setConnectionState(QStringLiteral("authenticating"));
    QJsonObject hello{{QStringLiteral("resumeSequence"), m_store.lastSequence()}};
    if (!m_token.isEmpty()) hello.insert(QStringLiteral("token"), m_token);
    else {
        hello.insert(QStringLiteral("username"), m_username);
        hello.insert(QStringLiteral("password"), m_password);
    }
    send(QString::fromLatin1(ProtocolType::Hello), hello);
    emit connectionStatusChanged(QStringLiteral("连接已建立，正在认证"));
}

void RemoteSessionAdapter::onDisconnected() {
    const bool wasAuthenticated = m_authenticated;
    m_authenticated = false;
    if (wasAuthenticated) emit connectedChanged();
    if (!m_manualDisconnect && m_reconnectAllowed) {
        setConnectionState(QStringLiteral("reconnecting"));
        emit connectionStatusChanged(QStringLiteral("连接中断，准备重连"));
        scheduleReconnect();
    } else if (!m_manualDisconnect) {
        setConnectionState(QStringLiteral("error"));
        emit connectionStatusChanged(QStringLiteral("连接已关闭，需要检查认证或协议配置"));
    }
}

void RemoteSessionAdapter::scheduleReconnect() {
    static const int delays[]{1000, 2000, 4000, 8000, 15000, 30000};
    const int base = delays[std::min(m_reconnectAttempt, 5)];
    ++m_reconnectAttempt;
    const int jitter = QRandomGenerator::global()->bounded(base / 5 + 1);
    m_reconnectTimer.start(base + jitter);
}

void RemoteSessionAdapter::send(const QString& type, const QJsonObject& payload) {
    if (m_socket.state() != QAbstractSocket::ConnectedState) return;
    const QJsonObject envelope = ProtocolCodec::envelope(
        type, payload, newId(), {}, m_clientId, -1,
        m_store.scenarioRevision(), m_store.serverTick());
    m_socket.sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(envelope)));
}

void RemoteSessionAdapter::requestResync() {
    send(QString::fromLatin1(ProtocolType::ResyncRequest),
         {{QStringLiteral("lastSequence"), m_lastEnvelopeSequence}});
}

void RemoteSessionAdapter::resendPendingCommands() {
    for (auto it = m_pendingCommands.constBegin(); it != m_pendingCommands.constEnd(); ++it) {
        send(QString::fromLatin1(ProtocolType::Command), it.value());
    }
}

void RemoteSessionAdapter::onTextMessage(const QString& text) {
    const ProtocolParseResult parsed = ProtocolCodec::parse(
        text.toUtf8(), ProtocolLimits::MaxSnapshotBytes);
    if (!parsed.accepted) {
        m_store.setError(parsed.message);
        return;
    }
    const QJsonObject envelope = parsed.envelope;
    const QString type = envelope.value(QStringLiteral("type")).toString();
    const qint64 sequence = envelope.value(QStringLiteral("sequence")).toInteger(-1);
    if (sequence < 1) {
        m_store.setError(QStringLiteral("服务器消息缺少有效序号"));
        m_reconnectAllowed = false;
        m_socket.close(QWebSocketProtocol::CloseCodeProtocolError,
                       QStringLiteral("服务器消息序号无效"));
        return;
    }
    if (sequence <= m_lastEnvelopeSequence) return;
    if (m_lastEnvelopeSequence > 0 && sequence != m_lastEnvelopeSequence + 1
        && type != QLatin1String(ProtocolType::Snapshot)) {
        requestResync();
        return;
    }
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    if (type == QLatin1String(ProtocolType::Welcome)) {
        const QString role = payload.value(QStringLiteral("role")).toString();
        const QString side = payload.value(QStringLiteral("side")).toString();
        QString sequenceError;
        if (!payload.value(QStringLiteral("userId")).isString()
            || payload.value(QStringLiteral("userId")).toString().isEmpty()
            || !payload.value(QStringLiteral("roomId")).isString()
            || payload.value(QStringLiteral("roomId")).toString().isEmpty()
            || !payload.value(QStringLiteral("role")).isString()
            || (role != QLatin1String("red") && role != QLatin1String("blue")
                && role != QLatin1String("director") && role != QLatin1String("editor")
                && role != QLatin1String("observer"))
            || ((role == QLatin1String("red") || role == QLatin1String("blue")) && side != role)
            || !m_store.advanceSequence(sequence, &sequenceError)) {
            m_store.setError(sequenceError.isEmpty()
                                 ? QStringLiteral("服务器 welcome 结构无效") : sequenceError);
            m_reconnectAllowed = false;
            m_socket.close(QWebSocketProtocol::CloseCodeProtocolError,
                           QStringLiteral("welcome 无效"));
            return;
        }
        m_store.setIdentity(payload.value(QStringLiteral("role")).toString(),
                            payload.value(QStringLiteral("side")).toString());
        m_authenticated = true;
        setConnectionState(QStringLiteral("syncing"));
        emit connectedChanged();
        emit connectionStatusChanged(QStringLiteral("认证成功，等待状态同步"));
    } else if (type == QLatin1String(ProtocolType::Snapshot)) {
        QString error;
        if (payload.value(QStringLiteral("lastSequence")).toInteger(-1) != sequence
            || !m_store.applySnapshot(payload, &error)) {
            if (error.isEmpty()) error = QStringLiteral("快照序号与协议包不一致");
            m_store.setError(error);
            requestResync();
            return;
        } else {
            m_store.setError({});
            setConnectionState(QStringLiteral("connected"));
            emit connectionStatusChanged(QStringLiteral("状态已同步"));
            resendPendingCommands();
        }
    } else if (type == QLatin1String(ProtocolType::Delta)) {
        QJsonObject delta = payload;
        delta.insert(QStringLiteral("sequence"), sequence);
        delta.insert(QStringLiteral("scenarioRevision"),
                     envelope.value(QStringLiteral("scenarioRevision")));
        delta.insert(QStringLiteral("serverTick"), envelope.value(QStringLiteral("serverTick")));
        QString error;
        if (!m_store.applyDelta(delta, &error)) {
            m_store.setError(error);
            requestResync();
            return;
        }
    } else if (type == QLatin1String(ProtocolType::CommandResult)) {
        QString sequenceError;
        if (!payload.value(QStringLiteral("commandId")).isString()
            || payload.value(QStringLiteral("commandId")).toString().isEmpty()
            || !payload.value(QStringLiteral("accepted")).isBool()
            || !payload.value(QStringLiteral("code")).isString()
            || !payload.value(QStringLiteral("message")).isString()
            || !m_store.advanceSequence(sequence, &sequenceError)) {
            m_store.setError(sequenceError.isEmpty()
                                 ? QStringLiteral("服务器命令回执结构无效") : sequenceError);
            requestResync();
            return;
        }
        m_pendingCommands.remove(payload.value(QStringLiteral("commandId")).toString());
        emit commandResultReceived(payload.value(QStringLiteral("commandId")).toString(),
                                   payload.toVariantMap());
        if (!payload.value(QStringLiteral("accepted")).toBool()) {
            m_store.setError(payload.value(QStringLiteral("message")).toString());
        }
    } else if (type == QLatin1String(ProtocolType::Error)) {
        QString sequenceError;
        if (!payload.value(QStringLiteral("code")).isString()
            || !payload.value(QStringLiteral("message")).isString()
            || !m_store.advanceSequence(sequence, &sequenceError)) {
            m_store.setError(sequenceError.isEmpty()
                                 ? QStringLiteral("服务器错误消息结构无效") : sequenceError);
            requestResync();
            return;
        }
        static const QSet<QString> nonRetryableErrors{
            QStringLiteral("AUTH_FAILED"), QStringLiteral("HELLO_REQUIRED"),
            QStringLiteral("INVALID_HELLO"), QStringLiteral("UNSUPPORTED_VERSION"),
            QStringLiteral("MALFORMED_JSON"), QStringLiteral("UNKNOWN_FIELD"),
            QStringLiteral("INVALID_ENVELOPE"), QStringLiteral("INVALID_PAYLOAD"),
            QStringLiteral("CLIENT_TYPE_NOT_ALLOWED"),
        };
        if (nonRetryableErrors.contains(payload.value(QStringLiteral("code")).toString())) {
            m_reconnectAllowed = false;
            setConnectionState(QStringLiteral("error"));
        }
        m_store.setError(payload.value(QStringLiteral("message")).toString());
    } else if (type == QLatin1String(ProtocolType::Ping)) {
        QString sequenceError;
        if (!payload.isEmpty() || !m_store.advanceSequence(sequence, &sequenceError)) {
            m_store.setError(sequenceError.isEmpty()
                                 ? QStringLiteral("服务器心跳结构无效") : sequenceError);
            requestResync();
            return;
        }
        send(QString::fromLatin1(ProtocolType::Pong), {});
    } else if (type == QLatin1String(ProtocolType::Pong)) {
        QString sequenceError;
        if (!payload.isEmpty() || !m_store.advanceSequence(sequence, &sequenceError)) {
            m_store.setError(sequenceError.isEmpty()
                                 ? QStringLiteral("服务器心跳响应结构无效") : sequenceError);
            requestResync();
            return;
        }
    } else {
        m_store.setError(QStringLiteral("服务器发送了客户端不接受的消息类型"));
        m_reconnectAllowed = false;
        m_socket.close(QWebSocketProtocol::CloseCodeProtocolError,
                       QStringLiteral("服务器消息类型无效"));
        return;
    }
    m_lastEnvelopeSequence = sequence;
}

QVariantMap RemoteSessionAdapter::command(const QString& action, const QVariantMap& args) {
    if (!m_authenticated) {
        return CommandResult::rejected(CommandCode::NotAuthenticated,
                                       QStringLiteral("尚未连接服务器")).toVariantMap();
    }
    if (m_pendingCommands.size() >= 128) {
        return CommandResult::rejected(CommandCode::RateLimited,
                                       QStringLiteral("待确认命令过多，请等待服务器回执"),
                                       m_store.serverTick()).toVariantMap();
    }
    const QString commandId = newId();
    const QJsonObject payload{
        {QStringLiteral("commandId"), commandId},
        {QStringLiteral("action"), action},
        {QStringLiteral("args"), QJsonObject::fromVariantMap(args)},
    };
    m_pendingCommands.insert(commandId, payload);
    send(QString::fromLatin1(ProtocolType::Command), payload);
    QVariantMap queued = CommandResult::success(m_store.serverTick(), QStringLiteral("命令已发送，等待服务器回执")).toVariantMap();
    queued.insert(QStringLiteral("commandId"), commandId);
    queued.insert(QStringLiteral("pending"), true);
    return queued;
}

void RemoteSessionAdapter::setRunning(bool running) {
    command(QStringLiteral("setRunning"), {{QStringLiteral("running"), running}});
}

void RemoteSessionAdapter::setSpeed(double speed) {
    command(QStringLiteral("setSimulationSpeed"), {{QStringLiteral("speed"), speed}});
}

void RemoteSessionAdapter::stepOnce() {
    command(QStringLiteral("stepOnce"), {});
}

} // namespace gbr
