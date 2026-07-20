#include "GameServer.h"
#include "StateProjector.h"

#include "core/SnapshotCodec.h"
#include "core/UnitBase.h"
#include "protocol/Protocol.h"
#include "protocol/StateDelta.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSet>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace gbr {

namespace {

constexpr qsizetype kMaxScenarioUnits = 512;
constexpr qsizetype kMaxSchedulePoints = 512;
constexpr int kMaxMessagesPerSecond = 60;
constexpr int kMaxConnectedClients = 64;
constexpr qint64 kMaxPendingBytes = 1024 * 1024;

QString env(const char* name, const QString& fallback) {
    const QString value = qEnvironmentVariable(name).trimmed();
    return value.isEmpty() ? fallback : value;
}

QString sideForRole(const QString& role) {
    return StateProjector::sideForRole(role);
}

QString commandCacheKey(qint64 userId, const QString& commandId) {
    return QStringLiteral("%1:%2").arg(userId).arg(commandId);
}

ScenarioUnit scenarioUnitFromJson(const QJsonObject& object) {
    QJsonObject wrapper;
    wrapper[QStringLiteral("units")] = QJsonArray{object};
    const Scenario scenario = ScenarioIo::fromJson(wrapper);
    return scenario.units.empty() ? ScenarioUnit{} : scenario.units.front();
}

QString validateNetworkScenario(const Scenario& scenario) {
    if (scenario.units.empty()) return QStringLiteral("场景至少需要一个单元");
    if (static_cast<qsizetype>(scenario.units.size()) > kMaxScenarioUnits) {
        return QStringLiteral("场景单元数量不能超过 %1").arg(kMaxScenarioUnits);
    }
    constexpr double kMaxMapExtent = 1'000'000.0;
    if (!std::isfinite(scenario.map.widthMeters) || !std::isfinite(scenario.map.heightMeters)
        || scenario.map.widthMeters <= 0.0 || scenario.map.heightMeters <= 0.0
        || scenario.map.widthMeters > kMaxMapExtent || scenario.map.heightMeters > kMaxMapExtent) {
        return QStringLiteral("场景地图尺寸无效或过大");
    }
    QSet<QString> ids;
    const QSet<QString> knownKinds{QStringLiteral("commandpost"), QStringLiteral("reconuav"),
                                   QStringLiteral("attackuav"), QStringLiteral("groundscout"),
                                   QStringLiteral("jammeruav")};
    for (const ScenarioUnit& unit : scenario.units) {
        if (unit.id.trimmed().isEmpty() || unit.id.size() > 64 || unit.callsign.size() > 128) {
            return QStringLiteral("单元 ID 或名称过长: %1").arg(unit.id.left(64));
        }
        if (ids.contains(unit.id)) return QStringLiteral("单元 ID 重复: %1").arg(unit.id);
        ids.insert(unit.id);
        if (!knownKinds.contains(unit.kind)) return QStringLiteral("未知单元类型: %1").arg(unit.kind);
        if (unit.side != QLatin1String("red") && unit.side != QLatin1String("blue")) {
            return QStringLiteral("单元阵营无效: %1").arg(unit.id);
        }
        if (!std::isfinite(unit.pos.x) || !std::isfinite(unit.pos.y)
            || unit.pos.x < 0.0 || unit.pos.y < 0.0
            || unit.pos.x > scenario.map.widthMeters || unit.pos.y > scenario.map.heightMeters) {
            return QStringLiteral("单元位置超出地图边界: %1").arg(unit.id);
        }
        if (!std::isfinite(unit.detectRange) || unit.detectRange < 0.0
            || !std::isfinite(unit.attackRange) || unit.attackRange < 0.0
            || !std::isfinite(unit.commRange) || unit.commRange < 0.0
            || !std::isfinite(unit.speed) || unit.speed < 0.0
            || !std::isfinite(unit.maxHp) || unit.maxHp <= 0.0
            || !std::isfinite(unit.attackPower) || unit.attackPower < 0.0) {
            return QStringLiteral("单元参数无效: %1").arg(unit.id);
        }
        if (static_cast<qsizetype>(unit.schedule.size()) > kMaxSchedulePoints) {
            return QStringLiteral("单元计划点不能超过 %1 个: %2")
                .arg(kMaxSchedulePoints).arg(unit.id);
        }
        for (const SchedulePoint& point : unit.schedule) {
            if (!std::isfinite(point.time) || point.time < 0.0
                || !std::isfinite(point.x) || !std::isfinite(point.y)
                || point.x < 0.0 || point.y < 0.0
                || point.x > scenario.map.widthMeters || point.y > scenario.map.heightMeters) {
                return QStringLiteral("单元计划点无效或超出地图边界: %1").arg(unit.id);
            }
        }
    }
    return {};
}

} // namespace

GameServer::GameServer(QObject* parent)
    : QObject(parent),
      m_server(QStringLiteral("兵器推演联网服务器"), QWebSocketServer::NonSecureMode, this),
      m_authServiceUrl(env("AUTH_SERVICE_URL", QStringLiteral("http://account-web:8080"))),
      m_internalKey(env("INTERNAL_API_KEY", QStringLiteral("change-this-internal-key"))),
      m_scenarioPath(env("SCENARIO_PATH", QStringLiteral("/data/scenario.json"))),
      m_monitorLogPath(env("MONITOR_LOG_PATH", QStringLiteral("/data/game-events.jsonl"))),
      m_monitorStatusPath(env("MONITOR_STATUS_PATH", QStringLiteral("/data/game-status.json"))),
      m_persistence(env("CHECKPOINT_PATH", QStringLiteral("/data/room-checkpoint.json")),
                    env("COMMAND_LOG_PATH", QStringLiteral("/data/room-commands.jsonl"))) {
    QString restoreError;
    if (!restoreRoomState(&restoreError)) {
        if (QFileInfo::exists(m_persistence.checkpointPath())) {
            m_recoveryError = restoreError;
            qCritical() << "房间状态恢复失败，服务器将拒绝监听:" << restoreError;
        } else {
            bool loaded = false;
            QString loadError;
            if (QFileInfo::exists(m_scenarioPath)) {
                const Scenario stored = ScenarioIo::loadFromFile(m_scenarioPath, &loadError);
                const QString validationError = validateNetworkScenario(stored);
                loaded = loadError.isEmpty() && validationError.isEmpty()
                    && m_engine.setScenario(stored);
                if (!loaded) {
                    qWarning() << "持久化场景无效，已恢复默认场景"
                               << (loadError.isEmpty() ? validationError : loadError);
                }
            }
            if (!loaded) m_engine.loadDefaultScenario();
            persistScenario();
            QString checkpointError;
            if (!persistRoomState(&checkpointError)) {
                m_recoveryError = checkpointError;
                qCritical() << "无法创建初始检查点:" << checkpointError;
            }
        }
    }

    connect(&m_server, &QWebSocketServer::newConnection, this, &GameServer::onNewConnection);
    connect(&m_engine, &SimulationEngine::simulationEnded, this,
            [this](const QString& winner, const QString& loser) {
                m_phase = QStringLiteral("finished");
                broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("simulationEnded")},
                                           {QStringLiteral("winner"), winner},
                                           {QStringLiteral("loser"), loser},
                                           {QStringLiteral("message"), loser.isEmpty()
                                                ? winner
                                                : QStringLiteral("%1指挥所已被摧毁，%2获胜").arg(loser, winner)}});
                broadcastSnapshots();
            });
    connect(&m_engine, &SimulationEngine::eventPosted, this,
            [this](const QString& title, const QString& body, const QString& level,
                   const QString& sourceUnitId) {
                QString side;
                if (auto* unit = m_engine.unit(sourceUnitId)) side = unit->sideStr();
                broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("simulationEvent")},
                                           {QStringLiteral("title"), title},
                                           {QStringLiteral("body"), body},
                                           {QStringLiteral("level"), level},
                                           {QStringLiteral("sourceUnitId"), sourceUnitId}}, side);
            });
    connect(&m_engine, &SimulationEngine::targetDestroyedVisual, this,
            [this](const QString& unitId, double x, double y) {
                QString side;
                if (auto* unit = m_engine.unit(unitId)) side = unit->sideStr();
                broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("targetDestroyed")},
                                           {QStringLiteral("unitId"), unitId},
                                           {QStringLiteral("x"), x},
                                           {QStringLiteral("y"), y}}, side);
            });

    m_snapshotTimer.setInterval(100);
    connect(&m_snapshotTimer, &QTimer::timeout, this,
            [this]() { broadcastSnapshots(); });
    m_sessionValidationTimer.setInterval(30000);
    connect(&m_sessionValidationTimer, &QTimer::timeout,
            this, &GameServer::validateActiveSessions);
    m_monitorStatusTimer.setInterval(1000);
    connect(&m_monitorStatusTimer, &QTimer::timeout, this, &GameServer::writeMonitorStatus);
    m_checkpointTimer.setInterval(10000);
    connect(&m_checkpointTimer, &QTimer::timeout, this, [this]() {
        QString error;
        if (!persistRoomState(&error)) {
            qWarning() << "定时检查点写入失败" << error;
            audit(QStringLiteral("persistence"),
                  QJsonObject{{QStringLiteral("event"), QStringLiteral("checkpointFailed")},
                              {QStringLiteral("message"), error}});
        }
    });
}

GameServer::~GameServer() {
    m_checkpointTimer.stop();
    m_snapshotTimer.stop();
    if (!m_engine.scenario().units.empty()) {
        QString error;
        if (!persistRoomState(&error)) qWarning() << "退出前检查点写入失败" << error;
    }
    m_engine.setRunning(false);
    m_server.close();
}

bool GameServer::listen(quint16 port) {
    if (!m_recoveryError.isEmpty()) return false;
    if (!m_server.listen(QHostAddress::Any, port)) return false;
    m_snapshotTimer.start();
    m_sessionValidationTimer.start();
    m_monitorStatusTimer.start();
    m_checkpointTimer.start();
    audit(QStringLiteral("server"), QJsonObject{{QStringLiteral("event"), QStringLiteral("started")},
                                                 {QStringLiteral("port"), static_cast<int>(port)}});
    writeMonitorStatus();
    return true;
}

void GameServer::onNewConnection() {
    while (m_server.hasPendingConnections()) {
        QWebSocket* socket = m_server.nextPendingConnection();
        if (m_clients.size() >= kMaxConnectedClients) {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("服务器连接数已达上限"));
            socket->deleteLater();
            continue;
        }
        m_clients.insert(socket, ClientSession{});
        audit(QStringLiteral("connection"), QJsonObject{{QStringLiteral("event"), QStringLiteral("opened")},
                                                         {QStringLiteral("peer"), socket->peerAddress().toString()},
                                                         {QStringLiteral("port"), static_cast<int>(socket->peerPort())}});
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString& text) { onTextMessage(socket, text); });
        connect(socket, &QWebSocket::disconnected, this,
                [this, socket]() { removeClient(socket); });
        QPointer<QWebSocket> guarded(socket);
        QTimer::singleShot(10000, this, [this, guarded]() {
            if (guarded && m_clients.contains(guarded) && !m_clients.value(guarded).authenticated) {
                guarded->close(QWebSocketProtocol::CloseCodePolicyViolated,
                               QStringLiteral("认证超时"));
            }
        });
    }
}

void GameServer::onTextMessage(QWebSocket* socket, const QString& text) {
    if (!m_clients.contains(socket)) return;
    if (text.toUtf8().size() > Protocol::MaxMessageBytes) {
        socket->close(QWebSocketProtocol::CloseCodeTooMuchData, QStringLiteral("消息过大"));
        return;
    }
    ClientSession& session = m_clients[socket];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (session.rateWindowStartedAt == 0 || now - session.rateWindowStartedAt >= 1000) {
        session.rateWindowStartedAt = now;
        session.messagesInRateWindow = 0;
    }
    if (++session.messagesInRateWindow > kMaxMessagesPerSecond) {
        sendError(socket, QStringLiteral("MESSAGE_RATE_LIMIT"),
                  QStringLiteral("消息发送频率过高"));
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                      QStringLiteral("消息频率过高"));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (!document.isObject()) {
        sendError(socket, QStringLiteral("INVALID_JSON"), QStringLiteral("消息不是有效 JSON 对象"));
        return;
    }
    const QJsonObject envelope = document.object();
    const Protocol::ValidationResult validation = Protocol::validateClientEnvelope(envelope);
    if (!validation.valid) {
        sendError(socket, validation.code, validation.message);
        if (validation.code == QLatin1String("PROTOCOL_MISMATCH")
            || validation.code == QLatin1String("SCHEMA_MISMATCH")) {
            socket->close(QWebSocketProtocol::CloseCodeProtocolError, validation.message);
        }
        return;
    }
    const QString messageId = envelope.value(QStringLiteral("messageId")).toString();
    if (session.recentMessageIds.contains(messageId)) {
        sendError(socket, QStringLiteral("DUPLICATE_MESSAGE"),
                  QStringLiteral("消息 ID 已处理"), messageId);
        return;
    }
    session.recentMessageIds.insert(messageId);
    session.recentMessageIdOrder.append(messageId);
    while (session.recentMessageIdOrder.size() > 2048) {
        session.recentMessageIds.remove(session.recentMessageIdOrder.takeFirst());
    }
    const QString type = envelope.value(QStringLiteral("type")).toString();
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    if (!session.authenticated) {
        if (type != QLatin1String("auth")) {
            sendError(socket, QStringLiteral("AUTH_REQUIRED"), QStringLiteral("请先完成登录认证"));
            return;
        }
        authenticate(socket, payload.value(QStringLiteral("token")).toString());
        return;
    }

    audit(QStringLiteral("message"), QJsonObject{{QStringLiteral("direction"), QStringLiteral("in")},
                                                   {QStringLiteral("type"), type},
                                                   {QStringLiteral("user"), session.username},
                                                   {QStringLiteral("role"), session.role},
                                                   {QStringLiteral("summary"), messageSummary(type, payload)}});

    if (type == QLatin1String("command")) handleCommand(socket, payload);
    else if (type == QLatin1String("control")) handleControl(socket, payload);
    else if (type == QLatin1String("setReady")) handleReady(socket, payload);
    else if (type == QLatin1String("chat")) handleChat(socket, payload);
    else if (type == QLatin1String("scenarioUpsert")) handleScenarioUpsert(socket, payload);
    else if (type == QLatin1String("scenarioRemove")) handleScenarioRemove(socket, payload);
    else if (type == QLatin1String("scenarioReplace")) handleScenarioReplace(socket, payload);
    else if (type == QLatin1String("resyncRequest")) sendFullSnapshot(socket);
    else if (type == QLatin1String("ping")) sendEnvelope(socket, QStringLiteral("pong"), QJsonObject{});
    else sendError(socket, QStringLiteral("UNKNOWN_MESSAGE"), QStringLiteral("不支持的消息类型"));
}

void GameServer::authenticate(QWebSocket* socket, const QString& token) {
    if (token.isEmpty()) {
        sendError(socket, QStringLiteral("INVALID_TOKEN"), QStringLiteral("登录令牌为空"));
        return;
    }
    if (m_clients.value(socket).authenticationPending) return;
    m_clients[socket].authenticationPending = true;
    m_clients[socket].token = token;
    const QUrl url(m_authServiceUrl + QStringLiteral("/api/internal/session"));
    QNetworkRequest request(url);
    request.setRawHeader("X-Internal-Key", m_internalKey.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = m_network.post(
        request, QJsonDocument(QJsonObject{{QStringLiteral("token"), token}}).toJson(QJsonDocument::Compact));
    QTimer::singleShot(5000, reply, [reply]() {
        if (reply->isRunning()) reply->abort();
    });
    QPointer<QWebSocket> guarded(socket);
    connect(reply, &QNetworkReply::finished, this, [this, guarded, reply]() {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        if (!guarded || !m_clients.contains(guarded)) return;
        if (statusCode != 200 || !document.isObject()) {
            sendError(guarded, QStringLiteral("INVALID_TOKEN"), QStringLiteral("登录会话无效或已过期"));
            guarded->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("认证失败"));
            return;
        }
        m_clients[guarded.data()].authenticationPending = false;
        finishAuthentication(guarded, document.object());
    });
}

void GameServer::validateActiveSessions() {
    const QList<QWebSocket*> sockets = m_clients.keys();
    for (QWebSocket* socket : sockets) {
        if (!m_clients.contains(socket) || !m_clients.value(socket).authenticated) continue;
        const ClientSession expected = m_clients.value(socket);
        const QUrl url(m_authServiceUrl + QStringLiteral("/api/internal/session"));
        QNetworkRequest request(url);
        request.setRawHeader("X-Internal-Key", m_internalKey.toUtf8());
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QNetworkReply* reply = m_network.post(
            request, QJsonDocument(QJsonObject{{QStringLiteral("token"), expected.token}}).toJson(QJsonDocument::Compact));
        QTimer::singleShot(5000, reply, [reply]() {
            if (reply->isRunning()) reply->abort();
        });
        QPointer<QWebSocket> guarded(socket);
        connect(reply, &QNetworkReply::finished, this, [this, guarded, reply, expected]() {
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
            reply->deleteLater();
            if (!guarded || !m_clients.contains(guarded)) return;
            const QJsonObject identity = document.object();
            const bool unchanged = statusCode == 200 && identity.value(QStringLiteral("valid")).toBool()
                && identity.value(QStringLiteral("userId")).toInteger() == expected.userId
                && identity.value(QStringLiteral("role")).toString() == expected.role;
            if (!unchanged) {
                sendError(guarded, QStringLiteral("SESSION_REVOKED"),
                          QStringLiteral("账号已停用、删除、改密或变更席位，请重新登录"));
                guarded->close(QWebSocketProtocol::CloseCodePolicyViolated,
                               QStringLiteral("账号会话已失效"));
            }
        });
    }
}

void GameServer::finishAuthentication(QWebSocket* socket, const QJsonObject& identity) {
    ClientSession& session = m_clients[socket];
    const QString role = identity.value(QStringLiteral("role")).toString();
    if (!identity.value(QStringLiteral("valid")).toBool()
        || identity.value(QStringLiteral("userId")).toInteger() <= 0
        || identity.value(QStringLiteral("username")).toString().isEmpty()
        || !QSet<QString>{QStringLiteral("director"), QStringLiteral("editor"),
                       QStringLiteral("red"), QStringLiteral("blue")}.contains(role)) {
        sendError(socket, QStringLiteral("INVALID_ROLE"), QStringLiteral("账号席位无效"));
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("席位无效"));
        return;
    }
    session.authenticated = true;
    session.userId = identity.value(QStringLiteral("userId")).toInteger();
    session.username = identity.value(QStringLiteral("username")).toString();
    session.displayName = identity.value(QStringLiteral("displayName")).toString();
    session.role = role;
    audit(QStringLiteral("connection"), QJsonObject{{QStringLiteral("event"), QStringLiteral("authenticated")},
                                                     {QStringLiteral("user"), session.username},
                                                     {QStringLiteral("displayName"), session.displayName},
                                                     {QStringLiteral("role"), session.role}});
    sendEnvelope(socket, QStringLiteral("welcome"),
                 QJsonObject{{QStringLiteral("username"), session.username},
                             {QStringLiteral("displayName"), session.displayName},
                             {QStringLiteral("role"), session.role},
                             {QStringLiteral("room"), QStringLiteral("local-main")},
                             {QStringLiteral("chatHistory"), m_chatHistory},
                             {QStringLiteral("roomState"), roomState()}});
    sendFullSnapshot(socket);
    broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("presence")},
                               {QStringLiteral("message"), QStringLiteral("%1 已进入推演室").arg(session.displayName)}});
}

void GameServer::removeClient(QWebSocket* socket) {
    if (!m_clients.contains(socket)) return;
    const ClientSession session = m_clients.take(socket);
    audit(QStringLiteral("connection"), QJsonObject{{QStringLiteral("event"), QStringLiteral("closed")},
                                                     {QStringLiteral("user"), session.username},
                                                     {QStringLiteral("role"), session.role}});
    writeMonitorStatus();
    socket->deleteLater();
    if (session.authenticated) {
        broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("presence")},
                                   {QStringLiteral("message"), QStringLiteral("%1 已离开推演室").arg(session.displayName)}});
    }
}

QString GameServer::controlledUnitId(const QString& action, const QVariantMap& args) const {
    if (action == QLatin1String("guideAttack")) return args.value(QStringLiteral("guideId")).toString();
    if (action == QLatin1String("assignTarget") || action == QLatin1String("setFlightPlan")
        || action == QLatin1String("engageTarget") || action == QLatin1String("pursue")) {
        return args.value(QStringLiteral("attackerId")).toString();
    }
    return args.value(QStringLiteral("unitId")).toString();
}

bool GameServer::validateCommandOwnership(const ClientSession& session, const QString& action,
                                          const QVariantMap& args, QString* code,
                                          QString* reason) const {
    auto reject = [code, reason](const QString& errorCode, const QString& message) {
        if (code) *code = errorCode;
        if (reason) *reason = message;
        return false;
    };
    static const QSet<QString> actions{
        QStringLiteral("assignTarget"), QStringLiteral("setFlightPlan"),
        QStringLiteral("engageTarget"), QStringLiteral("moveTo"),
        QStringLiteral("withdraw"), QStringLiteral("setSpeed"),
        QStringLiteral("pursue"), QStringLiteral("guideAttack"),
        QStringLiteral("setSchedule"), QStringLiteral("halt")};
    if (!actions.contains(action)) {
        return reject(QStringLiteral("UNKNOWN_ACTION"), QStringLiteral("未知操作"));
    }
    if (session.role == QLatin1String("editor")) {
        return reject(QStringLiteral("PERMISSION_DENIED"),
                      QStringLiteral("编辑席不能下达推演命令"));
    }
    const QString unitId = controlledUnitId(action, args);
    if (unitId.isEmpty() || unitId.size() > Protocol::MaxIdentifierLength) {
        return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("受控单元 ID 无效"));
    }
    UnitBase* unit = m_engine.unit(unitId);
    if (!unit) {
        return reject(QStringLiteral("UNIT_NOT_FOUND"), QStringLiteral("受控单元不存在"));
    }
    if (!unit->alive()) {
        return reject(QStringLiteral("UNIT_DESTROYED"), QStringLiteral("受控单元已摧毁"));
    }
    if (!unit->movable()) {
        return reject(QStringLiteral("UNIT_NOT_MOVABLE"),
                      QStringLiteral("该操作仅适用于可移动单元"));
    }
    const QString side = sideForRole(session.role);
    if (!StateProjector::canControlSide(session.role, unit->sideStr())) {
        return reject(QStringLiteral("UNIT_NOT_OWNED"),
                      QStringLiteral("不能控制其他阵营单元"));
    }
    if (action == QLatin1String("setFlightPlan") && unit->kind() != UnitKind::AttackUAV) {
        return reject(QStringLiteral("INVALID_UNIT_KIND"),
                      QStringLiteral("航路指令仅适用于攻击无人机"));
    }
    if (action == QLatin1String("guideAttack")) {
        UnitBase* attacker = m_engine.unit(args.value(QStringLiteral("attackerId")).toString());
        if (unit->kind() != UnitKind::GroundScout || !attacker || !attacker->alive()
            || attacker->kind() != UnitKind::AttackUAV || attacker->side() != unit->side()
            || (!side.isEmpty() && attacker->sideStr() != side)) {
            return reject(QStringLiteral("INVALID_TARGET"),
                          QStringLiteral("不能为其他阵营攻击单元提供引导"));
        }
    }
    const QString targetId = args.value(QStringLiteral("targetId")).toString();
    if (!side.isEmpty() && !targetId.isEmpty() && !visibleUnitIds(session).contains(targetId)) {
        return reject(QStringLiteral("TARGET_NOT_VISIBLE"),
                      QStringLiteral("目标不在己方当前可见态势中"));
    }
    if (action == QLatin1String("assignTarget") || action == QLatin1String("engageTarget")
        || action == QLatin1String("pursue") || action == QLatin1String("guideAttack")) {
        UnitBase* attacker = action == QLatin1String("guideAttack")
            ? m_engine.unit(args.value(QStringLiteral("attackerId")).toString()) : unit;
        UnitBase* target = m_engine.unit(targetId);
        if (!attacker || attacker->kind() != UnitKind::AttackUAV || !target || !target->alive()
            || attacker->side() == target->side()) {
            return reject(QStringLiteral("INVALID_TARGET"),
                          QStringLiteral("攻击单元或敌方目标无效"));
        }
    }
    auto validPoint = [this](const QVariantMap& point) {
        if (!point.contains(QStringLiteral("x")) || !point.contains(QStringLiteral("y"))) return false;
        const double x = point.value(QStringLiteral("x")).toDouble();
        const double y = point.value(QStringLiteral("y")).toDouble();
        return std::isfinite(x) && std::isfinite(y) && x >= 0.0 && y >= 0.0
            && x <= m_engine.mapInfo().value(QStringLiteral("widthMeters")).toDouble()
            && y <= m_engine.mapInfo().value(QStringLiteral("heightMeters")).toDouble();
    };
    if (action == QLatin1String("moveTo") && !validPoint(args.value(QStringLiteral("pos")).toMap())) {
        return reject(QStringLiteral("INVALID_ARGUMENT"),
                      QStringLiteral("移动目标超出地图边界"));
    }
    if (action == QLatin1String("setFlightPlan")) {
        const QVariantList waypoints = args.value(QStringLiteral("waypoints")).toList();
        if (waypoints.isEmpty() || waypoints.size() > kMaxSchedulePoints) {
            return reject(QStringLiteral("INVALID_ARGUMENT"),
                          QStringLiteral("航路不能为空且不能超过 512 个航点"));
        }
        for (const QVariant& waypoint : waypoints) {
            if (!validPoint(waypoint.toMap())) {
                return reject(QStringLiteral("INVALID_ARGUMENT"),
                              QStringLiteral("航路点超出地图边界"));
            }
        }
    }
    if (action == QLatin1String("guideAttack")
        && !validPoint(args.value(QStringLiteral("targetPos")).toMap())) {
        return reject(QStringLiteral("INVALID_ARGUMENT"),
                      QStringLiteral("引导目标位置无效"));
    }
    if (action == QLatin1String("setSpeed")) {
        bool ok = false;
        const double speed = args.value(QStringLiteral("speed")).toDouble(&ok);
        if (!ok || !std::isfinite(speed) || speed <= 0.0 || speed > 1000.0) {
            return reject(QStringLiteral("INVALID_ARGUMENT"),
                          QStringLiteral("单元速度必须在 0 到 1000 之间"));
        }
    }
    if (action == QLatin1String("setSchedule")) {
        const QVariantList schedule = args.value(QStringLiteral("schedule")).toList();
        if (schedule.size() > kMaxSchedulePoints) {
            return reject(QStringLiteral("INVALID_ARGUMENT"),
                          QStringLiteral("计划点数量过多"));
        }
        for (const QVariant& value : schedule) {
            const QVariantMap point = value.toMap();
            bool timeOk = false;
            const double time = point.value(QStringLiteral("time")).toDouble(&timeOk);
            if (!timeOk || !std::isfinite(time) || time < 0.0 || !validPoint(point)) {
                return reject(QStringLiteral("INVALID_ARGUMENT"),
                              QStringLiteral("计划点时间或位置无效"));
            }
        }
    }
    return true;
}

void GameServer::handleCommand(QWebSocket* socket, const QJsonObject& payload) {
    const QString commandId = payload.value(QStringLiteral("commandId")).toString();
    const QString action = payload.value(QStringLiteral("action")).toString();
    const QVariantMap args = payload.value(QStringLiteral("args")).toObject().toVariantMap();
    ClientSession& session = m_clients[socket];
    if (commandId.isEmpty() || commandId.size() > Protocol::MaxIdentifierLength) {
        sendCommandResult(socket, commandId,
                          CommandResult::reject(QStringLiteral("INVALID_COMMAND_ID"),
                                                QStringLiteral("命令 ID 缺失或过长")));
        return;
    }
    const QString cacheKey = commandCacheKey(session.userId, commandId);
    if (m_commandResults.contains(cacheKey)) {
        sendEnvelope(socket, QStringLiteral("commandResult"),
                     m_commandResults.value(cacheKey));
        return;
    }
    if (m_phase != QLatin1String("running")) {
        sendCommandResult(socket, commandId,
                          CommandResult::reject(QStringLiteral("MATCH_NOT_RUNNING"),
                                                QStringLiteral("推演尚未开始或已经结束")));
        return;
    }
    QString code;
    QString reason;
    if (!validateCommandOwnership(session, action, args, &code, &reason)) {
        sendCommandResult(socket, commandId,
                          CommandResult::reject(code, reason));
        return;
    }
    QString persistenceError;
    if (!recordDurableEvent(QStringLiteral("command"),
                            QJsonObject{{QStringLiteral("commandId"), commandId},
                                        {QStringLiteral("userId"), session.userId},
                                        {QStringLiteral("action"), action},
                                        {QStringLiteral("args"), QJsonObject::fromVariantMap(args)}},
                            &persistenceError)) {
        sendCommandResult(socket, commandId,
                          CommandResult::reject(QStringLiteral("PERSISTENCE_FAILED"),
                                                QStringLiteral("命令日志写入失败，命令未执行: %1")
                                                    .arg(persistenceError)));
        return;
    }
    CommandResult result = m_engine.executeCommand(action, args);
    QJsonObject durableResult = result.toJson();
    durableResult[QStringLiteral("commandId")] = commandId;
    durableResult[QStringLiteral("serverTime")] = m_engine.simTime();
    if (!m_commandResults.contains(cacheKey)) m_commandResultOrder.append(cacheKey);
    m_commandResults.insert(cacheKey, durableResult);
    if (!persistRoomState(&persistenceError)) {
        result = {result.accepted, QStringLiteral("PERSISTENCE_WARNING"),
                  QStringLiteral("%1；检查点写入失败: %2").arg(result.message, persistenceError)};
    }
    sendCommandResult(socket, commandId, result);
}

void GameServer::handleControl(QWebSocket* socket, const QJsonObject& payload) {
    const ClientSession& session = m_clients.value(socket);
    if (session.role != QLatin1String("director")) {
        sendError(socket, QStringLiteral("PERMISSION_DENIED"), QStringLiteral("仅导演席可控制推演进程"));
        return;
    }
    const QString action = payload.value(QStringLiteral("action")).toString();
    auto recordControl = [this, socket, &action](const QJsonObject& details = QJsonObject{}) {
        QJsonObject durablePayload = details;
        durablePayload[QStringLiteral("action")] = action;
        QString error;
        if (recordDurableEvent(QStringLiteral("control"), durablePayload, &error)) return true;
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"),
                  QStringLiteral("控制命令日志写入失败，操作未执行: %1").arg(error));
        return false;
    };
    if (action == QLatin1String("start")) {
        if (m_phase != QLatin1String("preparing") || !m_redReady || !m_blueReady || !m_engine.readyForSim()) {
            sendError(socket, QStringLiteral("NOT_READY"), QStringLiteral("红蓝双方均就绪且场景有效后才能开始"));
            return;
        }
        if (!recordControl()) return;
        m_runInitialScenario = m_engine.scenario();
        m_phase = QStringLiteral("running");
        m_engine.setRunning(true);
        broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("matchStarted")},
                                   {QStringLiteral("message"), QStringLiteral("导演已开启推演")}});
    } else if (action == QLatin1String("pause")) {
        if (m_phase != QLatin1String("running") || !m_engine.running()) {
            sendError(socket, QStringLiteral("INVALID_PHASE"), QStringLiteral("当前推演不能暂停"));
            return;
        }
        if (!recordControl()) return;
        m_engine.setRunning(false);
    } else if (action == QLatin1String("resume")) {
        if (m_phase != QLatin1String("running") || m_engine.running()) {
            sendError(socket, QStringLiteral("INVALID_PHASE"), QStringLiteral("当前推演不能继续"));
            return;
        }
        if (!recordControl()) return;
        m_engine.setRunning(true);
    } else if (action == QLatin1String("speed")) {
        if (m_phase != QLatin1String("running")) {
            sendError(socket, QStringLiteral("MATCH_NOT_RUNNING"), QStringLiteral("推演未进行，不能调整速率"));
            return;
        }
        const QJsonValue speedValue = payload.value(QStringLiteral("speed"));
        const double speed = speedValue.toDouble(-1.0);
        if (!speedValue.isDouble() || !std::isfinite(speed) || speed < 0.0 || speed > 8.0) {
            sendError(socket, QStringLiteral("INVALID_SPEED"), QStringLiteral("推演速率必须在 0 到 8 之间"));
            return;
        }
        if (!recordControl(QJsonObject{{QStringLiteral("speed"), speed}})) return;
        m_engine.setSpeedMul(speed);
    } else if (action == QLatin1String("step")) {
        if (m_phase != QLatin1String("running") || m_engine.running()) {
            sendError(socket, QStringLiteral("INVALID_PHASE"), QStringLiteral("只能在暂停状态下单步推进"));
            return;
        }
        if (!recordControl()) return;
        m_engine.stepOnce(1.0);
    } else if (action == QLatin1String("end")) {
        if (m_phase == QLatin1String("preparing") || m_runInitialScenario.units.empty()) {
            sendError(socket, QStringLiteral("INVALID_PHASE"), QStringLiteral("当前没有可结束的推演"));
            return;
        }
        if (!recordControl()) return;
        m_engine.setRunning(false);
        if (!m_engine.setScenario(m_runInitialScenario)) {
            sendError(socket, QStringLiteral("RESET_FAILED"),
                      QStringLiteral("无法恢复开局场景: %1").arg(m_engine.lastError()));
            return;
        }
        m_phase = QStringLiteral("preparing");
        resetReadiness();
        ++m_scenarioRevision;
        persistScenario();
        broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("matchReset")},
                                   {QStringLiteral("message"), QStringLiteral("推演已结束，事件时间与双方单位已恢复到开局状态")}});
    } else {
        sendError(socket, QStringLiteral("UNKNOWN_CONTROL"), QStringLiteral("未知推演控制操作"));
        return;
    }
    QString checkpointError;
    if (!persistRoomState(&checkpointError)) {
        broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("serverWarning")},
                                   {QStringLiteral("message"),
                                    QStringLiteral("控制操作已执行，但检查点写入失败: %1")
                                        .arg(checkpointError)}});
    }
    broadcastSnapshots(action == QLatin1String("end"));
}

void GameServer::handleReady(QWebSocket* socket, const QJsonObject& payload) {
    const ClientSession& session = m_clients.value(socket);
    if (m_phase != QLatin1String("preparing")
        || (session.role != QLatin1String("red") && session.role != QLatin1String("blue"))) {
        sendError(socket, QStringLiteral("PERMISSION_DENIED"), QStringLiteral("当前席位或阶段不能提交就绪状态"));
        return;
    }
    if (!payload.value(QStringLiteral("ready")).isBool()) {
        sendError(socket, QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("就绪状态必须是布尔值"));
        return;
    }
    const bool ready = payload.value(QStringLiteral("ready")).toBool();
    if (ready && !m_engine.readyForSim()) {
        sendError(socket, QStringLiteral("SCENARIO_INVALID"), QStringLiteral("双方必须各有且仅有一个存活指挥所"));
        return;
    }
    QString persistenceError;
    if (!recordDurableEvent(QStringLiteral("ready"),
                            QJsonObject{{QStringLiteral("role"), session.role},
                                        {QStringLiteral("ready"), ready}},
                            &persistenceError)) {
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"),
                  QStringLiteral("就绪状态日志写入失败，操作未执行: %1").arg(persistenceError));
        return;
    }
    if (session.role == QLatin1String("red")) m_redReady = ready;
    else m_blueReady = ready;
    broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("readiness")},
                               {QStringLiteral("message"), QStringLiteral("%1已%2就绪")
                                    .arg(session.role == QLatin1String("red") ? QStringLiteral("红方") : QStringLiteral("蓝方"),
                                         ready ? QString() : QStringLiteral("取消"))}});
    if (!persistRoomState(&persistenceError)) {
        broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("serverWarning")},
                                   {QStringLiteral("message"),
                                    QStringLiteral("就绪状态已更新，但检查点写入失败: %1")
                                        .arg(persistenceError)}});
    }
    broadcastSnapshots();
}

void GameServer::handleChat(QWebSocket* socket, const QJsonObject& payload) {
    ClientSession& session = m_clients[socket];
    QString text = payload.value(QStringLiteral("text")).toString().trimmed();
    if (text.isEmpty()) return;
    if (text.size() > 500) text.truncate(500);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - session.lastChatAt < 600) {
        sendError(socket, QStringLiteral("CHAT_RATE_LIMIT"), QStringLiteral("消息发送过快"));
        return;
    }
    session.lastChatAt = now;
    const QJsonObject message{
        {QStringLiteral("id"), QStringLiteral("chat_%1").arg(++m_chatSequence)},
        {QStringLiteral("username"), session.username},
        {QStringLiteral("displayName"), session.displayName},
        {QStringLiteral("role"), session.role},
        {QStringLiteral("text"), text},
        {QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    m_chatHistory.append(message);
    while (m_chatHistory.size() > 100) m_chatHistory.removeFirst();
    broadcastChat(message);
}

bool GameServer::canEditSide(const ClientSession& session, const QString& side) const {
    return StateProjector::canEditSide(session.role, side);
}

void GameServer::handleScenarioUpsert(QWebSocket* socket, const QJsonObject& payload) {
    const ClientSession& session = m_clients.value(socket);
    if (m_phase != QLatin1String("preparing")) {
        sendError(socket, QStringLiteral("SCENARIO_LOCKED"), QStringLiteral("推演开始后不能修改初始阵容"));
        return;
    }
    QJsonObject data = payload.value(QStringLiteral("unit")).toObject();
    const QString existingId = data.value(QStringLiteral("id")).toString();
    if (existingId.isEmpty()) {
        data[QStringLiteral("id")] = QStringLiteral("u_%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
    }
    const QString side = data.value(QStringLiteral("side")).toString();
    if (!canEditSide(session, side)) {
        sendError(socket, QStringLiteral("UNIT_NOT_OWNED"), QStringLiteral("不能创建或修改其他阵营单位"));
        return;
    }
    if (UnitBase* existing = m_engine.unit(data.value(QStringLiteral("id")).toString());
        existing && !canEditSide(session, existing->sideStr())) {
        sendError(socket, QStringLiteral("UNIT_NOT_OWNED"), QStringLiteral("不能修改其他阵营单位"));
        return;
    }
    Scenario candidate = m_engine.scenario();
    const ScenarioUnit update = scenarioUnitFromJson(data);
    auto it = std::find_if(candidate.units.begin(), candidate.units.end(),
                           [&update](const ScenarioUnit& unit) { return unit.id == update.id; });
    if (it == candidate.units.end()) candidate.units.push_back(update);
    else *it = update;
    const QString networkValidationError = validateNetworkScenario(candidate);
    if (!networkValidationError.isEmpty()) {
        sendError(socket, QStringLiteral("INVALID_SCENARIO"), networkValidationError);
        return;
    }
    QString persistenceError;
    if (!recordDurableEvent(QStringLiteral("scenario"),
                            QJsonObject{{QStringLiteral("scenario"), ScenarioIo::toJson(candidate)},
                                        {QStringLiteral("scenarioRevision"),
                                         static_cast<qint64>(m_scenarioRevision + 1)}},
                            &persistenceError)) {
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"),
                  QStringLiteral("场景日志写入失败，修改未应用: %1").arg(persistenceError));
        return;
    }
    if (!m_engine.setScenario(candidate)) {
        sendError(socket, QStringLiteral("INVALID_SCENARIO"), m_engine.lastError());
        return;
    }
    scenarioChanged();
}

void GameServer::handleScenarioRemove(QWebSocket* socket, const QJsonObject& payload) {
    const ClientSession& session = m_clients.value(socket);
    if (m_phase != QLatin1String("preparing")) {
        sendError(socket, QStringLiteral("SCENARIO_LOCKED"), QStringLiteral("推演开始后不能修改初始阵容"));
        return;
    }
    const QString id = payload.value(QStringLiteral("unitId")).toString();
    UnitBase* unit = m_engine.unit(id);
    if (!unit || !canEditSide(session, unit->sideStr())) {
        sendError(socket, QStringLiteral("UNIT_NOT_OWNED"), QStringLiteral("不能删除其他阵营单位"));
        return;
    }
    Scenario candidate = m_engine.scenario();
    std::erase_if(candidate.units, [&id](const ScenarioUnit& item) { return item.id == id; });
    if (candidate.units.empty()) {
        sendError(socket, QStringLiteral("INVALID_SCENARIO"), QStringLiteral("场景至少需要保留一个单位"));
        return;
    }
    QString persistenceError;
    if (!recordDurableEvent(QStringLiteral("scenario"),
                            QJsonObject{{QStringLiteral("scenario"), ScenarioIo::toJson(candidate)},
                                        {QStringLiteral("scenarioRevision"),
                                         static_cast<qint64>(m_scenarioRevision + 1)}},
                            &persistenceError)) {
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"),
                  QStringLiteral("场景日志写入失败，修改未应用: %1").arg(persistenceError));
        return;
    }
    if (!m_engine.setScenario(candidate)) {
        sendError(socket, QStringLiteral("INVALID_SCENARIO"), m_engine.lastError());
        return;
    }
    scenarioChanged();
}

void GameServer::handleScenarioReplace(QWebSocket* socket, const QJsonObject& payload) {
    const ClientSession& session = m_clients.value(socket);
    if (m_phase != QLatin1String("preparing") || session.role != QLatin1String("editor")) {
        sendError(socket, QStringLiteral("PERMISSION_DENIED"), QStringLiteral("仅编辑席可在准备阶段替换完整场景"));
        return;
    }
    const QJsonObject scenarioObject = payload.value(QStringLiteral("scenario")).toObject();
    if (scenarioObject.value(QStringLiteral("schemaVersion")).toInt()
        != ScenarioIo::SchemaVersion) {
        sendError(socket, QStringLiteral("SCHEMA_MISMATCH"),
                  QStringLiteral("场景结构版本不兼容"));
        return;
    }
    const Scenario candidate = ScenarioIo::fromJson(scenarioObject);
    const QString networkValidationError = validateNetworkScenario(candidate);
    if (!networkValidationError.isEmpty()) {
        sendError(socket, QStringLiteral("INVALID_SCENARIO"), networkValidationError);
        return;
    }
    QString persistenceError;
    if (!recordDurableEvent(QStringLiteral("scenario"),
                            QJsonObject{{QStringLiteral("scenario"), ScenarioIo::toJson(candidate)},
                                        {QStringLiteral("scenarioRevision"),
                                         static_cast<qint64>(m_scenarioRevision + 1)}},
                            &persistenceError)) {
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"),
                  QStringLiteral("场景日志写入失败，修改未应用: %1").arg(persistenceError));
        return;
    }
    if (!m_engine.setScenario(candidate)) {
        sendError(socket, QStringLiteral("INVALID_SCENARIO"), m_engine.lastError());
        return;
    }
    scenarioChanged();
}

void GameServer::scenarioChanged() {
    ++m_scenarioRevision;
    resetReadiness();
    QString persistError;
    if (!persistScenario(&persistError)) {
        broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("serverWarning")},
                                   {QStringLiteral("message"), QStringLiteral("场景已更新，但持久化失败: %1").arg(persistError)}});
    }
    if (!persistRoomState(&persistError)) {
        broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("serverWarning")},
                                   {QStringLiteral("message"),
                                    QStringLiteral("场景已更新，但检查点写入失败: %1").arg(persistError)}});
    }
    broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("scenarioChanged")},
                               {QStringLiteral("message"), QStringLiteral("初始阵容已更新，双方就绪状态已重置")}});
    broadcastSnapshots(true);
}

bool GameServer::persistScenario(QString* error) {
    QString localError;
    const bool saved = ScenarioIo::saveToFile(m_engine.scenario(), m_scenarioPath, &localError);
    if (!saved) qWarning() << "场景持久化失败" << localError;
    if (error) *error = localError;
    return saved;
}

bool GameServer::recordDurableEvent(const QString& kind, const QJsonObject& payload,
                                    QString* error) {
    const quint64 nextSequence = m_eventSequence + 1;
    if (!m_persistence.appendEvent(nextSequence, kind, payload, error)) return false;
    m_eventSequence = nextSequence;
    return true;
}

bool GameServer::persistRoomState(QString* error) {
    RoomCheckpoint checkpoint;
    checkpoint.scenario = m_engine.scenario();
    checkpoint.runInitialScenario = m_runInitialScenario;
    checkpoint.runtimeUnits = m_engine.collectCheckpointState();
    for (const QString& key : m_commandResultOrder) {
        checkpoint.commandHistory.append(
            QJsonObject{{QStringLiteral("key"), key},
                        {QStringLiteral("result"), m_commandResults.value(key)}});
    }
    checkpoint.phase = m_phase;
    checkpoint.redReady = m_redReady;
    checkpoint.blueReady = m_blueReady;
    checkpoint.running = m_engine.running();
    checkpoint.simTime = m_engine.simTime();
    checkpoint.speed = m_engine.speedMul();
    checkpoint.scenarioRevision = m_scenarioRevision;
    checkpoint.stateRevision = m_stateRevision;
    checkpoint.eventSequence = m_eventSequence;
    return m_persistence.saveCheckpoint(checkpoint, error);
}

bool GameServer::restoreRoomState(QString* error) {
    if (error) error->clear();
    if (!QFileInfo::exists(m_persistence.checkpointPath())) {
        if (error) *error = QStringLiteral("尚无房间检查点");
        return false;
    }
    RoomCheckpoint checkpoint;
    if (!m_persistence.loadCheckpoint(&checkpoint, error)) return false;
    const QString scenarioError = validateNetworkScenario(checkpoint.scenario);
    if (!scenarioError.isEmpty()) {
        if (error) *error = QStringLiteral("检查点场景无效: %1").arg(scenarioError);
        return false;
    }
    if ((checkpoint.phase == QLatin1String("preparing") && checkpoint.running)
        || (checkpoint.phase == QLatin1String("finished") && checkpoint.running)) {
        if (error) *error = QStringLiteral("检查点阶段与运行状态冲突");
        return false;
    }
    if (!m_engine.setScenario(checkpoint.scenario)) {
        if (error) *error = m_engine.lastError();
        return false;
    }
    QString runtimeError;
    if (!m_engine.restoreCheckpointState(checkpoint.runtimeUnits, checkpoint.simTime,
                                         checkpoint.running, checkpoint.speed,
                                         &runtimeError)) {
        if (error) *error = QStringLiteral("运行态恢复失败: %1").arg(runtimeError);
        return false;
    }
    m_runInitialScenario = checkpoint.runInitialScenario;
    m_phase = checkpoint.phase;
    m_redReady = checkpoint.redReady;
    m_blueReady = checkpoint.blueReady;
    m_scenarioRevision = checkpoint.scenarioRevision;
    m_stateRevision = checkpoint.stateRevision;
    m_eventSequence = checkpoint.eventSequence;
    m_commandResults.clear();
    m_commandResultOrder.clear();
    if (checkpoint.commandHistory.size() > 2048) {
        if (error) *error = QStringLiteral("检查点命令幂等记录过多");
        return false;
    }
    for (const QJsonValue& value : checkpoint.commandHistory) {
        const QJsonObject entry = value.toObject();
        const QString key = entry.value(QStringLiteral("key")).toString();
        const QJsonObject result = entry.value(QStringLiteral("result")).toObject();
        if (key.isEmpty() || result.value(QStringLiteral("commandId")).toString().isEmpty()) {
            if (error) *error = QStringLiteral("检查点命令幂等记录无效");
            return false;
        }
        m_commandResultOrder.append(key);
        m_commandResults.insert(key, result);
    }
    if (!replayDurableEvents(error)) return false;
    persistScenario();
    return persistRoomState(error);
}

bool GameServer::replayDurableEvents(QString* error) {
    QString readError;
    const QJsonArray events = m_persistence.eventsAfter(m_eventSequence, &readError);
    if (!readError.isEmpty()) {
        if (error) *error = readError;
        return false;
    }
    for (const QJsonValue& value : events) {
        const QJsonObject event = value.toObject();
        const quint64 sequence = static_cast<quint64>(
            event.value(QStringLiteral("sequence")).toInteger());
        QString applyError;
        if (!applyDurableEvent(event.value(QStringLiteral("kind")).toString(),
                               event.value(QStringLiteral("payload")).toObject(),
                               &applyError)) {
            if (error) {
                *error = QStringLiteral("重放事件 %1 失败: %2").arg(sequence).arg(applyError);
            }
            return false;
        }
        m_eventSequence = sequence;
        ++m_stateRevision;
    }
    return true;
}

bool GameServer::applyDurableEvent(const QString& kind, const QJsonObject& payload,
                                   QString* error) {
    if (error) error->clear();
    if (kind == QLatin1String("command")) {
        const CommandResult result = m_engine.executeCommand(
            payload.value(QStringLiteral("action")).toString(),
            payload.value(QStringLiteral("args")).toObject().toVariantMap());
        const QString commandId = payload.value(QStringLiteral("commandId")).toString();
        const qint64 userId = payload.value(QStringLiteral("userId")).toInteger();
        if (!commandId.isEmpty() && userId > 0) {
            QJsonObject resultPayload = result.toJson();
            resultPayload[QStringLiteral("commandId")] = commandId;
            resultPayload[QStringLiteral("serverTime")] = m_engine.simTime();
            const QString key = commandCacheKey(userId, commandId);
            if (!m_commandResults.contains(key)) m_commandResultOrder.append(key);
            m_commandResults.insert(key, resultPayload);
        }
        return true;
    }
    if (kind == QLatin1String("ready")) {
        const QString role = payload.value(QStringLiteral("role")).toString();
        if (role != QLatin1String("red") && role != QLatin1String("blue")) {
            if (error) *error = QStringLiteral("就绪事件阵营无效");
            return false;
        }
        if (role == QLatin1String("red")) m_redReady = payload.value(QStringLiteral("ready")).toBool();
        else m_blueReady = payload.value(QStringLiteral("ready")).toBool();
        return true;
    }
    if (kind == QLatin1String("scenario")) {
        const Scenario scenario = ScenarioIo::fromJson(
            payload.value(QStringLiteral("scenario")).toObject());
        const qint64 revision = payload.value(QStringLiteral("scenarioRevision")).toInteger();
        const QString validationError = validateNetworkScenario(scenario);
        if (!validationError.isEmpty() || revision <= 0 || !m_engine.setScenario(scenario)) {
            if (error) *error = validationError.isEmpty() ? m_engine.lastError() : validationError;
            return false;
        }
        m_scenarioRevision = static_cast<quint64>(revision);
        resetReadiness();
        return true;
    }
    if (kind == QLatin1String("control")) {
        const QString action = payload.value(QStringLiteral("action")).toString();
        if (action == QLatin1String("start")) {
            m_runInitialScenario = m_engine.scenario();
            m_phase = QStringLiteral("running");
            m_engine.setRunning(true);
        } else if (action == QLatin1String("pause")) {
            m_engine.setRunning(false);
        } else if (action == QLatin1String("resume")) {
            m_phase = QStringLiteral("running");
            m_engine.setRunning(true);
        } else if (action == QLatin1String("speed")) {
            const double speed = payload.value(QStringLiteral("speed")).toDouble(-1.0);
            if (!std::isfinite(speed) || speed < 0.0 || speed > 8.0) {
                if (error) *error = QStringLiteral("速率事件参数无效");
                return false;
            }
            m_engine.setSpeedMul(speed);
        } else if (action == QLatin1String("step")) {
            m_engine.setRunning(false);
            m_engine.stepOnce(1.0);
        } else if (action == QLatin1String("end")) {
            if (m_runInitialScenario.units.empty() || !m_engine.setScenario(m_runInitialScenario)) {
                if (error) *error = QStringLiteral("结束事件缺少开局场景");
                return false;
            }
            m_phase = QStringLiteral("preparing");
            resetReadiness();
            ++m_scenarioRevision;
        } else {
            if (error) *error = QStringLiteral("未知控制事件: %1").arg(action);
            return false;
        }
        return true;
    }
    if (error) *error = QStringLiteral("未知持久化事件类型: %1").arg(kind);
    return false;
}

void GameServer::resetReadiness() {
    m_redReady = false;
    m_blueReady = false;
}

QJsonObject GameServer::roomState() const {
    QJsonObject online{{QStringLiteral("director"), 0}, {QStringLiteral("editor"), 0},
                       {QStringLiteral("red"), 0}, {QStringLiteral("blue"), 0}};
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (!it->authenticated) continue;
        online[it->role] = online.value(it->role).toInt() + 1;
    }
    return QJsonObject{{QStringLiteral("phase"), m_phase},
                       {QStringLiteral("redReady"), m_redReady},
                       {QStringLiteral("blueReady"), m_blueReady},
                       {QStringLiteral("running"), m_engine.running()},
                       {QStringLiteral("readyForSim"), m_engine.readyForSim()},
                       {QStringLiteral("cpIssues"), m_engine.cpIssues()},
                       {QStringLiteral("simTime"), m_engine.simTime()},
                       {QStringLiteral("speed"), m_engine.speedMul()},
                       {QStringLiteral("scenarioRevision"), static_cast<qint64>(m_scenarioRevision)},
                       {QStringLiteral("stateRevision"), static_cast<qint64>(m_stateRevision)},
                       {QStringLiteral("online"), online}};
}

QString GameServer::messageSummary(const QString& type, const QJsonObject& payload) const {
    QString summary;
    if (type == QLatin1String("chat")) summary = payload.value(QStringLiteral("text")).toString();
    else if (payload.contains(QStringLiteral("action"))) summary = payload.value(QStringLiteral("action")).toString();
    else if (payload.contains(QStringLiteral("kind"))) summary = payload.value(QStringLiteral("kind")).toString();
    else if (payload.contains(QStringLiteral("code"))) summary = payload.value(QStringLiteral("code")).toString();
    else summary = payload.value(QStringLiteral("message")).toString();
    summary.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return summary.left(160);
}

void GameServer::audit(const QString& category, const QJsonObject& detail) {
    QFileInfo info(m_monitorLogPath);
    QDir().mkpath(info.absolutePath());
    QFile file(m_monitorLogPath);
    const QJsonObject entry{{QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                            {QStringLiteral("category"), category},
                            {QStringLiteral("detail"), detail}};
    const QByteArray line = QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
    const QIODevice::OpenMode mode = info.exists() && info.size() > 1024 * 1024
        ? QIODevice::WriteOnly | QIODevice::Truncate : QIODevice::WriteOnly | QIODevice::Append;
    if (!file.open(mode)) {
        qWarning() << "无法写入服务器监控日志" << m_monitorLogPath;
        return;
    }
    file.write(line);
}

void GameServer::writeMonitorStatus() {
    QFileInfo info(m_monitorStatusPath);
    QDir().mkpath(info.absolutePath());
    QSaveFile file(m_monitorStatusPath);
    if (!file.open(QIODevice::WriteOnly)) return;
    const QJsonObject status{{QStringLiteral("status"), QStringLiteral("healthy")},
                             {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                             {QStringLiteral("connectedClients"), m_clients.size()},
                             {QStringLiteral("roomState"), roomState()}};
    file.write(QJsonDocument(status).toJson(QJsonDocument::Compact));
    file.commit();
}

QSet<QString> GameServer::visibleUnitIds(const ClientSession& session) const {
    return StateProjector::visibleUnitIds(m_engine, session.role);
}

QJsonArray GameServer::filteredMessages(const ClientSession& session) const {
    return StateProjector::filteredMessages(m_engine, session.role);
}

QJsonObject GameServer::snapshotFor(const ClientSession& session) const {
    return StateProjector::snapshotFor(m_engine, session.role, m_stateRevision, roomState());
}

void GameServer::sendEnvelope(QWebSocket* socket, const QString& type, const QJsonObject& payload) {
    if (!socket || socket->state() != QAbstractSocket::ConnectedState || !m_clients.contains(socket)) return;
    if (socket->bytesToWrite() > kMaxPendingBytes) {
        socket->close(QWebSocketProtocol::CloseCodeTooMuchData,
                      QStringLiteral("客户端接收速度过慢"));
        return;
    }
    ClientSession& session = m_clients[socket];
    const QJsonObject envelope = Protocol::makeServerEnvelope(type, ++session.sequence, payload);
    const QByteArray encoded = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (encoded.size() > Protocol::MaxServerMessageBytes) {
        audit(QStringLiteral("security"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("outgoingMessageTooLarge")},
                          {QStringLiteral("type"), type},
                          {QStringLiteral("bytes"), encoded.size()},
                          {QStringLiteral("user"), session.username}});
        socket->close(QWebSocketProtocol::CloseCodeTooMuchData,
                      QStringLiteral("服务器状态快照超过大小限制"));
        return;
    }
    socket->sendTextMessage(QString::fromUtf8(encoded));
    if (type != QLatin1String("snapshot") && type != QLatin1String("delta")
        && type != QLatin1String("pong")) {
        audit(QStringLiteral("message"), QJsonObject{{QStringLiteral("direction"), QStringLiteral("out")},
                                                       {QStringLiteral("type"), type},
                                                       {QStringLiteral("user"), session.username},
                                                       {QStringLiteral("role"), session.role},
                                                       {QStringLiteral("summary"), messageSummary(type, payload)}});
    }
}

void GameServer::sendError(QWebSocket* socket, const QString& code, const QString& message,
                           const QString& requestId) {
    if (code == QLatin1String("PERMISSION_DENIED")
        || code == QLatin1String("UNIT_NOT_OWNED")
        || code == QLatin1String("TARGET_NOT_VISIBLE")
        || code == QLatin1String("MESSAGE_RATE_LIMIT")
        || code == QLatin1String("DUPLICATE_MESSAGE")) {
        const ClientSession session = m_clients.value(socket);
        audit(QStringLiteral("security"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("requestRejected")},
                          {QStringLiteral("code"), code},
                          {QStringLiteral("user"), session.username},
                          {QStringLiteral("role"), session.role},
                          {QStringLiteral("requestId"), requestId}});
    }
    sendEnvelope(socket, QStringLiteral("error"),
                 QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message},
                             {QStringLiteral("requestId"), requestId}});
}

void GameServer::sendCommandResult(QWebSocket* socket, const QString& commandId,
                                   const CommandResult& result) {
    QJsonObject payload = result.toJson();
    payload[QStringLiteral("commandId")] = commandId;
    payload[QStringLiteral("serverTime")] = m_engine.simTime();
    if (socket && m_clients.contains(socket) && !commandId.isEmpty()) {
        const QString key = commandCacheKey(m_clients.value(socket).userId, commandId);
        if (!m_commandResults.contains(key)) m_commandResultOrder.append(key);
        m_commandResults.insert(key, payload);
        while (m_commandResultOrder.size() > 2048) {
            m_commandResults.remove(m_commandResultOrder.takeFirst());
        }
    }
    if (!result.accepted && socket && m_clients.contains(socket)) {
        const ClientSession& session = m_clients.value(socket);
        audit(QStringLiteral("security"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("commandRejected")},
                          {QStringLiteral("code"), result.code},
                          {QStringLiteral("commandId"), commandId},
                          {QStringLiteral("user"), session.username},
                          {QStringLiteral("role"), session.role}});
    }
    sendEnvelope(socket, QStringLiteral("commandResult"), payload);
}

void GameServer::sendFullSnapshot(QWebSocket* socket) {
    if (!socket || !m_clients.contains(socket)) return;
    ClientSession& session = m_clients[socket];
    const QJsonObject snapshot = snapshotFor(session);
    sendEnvelope(socket, QStringLiteral("snapshot"), snapshot);
    session.lastSnapshot = snapshot;
}

void GameServer::broadcastSnapshots(bool forceFull) {
    ++m_stateRevision;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (!it->authenticated) continue;
        const QJsonObject current = snapshotFor(it.value());
        if (!forceFull && StateDelta::canCreate(it->lastSnapshot, current)) {
            const QJsonObject delta = StateDelta::create(it->lastSnapshot, current);
            sendEnvelope(it.key(), QStringLiteral("delta"), delta);
        } else {
            sendEnvelope(it.key(), QStringLiteral("snapshot"), current);
        }
        it->lastSnapshot = current;
    }
}

void GameServer::broadcastEvent(const QJsonObject& event, const QString& side) {
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (!it->authenticated) continue;
        if (!side.isEmpty() && (it->role == QLatin1String("red") || it->role == QLatin1String("blue"))
            && it->role != side) continue;
        const QJsonObject projected = StateProjector::projectEvent(m_engine, it->role, event);
        if (!projected.isEmpty()) sendEnvelope(it.key(), QStringLiteral("event"), projected);
    }
}

void GameServer::broadcastChat(const QJsonObject& message) {
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->authenticated) sendEnvelope(it.key(), QStringLiteral("chat"), message);
    }
}

} // namespace gbr
