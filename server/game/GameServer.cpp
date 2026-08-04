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
#include <QRandomGenerator>
#include <QSet>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace gbr {

namespace {

constexpr qsizetype kMaxScenarioUnits = 512;
constexpr qsizetype kMaxSchedulePoints = 512;
constexpr int kMaxMessagesPerSecond = 60;
constexpr int kMaxMapMarksPerSecond = 8;
constexpr qint64 kMaxPendingBytes = 1024 * 1024;
constexpr qint64 kDdsTicketLifetimeMs = 120000;

QString env(const char* name, const QString& fallback) {
    const QString value = qEnvironmentVariable(name).trimmed();
    return value.isEmpty() ? fallback : value;
}

QString configuredDataDir() {
    if (qEnvironmentVariableIsSet("DATA_DIR")) {
        return env("DATA_DIR", QStringLiteral("/data"));
    }
    const QString explicitCheckpoint = qEnvironmentVariable("CHECKPOINT_PATH").trimmed();
    if (!explicitCheckpoint.isEmpty()
        && QFileInfo(explicitCheckpoint).absoluteFilePath().startsWith(
            QFileInfo(QDir::tempPath()).absoluteFilePath() + QLatin1Char('/'))) {
        return QFileInfo(explicitCheckpoint).absolutePath();
    }
    return QStringLiteral("/data");
}

bool validateAuthServiceUrl(const QString& input, QString* normalized, QString* error) {
    const QUrl url(input.trimmed());
    if (!url.isValid() || url.isRelative()
        || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
        || url.host().isEmpty() || !url.userInfo().isEmpty()
        || !url.query().isEmpty() || !url.fragment().isEmpty()) {
        if (error) *error = QStringLiteral("AUTH_SERVICE_URL 必须是无用户信息、查询参数和片段的 http/https 绝对 URL");
        return false;
    }
    const QString path = url.path();
    if (path.contains(QStringLiteral(".."))) {
        if (error) *error = QStringLiteral("AUTH_SERVICE_URL 基础路径不能包含目录穿越");
        return false;
    }
    QUrl canonical = url;
    canonical.setPath(QDir::cleanPath(path.isEmpty() ? QStringLiteral("/") : path));
    QString value = canonical.toString(QUrl::FullyEncoded);
    while (value.endsWith(QLatin1Char('/'))) value.chop(1);
    if (normalized) *normalized = value;
    return true;
}

bool isPlaceholderInternalKey(const QString& key) {
    const QString normalized = key.trimmed().toLower();
    return normalized == QLatin1String("change-this-internal-key")
        || normalized == QLatin1String("replace-with-a-long-random-internal-key")
        || normalized == QLatin1String("replace-me");
}

QString sideForRole(const QString& role) {
    return StateProjector::sideForRole(role);
}

QString commandCacheKey(qint64 userId, const QString& commandId) {
    return QStringLiteral("%1:%2").arg(userId).arg(commandId);
}

struct SeatDescriptor {
    QString baseId;
    QString side;
    QString type;
    int index = 1;
};

SeatDescriptor describeSeat(const QString& seatId) {
    SeatDescriptor result;
    const QStringList parts = seatId.split(QLatin1Char('_'));
    if (parts.size() >= 2) {
        result.side = parts.value(0);
        result.type = parts.value(1);
        result.baseId = result.side + QLatin1Char('_') + result.type;
    }
    if (parts.size() >= 3) {
        bool ok = false;
        const int parsed = parts.last().toInt(&ok);
        if (ok && parsed > 0) result.index = parsed;
    }
    if (result.type == QLatin1String("commander")) result.index = 1;
    return result;
}

QString canonicalSeatId(const QString& baseId, int index) {
    const SeatDescriptor descriptor = describeSeat(baseId);
    return descriptor.type == QLatin1String("commander")
        ? descriptor.baseId : QStringLiteral("%1_%2").arg(descriptor.baseId).arg(index);
}

QString seatParameterKey(const QString& seatId) {
    const SeatDescriptor descriptor = describeSeat(seatId);
    return descriptor.baseId;
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
            || !std::isfinite(unit.pos.alt)
            || unit.pos.x < 0.0 || unit.pos.y < 0.0
            || unit.pos.x > scenario.map.widthMeters || unit.pos.y > scenario.map.heightMeters) {
            return QStringLiteral("单元位置超出地图边界: %1").arg(unit.id);
        }
        if (!std::isfinite(unit.detectRange) || unit.detectRange < 0.0
            || !std::isfinite(unit.attackRange) || unit.attackRange < 0.0
            || !std::isfinite(unit.commRange) || unit.commRange < 0.0
            || !std::isfinite(unit.speed) || unit.speed < 0.0
            || !std::isfinite(unit.maxHp) || unit.maxHp <= 0.0
            || !std::isfinite(unit.attackPower) || unit.attackPower < 0.0
            || !std::isfinite(unit.armor) || unit.armor < 0.0 || unit.armor > 0.9
            || !std::isfinite(unit.repairRate) || unit.repairRate < 0.0
            || !std::isfinite(unit.subsystemRepairRate)
            || unit.subsystemRepairRate < 0.0) {
            return QStringLiteral("单元参数无效: %1").arg(unit.id);
        }
        if (unit.kind == QLatin1String("attackuav")) {
            const bool validWeapon = unit.ammoCapacity >= 0 && unit.ammoCapacity <= 100000
                && unit.initialAmmo >= 0 && unit.initialAmmo <= unit.ammoCapacity
                && std::isfinite(unit.hitProbability) && unit.hitProbability >= 0.0
                && unit.hitProbability <= 1.0
                && std::isfinite(unit.minAttackRange) && unit.minAttackRange >= 0.0
                && std::isfinite(unit.optimalRange)
                && unit.optimalRange >= unit.minAttackRange
                && unit.optimalRange <= unit.attackRange
                && std::isfinite(unit.cooldownSec) && unit.cooldownSec >= 0.0
                && std::isfinite(unit.damageMin) && unit.damageMin >= 0.0
                && std::isfinite(unit.damageMax) && unit.damageMax >= unit.damageMin
                && std::isfinite(unit.rangeFalloff) && unit.rangeFalloff >= 0.0
                && unit.rangeFalloff <= 1.0
                && std::isfinite(unit.fuelCapacitySec) && unit.fuelCapacitySec > 0.0
                && std::isfinite(unit.initialFuelSec) && unit.initialFuelSec >= 0.0
                && unit.initialFuelSec <= unit.fuelCapacitySec
                && std::isfinite(unit.rearmDurationSec) && unit.rearmDurationSec >= 0.0;
            if (!validWeapon) {
                return QStringLiteral("攻击单元武器参数无效: %1").arg(unit.id);
            }
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
      m_server(QStringLiteral("兵棋推演联网服务器"), QWebSocketServer::NonSecureMode, this),
      m_dataDir(configuredDataDir()),
      m_authServiceUrl(env("AUTH_SERVICE_URL", QStringLiteral("http://account-web:8080"))),
      m_internalKey(env("INTERNAL_API_KEY", {})),
      m_scenarioPath(env("SCENARIO_PATH", QDir(m_dataDir).filePath(QStringLiteral("scenario.json")))),
      m_monitorLogPath(env("MONITOR_LOG_PATH", QDir(m_dataDir).filePath(QStringLiteral("game-events.jsonl")))),
      m_monitorStatusPath(env("MONITOR_STATUS_PATH", QDir(m_dataDir).filePath(QStringLiteral("game-status.json")))),
      m_persistence(env("CHECKPOINT_PATH", QDir(m_dataDir).filePath(QStringLiteral("room-checkpoint.json"))),
                    env("COMMAND_LOG_PATH", QDir(m_dataDir).filePath(QStringLiteral("room-commands.jsonl"))), m_dataDir),
      m_roomId(env("GAME_ROOM_ID", QStringLiteral("main"))) {
    m_server.setMaxPendingConnections(kMaxPendingConnections);
    m_seatLimits = {
        {QStringLiteral("red_commander"), 1}, {QStringLiteral("red_attack"), 2},
        {QStringLiteral("red_recon"), 1}, {QStringLiteral("red_ground"), 2},
        {QStringLiteral("red_jammer"), 1}, {QStringLiteral("blue_commander"), 1},
        {QStringLiteral("blue_attack"), 2}, {QStringLiteral("blue_recon"), 1},
        {QStringLiteral("blue_ground"), 2}, {QStringLiteral("blue_jammer"), 1}};
    QString templateError;
    if (!m_authoritativeRoom.setTemplateCatalog(
            AuthoritativeRoom::defaultTemplateCatalog(), &templateError)) {
        m_recoveryError = templateError;
        return;
    }
    m_uptime.start();
    QString configurationError;
    QString normalizedAuthUrl;
    if (!validateAuthServiceUrl(m_authServiceUrl, &normalizedAuthUrl, &configurationError)) {
        m_recoveryError = configurationError;
        qCritical() << m_recoveryError;
        return;
    }
    m_authServiceUrl = normalizedAuthUrl;
    if (m_internalKey.size() < 32 || isPlaceholderInternalKey(m_internalKey)) {
        m_recoveryError = QStringLiteral("INTERNAL_API_KEY 必须是至少 32 位的随机密钥");
        qCritical() << m_recoveryError;
        return;
    }
    if (!m_persistence.configurationError().isEmpty()) {
        m_recoveryError = m_persistence.configurationError();
        qCritical() << m_recoveryError;
        return;
    }
    const auto normalizeConfiguredPath = [this](QString* path, const QString& name) {
        QString pathError;
        const QString resolved = RoomPersistence::resolvePathWithinRoot(*path, m_dataDir,
                                                                         &pathError);
        if (resolved.isEmpty()) {
            m_recoveryError = QStringLiteral("%1 无效: %2").arg(name, pathError);
            return false;
        }
        *path = resolved;
        return true;
    };
    if (!normalizeConfiguredPath(&m_scenarioPath, QStringLiteral("SCENARIO_PATH"))
        || !normalizeConfiguredPath(&m_monitorLogPath, QStringLiteral("MONITOR_LOG_PATH"))
        || !normalizeConfiguredPath(&m_monitorStatusPath, QStringLiteral("MONITOR_STATUS_PATH"))) {
        qCritical() << m_recoveryError;
        return;
    }

    const auto bootstrapRoomState = [this]() {
        // A failed restore may have partially applied a checkpoint before an
        // event or runtime validation error. Reset every room-owned value
        // before loading the persisted scenario or the built-in baseline.
        m_engine.setRunning(false);
        m_engine.setSpeedMul(1.0);
        m_phase = QStringLiteral("preparing");
        m_redReady = false;
        m_blueReady = false;
        m_runInitialScenario = {};
        m_mapMarks = {};
        m_commandResults.clear();
        m_commandResultOrder.clear();
        m_scenarioRevision = 1;
        m_stateRevision = 1;
        m_eventSequence = 0;

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
        m_runInitialScenario = m_engine.scenario();
        persistScenario();
        QString checkpointError;
        if (!persistRoomState(&checkpointError)) {
            m_recoveryError = checkpointError;
            qCritical() << "无法创建初始检查点:" << checkpointError;
        }
    };

    QString restoreError;
    if (!restoreRoomState(&restoreError)) {
        if (QFileInfo::exists(m_persistence.checkpointPath())) {
            const bool allowReset = qEnvironmentVariable("WARGAME_ALLOW_RECOVERY_RESET").trimmed()
                == QLatin1String("1");
            if (!allowReset) {
                m_recoveryError = QStringLiteral(
                    "房间状态恢复失败，服务已拒绝启动以保护权威数据: %1 (%2)")
                                      .arg(restoreError, m_persistence.checkpointPath());
                qCritical() << m_recoveryError;
            } else {
                const QString suffix = QStringLiteral(".incompatible-%1")
                                           .arg(QDateTime::currentMSecsSinceEpoch());
                const QString archivedCheckpoint = m_persistence.checkpointPath() + suffix;
                const bool checkpointArchived = QFile::rename(
                    m_persistence.checkpointPath(), archivedCheckpoint);
                const QString eventLog = m_persistence.eventLogPath();
                const QStringList eventGenerations{
                    eventLog, eventLog + QStringLiteral(".1"),
                    eventLog + QStringLiteral(".2"), eventLog + QStringLiteral(".3")};
                bool eventLogsArchived = true;
                for (const QString& generation : eventGenerations) {
                    if (QFileInfo::exists(generation)
                        && !QFile::rename(generation, generation + suffix)) {
                        eventLogsArchived = false;
                    }
                }
                if (!checkpointArchived || !eventLogsArchived) {
                    m_recoveryError = QStringLiteral("房间状态恢复失败，且无法完整归档检查点或事件日志: %1 (%2)")
                                           .arg(restoreError, m_persistence.checkpointPath());
                    qCritical() << m_recoveryError;
                } else {
                    qWarning() << "已按 WARGAME_ALLOW_RECOVERY_RESET 归档损坏检查点并建立准备态:"
                               << restoreError << archivedCheckpoint;
                    bootstrapRoomState();
                }
            }
        } else {
            bootstrapRoomState();
        }
    }

    connect(&m_server, &QWebSocketServer::newConnection, this, &GameServer::onNewConnection);
    connect(&m_engine, &SimulationEngine::simulationEnded, this,
            [this](const QString& winner, const QString& loser) {
                const QString result = winner.startsWith(QStringLiteral("红"))
                    ? QStringLiteral("red")
                    : winner.startsWith(QStringLiteral("蓝")) ? QStringLiteral("blue")
                                                               : QStringLiteral("draw");
                m_authoritativeRoom.finish(result);
                m_phase = QStringLiteral("finished");
                m_engine.setRunning(false);
                persistRoomState();
                reportRoomStatus(QStringLiteral("finished"),
                                 loser.isEmpty() ? winner
                                                 : QStringLiteral("%1指挥所已被摧毁").arg(loser),
                                 result);
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
    m_fastDds.setTicketValidator([this](const QString& seatId, const QString& ticket) {
        if (seatId.isEmpty() || ticket.isEmpty()) return false;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
            if (it->authenticated && it->roomId == m_roomId && it->seatId == seatId
                && it->ddsTicket == ticket && it->ddsTicketExpiresAtMs > now) {
                return true;
            }
        }
        return false;
    });
    connect(&m_fastDds, &FastDdsNode::transportSecurityWarning, this,
            [this](const QString& message) {
                audit(QStringLiteral("security"),
                      QJsonObject{{QStringLiteral("event"), QStringLiteral("fastDdsRejected")},
                                  {QStringLiteral("message"), message}});
            });
    connect(&m_fastDds, &FastDdsNode::envelopeReceived, this,
            [this](const QString& topic, const QJsonObject& payload) {
                handleFastDdsEnvelope(topic, payload);
            });

    m_snapshotTimer.setInterval(100);
    connect(&m_snapshotTimer, &QTimer::timeout, this,
            [this]() { broadcastSnapshots(); });
    m_sessionValidationTimer.setInterval(30000);
    connect(&m_sessionValidationTimer, &QTimer::timeout,
            this, &GameServer::validateActiveSessions);
    m_roomSyncTimer.setInterval(3000);
    connect(&m_roomSyncTimer, &QTimer::timeout, this,
            [this]() { syncRoomControl(); });
    m_presenceTimer.setInterval(2000);
    connect(&m_presenceTimer, &QTimer::timeout, this, &GameServer::reportRoomPresence);
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
    m_roomSyncTimer.stop();
    m_presenceTimer.stop();
    if (!m_engine.scenario().units.empty()) {
        QString error;
        if (!persistRoomState(&error)) qWarning() << "退出前检查点写入失败" << error;
    }
    m_engine.setRunning(false);
    m_fastDds.stop();
    m_server.close();
}

bool GameServer::listen(quint16 port) {
    if (!m_recoveryError.isEmpty()) return false;
    syncRoomControl();
    QString ddsError;
    if (!m_fastDds.start(m_roomId, qEnvironmentVariableIntValue("FASTDDS_DOMAIN_ID"), &ddsError)) {
        qWarning() << "Fast DDS 数据面启动失败:" << ddsError;
        audit(QStringLiteral("server"), QJsonObject{{QStringLiteral("event"), QStringLiteral("fastDdsUnavailable")},
                                                    {QStringLiteral("message"), ddsError}});
    }
    if (!m_server.listen(QHostAddress::Any, port)) return false;
    m_snapshotTimer.start();
    m_sessionValidationTimer.start();
    m_roomSyncTimer.start();
    m_presenceTimer.start();
    m_monitorStatusTimer.start();
    m_checkpointTimer.start();
    audit(QStringLiteral("server"), QJsonObject{{QStringLiteral("event"), QStringLiteral("started")},
                                                 {QStringLiteral("port"), static_cast<int>(port)}});
    writeMonitorStatus();
    return true;
}

void GameServer::refreshRoomControlForJoin(QWebSocket* socket, const QJsonObject& payload,
                                           qint64 expectedUserId) {
    const QPointer<QWebSocket> guardedSocket(socket);
    const QUrl url(m_authServiceUrl + QStringLiteral("/api/internal/rooms"));
    QNetworkRequest request(url);
    request.setRawHeader("X-Internal-Key", m_internalKey.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = m_network.get(request);
    QTimer::singleShot(2500, reply, [reply]() {
        if (reply->isRunning()) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, guardedSocket, payload, expectedUserId]() {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        if (!guardedSocket || !m_clients.contains(guardedSocket)) return;
        if (statusCode != 200 || !document.isObject()) {
            sendError(guardedSocket, QStringLiteral("ROOM_SYNC_FAILED"),
                      QStringLiteral("暂时无法确认房间状态，请稍后重试"));
            return;
        }

        processKickRequests(document.object().value(QStringLiteral("kickRequests")).toArray());
        processLogoutRequests(document.object().value(QStringLiteral("logoutRequests")).toArray(),
                              guardedSocket.data());
        if (!guardedSocket || !m_clients.contains(guardedSocket)) return;

        QJsonObject selected;
        for (const QJsonValue& value : document.object().value(QStringLiteral("rooms")).toArray()) {
            const QJsonObject room = value.toObject();
            if (room.value(QStringLiteral("roomId")).toString() == m_roomId) {
                selected = room;
                break;
            }
        }
        if (selected.isEmpty()) {
            m_roomStatus = QStringLiteral("stopped");
        } else {
            const QJsonObject configuredLimits = selected.value(QStringLiteral("seatLimits")).toObject();
            if (!configuredLimits.isEmpty() && m_phase == QLatin1String("preparing")) {
                QHash<QString, int> nextLimits;
                for (auto it = configuredLimits.begin(); it != configuredLimits.end(); ++it) {
                    if (!it.value().isDouble()) continue;
                    const SeatDescriptor descriptor = describeSeat(it.key());
                    if (descriptor.side != QLatin1String("red")
                        && descriptor.side != QLatin1String("blue")) continue;
                    nextLimits.insert(descriptor.baseId, qBound(0, it.value().toInt(), 64));
                }
                if (!nextLimits.isEmpty()) m_seatLimits = nextLimits;
            }
            QHash<QString, QJsonObject> nextParameters;
            const QJsonObject configuredParameters = selected.value(QStringLiteral("seatParameters")).toObject();
            for (auto it = configuredParameters.begin(); it != configuredParameters.end(); ++it) {
                if (it.value().isObject()) nextParameters.insert(it.key(), it.value().toObject());
            }
            if (m_phase == QLatin1String("preparing")) m_seatParameters = nextParameters;
            reconcileSeatConfiguration();
            m_roomName = selected.value(QStringLiteral("name")).toString(m_roomName);
            m_roomStatus = selected.value(QStringLiteral("status")).toString(QStringLiteral("stopped"));
            m_lastRoomUpdate = selected.value(QStringLiteral("updatedAt")).toString();
            processRoomOperation(selected.value(QStringLiteral("pendingOperation")).toObject());
        }
        completeJoinRoom(guardedSocket, payload, expectedUserId);
    });
}

void GameServer::reconcileSeatConfiguration() {
    const RoomStateBackup backup = captureRoomState();
    QSet<qint64> revokedUsers;
    QStringList revokedUnitIds;
    bool parameterChanged = false;
    const QHash<QString, AuthoritativeRoom::Seat> seats = m_authoritativeRoom.seats();
    for (const AuthoritativeRoom::Seat& seat : seats) {
        const SeatDescriptor descriptor = describeSeat(seat.seatId);
        const int capacity = m_seatLimits.value(descriptor.baseId, 0);
        if (descriptor.side.isEmpty() || descriptor.type.isEmpty()
            || descriptor.index > capacity) {
            if (m_authoritativeRoom.removeUser(seat.userId)) {
                revokedUsers.insert(seat.userId);
                if (!seat.unitId.isEmpty()) revokedUnitIds.append(seat.unitId);
                m_sharedIntel.remove(seat.seatId);
                removeParticipantMarksForUser(seat.userId, seat.seatId);
            }
            continue;
        }

        QJsonObject parameters = m_seatParameters.value(seat.seatId);
        if (parameters.isEmpty()) parameters = m_seatParameters.value(descriptor.baseId);
        UnitBase* unit = seatUnit(seat.seatId);
        if (unit) {
            const Scenario& currentScenario = m_engine.scenario();
            const auto baseline = std::find_if(currentScenario.units.cbegin(),
                                               currentScenario.units.cend(),
                                               [unit](const ScenarioUnit& candidate) {
                                                   return candidate.id == unit->id();
                                               });
            const double defaultCommunicationRange = baseline == currentScenario.units.cend()
                ? unit->commRange() : baseline->commRange;
            const double defaultDetectRange = baseline == currentScenario.units.cend()
                ? unit->detectRange() : baseline->detectRange;
            const double communicationRange = parameters.contains(QStringLiteral("communicationRange"))
                ? parameters.value(QStringLiteral("communicationRange")).toDouble(defaultCommunicationRange)
                : defaultCommunicationRange;
            const double detectRange = parameters.contains(QStringLiteral("detectRange"))
                ? parameters.value(QStringLiteral("detectRange")).toDouble(defaultDetectRange)
                : defaultDetectRange;
            if (std::isfinite(communicationRange)
                && std::abs(unit->commRange() - communicationRange) > 1e-9) {
                unit->setCommRange(communicationRange);
                parameterChanged = true;
            }
            if (std::isfinite(detectRange)
                && std::abs(unit->detectRange() - detectRange) > 1e-9) {
                unit->setDetectRange(detectRange);
                parameterChanged = true;
            }
        }
    }

    if (revokedUsers.isEmpty() && !parameterChanged) return;
    resetReadiness();
    QString persistenceError;
    bool committed = true;
    if (!revokedUsers.isEmpty()) {
        committed = applyDepartureToRuntime(revokedUnitIds, &persistenceError);
    }
    if (committed) committed = persistRoomState(&persistenceError);
    if (!committed) {
        QString rollbackError;
        restoreRoomStateBackup(backup, &rollbackError);
        audit(QStringLiteral("persistence"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("seatConfigurationCheckpointFailed")},
                          {QStringLiteral("message"), persistenceError},
                          {QStringLiteral("rollbackError"), rollbackError}});
        return;
    }

    syncAuthoritativeSeats();
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (!it->authenticated || it->roomId != m_roomId) continue;
        if (revokedUsers.contains(it->userId)) {
            sendError(it.key(), QStringLiteral("SEAT_REVOKED"),
                      QStringLiteral("网页管理员调整了房间战位配置，当前战位已释放"));
        }
        sendSeatDirectory(it.key());
        if (revokedUsers.contains(it->userId)) sendFullSnapshot(it.key());
    }
    broadcastEvent(QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("seatConfigurationChanged")},
        {QStringLiteral("message"), QStringLiteral("网页管理员更新了战位容量或通信参数，已重新校验席位")}});
    broadcastSnapshots(true);
}

QHostAddress GameServer::normalizedPeerAddress(const QHostAddress& address) {
    bool isIpv4 = false;
    const quint32 ipv4 = address.toIPv4Address(&isIpv4);
    if (isIpv4) return QHostAddress(ipv4);
    QHostAddress normalized = address;
    normalized.setScopeId({});
    return normalized;
}

bool GameServer::incomingTextExceedsPreflight(const QString& text) {
    return text.size() > Protocol::MaxServerMessageBytes / 3;
}

int GameServer::authenticatedClientCount() const {
    return std::count_if(m_clients.cbegin(), m_clients.cend(), [](const ClientSession& session) {
        return session.authenticated;
    });
}

int GameServer::unauthenticatedClientCount() const {
    return std::count_if(m_clients.cbegin(), m_clients.cend(), [](const ClientSession& session) {
        return !session.authenticated;
    });
}

int GameServer::unauthenticatedClientCount(const QHostAddress& peerAddress) const {
    const QHostAddress normalized = normalizedPeerAddress(peerAddress);
    return std::count_if(m_clients.cbegin(), m_clients.cend(), [&normalized](const ClientSession& session) {
        return !session.authenticated && session.peerAddress == normalized;
    });
}

void GameServer::onNewConnection() {
    while (m_server.hasPendingConnections()) {
        QWebSocket* socket = m_server.nextPendingConnection();
        const QHostAddress peerAddress = normalizedPeerAddress(socket->peerAddress());
        if (unauthenticatedClientCount() >= kMaxUnauthenticated) {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("服务器未认证连接数已达上限"));
            connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
            continue;
        }
        if (unauthenticatedClientCount(peerAddress) >= kMaxUnauthenticatedPerIp) {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("当前地址未认证连接数已达上限"));
            connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
            continue;
        }
        ClientSession session;
        session.peerAddress = peerAddress;
        m_clients.insert(socket, session);
        ++m_totalConnections;
        audit(QStringLiteral("connection"), QJsonObject{{QStringLiteral("event"), QStringLiteral("opened")},
                                                         {QStringLiteral("peer"), socket->peerAddress().toString()},
                                                         {QStringLiteral("port"), static_cast<int>(socket->peerPort())}});
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString& text) { onTextMessage(socket, text); });
        connect(socket, &QWebSocket::disconnected, this,
                [this, socket]() { removeClient(socket); });
        QPointer<QWebSocket> guarded(socket);
        QTimer::singleShot(kUnauthenticatedTimeoutMs, this, [this, guarded]() {
            if (guarded && m_clients.contains(guarded)
                && !m_clients.value(guarded).authenticated
                && !m_clients.value(guarded).authenticationPending) {
                guarded->close(QWebSocketProtocol::CloseCodePolicyViolated,
                               QStringLiteral("认证超时"));
            }
        });
    }
}

void GameServer::onTextMessage(QWebSocket* socket, const QString& text) {
    if (!m_clients.contains(socket)) return;
    if (incomingTextExceedsPreflight(text)) {
        socket->close(QWebSocketProtocol::CloseCodeTooMuchData, QStringLiteral("消息过大"));
        return;
    }
    const QByteArray encoded = text.toUtf8();
    if (encoded.size() > Protocol::MaxMessageBytes) {
        socket->close(QWebSocketProtocol::CloseCodeTooMuchData, QStringLiteral("消息过大"));
        return;
    }
    ClientSession& session = m_clients[socket];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (session.authenticated) {
        session.lastSeenAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    }
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
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &parseError);
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

    if (type == QLatin1String("roomList")) handleRoomList(socket);
    else if (type == QLatin1String("joinRoom")) handleJoinRoom(socket, payload);
    else if (type == QLatin1String("claimSeat")) handleClaimSeat(socket, payload);
    else if (type == QLatin1String("leaveRoom")) handleLeaveRoom(socket, payload);
    else if (type == QLatin1String("releaseSeat")) handleLeaveRoom(socket, payload);
    else if (type == QLatin1String("seatReady")) handleSeatReady(socket, payload);
    else if (type == QLatin1String("deployment")) handleDeployment(socket, payload);
    else if (type == QLatin1String("requestRedeploy")) handleRedeployRequest(socket);
    else if (type == QLatin1String("redeploy")) handleRedeploy(socket, payload);
    else if (type == QLatin1String("setUnitName")) handleSetUnitName(socket, payload);
    else if (type == QLatin1String("shareIntel")) handleShareIntel(socket, payload);
    else if (type == QLatin1String("mapMark")) handleMapMark(socket, payload);
    else if (type == QLatin1String("heartbeat")) sendEnvelope(socket, QStringLiteral("pong"), QJsonObject{{QStringLiteral("sessionAlive"), true}});
    else if (type == QLatin1String("command")) handleCommand(socket, payload);
    else if (type == QLatin1String("control")) handleControl(socket, payload);
    else if (type == QLatin1String("setReady")) handleReady(socket, payload);
    else if (type == QLatin1String("chat")) handleChat(socket, payload);
    else if (type == QLatin1String("scenarioUpsert")) handleScenarioUpsert(socket, payload);
    else if (type == QLatin1String("scenarioRemove")) handleScenarioRemove(socket, payload);
    else if (type == QLatin1String("scenarioReplace")) handleScenarioReplace(socket, payload);
    else if (type == QLatin1String("resyncRequest")) {
        ++m_totalResyncRequests;
        sendFullSnapshot(socket);
    }
    else if (type == QLatin1String("ping")) sendEnvelope(socket, QStringLiteral("pong"), QJsonObject{});
    else sendError(socket, QStringLiteral("UNKNOWN_MESSAGE"), QStringLiteral("不支持的消息类型"));
}

void GameServer::authenticate(QWebSocket* socket, const QString& token) {
    if (!m_clients.contains(socket)) return;
    ClientSession& session = m_clients[socket];
    if (session.authenticationPending) return;
    if (token.isEmpty()) {
        failAuthentication(socket, QStringLiteral("INVALID_TOKEN"), QStringLiteral("登录令牌为空"),
                           QStringLiteral("invalid_credentials"), true, 0);
        return;
    }
    session.authenticationPending = true;
    session.authenticationAttempts = 0;
    session.authenticationDeadlineAtMs = QDateTime::currentMSecsSinceEpoch()
        + kAuthenticationDeadlineMs;
    session.token = token;
    startAuthenticationAttempt(socket);
}

void GameServer::startAuthenticationAttempt(QWebSocket* socket) {
    if (!m_clients.contains(socket)) return;
    ClientSession& session = m_clients[socket];
    if (!session.authenticationPending) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (session.authenticationAttempts >= kAuthenticationMaxAttempts
        || now >= session.authenticationDeadlineAtMs) {
        failAuthentication(socket, QStringLiteral("AUTH_SERVICE_UNAVAILABLE"),
                           QStringLiteral("认证服务不可用，请稍后重试"), QStringLiteral("deadline"),
                           false, 0);
        return;
    }

    ++session.authenticationAttempts;
    ++m_totalAuthenticationAttempts;
    const QUrl url(m_authServiceUrl + QStringLiteral("/api/internal/session"));
    QNetworkRequest request(url);
    request.setRawHeader("X-Internal-Key", m_internalKey.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = m_network.post(
        request, QJsonDocument(QJsonObject{{QStringLiteral("token"), session.token}})
                     .toJson(QJsonDocument::Compact));
    const int timeoutMs = qMin(kAuthenticationAttemptTimeoutMs,
                               qMax(1, static_cast<int>(session.authenticationDeadlineAtMs - now)));
    QPointer<QNetworkReply> guardedReply(reply);
    QTimer* timeout = new QTimer(this);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, this, [guardedReply]() {
        if (guardedReply && guardedReply->isRunning()) {
            guardedReply->setProperty("wargameAuthenticationTimedOut", true);
            guardedReply->abort();
        }
    });
    timeout->start(timeoutMs);
    QPointer<QWebSocket> guarded(socket);
    connect(reply, &QNetworkReply::finished, this, [this, guarded, reply, timeout]() {
        timeout->stop();
        timeout->deleteLater();
        handleAuthenticationReply(guarded.data(), reply);
    });
}

void GameServer::handleAuthenticationReply(QWebSocket* socket, QNetworkReply* reply) {
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const bool timedOut = reply->property("wargameAuthenticationTimedOut").toBool();
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    reply->deleteLater();
    if (!socket || !m_clients.contains(socket)) return;
    if (!m_clients.value(socket).authenticationPending) return;

    if (statusCode == 200 && networkError == QNetworkReply::NoError && document.isObject()) {
        const QJsonObject identity = document.object();
        const bool validIdentity = identity.value(QStringLiteral("valid")).toBool()
            && identity.value(QStringLiteral("userId")).toInteger() > 0
            && !identity.value(QStringLiteral("username")).toString().isEmpty();
        if (!validIdentity) {
            failAuthentication(socket, QStringLiteral("INVALID_ACCOUNT"), QStringLiteral("账号身份无效"),
                               QStringLiteral("invalid_credentials"), true, statusCode);
            return;
        }
        const int attempts = m_clients.value(socket).authenticationAttempts;
        m_clients[socket].authenticationPending = false;
        m_clients[socket].authenticationAttempts = 0;
        m_clients[socket].authenticationDeadlineAtMs = 0;
        m_authenticationHealth = QStringLiteral("healthy");
        m_lastAuthenticationFailureClass.clear();
        if (attempts > 1) {
            audit(QStringLiteral("connection"),
                  QJsonObject{{QStringLiteral("event"), QStringLiteral("authenticationRecovered")},
                              {QStringLiteral("attempts"), attempts}});
        }
        finishAuthentication(socket, identity);
        return;
    }

    const bool credentialFailure = statusCode >= 400 && statusCode <= 499;
    if (credentialFailure) {
        failAuthentication(socket, QStringLiteral("INVALID_TOKEN"),
                           QStringLiteral("登录会话无效或已过期"),
                           QStringLiteral("invalid_credentials"), true, statusCode);
        return;
    }

    const QString classification = timedOut ? QStringLiteral("timeout")
        : statusCode >= 500 ? QStringLiteral("http_5xx")
        : networkError != QNetworkReply::NoError ? QStringLiteral("transport")
                                                  : QStringLiteral("invalid_response");
    ++m_totalAuthenticationTransientFailures;
    m_authenticationHealth = QStringLiteral("degraded");
    m_lastAuthenticationFailureClass = classification;
    const ClientSession session = m_clients.value(socket);
    audit(QStringLiteral("connection"),
          QJsonObject{{QStringLiteral("event"), QStringLiteral("authenticationTransientFailure")},
                      {QStringLiteral("classification"), classification},
                      {QStringLiteral("statusCode"), statusCode},
                      {QStringLiteral("attempt"), session.authenticationAttempts}});
    if (session.authenticationAttempts < kAuthenticationMaxAttempts
        && QDateTime::currentMSecsSinceEpoch() < session.authenticationDeadlineAtMs) {
        scheduleAuthenticationRetry(socket, classification, statusCode);
        return;
    }
    failAuthentication(socket, QStringLiteral("AUTH_SERVICE_UNAVAILABLE"),
                       QStringLiteral("认证服务不可用，请稍后重试"), classification, false, statusCode);
}

void GameServer::scheduleAuthenticationRetry(QWebSocket* socket, const QString& classification,
                                              int statusCode) {
    if (!m_clients.contains(socket)) return;
    const ClientSession& session = m_clients.value(socket);
    const int baseDelayMs = session.authenticationAttempts == 1
        ? kAuthenticationBackoffBaseMs : kAuthenticationBackoffBaseMs * 2;
    const int delayMs = baseDelayMs
        + QRandomGenerator::global()->bounded(kAuthenticationJitterMs + 1);
    const qint64 remainingMs = session.authenticationDeadlineAtMs
        - QDateTime::currentMSecsSinceEpoch();
    if (remainingMs <= delayMs) {
        failAuthentication(socket, QStringLiteral("AUTH_SERVICE_UNAVAILABLE"),
                           QStringLiteral("认证服务不可用，请稍后重试"), QStringLiteral("deadline"),
                           false, statusCode);
        return;
    }
    ++m_totalAuthenticationRetries;
    audit(QStringLiteral("connection"),
          QJsonObject{{QStringLiteral("event"), QStringLiteral("authenticationRetryScheduled")},
                      {QStringLiteral("classification"), classification},
                      {QStringLiteral("attempt"), session.authenticationAttempts},
                      {QStringLiteral("delayMs"), delayMs}});
    QPointer<QWebSocket> guarded(socket);
    QTimer::singleShot(delayMs, this, [this, guarded]() {
        if (guarded && m_clients.contains(guarded)) startAuthenticationAttempt(guarded.data());
    });
}

void GameServer::failAuthentication(QWebSocket* socket, const QString& code,
                                    const QString& message, const QString& classification,
                                    bool credentialFailure, int statusCode) {
    if (!socket || !m_clients.contains(socket)) return;
    const ClientSession session = m_clients.value(socket);
    m_clients[socket].authenticationPending = false;
    m_clients[socket].authenticationAttempts = 0;
    m_clients[socket].authenticationDeadlineAtMs = 0;
    m_clients[socket].token.clear();
    ++m_totalAuthenticationFinalFailures;
    if (credentialFailure) {
        ++m_totalAuthenticationCredentialFailures;
        m_authenticationHealth = QStringLiteral("healthy");
    } else {
        m_authenticationHealth = QStringLiteral("unavailable");
    }
    m_lastAuthenticationFailureClass = classification;
    audit(QStringLiteral("connection"),
          QJsonObject{{QStringLiteral("event"), QStringLiteral("authenticationFinalFailure")},
                      {QStringLiteral("classification"), classification},
                      {QStringLiteral("statusCode"), statusCode},
                      {QStringLiteral("attempts"), session.authenticationAttempts},
                      {QStringLiteral("credentialFailure"), credentialFailure}});
    sendError(socket, code, message);
    socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                  credentialFailure ? QStringLiteral("认证失败")
                                    : QStringLiteral("认证服务不可用"));
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
                && identity.value(QStringLiteral("userId")).toInteger() == expected.userId;
            if (!unchanged) {
                sendError(guarded, QStringLiteral("SESSION_REVOKED"),
                          QStringLiteral("账号已停用、删除、改密或变更席位，请重新登录"));
                guarded->close(QWebSocketProtocol::CloseCodePolicyViolated,
                               QStringLiteral("账号会话已失效"));
            }
        });
    }
}

void GameServer::syncRoomControl(QWebSocket* requester) {
    m_roomListWaiters.erase(std::remove_if(
        m_roomListWaiters.begin(), m_roomListWaiters.end(),
        [](const QPointer<QWebSocket>& waiter) { return waiter.isNull(); }),
        m_roomListWaiters.end());
    if (requester) {
        const bool alreadyWaiting = std::any_of(
            m_roomListWaiters.cbegin(), m_roomListWaiters.cend(),
            [requester](const QPointer<QWebSocket>& waiter) {
                return waiter.data() == requester;
            });
        if (!alreadyWaiting) {
            if (m_roomListWaiters.size() < kMaxConnectedClients) {
                m_roomListWaiters.append(QPointer<QWebSocket>(requester));
            } else {
                sendRoomDirectory(requester);
            }
        }
    }
    if (m_roomSyncInFlight) {
        return;
    }
    m_roomSyncInFlight = true;
    const QUrl url(m_authServiceUrl + QStringLiteral("/api/internal/rooms"));
    QNetworkRequest request(url);
    request.setRawHeader("X-Internal-Key", m_internalKey.toUtf8());
    QNetworkReply* reply = m_network.get(request);
    QTimer::singleShot(2500, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const auto flushRoomListWaiters = [this]() {
            const QList<QPointer<QWebSocket>> waiters = m_roomListWaiters;
            m_roomListWaiters.clear();
            m_roomSyncInFlight = false;
            for (const QPointer<QWebSocket>& waiter : waiters) {
                if (waiter) sendRoomDirectory(waiter.data());
            }
        };
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        if (statusCode != 200 || !document.isObject()) {
            flushRoomListWaiters();
            return;
        }
        const QJsonArray rooms = document.object().value(QStringLiteral("rooms")).toArray();
        const bool roomDirectoryChanged = !m_roomDirectoryLoaded || m_roomDirectory != rooms;
        m_roomDirectory = rooms;
        m_roomDirectoryLoaded = true;
        processKickRequests(document.object().value(QStringLiteral("kickRequests")).toArray());
        processLogoutRequests(document.object().value(QStringLiteral("logoutRequests")).toArray());
        QJsonObject selected;
        for (const QJsonValue& value : rooms) {
            if (value.toObject().value(QStringLiteral("roomId")).toString() == m_roomId) {
                selected = value.toObject(); break;
            }
        }
        if (selected.isEmpty()) {
            const bool hasAuthoritativeState = m_phase != QLatin1String("preparing")
                || !m_authoritativeRoom.seats().isEmpty()
                || !m_authoritativeRoom.runtimeUnits().isEmpty()
                || !m_mapMarks.isEmpty() || !m_sharedIntel.isEmpty()
                || !m_chatHistory.isEmpty();
            QString resetError;
            if (hasAuthoritativeState
                && !resetAuthoritativeRuntime(
                    QStringLiteral("internal-room-removed-%1")
                        .arg(m_authoritativeRoom.revision()), &resetError)) {
                audit(QStringLiteral("persistence"),
                      QJsonObject{{QStringLiteral("event"), QStringLiteral("removedRoomResetFailed")},
                                  {QStringLiteral("message"), resetError}});
                flushRoomListWaiters();
                return;
            }
            m_roomStatus = QStringLiteral("stopped");
            closeRoomSessions(QStringLiteral("房间已被网页管理员关闭或删除"));
            if (roomDirectoryChanged) broadcastRoomDirectory();
            broadcastSnapshots(true);
            flushRoomListWaiters();
            return;
        }
        const QString status = selected.value(QStringLiteral("status")).toString(QStringLiteral("stopped"));
        const QJsonObject configuredLimits = selected.value(QStringLiteral("seatLimits")).toObject();
        if (!configuredLimits.isEmpty()) {
            QHash<QString, int> nextLimits;
            for (auto it = configuredLimits.begin(); it != configuredLimits.end(); ++it) {
                if (!it.value().isDouble()) continue;
                const SeatDescriptor descriptor = describeSeat(it.key());
                if (descriptor.side != QLatin1String("red")
                    && descriptor.side != QLatin1String("blue")) continue;
                const int limit = qBound(0, it.value().toInt(), 64);
                // Room configuration uses a base key (red_attack). Accepting
                // a suffixed key keeps older room JSON files readable while
                // the runtime always expands it into concrete seat ids.
                nextLimits.insert(descriptor.baseId, limit);
            }
            if (!nextLimits.isEmpty()) {
                if (m_phase == QLatin1String("preparing")) {
                    m_seatLimits = nextLimits;
                } else if (nextLimits != m_seatLimits) {
                    audit(QStringLiteral("security"),
                          QJsonObject{{QStringLiteral("event"), QStringLiteral("activeSeatLimitChangeRejected")},
                                      {QStringLiteral("phase"), m_phase}});
                }
            }
        }
        QHash<QString, QJsonObject> nextParameters;
        const QJsonObject configuredParameters = selected.value(QStringLiteral("seatParameters")).toObject();
        for (auto it = configuredParameters.begin(); it != configuredParameters.end(); ++it) {
            if (it.value().isObject()) nextParameters.insert(it.key(), it.value().toObject());
        }
        if (m_phase == QLatin1String("preparing")) {
            m_seatParameters = nextParameters;
        } else if (nextParameters != m_seatParameters) {
            audit(QStringLiteral("security"),
                  QJsonObject{{QStringLiteral("event"), QStringLiteral("activeSeatParameterChangeRejected")},
                              {QStringLiteral("phase"), m_phase}});
        }
        reconcileSeatConfiguration();
        const QString updated = selected.value(QStringLiteral("updatedAt")).toString();
        const bool changed = !updated.isEmpty() && updated != m_lastRoomUpdate;
        const bool stoppedNow = status == QLatin1String("stopped")
            && m_roomStatus != QLatin1String("stopped");
        m_lastRoomUpdate = updated;
        m_roomName = selected.value(QStringLiteral("name")).toString(m_roomName);
        m_roomStatus = status;
        const QJsonObject latestOperation = selected.value(QStringLiteral("operation")).toObject();
        const QJsonObject pendingOperation = selected.value(QStringLiteral("pendingOperation")).toObject();
        const QString pendingAction = pendingOperation.value(QStringLiteral("action")).toString();
        const QString latestAction = latestOperation.value(QStringLiteral("action")).toString();
        const bool deploymentOperation = latestAction == QLatin1String("reset")
            || latestAction == QLatin1String("redeploy");
        processRoomOperation(pendingOperation);
        if ((changed || stoppedNow) && status == QLatin1String("stopped")) {
            closeRoomSessions(QStringLiteral("网页管理员已停止房间"));
        }
        const bool newRoundResetRequired = !deploymentOperation
            && ((status == QLatin1String("preparing")
                 && m_phase != QLatin1String("preparing"))
                || (status == QLatin1String("stopped")
                    && (changed || stoppedNow || m_phase != QLatin1String("preparing"))));
        if (newRoundResetRequired) {
            QString resetError;
            const bool committed = resetAuthoritativeRuntime(
                QStringLiteral("internal-new-round-%1").arg(updated), &resetError);
            if (committed) {
                for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
                    if (it->authenticated && it->roomId == m_roomId) sendSeatDirectory(it.key());
                }
                broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("matchReset")},
                                           {QStringLiteral("message"), status == QLatin1String("stopped")
                                                ? QStringLiteral("网页管理员已停止房间")
                                                : QStringLiteral("网页管理员已开启新一局推演")}});
                if (status == QLatin1String("preparing")) {
                    reportRoomStatus(status, QStringLiteral("房间状态已同步"));
                }
            } else {
                audit(QStringLiteral("persistence"),
                      QJsonObject{{QStringLiteral("event"), QStringLiteral("newRoundResetFailed")},
                                  {QStringLiteral("message"), resetError}});
                flushRoomListWaiters();
                return;
            }
        }
        if (status == QLatin1String("preparing") && m_phase != QLatin1String("preparing")) {
            m_engine.setRunning(false);
            m_phase = QStringLiteral("preparing");
            resetReadiness();
            reportRoomStatus(status, QStringLiteral("房间状态已同步"));
        }
        if (status == QLatin1String("preparing")
            && m_authoritativeRoom.phase() != QLatin1String("preparing")) {
            m_authoritativeRoom.applyOperation(
                QStringLiteral("internal-prepare-%1").arg(m_scenarioRevision),
                QStringLiteral("reset"), m_authoritativeRoom.revision());
            syncAuthoritativeSeats();
            persistRoomState();
        }
        if (status == QLatin1String("running") && m_phase != QLatin1String("running")) {
            const bool engineReady = m_engine.readyForSim();
            const AuthoritativeRoom::Result startResult = engineReady
                ? m_authoritativeRoom.start()
                : AuthoritativeRoom::Result{false, QStringLiteral("ENGINE_NOT_READY"),
                                            m_authoritativeRoom.revision()};
            if (startResult.ok) {
                m_runInitialScenario = m_engine.scenario();
                m_phase = QStringLiteral("running");
                m_engine.setRunning(true);
                broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("matchStarted")},
                                           {QStringLiteral("message"), QStringLiteral("房间管理员已开启推演")} });
            } else {
                m_roomStatus = QStringLiteral("preparing");
                reportRoomStatus(QStringLiteral("preparing"),
                                 QStringLiteral("所有已占用战位必须完成部署并就绪"));
                audit(QStringLiteral("security"), QJsonObject{{QStringLiteral("event"), QStringLiteral("adminStartRejected")},
                                                               {QStringLiteral("reason"), startResult.code}});
            }
        } else if (status == QLatin1String("finished") && m_phase != QLatin1String("finished")) {
            m_engine.setRunning(false);
            m_authoritativeRoom.finish(QStringLiteral("draw"));
            m_phase = QStringLiteral("finished");
            persistRoomState();
            broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("matchEndedByAdmin")},
                                       {QStringLiteral("message"), QStringLiteral("房间管理员已结束推演")} });
        } else if (status == QLatin1String("paused") && m_phase == QLatin1String("running")) {
            m_engine.setRunning(false);
            m_authoritativeRoom.pause();
            m_phase = QStringLiteral("paused");
            persistRoomState();
        }
        if (pendingOperation.value(QStringLiteral("state")).toString() == QLatin1String("pending")
            && pendingAction != QLatin1String("reset")
            && pendingAction != QLatin1String("redeploy")
            && pendingOperation.value(QStringLiteral("expectedStatus")).toString() == m_roomStatus) {
            reportRoomStatus(m_roomStatus, QStringLiteral("房间状态已同步"));
        }
        if (roomDirectoryChanged) broadcastRoomDirectory();
        broadcastSnapshots(changed);
        flushRoomListWaiters();
    });
}

QJsonArray GameServer::roomOccupants() const {
    QJsonArray occupants;
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (!it->authenticated || it->roomId != m_roomId) continue;
        occupants.append(QJsonObject{{QStringLiteral("userId"), it->userId},
                                     {QStringLiteral("username"), it->username},
                                     {QStringLiteral("displayName"), it->displayName},
                                     {QStringLiteral("seatId"), it->seatId},
                                     {QStringLiteral("seatType"), it->seatType},
                                     {QStringLiteral("side"), it->side},
                                     {QStringLiteral("connectedAt"), it->connectedAt},
                                     {QStringLiteral("lastSeenAt"), it->lastSeenAt}});
    }
    return occupants;
}

void GameServer::reportRoomStatus(const QString& status, const QString& reason,
                                  const QString& winner) {
    if (m_roomId.trimmed().isEmpty()) return;
    const QUrl url(m_authServiceUrl + QStringLiteral("/api/internal/rooms/%1/status")
                       .arg(QUrl::toPercentEncoding(m_roomId)));
    QNetworkRequest request(url);
    request.setRawHeader("X-Internal-Key", m_internalKey.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = m_network.post(
        request, QJsonDocument(QJsonObject{{QStringLiteral("status"), status},
                                            {QStringLiteral("reason"), reason},
                                            {QStringLiteral("winner"), winner},
                                            {QStringLiteral("occupants"), roomOccupants()}})
                         .toJson(QJsonDocument::Compact));
    QTimer::singleShot(2500, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, status]() {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200) {
            audit(QStringLiteral("server"),
                  QJsonObject{{QStringLiteral("event"), QStringLiteral("roomStatusWritebackFailed")},
                              {QStringLiteral("status"), status},
                              {QStringLiteral("httpStatus"), statusCode}});
        }
        reply->deleteLater();
    });
}

void GameServer::reportRoomPresence() {
    if (m_roomId.trimmed().isEmpty() || m_internalKey.isEmpty()) return;
    const QUrl url(m_authServiceUrl + QStringLiteral("/api/internal/rooms/%1/presence")
                       .arg(QUrl::toPercentEncoding(m_roomId)));
    QNetworkRequest request(url);
    request.setRawHeader("X-Internal-Key", m_internalKey.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = m_network.post(request,
        QJsonDocument(QJsonObject{{QStringLiteral("occupants"), roomOccupants()}}).toJson(QJsonDocument::Compact));
    QTimer::singleShot(2500, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

void GameServer::processKickRequests(const QJsonArray& requests) {
    for (const QJsonValue& value : requests) {
        const QJsonObject request = value.toObject();
        if (request.value(QStringLiteral("roomId")).toString() != m_roomId) continue;
        const qint64 userId = request.value(QStringLiteral("userId")).toInteger();
        const qint64 requestId = request.value(QStringLiteral("id")).toInteger();
        if (requestId <= 0 || userId <= 0) continue;
        QWebSocket* target = nullptr;
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            if (it->authenticated && it->userId == userId && it->roomId == m_roomId) {
                target = it.key();
                break;
            }
        }
        QString result = QStringLiteral("not_found");
        if (target && m_clients.contains(target)) {
            ClientSession& session = m_clients[target];
            const ClientSession originalSession = session;
            bool anotherRoomClient = false;
            for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
                if (it.key() != target && it->authenticated && it->roomId == m_roomId) {
                    anotherRoomClient = true;
                    break;
                }
            }

            AuthoritativeRoom::Result departure;
            QString persistenceError;
            if (anotherRoomClient) {
                const RoomStateBackup backup = captureRoomState();
                QHash<qint64, QString> previousSeats;
                QHash<qint64, AuthoritativeRoom::Seat> previousSeatStates;
                for (const AuthoritativeRoom::Seat& seat : m_authoritativeRoom.seats()) {
                    previousSeats.insert(seat.userId, seat.seatId);
                    previousSeatStates.insert(seat.userId, seat);
                }
                departure = m_authoritativeRoom.disconnect(session.userId);
                if (departure.ok) {
                    m_sharedIntel.remove(session.seatId);
                    removeParticipantMarksForUser(session.userId, session.seatId);
                    if (departure.successorUserId > 0) {
                        removeParticipantMarksForUser(
                            departure.successorUserId,
                            previousSeats.value(departure.successorUserId));
                    }
                }
                QStringList removedUnitIds;
                if (departure.successorUserId > 0) {
                    removedUnitIds.append(previousSeatStates.value(
                        departure.successorUserId).unitId);
                } else {
                    removedUnitIds.append(previousSeatStates.value(session.userId).unitId);
                }
                bool committed = departure.ok
                    && applyDepartureToRuntime(removedUnitIds, &persistenceError);
                if (committed && departure.forfeit) {
                    m_phase = QStringLiteral("finished");
                    m_engine.setRunning(false);
                }
                if (committed) {
                    syncAuthoritativeSeats();
                    committed = persistRoomState(&persistenceError);
                }
                if (!committed) {
                    restoreRoomStateBackup(backup);
                    result = QStringLiteral("failed");
                }
            } else {
                session.roomId.clear();
                session.seatId.clear();
                session.seatType.clear();
                session.side.clear();
                session.seatReady = false;
                session.role = QStringLiteral("player");
                if (!resetRoomIfEmpty(&persistenceError)) {
                    session = originalSession;
                    result = QStringLiteral("failed");
                }
            }

            if (result != QLatin1String("failed")) {
                session.roomId.clear();
                session.seatId.clear();
                session.seatType.clear();
                session.side.clear();
                session.seatReady = false;
                session.role = QStringLiteral("player");
                const QString reason = request.value(QStringLiteral("reason"))
                                           .toString(QStringLiteral("你已被管理员移出房间"));
                sendEnvelope(target, QStringLiteral("event"),
                             QJsonObject{{QStringLiteral("kind"), QStringLiteral("roomClosed")},
                                         {QStringLiteral("message"), reason}});
                if (departure.successorUserId > 0) {
                    broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("commanderPromoted")},
                                               {QStringLiteral("userId"), departure.successorUserId},
                                               {QStringLiteral("side"), originalSession.side}});
                } else if (departure.forfeit) {
                    broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("forfeit")},
                                               {QStringLiteral("winner"), departure.winner},
                                               {QStringLiteral("loser"), originalSession.side}});
                    reportRoomStatus(QStringLiteral("finished"),
                                     QStringLiteral("commander disconnected"), departure.winner);
                }
                handleRoomList(target);
                sendFullSnapshot(target);
                audit(QStringLiteral("security"),
                      QJsonObject{{QStringLiteral("event"), QStringLiteral("userKicked")},
                                  {QStringLiteral("user"), originalSession.username},
                                  {QStringLiteral("displayName"), originalSession.displayName},
                                  {QStringLiteral("userId"), userId},
                                  {QStringLiteral("seatId"), originalSession.seatId},
                                  {QStringLiteral("reason"), reason}});
                result = QStringLiteral("kicked");
                for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
                    if (it->authenticated && it->roomId == m_roomId) sendSeatDirectory(it.key());
                }
                broadcastSnapshots(true);
            } else {
                audit(QStringLiteral("persistence"),
                      QJsonObject{{QStringLiteral("event"), QStringLiteral("kickRollback")},
                                  {QStringLiteral("userId"), userId},
                                  {QStringLiteral("message"), persistenceError}});
            }
        }
        const QUrl ackUrl(m_authServiceUrl + QStringLiteral("/api/internal/rooms/%1/kick-requests/%2/ack")
                              .arg(QUrl::toPercentEncoding(m_roomId)).arg(requestId));
        QNetworkRequest ack(ackUrl);
        ack.setRawHeader("X-Internal-Key", m_internalKey.toUtf8());
        ack.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QNetworkReply* reply = m_network.post(ack,
            QJsonDocument(QJsonObject{{QStringLiteral("request_id"), requestId},
                                      {QStringLiteral("result"), result}}).toJson(QJsonDocument::Compact));
        QTimer::singleShot(2500, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
}

void GameServer::processLogoutRequests(const QJsonArray& requests, QWebSocket* joiningSocket) {
    for (const QJsonValue& value : requests) {
        const QJsonObject request = value.toObject();
        const qint64 requestId = request.value(QStringLiteral("id")).toInteger();
        const qint64 userId = request.value(QStringLiteral("userId")).toInteger();
        if (requestId <= 0 || userId <= 0) continue;
        QList<QWebSocket*> targets;
        for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
            if (it->authenticated && it->userId == userId) targets.append(it.key());
        }
        QString result = targets.isEmpty() ? QStringLiteral("not_found") : QStringLiteral("kicked");
        for (QWebSocket* target : targets) {
            if (!m_clients.contains(target)) continue;
            const ClientSession session = m_clients.value(target);
            const bool revokedDuringJoin = target == joiningSocket;
            sendError(target, revokedDuringJoin ? QStringLiteral("SESSION_REVOKED")
                                                 : QStringLiteral("USER_KICKED_OFFLINE"),
                      request.value(QStringLiteral("reason")).toString(
                          revokedDuringJoin ? QStringLiteral("账号会话已失效，请重新登录")
                                            : QStringLiteral("你已被管理员强制下线，请重新登录")));
            audit(QStringLiteral("security"),
                  QJsonObject{{QStringLiteral("event"), QStringLiteral("userForcedOffline")},
                              {QStringLiteral("user"), session.username},
                              {QStringLiteral("userId"), session.userId},
                              {QStringLiteral("reason"), request.value(QStringLiteral("reason"))}});
            target->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("已被管理员强制下线"));
        }
        const QUrl ackUrl(m_authServiceUrl + QStringLiteral("/api/internal/logout-requests/%1/ack")
                              .arg(requestId));
        QNetworkRequest ack(ackUrl);
        ack.setRawHeader("X-Internal-Key", m_internalKey.toUtf8());
        ack.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QNetworkReply* reply = m_network.post(
            ack, QJsonDocument(QJsonObject{{QStringLiteral("request_id"), requestId},
                                           {QStringLiteral("result"), result}})
                                  .toJson(QJsonDocument::Compact));
        QTimer::singleShot(2500, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
}

void GameServer::finishAuthentication(QWebSocket* socket, const QJsonObject& identity) {
    ClientSession& session = m_clients[socket];
    if (!identity.value(QStringLiteral("valid")).toBool()
        || identity.value(QStringLiteral("userId")).toInteger() <= 0
        || identity.value(QStringLiteral("username")).toString().isEmpty()) {
        sendError(socket, QStringLiteral("INVALID_ACCOUNT"), QStringLiteral("账号身份无效"));
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("账号无效"));
        return;
    }
    const qint64 userId = identity.value(QStringLiteral("userId")).toInteger();
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (it.key() != socket && it->authenticated && it->userId == userId) {
            sendError(socket, QStringLiteral("USER_ALREADY_ONLINE"), QStringLiteral("该用户已在其他客户端登录"));
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("用户已登录"));
            return;
        }
    }
    if (authenticatedClientCount() >= kMaxConnectedClients) {
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                      QStringLiteral("服务器认证连接数已达上限"));
        return;
    }
    session.authenticated = true;
    session.userId = userId;
    session.username = identity.value(QStringLiteral("username")).toString();
    session.displayName = identity.value(QStringLiteral("displayName")).toString();
    session.role = QStringLiteral("player");
    session.connectedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    session.lastSeenAt = session.connectedAt;
    session.ddsTicket = QUuid::createUuid().toString(QUuid::WithoutBraces)
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.ddsTicketExpiresAtMs = QDateTime::currentMSecsSinceEpoch() + kDdsTicketLifetimeMs;
    audit(QStringLiteral("connection"), QJsonObject{{QStringLiteral("event"), QStringLiteral("authenticated")},
                                                     {QStringLiteral("user"), session.username},
                                                     {QStringLiteral("displayName"), session.displayName},
                                                     {QStringLiteral("userId"), session.userId}});
    QJsonObject welcome{{QStringLiteral("username"), session.username},
                        {QStringLiteral("displayName"), session.displayName},
                        {QStringLiteral("room"), m_roomId},
                        {QStringLiteral("dataPlane"), m_fastDds.backendName()},
                        {QStringLiteral("dataPlaneTopics"), QJsonArray::fromStringList(m_fastDds.topicNames())},
                        {QStringLiteral("chatHistory"), filteredChatHistory(session)},
                        {QStringLiteral("roomState"), roomState()},
                        {QStringLiteral("ddsTicket"), session.ddsTicket},
                        {QStringLiteral("ddsTicketExpiresAt"), session.ddsTicketExpiresAtMs},
                        {QStringLiteral("ddsTicketExpiresInSeconds"),
                         static_cast<qint64>(kDdsTicketLifetimeMs / 1000)},
                        {QStringLiteral("serverVersion"), QStringLiteral(WARGAME_VERSION)},
                        {QStringLiteral("sourceDigest"), QStringLiteral(WARGAME_SOURCE_DIGEST)}};
    // 未进入房间时不发送空 seatId；协议中的战位字段是可选的，存在时必须是有效 ID。
    if (!session.seatId.isEmpty()) welcome.insert(QStringLiteral("seatId"), session.seatId);
    if (!session.seatType.isEmpty()) welcome.insert(QStringLiteral("seatType"), session.seatType);
    if (!session.side.isEmpty()) welcome.insert(QStringLiteral("side"), session.side);
    sendEnvelope(socket, QStringLiteral("welcome"), welcome);
    sendFullSnapshot(socket);
    sendSeatDirectory(socket);
    broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("presence")},
                               {QStringLiteral("message"), QStringLiteral("%1 已进入推演室").arg(session.displayName)}});
}

void GameServer::removeClient(QWebSocket* socket) {
    if (!m_clients.contains(socket)) return;
    const ClientSession session = m_clients.take(socket);
    bool roomEmpty = session.roomId == m_roomId;
    if (roomEmpty) {
        for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
            if (it->authenticated && it->roomId == m_roomId) {
                roomEmpty = false;
                break;
            }
        }
    }
    if (roomEmpty) {
        QString resetError;
        if (!resetRoomIfEmpty(&resetError)) {
            audit(QStringLiteral("persistence"),
                  QJsonObject{{QStringLiteral("event"), QStringLiteral("emptyRoomResetFailed")},
                              {QStringLiteral("message"), resetError}});
        }
    } else if (!session.seatId.isEmpty()) {
        const RoomStateBackup backup = captureRoomState();
        QHash<qint64, QString> previousSeats;
        QHash<qint64, AuthoritativeRoom::Seat> previousSeatStates;
        for (const AuthoritativeRoom::Seat& seat : m_authoritativeRoom.seats()) {
            previousSeats.insert(seat.userId, seat.seatId);
            previousSeatStates.insert(seat.userId, seat);
        }
        m_sharedIntel.remove(session.seatId);
        removeParticipantMarksForUser(session.userId, session.seatId);
        AuthoritativeRoom::Result departure = m_authoritativeRoom.disconnect(session.userId);
        if (departure.successorUserId > 0) {
            removeParticipantMarksForUser(departure.successorUserId,
                                          previousSeats.value(departure.successorUserId));
        }
        QString persistenceError;
        QString runtimeError;
        QStringList removedUnitIds;
        if (departure.successorUserId > 0) {
            removedUnitIds.append(previousSeatStates.value(departure.successorUserId).unitId);
        } else {
            removedUnitIds.append(previousSeatStates.value(session.userId).unitId);
        }
        bool committed = departure.ok
            && applyDepartureToRuntime(removedUnitIds, &runtimeError);
        if (committed && departure.forfeit) {
            m_phase = QStringLiteral("finished");
            m_engine.setRunning(false);
        }
        if (committed) committed = persistRoomState(&persistenceError);
        if (!committed) {
            restoreRoomStateBackup(backup);
            departure = AuthoritativeRoom::Result{};
        }
        syncAuthoritativeSeats();
        if (committed && departure.successorUserId > 0) {
            broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("commanderPromoted")},
                                       {QStringLiteral("userId"), departure.successorUserId},
                                       {QStringLiteral("side"), session.side}});
        } else if (committed && departure.forfeit) {
            broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("forfeit")},
                                       {QStringLiteral("winner"), departure.winner},
                                       {QStringLiteral("loser"), session.side}});
            reportRoomStatus(QStringLiteral("finished"),
                             QStringLiteral("commander disconnected"), departure.winner);
        }
        if (!committed) {
            audit(QStringLiteral("persistence"),
                  QJsonObject{{QStringLiteral("event"), QStringLiteral("disconnectCheckpointFailed")},
                              {QStringLiteral("message"), runtimeError.isEmpty()
                                   ? persistenceError : runtimeError}});
        }
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            if (it->authenticated && it->roomId == m_roomId) sendSeatDirectory(it.key());
        }
    }
    ++m_totalDisconnects;
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

QString GameServer::normalizedRole(const ClientSession& session) const {
    return session.seatId.isEmpty() ? session.role : session.seatId;
}

bool GameServer::hasSeatPermission(const ClientSession& session, const QString& action) const {
    if (session.seatId.isEmpty()) return false;
    if (action == QLatin1String("deployment")) return session.seatType == QLatin1String("commander");
    if (action == QLatin1String("shareIntel")) return session.seatType != QLatin1String("commander")
                                                          || !session.side.isEmpty();
    return false;
}

UnitBase* GameServer::seatUnit(const QString& seatId) const {
    return m_engine.unit(m_authoritativeRoom.seat(seatId).unitId);
}

void GameServer::broadcastRoomDirectory() {
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->authenticated && it->roomId.isEmpty()) sendRoomDirectory(it.key());
    }
}

void GameServer::handleRoomList(QWebSocket* socket) {
    syncRoomControl(socket);
}

void GameServer::sendRoomDirectory(QWebSocket* socket) {
    QJsonObject room{{QStringLiteral("roomId"), m_roomId},
                     {QStringLiteral("name"), m_roomName},
                     {QStringLiteral("description"), QStringLiteral("由网页管理员控制生命周期的联网推演室")},
                     {QStringLiteral("status"), m_roomStatus},
                     {QStringLiteral("hostedByGameServer"), true},
                     {QStringLiteral("scenarioId"), QStringLiteral("default")}};
    QJsonObject limits;
    for (auto it = m_seatLimits.cbegin(); it != m_seatLimits.cend(); ++it) limits[it.key()] = it.value();
    room[QStringLiteral("seatLimits")] = limits;
    QJsonObject parameters;
    for (auto it = m_seatParameters.cbegin(); it != m_seatParameters.cend(); ++it)
        parameters[it.key()] = it.value();
    room[QStringLiteral("seatParameters")] = parameters;
    QJsonArray rooms;
    if (m_roomDirectoryLoaded) {
        rooms = m_roomDirectory;
        for (qsizetype index = 0; index < rooms.size(); ++index) {
            if (rooms.at(index).toObject().value(QStringLiteral("roomId")).toString()
                == m_roomId) {
                rooms[index] = room;
                break;
            }
        }
    } else {
        rooms.append(room);
    }
    sendEnvelope(socket, QStringLiteral("roomDirectory"), QJsonObject{{QStringLiteral("rooms"), rooms}});
}

void GameServer::sendSeatDirectory(QWebSocket* socket) {
    if (!m_clients.contains(socket) || m_clients.value(socket).roomId.isEmpty()) return;
    QJsonArray seats;
    for (auto it = m_seatLimits.cbegin(); it != m_seatLimits.cend(); ++it) {
        const SeatDescriptor descriptor = describeSeat(it.key());
        const int capacity = qBound(0, it.value(), 64);
        if (capacity == 0) continue;
        const int seatCount = descriptor.type == QLatin1String("commander") ? 1 : capacity;
        for (int index = 1; index <= seatCount; ++index) {
            const QString seatId = canonicalSeatId(descriptor.baseId, index);
            const SeatOccupant occupant = m_seats.value(seatId);
            const AuthoritativeRoom::Seat authoritative = m_authoritativeRoom.seat(seatId);
            seats.append(QJsonObject{{QStringLiteral("seatId"), seatId},
                                     {QStringLiteral("seatType"), descriptor.type},
                                     {QStringLiteral("side"), descriptor.side},
                                     {QStringLiteral("slot"), index},
                                     {QStringLiteral("capacity"), capacity},
                                     {QStringLiteral("occupied"), occupant.userId > 0},
                                     {QStringLiteral("displayName"), occupant.username},
                                     {QStringLiteral("connected"), authoritative.connected},
                                     {QStringLiteral("commander"), descriptor.type == QLatin1String("commander")},
                                     {QStringLiteral("selectedTemplate"), authoritative.selectedTemplate},
                                     {QStringLiteral("unitId"), authoritative.unitId},
                                     {QStringLiteral("deployed"), authoritative.deployed},
                                     {QStringLiteral("ready"), occupant.ready},
                                     {QStringLiteral("pendingTransfer"), authoritative.pendingTransfer},
                                     {QStringLiteral("redeployRequested"), authoritative.redeployRequested},
                                     {QStringLiteral("unitName"), authoritative.unitName},
                                     {QStringLiteral("state"), occupant.userId > 0
                                          ? QStringLiteral("occupied") : QStringLiteral("vacant")},
                                     {QStringLiteral("revision"),
                                      static_cast<qint64>(authoritative.revision)}});
        }
    }
    sendEnvelope(socket, QStringLiteral("seatState"), QJsonObject{{QStringLiteral("roomId"), m_clients.value(socket).roomId},
                                                                    {QStringLiteral("seats"), seats},
                                                                    {QStringLiteral("yourSeatId"), m_clients.value(socket).seatId}});
}

void GameServer::handleJoinRoom(QWebSocket* socket, const QJsonObject& payload) {
    const qint64 expectedUserId = m_clients.value(socket).userId;
    const QString roomId = payload.value(QStringLiteral("roomId")).toString();
    if (roomId != m_roomId) {
        sendError(socket, QStringLiteral("ROOM_NOT_FOUND"), QStringLiteral("房间不存在或已关闭"));
        return;
    }
    if (m_roomStatus == QLatin1String("stopped")) {
        refreshRoomControlForJoin(socket, payload, expectedUserId);
        return;
    }
    completeJoinRoom(socket, payload, expectedUserId);
}

void GameServer::completeJoinRoom(QWebSocket* socket, const QJsonObject& payload,
                                  qint64 expectedUserId) {
    auto sessionIt = m_clients.find(socket);
    if (sessionIt == m_clients.end()) return;
    if (!sessionIt->authenticated || sessionIt->userId != expectedUserId
        || socket->state() != QAbstractSocket::ConnectedState) {
        sendError(socket, QStringLiteral("SESSION_REVOKED"),
                  QStringLiteral("账号会话已失效，请重新登录"));
        return;
    }
    if (m_roomStatus == QLatin1String("stopped")) {
        sendError(socket, QStringLiteral("ROOM_CLOSED"), QStringLiteral("房间当前未开启"));
        return;
    }
    if (m_roomStatus == QLatin1String("finished")) {
        sendError(socket, QStringLiteral("ROOM_FINISHED"), QStringLiteral("本局推演已结束，请等待管理员开启下一局"));
        return;
    }
    if (payload.value(QStringLiteral("asObserver")).toBool()) {
        sendError(socket, QStringLiteral("OBSERVER_NOT_AUTHORIZED"),
                  QStringLiteral("当前账号无权以观察员身份加入"));
        return;
    }
    ClientSession& session = sessionIt.value();
    if (!session.seatId.isEmpty()) {
        sendError(socket, QStringLiteral("ALREADY_SEATED"),
                  QStringLiteral("请先通过离开房间流程释放当前战位"));
        return;
    }
    session.roomId = payload.value(QStringLiteral("roomId")).toString();
    session.seatId.clear();
    session.seatType.clear();
    session.side.clear();
    session.seatReady = false;
    session.role = QStringLiteral("player");
    sendSeatDirectory(socket);
    sendFullSnapshot(socket);
    broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("roomPresence")},
                               {QStringLiteral("message"), QStringLiteral("%1 已进入房间大厅").arg(session.displayName)}});
}

void GameServer::handleLeaveRoom(QWebSocket* socket, const QJsonObject& payload) {
    ClientSession& session = m_clients[socket];
    bool lastRoomClient = session.roomId == m_roomId;
    if (lastRoomClient) {
        for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
            if (it.key() != socket && it->authenticated && it->roomId == m_roomId) {
                lastRoomClient = false;
                break;
            }
        }
    }
    if (lastRoomClient) {
        const ClientSession originalSession = session;
        session.roomId.clear();
        session.seatId.clear();
        session.seatType.clear();
        session.side.clear();
        session.seatReady = false;
        session.role = QStringLiteral("player");
        QString resetError;
        if (!resetRoomIfEmpty(&resetError)) {
            session = originalSession;
            sendError(socket, QStringLiteral("PERSISTENCE_FAILED"), resetError);
            return;
        }
        handleRoomList(socket);
        sendFullSnapshot(socket);
        broadcastSnapshots(true);
        return;
    }
    const RoomStateBackup backup = captureRoomState();
    QHash<qint64, QString> previousSeats;
    QHash<qint64, AuthoritativeRoom::Seat> previousSeatStates;
    for (const AuthoritativeRoom::Seat& seat : m_authoritativeRoom.seats()) {
        previousSeats.insert(seat.userId, seat.seatId);
        previousSeatStates.insert(seat.userId, seat);
    }
    const qint64 successorUserId = payload.value(QStringLiteral("successorUserId")).toInteger();
    const AuthoritativeRoom::Result result = successorUserId > 0
        ? m_authoritativeRoom.leave(session.userId, successorUserId)
        : m_authoritativeRoom.leaveRoom(session.userId);
    if (!result.ok) {
        sendError(socket, result.code, QStringLiteral("离开或指挥权移交请求被拒绝"));
        return;
    }
    const QString previousSeat = session.seatId;
    m_sharedIntel.remove(previousSeat);
    removeParticipantMarksForUser(session.userId, previousSeat);
    if (result.successorUserId > 0) {
        removeParticipantMarksForUser(result.successorUserId,
                                      previousSeats.value(result.successorUserId));
    }
    QString scenarioError;
    QStringList removedUnitIds;
    if (result.successorUserId > 0) {
        removedUnitIds.append(previousSeatStates.value(result.successorUserId).unitId);
    } else {
        removedUnitIds.append(previousSeatStates.value(session.userId).unitId);
    }
    if (!applyDepartureToRuntime(removedUnitIds, &scenarioError)) {
        restoreRoomStateBackup(backup);
        sendError(socket, QStringLiteral("RUNTIME_RESET_FAILED"), scenarioError);
        return;
    }
    syncAuthoritativeSeats();
    QString persistenceError;
    if (!persistRoomState(&persistenceError)) {
        restoreRoomStateBackup(backup);
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"), persistenceError);
        return;
    }
    session.roomId.clear();
    session.seatId.clear();
    session.seatType.clear();
    session.side.clear();
    session.seatReady = false;
    session.role = QStringLiteral("player");
    handleRoomList(socket);
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->authenticated && it->roomId == m_roomId) sendSeatDirectory(it.key());
    }
    sendFullSnapshot(socket);
    broadcastSnapshots(true);
}

void GameServer::handleClaimSeat(QWebSocket* socket, const QJsonObject& payload) {
    ClientSession& session = m_clients[socket];
    if (session.roomId != m_roomId) {
        sendError(socket, QStringLiteral("ROOM_REQUIRED"), QStringLiteral("请先选择推演房间"));
        return;
    }
    if (m_roomStatus == QLatin1String("preparing")
        && m_authoritativeRoom.phase() != QLatin1String("preparing")) {
        m_authoritativeRoom.applyOperation(
            QStringLiteral("internal-claim-prepare-%1").arg(m_scenarioRevision),
            QStringLiteral("reset"), m_authoritativeRoom.revision());
        syncAuthoritativeSeats();
        persistRoomState();
    }
    if (m_phase != QLatin1String("preparing")) {
        sendError(socket, QStringLiteral("SEAT_LOCKED"), QStringLiteral("仅准备阶段可以更换战位"));
        return;
    }
    const auto sendTransferEvent = [this](qint64 userId, const QJsonObject& event) {
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            if (it->authenticated && it->roomId == m_roomId && it->userId == userId) {
                sendEnvelope(it.key(), QStringLiteral("event"), event);
            }
        }
    };
    const auto sendTransferOutcome = [this, &sendTransferEvent](const QString& kind,
                                                                 const QJsonObject& transfer,
                                                                 quint64 revision,
                                                                 const QString& reason) {
        if (transfer.isEmpty()) return;
        QJsonObject event{{QStringLiteral("kind"), kind},
                          {QStringLiteral("revision"), static_cast<qint64>(revision)},
                          {QStringLiteral("requestRevision"), transfer.value(QStringLiteral("revision"))},
                          {QStringLiteral("userId"), transfer.value(QStringLiteral("userId"))},
                          {QStringLiteral("sourceSeatId"), transfer.value(QStringLiteral("sourceSeatId"))},
                          {QStringLiteral("targetSeatId"), transfer.value(QStringLiteral("targetSeatId"))},
                          {QStringLiteral("templateId"), transfer.value(QStringLiteral("templateId"))}};
        if (!reason.isEmpty()) event.insert(QStringLiteral("reason"), reason);
        sendTransferEvent(transfer.value(QStringLiteral("userId")).toInteger(), event);
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            if (it->authenticated && it->roomId == m_roomId
                && it->seatType == QLatin1String("commander")
                && it->side == transfer.value(QStringLiteral("sourceSide")).toString()) {
                sendEnvelope(it.key(), QStringLiteral("event"), event);
            }
        }
    };
    const qint64 approvedUserId = payload.value(QStringLiteral("approveUserId")).toInteger();
    if (approvedUserId > 0) {
        const QJsonObject transfer = m_authoritativeRoom.pendingTransfer(approvedUserId);
        const RoomStateBackup backup = captureRoomState();
        const AuthoritativeRoom::Result approval = m_authoritativeRoom.approveTransfer(
            session.userId, approvedUserId,
            static_cast<quint64>(payload.value(QStringLiteral("requestedRevision")).toInteger()));
        if (!approval.ok) {
            sendTransferOutcome(QStringLiteral("transferRejected"), transfer, approval.revision,
                                approval.code);
            sendError(socket, approval.code, QStringLiteral("战位切换批准失败"));
            return;
        }
        removeParticipantMarksForUser(approvedUserId,
                                      transfer.value(QStringLiteral("sourceSeatId")).toString());
        QString scenarioError;
        if (!applyDeployedScenario(&scenarioError)) {
            restoreRoomStateBackup(backup);
            sendTransferOutcome(QStringLiteral("transferRejected"), transfer, approval.revision,
                                QStringLiteral("RUNTIME_RESET_FAILED"));
            sendError(socket, QStringLiteral("RUNTIME_RESET_FAILED"), scenarioError);
            return;
        }
        syncAuthoritativeSeats();
        QString persistenceError;
        if (!persistRoomState(&persistenceError)) {
            restoreRoomStateBackup(backup);
            sendTransferOutcome(QStringLiteral("transferRejected"), transfer, approval.revision,
                                QStringLiteral("PERSISTENCE_FAILED"));
            sendError(socket, QStringLiteral("PERSISTENCE_FAILED"), persistenceError);
            return;
        }
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            if (it->authenticated && it->roomId == m_roomId) sendSeatDirectory(it.key());
        }
        broadcastSnapshots(true);
        sendTransferOutcome(QStringLiteral("transferCompleted"), transfer, approval.revision, {});
        return;
    }
    const qint64 rejectedUserId = payload.value(QStringLiteral("rejectUserId")).toInteger();
    if (rejectedUserId > 0) {
        const QJsonObject transfer = m_authoritativeRoom.pendingTransfer(rejectedUserId);
        const QJsonObject beforeRoom = m_authoritativeRoom.toJson();
        const AuthoritativeRoom::Result rejection = m_authoritativeRoom.rejectTransfer(
            session.userId, rejectedUserId,
            static_cast<quint64>(payload.value(QStringLiteral("requestedRevision")).toInteger()));
        if (!rejection.ok) {
            sendTransferOutcome(QStringLiteral("transferRejected"), transfer, rejection.revision,
                                rejection.code);
            sendError(socket, rejection.code, QStringLiteral("战位切换拒绝失败"));
            return;
        }
        QString persistenceError;
        if (!persistRoomState(&persistenceError)) {
            m_authoritativeRoom.restore(beforeRoom);
            syncAuthoritativeSeats();
            sendError(socket, QStringLiteral("PERSISTENCE_FAILED"), persistenceError);
            return;
        }
        sendTransferOutcome(QStringLiteral("transferRejected"), transfer, rejection.revision,
                            QStringLiteral("COMMANDER_REJECTED"));
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            if (it->authenticated && it->roomId == m_roomId) sendSeatDirectory(it.key());
        }
        return;
    }
    if (payload.value(QStringLiteral("cancelTransfer")).toBool()) {
        const QJsonObject transfer = m_authoritativeRoom.pendingTransfer(session.userId);
        const QJsonObject beforeRoom = m_authoritativeRoom.toJson();
        const AuthoritativeRoom::Result cancellation = m_authoritativeRoom.cancelTransfer(
            session.userId,
            static_cast<quint64>(payload.value(QStringLiteral("requestedRevision")).toInteger()));
        if (!cancellation.ok) {
            sendTransferOutcome(QStringLiteral("transferRejected"), transfer, cancellation.revision,
                                cancellation.code);
            sendError(socket, cancellation.code, QStringLiteral("战位切换取消失败"));
            return;
        }
        QString persistenceError;
        if (!persistRoomState(&persistenceError)) {
            m_authoritativeRoom.restore(beforeRoom);
            syncAuthoritativeSeats();
            sendError(socket, QStringLiteral("PERSISTENCE_FAILED"), persistenceError);
            return;
        }
        sendTransferOutcome(QStringLiteral("transferRejected"), transfer, cancellation.revision,
                            QStringLiteral("REQUESTER_CANCELLED"));
        sendSeatDirectory(socket);
        return;
    }
    const QString requestedSeatId = payload.value(QStringLiteral("seatId")).toString();
    const SeatDescriptor descriptor = describeSeat(requestedSeatId);
    const int capacity = m_seatLimits.value(descriptor.baseId, -1);
    if (capacity < 0 || descriptor.side.isEmpty() || descriptor.type.isEmpty()) {
        sendError(socket, QStringLiteral("SEAT_NOT_FOUND"), QStringLiteral("战位不存在"));
        return;
    }
    if (capacity == 0 || descriptor.index > capacity
        || (descriptor.type == QLatin1String("commander") && descriptor.index != 1)) {
        sendError(socket, QStringLiteral("SEAT_NOT_FOUND"), QStringLiteral("战位不存在或已超出房间容量"));
        return;
    }
    const QString seatId = canonicalSeatId(descriptor.baseId, descriptor.index);
    const QString seatType = descriptor.type;
    const QString side = descriptor.side;
    if (m_seats.contains(seatId) && m_seats.value(seatId).userId != session.userId) {
        sendError(socket, QStringLiteral("SEAT_OCCUPIED"), QStringLiteral("该战位已被占用"));
        return;
    }
    const bool redCommander = m_seats.contains(QStringLiteral("red_commander"));
    const bool blueCommander = m_seats.contains(QStringLiteral("blue_commander"));
    if ((seatType != QLatin1String("commander")) && (!redCommander || !blueCommander)) {
        sendError(socket, QStringLiteral("COMMANDER_PRIORITY"), QStringLiteral("双方指挥官尚未就位，请先选择空缺的指挥官战位"));
        return;
    }
    const QString templateId = payload.value(QStringLiteral("templateId")).toString(
        seatType == QLatin1String("commander") ? QStringLiteral("commandpost")
        : seatType == QLatin1String("attack") ? QStringLiteral("attackuav")
        : seatType == QLatin1String("recon") ? QStringLiteral("reconuav")
        : seatType == QLatin1String("ground") ? QStringLiteral("groundscout")
                                               : QStringLiteral("jammeruav"));
    if (!session.seatId.isEmpty() && session.seatId != seatId) {
        const QJsonObject beforeRoom = m_authoritativeRoom.toJson();
        const AuthoritativeRoom::Result transfer = m_authoritativeRoom.requestTransfer(
            session.userId, seatId, templateId);
        if (transfer.code == QLatin1String("TRANSFER_PENDING")) {
            QString persistenceError;
            if (!persistRoomState(&persistenceError)) {
                m_authoritativeRoom.restore(beforeRoom);
                syncAuthoritativeSeats();
                sendError(socket, QStringLiteral("PERSISTENCE_FAILED"), persistenceError);
                return;
            }
            const QJsonObject pending = m_authoritativeRoom.pendingTransfer(session.userId);
            sendTransferOutcome(QStringLiteral("transferRequested"), pending,
                                transfer.revision, {});
            sendSeatDirectory(socket);
        } else {
            sendError(socket, transfer.code, QStringLiteral("战位切换请求被拒绝"));
        }
        return;
    }
    const QJsonObject before = m_authoritativeRoom.toJson();
    const AuthoritativeRoom::Result claimed = m_authoritativeRoom.claimSeat(
        session.userId, session.username, seatId, templateId);
    if (!claimed.ok) {
        sendError(socket, claimed.code, QStringLiteral("战位选择被服务器拒绝"));
        return;
    }
    if (!session.seatId.isEmpty()) {
        const SeatDescriptor previous = describeSeat(session.seatId);
        m_seats.remove(session.seatId);
        m_sharedIntel.remove(session.seatId);
        if (previous.type == QLatin1String("commander")) {
            if (previous.side == QLatin1String("red")) m_redReady = false;
            if (previous.side == QLatin1String("blue")) m_blueReady = false;
        }
    }
    session.roomId = m_roomId;
    session.seatId = seatId;
    session.seatType = seatType;
    session.side = side;
    session.seatReady = false;
    m_seats.insert(seatId, SeatOccupant{seatId, seatType, side, session.userId, session.username, false});
    syncAuthoritativeSeats();
    QString claimPersistenceError;
    if (!persistRoomState(&claimPersistenceError)) {
        m_authoritativeRoom.restore(before);
        syncAuthoritativeSeats();
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"), claimPersistenceError);
        return;
    }
    const QString mappedUnitId = m_authoritativeRoom.seat(seatId).unitId;
    sendSeatDirectory(socket);
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->authenticated && it->roomId == m_roomId) sendSeatDirectory(it.key());
    }
    sendFullSnapshot(socket);
    if (seatType != QLatin1String("commander")) {
        const QString sideName = side == QLatin1String("red") ? QStringLiteral("红方") : QStringLiteral("蓝方");
        const QJsonObject prompt{{QStringLiteral("unitId"), mappedUnitId},
                                 {QStringLiteral("seatId"), seatId},
                                 {QStringLiteral("targetSeatId"), seatId},
                                 {QStringLiteral("message"), QStringLiteral("请为%1选择初始部署位置").arg(sideName)}};
        sendEnvelope(socket, QStringLiteral("deploymentPrompt"),
                     QJsonObject{{QStringLiteral("unitId"), mappedUnitId},
                                 {QStringLiteral("seatId"), seatId},
                                 {QStringLiteral("message"), QStringLiteral("等待%1指挥官选择初始部署位置").arg(sideName)}});
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            if (it->authenticated && it->roomId == m_roomId && it->side == side
                && it->seatType == QLatin1String("commander")) {
                sendEnvelope(it.key(), QStringLiteral("deploymentPrompt"), prompt);
            }
        }
    }
    broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("seatClaimed")},
                               {QStringLiteral("seatId"), seatId}}, side);
}

void GameServer::handleSeatReady(QWebSocket* socket, const QJsonObject& payload) {
    ClientSession& session = m_clients[socket];
    if (session.seatId.isEmpty() || m_roomStatus == QLatin1String("stopped")
        || m_phase != QLatin1String("preparing")) {
        sendError(socket, QStringLiteral("SEAT_REQUIRED"), QStringLiteral("当前阶段不能提交战位就绪"));
        return;
    }
    const QJsonObject before = m_authoritativeRoom.toJson();
    const bool ready = payload.value(QStringLiteral("ready")).toBool();
    const AuthoritativeRoom::Result result = m_authoritativeRoom.setReady(session.userId, ready);
    if (!result.ok) {
        sendError(socket, result.code, QStringLiteral("单位部署完成后才能就绪"));
        return;
    }
    session.seatReady = ready;
    if (m_seats.contains(session.seatId)) m_seats[session.seatId].ready = session.seatReady;
    if (session.seatType == QLatin1String("commander")) {
        if (session.side == QLatin1String("red")) m_redReady = session.seatReady;
        if (session.side == QLatin1String("blue")) m_blueReady = session.seatReady;
    }
    syncAuthoritativeSeats();
    QString readyPersistenceError;
    if (!persistRoomState(&readyPersistenceError)) {
        m_authoritativeRoom.restore(before);
        syncAuthoritativeSeats();
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"), readyPersistenceError);
        return;
    }
    sendSeatDirectory(socket);
    broadcastSnapshots();
}

void GameServer::handleSetUnitName(QWebSocket* socket, const QJsonObject& payload) {
    const ClientSession& session = m_clients.value(socket);
    if (session.seatId.isEmpty() || m_phase != QLatin1String("preparing")) {
        sendError(socket, QStringLiteral("INVALID_STATE"), QStringLiteral("准备阶段且已进入战位后才能设置单位名称"));
        return;
    }
    const QJsonObject before = m_authoritativeRoom.toJson();
    const quint64 revisionBeforeName = m_scenarioRevision;
    const AuthoritativeRoom::Result result = m_authoritativeRoom.setUnitName(
        session.userId, payload.value(QStringLiteral("unitName")).toString());
    if (!result.ok) {
        sendError(socket, result.code, QStringLiteral("单位显示名称无效"));
        return;
    }
    QString scenarioError;
    if (!applyDeployedScenario(&scenarioError)) {
        m_authoritativeRoom.restore(before);
        m_scenarioRevision = revisionBeforeName;
        sendError(socket, QStringLiteral("INVALID_UNIT_NAME"), scenarioError);
        return;
    }
    if (m_scenarioRevision == revisionBeforeName) ++m_scenarioRevision;
    QString persistenceError;
    if (!persistRoomState(&persistenceError)) {
        m_authoritativeRoom.restore(before);
        applyDeployedScenario();
        m_scenarioRevision = revisionBeforeName;
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"), persistenceError);
        return;
    }
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->authenticated && it->roomId == m_roomId) sendSeatDirectory(it.key());
    }
    broadcastSnapshots(true);
}

void GameServer::handleDeployment(QWebSocket* socket, const QJsonObject& payload) {
    const ClientSession& session = m_clients.value(socket);
    if (m_roomStatus == QLatin1String("stopped")
        || !hasSeatPermission(session, QStringLiteral("deployment")) || m_phase != QLatin1String("preparing")) {
        sendError(socket, QStringLiteral("PERMISSION_DENIED"), QStringLiteral("只有准备阶段的指挥官可以部署单位"));
        return;
    }
    const QString unitId = payload.value(QStringLiteral("unitId")).toString();
    const QJsonObject point = payload.value(QStringLiteral("position")).toObject();
    QString targetSeatId = payload.value(QStringLiteral("targetSeatId")).toString();
    if (targetSeatId.isEmpty()) {
        for (const auto& seat : m_authoritativeRoom.seats()) {
            if (seat.unitId == unitId) {
                targetSeatId = seat.seatId;
                break;
            }
        }
    }
    const AuthoritativeRoom::Seat targetSeat = m_authoritativeRoom.seat(targetSeatId);
    if (targetSeat.userId <= 0 || targetSeat.side != session.side
        || !point.contains(QStringLiteral("x")) || !point.contains(QStringLiteral("y"))) {
        sendError(socket, QStringLiteral("INVALID_DEPLOYMENT"), QStringLiteral("只能部署本方有效单位"));
        return;
    }
    const double x = point.value(QStringLiteral("x")).toDouble();
    const double y = point.value(QStringLiteral("y")).toDouble();
    const QJsonObject map = m_engine.mapInfo();
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || y < 0.0
        || x > map.value(QStringLiteral("widthMeters")).toDouble()
        || y > map.value(QStringLiteral("heightMeters")).toDouble()) {
        sendError(socket, QStringLiteral("INVALID_DEPLOYMENT"), QStringLiteral("部署位置超出地图范围"));
        return;
    }
    const QJsonObject before = m_authoritativeRoom.toJson();
    const AuthoritativeRoom::Result deployed = m_authoritativeRoom.deploy(
        session.userId, targetSeatId,
        GeoPos{x, y, point.value(QStringLiteral("alt")).toDouble()});
    if (!deployed.ok) {
        sendError(socket, deployed.code, QStringLiteral("部署请求被服务器拒绝"));
        return;
    }
    QString scenarioError;
    if (!applyDeployedScenario(&scenarioError)) {
        m_authoritativeRoom.restore(before);
        sendError(socket, QStringLiteral("INVALID_DEPLOYMENT"), scenarioError);
        return;
    }
    QString persistenceError;
    if (!persistRoomState(&persistenceError)) {
        m_authoritativeRoom.restore(before);
        applyDeployedScenario();
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"),
                  QStringLiteral("部署位置未能持久化: %1").arg(persistenceError));
        return;
    }
    broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("deploymentAssigned")},
                               {QStringLiteral("unitId"),
                                m_authoritativeRoom.seat(targetSeatId).unitId}}, session.side);
    broadcastSnapshots(true);
}

void GameServer::handleRedeployRequest(QWebSocket* socket) {
    const ClientSession& session = m_clients.value(socket);
    if (session.roomId != m_roomId || session.seatId.isEmpty()
        || session.seatType == QLatin1String("commander") || m_phase != QLatin1String("preparing")) {
        sendError(socket, QStringLiteral("PERMISSION_DENIED"), QStringLiteral("只有准备阶段的友方单位可以申请重新部署"));
        return;
    }
    const QJsonObject before = m_authoritativeRoom.toJson();
    const AuthoritativeRoom::Result result = m_authoritativeRoom.requestRedeploy(session.userId);
    if (!result.ok) {
        sendError(socket, result.code, QStringLiteral("重新部署申请被拒绝"));
        return;
    }
    syncAuthoritativeSeats();
    QString persistenceError;
    if (!persistRoomState(&persistenceError)) {
        m_authoritativeRoom.restore(before);
        syncAuthoritativeSeats();
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"), persistenceError);
        return;
    }
    broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("redeployRequested")},
                               {QStringLiteral("seatId"), session.seatId}}, session.side);
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->authenticated && it->roomId == m_roomId) sendSeatDirectory(it.key());
    }
    broadcastSnapshots();
}

void GameServer::handleRedeploy(QWebSocket* socket, const QJsonObject& payload) {
    const ClientSession& session = m_clients.value(socket);
    if (session.roomId != m_roomId || session.seatType != QLatin1String("commander")
        || m_phase != QLatin1String("preparing")) {
        sendError(socket, QStringLiteral("PERMISSION_DENIED"), QStringLiteral("只有准备阶段的指挥官可以重新部署本方单位"));
        return;
    }
    const RoomStateBackup backup = captureRoomState();
    const QString targetSeatId = payload.value(QStringLiteral("seatId")).toString();
    const AuthoritativeRoom::Result result = m_authoritativeRoom.redeploy(
        session.userId, targetSeatId);
    if (!result.ok) {
        sendError(socket, result.code, QStringLiteral("重新部署请求被拒绝"));
        return;
    }
    QString scenarioError;
    if (!applyDeployedScenario(&scenarioError)) {
        restoreRoomStateBackup(backup);
        sendError(socket, QStringLiteral("RUNTIME_RESET_FAILED"), scenarioError);
        return;
    }
    syncAuthoritativeSeats();
    QString persistenceError;
    if (!persistRoomState(&persistenceError)) {
        restoreRoomStateBackup(backup);
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"), persistenceError);
        return;
    }
    broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("redeployed")},
                               {QStringLiteral("side"), session.side},
                               {QStringLiteral("seatId"), targetSeatId}}, session.side);
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->authenticated && it->roomId == m_roomId) sendSeatDirectory(it.key());
    }
    broadcastSnapshots(true);
}

void GameServer::handleShareIntel(QWebSocket* socket, const QJsonObject& payload) {
    const ClientSession& sender = m_clients.value(socket);
    if (m_roomStatus == QLatin1String("stopped") || sender.seatId.isEmpty() || sender.roomId != m_roomId) {
        sendError(socket, QStringLiteral("SEAT_REQUIRED"), QStringLiteral("请先选择战位"));
        return;
    }
    const QString targetId = payload.value(QStringLiteral("targetId")).toString();
    if (!visibleUnitIds(sender).contains(targetId)) {
        sendError(socket, QStringLiteral("TARGET_NOT_VISIBLE"), QStringLiteral("只能共享当前视角已掌握的信息"));
        return;
    }
    UnitBase* senderUnit = seatUnit(sender.seatId);
    if (!senderUnit) {
        sendError(socket, QStringLiteral("SEAT_UNIT_NOT_FOUND"), QStringLiteral("当前战位没有对应的仿真单位"));
        return;
    }
    const QJsonArray recipients = payload.value(QStringLiteral("recipientSeatIds")).toArray();
    for (const QJsonValue& value : recipients) {
        const QString recipientSeat = value.toString();
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            if (!it->authenticated || it->seatId != recipientSeat || it->side != sender.side) continue;
            UnitBase* recipientUnit = seatUnit(it->seatId);
            if (!recipientUnit) continue;
            if (!StateProjector::canTransmit(m_engine, senderUnit->id(), recipientUnit->id())) continue;
            m_sharedIntel[recipientSeat].insert(targetId);
            sendEnvelope(it.key(), QStringLiteral("intelShare"),
                         QJsonObject{{QStringLiteral("senderSeatId"), sender.seatId},
                                     {QStringLiteral("targetId"), targetId},
                                     {QStringLiteral("sharedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                                     {QStringLiteral("note"), payload.value(QStringLiteral("note"))}});
        }
    }
}

void GameServer::handleMapMark(QWebSocket* socket, const QJsonObject& payload) {
    ClientSession& sender = m_clients[socket];
    if (sender.roomId != m_roomId || sender.seatId.isEmpty() || sender.side.isEmpty()) {
        sendError(socket, QStringLiteral("SEAT_REQUIRED"), QStringLiteral("请先选择战位"));
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    MapMarkRateWindow& rateWindow = m_mapMarkRateWindows[sender.seatId];
    if (rateWindow.startedAt == 0 || now - rateWindow.startedAt >= 1000) {
        rateWindow.startedAt = now;
        rateWindow.count = 0;
    }
    if (++rateWindow.count > kMaxMapMarksPerSecond) {
        sendError(socket, QStringLiteral("MAP_MARK_RATE_LIMIT"),
                  QStringLiteral("地图标记发送频率过高"));
        return;
    }
    const QJsonObject position = payload.value(QStringLiteral("position")).toObject();
    const double x = position.value(QStringLiteral("x")).toDouble();
    const double y = position.value(QStringLiteral("y")).toDouble();
    const QJsonObject map = m_engine.mapInfo();
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || y < 0.0
        || x > map.value(QStringLiteral("widthMeters")).toDouble()
        || y > map.value(QStringLiteral("heightMeters")).toDouble()) {
        sendError(socket, QStringLiteral("INVALID_MAP_MARK"), QStringLiteral("地图标记超出地图范围"));
        return;
    }
    UnitBase* senderUnit = seatUnit(sender.seatId);
    if (!senderUnit) {
        sendError(socket, QStringLiteral("SEAT_UNIT_NOT_FOUND"), QStringLiteral("当前战位没有对应的仿真单位"));
        return;
    }
    QSet<QString> deliveredSeats{sender.seatId};
    if (sender.seatType != QLatin1String("commander")) {
        deliveredSeats.insert(sender.side + QStringLiteral("_commander"));
    }
    QJsonArray requestedRecipients = payload.value(QStringLiteral("recipientSeatIds")).toArray();
    if (sender.seatType == QLatin1String("commander") && requestedRecipients.isEmpty()) {
        for (const AuthoritativeRoom::Seat& seat : m_authoritativeRoom.seats()) {
            if (seat.side == sender.side && seat.seatId != sender.seatId && seat.deployed) {
                requestedRecipients.append(seat.seatId);
            }
        }
    }
    for (const QJsonValue& value : requestedRecipients) {
        const QString requestedSeat = value.toString();
        const AuthoritativeRoom::Seat recipient = m_authoritativeRoom.seat(requestedSeat);
        if (recipient.seatId.isEmpty() || recipient.side != sender.side
            || !recipient.deployed) continue;
        UnitBase* recipientUnit = seatUnit(recipient.seatId);
        if (recipientUnit && StateProjector::canTransmit(
                m_engine, senderUnit->id(), recipientUnit->id())) {
            deliveredSeats.insert(requestedSeat);
        }
    }
    QStringList delivered = deliveredSeats.values();
    delivered.sort();
    QJsonArray visibleTo;
    for (const QString& seatId : delivered) visibleTo.append(seatId);
    const QString markType = sender.seatType == QLatin1String("commander")
        ? QStringLiteral("commander") : QStringLiteral("self");
    const QJsonObject event{{QStringLiteral("kind"), QStringLiteral("mapMark")},
                            {QStringLiteral("seatId"), sender.seatId},
                            {QStringLiteral("side"), sender.side},
                            {QStringLiteral("markType"), markType},
                            {QStringLiteral("authorUserId"), sender.userId},
                            {QStringLiteral("position"), position},
                            {QStringLiteral("label"), payload.value(QStringLiteral("label"))},
                            {QStringLiteral("visibleToSeatIds"), visibleTo},
                            {QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    const QJsonArray previousMarks = m_mapMarks;
    appendMapMark(event);
    QString persistenceError;
    if (!persistRoomState(&persistenceError)) {
        m_mapMarks = previousMarks;
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"),
                  QStringLiteral("地图标记未能持久化: %1").arg(persistenceError));
        return;
    }
    QJsonObject projectedEvent = event;
    projectedEvent.remove(QStringLiteral("authorUserId"));
    projectedEvent.remove(QStringLiteral("visibleToSeatIds"));
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (!it->authenticated || it->roomId != m_roomId
            || !deliveredSeats.contains(it->seatId)) continue;
        sendEnvelope(it.key(), QStringLiteral("event"), projectedEvent);
    }
    broadcastSnapshots();
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
        QStringLiteral("unitOrder"), QStringLiteral("attackAt"),
        QStringLiteral("engageTarget"), QStringLiteral("moveTo"),
        QStringLiteral("withdraw"), QStringLiteral("setSpeed"),
        QStringLiteral("pursue"), QStringLiteral("guideAttack"),
        QStringLiteral("setSchedule"), QStringLiteral("halt"),
        QStringLiteral("service"), QStringLiteral("cancelEngagement"),
        QStringLiteral("setRoe")};
    if (!actions.contains(action)) {
        return reject(QStringLiteral("UNKNOWN_ACTION"), QStringLiteral("未知操作"));
    }
    if (session.seatId.isEmpty()) {
        return reject(QStringLiteral("SEAT_REQUIRED"), QStringLiteral("选择战位后才能下达指令"));
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
    if (action != QLatin1String("unitOrder") && !unit->movable()) {
        return reject(QStringLiteral("UNIT_NOT_MOVABLE"),
                      QStringLiteral("该操作仅适用于可移动单元"));
    }
    const QString effectiveRole = normalizedRole(session);
    const QString side = session.side.isEmpty() ? sideForRole(effectiveRole) : session.side;
    if (!StateProjector::canControlSide(effectiveRole, unit->sideStr())) {
        return reject(QStringLiteral("UNIT_NOT_OWNED"),
                      QStringLiteral("不能控制其他阵营单元"));
    }
    UnitBase* seatUnit = this->seatUnit(session.seatId);
    if (!seatUnit) {
        return reject(QStringLiteral("SEAT_UNIT_NOT_FOUND"), QStringLiteral("当前战位没有对应的仿真单位"));
    }
    if (session.seatType != QLatin1String("commander")) {
        const QString requiredKind = session.seatType == QLatin1String("attack") ? QStringLiteral("attackuav")
            : session.seatType == QLatin1String("recon") ? QStringLiteral("reconuav")
            : session.seatType == QLatin1String("ground") ? QStringLiteral("groundscout")
            : session.seatType == QLatin1String("jammer") ? QStringLiteral("jammeruav") : QString();
        if (unit->id() != seatUnit->id()
            || (!requiredKind.isEmpty() && unit->kindStr() != requiredKind)) {
            return reject(QStringLiteral("SEAT_UNIT_MISMATCH"), QStringLiteral("当前战位不能控制该类型单位"));
        }
    } else if (unit->id() != seatUnit->id()
               && !StateProjector::canTransmit(m_engine, seatUnit->id(), unit->id())) {
        return reject(QStringLiteral("COMMUNICATION_LOST"),
                      QStringLiteral("指挥官与目标单位之间没有可用通信链路"));
    }
    if ((action == QLatin1String("attackAt") || action == QLatin1String("setFlightPlan"))
        && unit->kind() != UnitKind::AttackUAV) {
        return reject(QStringLiteral("INVALID_UNIT_KIND"),
                      QStringLiteral("航路指令仅适用于攻击无人机"));
    }
    if ((action == QLatin1String("cancelEngagement") || action == QLatin1String("setRoe"))
        && unit->kind() != UnitKind::AttackUAV) {
        return reject(QStringLiteral("INVALID_UNIT_KIND"),
                      QStringLiteral("交战控制仅适用于攻击无人机"));
    }
    if (action == QLatin1String("setRoe")) {
        const QString roe = args.value(QStringLiteral("roe")).toString();
        if (roe != QLatin1String("hold") && roe != QLatin1String("free")) {
            return reject(QStringLiteral("INVALID_ARGUMENT"),
                          QStringLiteral("交战规则无效"));
        }
    }
    if (action == QLatin1String("guideAttack")) {
        UnitBase* attacker = m_engine.unit(args.value(QStringLiteral("attackerId")).toString());
        if (unit->kind() != UnitKind::GroundScout || !attacker || !attacker->alive()
            || attacker->kind() != UnitKind::AttackUAV || attacker->side() != unit->side()
            || (!side.isEmpty() && attacker->sideStr() != side)) {
            return reject(QStringLiteral("INVALID_TARGET"),
                          QStringLiteral("不能为其他阵营攻击单元提供引导"));
        }
        if (!StateProjector::canTransmit(m_engine, unit->id(), attacker->id())) {
            return reject(QStringLiteral("COMMUNICATION_LOST"),
                          QStringLiteral("引导单元与攻击机之间没有可用通信链路"));
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
    if ((action == QLatin1String("attackAt") || action == QLatin1String("moveTo"))
        && !validPoint(args.value(QStringLiteral("pos")).toMap())) {
        return reject(QStringLiteral("INVALID_ARGUMENT"),
                      QStringLiteral("命令目标超出地图边界"));
    }
    if (action == QLatin1String("withdraw") && args.contains(QStringLiteral("pos"))
        && !validPoint(args.value(QStringLiteral("pos")).toMap())) {
        return reject(QStringLiteral("INVALID_ARGUMENT"),
                      QStringLiteral("撤离位置超出地图边界"));
    }
    if (action == QLatin1String("unitOrder")) {
        const QString text = args.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty() || text.size() > 420) {
            return reject(QStringLiteral("INVALID_ARGUMENT"),
                          QStringLiteral("文本命令不能为空且不能超过 420 字"));
        }
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
                          QStringLiteral("速度必须大于 0 且不超过 1000"));
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
    const qint64 stateRevision = payload.value(QStringLiteral("stateRevision")).toInteger();
    QVariantMap args = payload.value(QStringLiteral("args")).toObject().toVariantMap();
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
    // Snapshots advance on every simulation tick. A client can legitimately
    // act on the immediately preceding snapshot, while ownership and target
    // validation below still guard the command against current world state.
    if (stateRevision > static_cast<qint64>(m_stateRevision)) {
        sendCommandResult(socket, commandId,
                          CommandResult::reject(QStringLiteral("STALE_REVISION"),
                                                QStringLiteral("命令基于服务器尚未发布的状态版本")));
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
    static const QSet<QString> commanderOrderActions{
        QStringLiteral("unitOrder"), QStringLiteral("assignTarget"),
        QStringLiteral("moveTo"), QStringLiteral("withdraw")};
    if (session.seatType == QLatin1String("commander")
        && commanderOrderActions.contains(action)) {
        args.insert(QStringLiteral("notificationOnly"), true);
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
    sendCommandResult(socket, commandId, result);
}

void GameServer::handleControl(QWebSocket* socket, const QJsonObject& payload) {
    Q_UNUSED(payload);
    // 推演生命周期属于网页管理员，客户端战位不能伪装导演席控制
    // start/stop/pause/speed 等全局状态。
    sendError(socket, QStringLiteral("PERMISSION_DENIED"),
              QStringLiteral("推演生命周期由网页管理员控制"));
}

void GameServer::handleReady(QWebSocket* socket, const QJsonObject& payload) {
    // 兼容旧客户端消息名，但统一转入独立战位就绪流程。
    handleSeatReady(socket, payload);
}

void GameServer::handleChat(QWebSocket* socket, const QJsonObject& payload) {
    ClientSession& session = m_clients[socket];
    if (session.seatId.isEmpty() || session.roomId != m_roomId) {
        sendError(socket, QStringLiteral("SEAT_REQUIRED"), QStringLiteral("选择战位后才能通信"));
        return;
    }
    QString text = payload.value(QStringLiteral("text")).toString().trimmed();
    if (text.isEmpty()) return;
    if (text.size() > 500) text.truncate(500);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - session.lastChatAt < 600) {
        sendError(socket, QStringLiteral("CHAT_RATE_LIMIT"), QStringLiteral("消息发送过快"));
        return;
    }
    session.lastChatAt = now;
    const bool preparing = m_phase == QLatin1String("preparing");
    const bool rangeRestricted = m_phase == QLatin1String("running")
        || m_phase == QLatin1String("paused");
    UnitBase* senderUnit = preparing ? nullptr : seatUnit(session.seatId);
    if (!preparing && !senderUnit) {
        sendError(socket, QStringLiteral("SEAT_UNIT_NOT_FOUND"), QStringLiteral("当前战位没有对应的仿真单位"));
        return;
    }
    QSet<QString> deliveredSeats;
    for (const QJsonValue& value : payload.value(QStringLiteral("recipientSeatIds")).toArray()) {
        const QString requestedSeat = value.toString();
        for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
            if (!it->authenticated || it->roomId != m_roomId || it->seatId != requestedSeat
                || it->side != session.side) continue;
            UnitBase* recipientUnit = preparing ? nullptr : seatUnit(it->seatId);
            const bool bilateral = rangeRestricted && recipientUnit
                && StateProjector::canTransmit(m_engine, senderUnit->id(), recipientUnit->id())
                && StateProjector::canTransmit(m_engine, recipientUnit->id(), senderUnit->id());
            if (preparing || bilateral) {
                deliveredSeats.insert(requestedSeat);
            }
        }
    }
    if (deliveredSeats.isEmpty()) {
        sendError(socket, QStringLiteral("COMMUNICATION_LOST"),
                  QStringLiteral("指定战位当前均不在可用通信链路内"));
        return;
    }
    QStringList delivered = deliveredSeats.values();
    delivered.sort();
    QJsonArray recipients;
    for (const QString& seatId : delivered) recipients.append(seatId);
    const QJsonObject message{
        {QStringLiteral("id"), QStringLiteral("chat_%1").arg(++m_chatSequence)},
        {QStringLiteral("username"), session.username},
        {QStringLiteral("displayName"), session.displayName},
        {QStringLiteral("seatId"), session.seatId},
        {QStringLiteral("side"), session.side},
        {QStringLiteral("recipientSeatIds"), recipients},
        {QStringLiteral("text"), text},
        {QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    m_chatHistory.append(message);
    while (m_chatHistory.size() > 100) m_chatHistory.removeFirst();
    broadcastChat(message);
}

void GameServer::handleFastDdsEnvelope(const QString& topic, const QJsonObject& payload) {
    if (topic == QLatin1String("Heartbeat") || topic == QLatin1String("heartbeat")) return;
    if (topic != QLatin1String("ChatMessage") && topic != QLatin1String("MapMark")) return;
    const QString seatId = payload.value(QStringLiteral("seatId")).toString();
    if (seatId.isEmpty()) return;
    QWebSocket* socket = nullptr;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->authenticated && it->roomId == m_roomId && it->seatId == seatId) {
            socket = it.key();
            break;
        }
    }
    if (!socket) return;
    QJsonObject sanitized = payload;
    sanitized.remove(QStringLiteral("ddsTicket"));
    sanitized.remove(QStringLiteral("seatId"));
    const QString protocolType = topic == QLatin1String("ChatMessage")
        ? QStringLiteral("chat") : QStringLiteral("mapMark");
    const Protocol::ValidationResult validation =
        Protocol::validateClientPayload(protocolType, sanitized);
    if (!validation.valid) {
        audit(QStringLiteral("security"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("fastDdsPayloadRejected")},
                          {QStringLiteral("topic"), topic},
                          {QStringLiteral("code"), validation.code},
                          {QStringLiteral("message"), validation.message},
                          {QStringLiteral("seatId"), seatId}});
        return;
    }
    if (topic == QLatin1String("ChatMessage")) {
        handleChat(socket, sanitized);
    } else {
        handleMapMark(socket, sanitized);
    }
}

void GameServer::handleScenarioUpsert(QWebSocket* socket, const QJsonObject& payload) {
    Q_UNUSED(payload);
    sendError(socket, QStringLiteral("PERMISSION_DENIED"), QStringLiteral("初始阵容由网页管理员配置"));
}

void GameServer::handleScenarioRemove(QWebSocket* socket, const QJsonObject& payload) {
    Q_UNUSED(payload);
    sendError(socket, QStringLiteral("PERMISSION_DENIED"), QStringLiteral("初始阵容由网页管理员配置"));
}

void GameServer::handleScenarioReplace(QWebSocket* socket, const QJsonObject& payload) {
    Q_UNUSED(payload);
    sendError(socket, QStringLiteral("PERMISSION_DENIED"), QStringLiteral("初始阵容由网页管理员配置"));
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
    bool checkpointRequired = false;
    QString appendError;
    if (!m_persistence.appendEvent(nextSequence, kind, payload, &appendError,
                                   &checkpointRequired)) {
        if (!checkpointRequired) {
            if (error) *error = appendError;
            return false;
        }
        QString checkpointError;
        if (!persistRoomState(&checkpointError)) {
            if (error) {
                *error = QStringLiteral("%1；更新检查点失败: %2")
                             .arg(appendError, checkpointError);
            }
            return false;
        }
        if (!m_persistence.appendEvent(nextSequence, kind, payload, &appendError)) {
            if (error) *error = appendError;
            return false;
        }
    }
    m_eventSequence = nextSequence;
    return true;
}

bool GameServer::persistRoomState(QString* error) {
    RoomCheckpoint checkpoint;
    checkpoint.scenario = m_engine.scenario();
    checkpoint.runInitialScenario = m_runInitialScenario;
    checkpoint.runtimeUnits = checkpoint.scenario.units.empty()
        ? QJsonArray{} : m_engine.collectCheckpointState();
    for (const QString& key : m_commandResultOrder) {
    checkpoint.commandHistory.append(
            QJsonObject{{QStringLiteral("key"), key},
                        {QStringLiteral("result"), m_commandResults.value(key)}});
    }
    checkpoint.mapMarks = m_mapMarks;
    checkpoint.authoritativeRoom = m_authoritativeRoom.toJson();
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
    if ((checkpoint.phase == QLatin1String("running")) != checkpoint.running) {
        if (error) *error = QStringLiteral("检查点阶段与运行状态冲突");
        return false;
    }
    const bool emptyPreparingRuntime = checkpoint.phase == QLatin1String("preparing")
        && checkpoint.scenario.units.empty() && checkpoint.runtimeUnits.isEmpty();
    const QString scenarioError = emptyPreparingRuntime
        ? QString() : validateNetworkScenario(checkpoint.scenario);
    if (!scenarioError.isEmpty()) {
        if (error) *error = QStringLiteral("检查点场景无效: %1").arg(scenarioError);
        return false;
    }
    const bool scenarioRestored = emptyPreparingRuntime
        ? m_engine.setRemoteScenario(checkpoint.scenario)
        : m_engine.setScenario(checkpoint.scenario);
    if (!scenarioRestored) {
        if (error) *error = m_engine.lastError();
        return false;
    }
    if (emptyPreparingRuntime) {
        m_engine.setRunning(false);
        m_engine.setSpeedMul(checkpoint.speed);
    } else {
        QString runtimeError;
        if (!m_engine.restoreCheckpointState(checkpoint.runtimeUnits, checkpoint.simTime,
                                             checkpoint.running, checkpoint.speed,
                                             &runtimeError)) {
            if (error) *error = QStringLiteral("运行态恢复失败: %1").arg(runtimeError);
            return false;
        }
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
    m_mapMarks = checkpoint.mapMarks;
    if (!checkpoint.authoritativeRoom.isEmpty()
        && !m_authoritativeRoom.restore(checkpoint.authoritativeRoom, error)) return false;
    if (m_authoritativeRoom.phase() != checkpoint.phase) {
        if (error) *error = QStringLiteral("检查点权威房间阶段与运行阶段不一致");
        return false;
    }
    syncAuthoritativeSeats();
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
    m_authoritativeRoom.clearReadiness();
    m_redReady = false;
    m_blueReady = false;
    for (auto it = m_seats.begin(); it != m_seats.end(); ++it) it->ready = false;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) it->seatReady = false;
}

void GameServer::syncAuthoritativeSeats() {
    m_seats.clear();
    m_redReady = false;
    m_blueReady = false;
    for (const AuthoritativeRoom::Seat& seat : m_authoritativeRoom.seats()) {
        m_seats.insert(seat.seatId,
                       SeatOccupant{seat.seatId, seat.seatType, seat.side, seat.userId,
                                    seat.username, seat.ready});
        if (seat.seatType == QLatin1String("commander")) {
            if (seat.side == QLatin1String("red")) m_redReady = seat.ready;
            if (seat.side == QLatin1String("blue")) m_blueReady = seat.ready;
        }
    }
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (!it->authenticated || it->roomId != m_roomId) continue;
        QString seatId;
        AuthoritativeRoom::Seat authoritativeSeat;
        for (const AuthoritativeRoom::Seat& candidate : m_authoritativeRoom.seats()) {
            if (candidate.userId == it->userId) {
                seatId = candidate.seatId;
                authoritativeSeat = candidate;
                break;
            }
        }
        it->seatId = seatId;
        it->seatType = authoritativeSeat.seatType;
        it->side = authoritativeSeat.side;
        it->seatReady = authoritativeSeat.ready;
    }
}

bool GameServer::applyDeployedScenario(QString* error) {
    if (error) error->clear();
    const QJsonArray units = m_authoritativeRoom.runtimeUnits();
    if (units.isEmpty()) return clearDeploymentRuntime(error);
    QJsonObject scenarioJson = ScenarioIo::toJson(
        m_runInitialScenario.units.empty() ? ScenarioIo::defaultScenario() : m_runInitialScenario);
    scenarioJson[QStringLiteral("units")] = units;
    const Scenario scenario = ScenarioIo::fromJson(scenarioJson);
    const QString validationError = validateNetworkScenario(scenario);
    if (!validationError.isEmpty() || !m_engine.setScenario(scenario)) {
        if (error) *error = validationError.isEmpty() ? m_engine.lastError() : validationError;
        return false;
    }
    ++m_scenarioRevision;
    return true;
}

bool GameServer::applyDeploymentIfPreparing(QString* error) {
    if (error) error->clear();
    if (m_phase != QLatin1String("preparing")) return true;
    return applyDeployedScenario(error);
}

bool GameServer::applyDepartureToRuntime(const QStringList& removedUnitIds, QString* error) {
    if (error) error->clear();
    if (m_phase == QLatin1String("preparing")) return applyDeployedScenario(error);
    QSet<QString> uniqueIds(removedUnitIds.cbegin(), removedUnitIds.cend());
    bool changed = false;
    for (const QString& unitId : uniqueIds) {
        if (unitId.isEmpty() || !m_engine.unit(unitId)) continue;
        m_engine.removeUnit(unitId);
        changed = true;
    }
    if (changed) ++m_scenarioRevision;
    return true;
}

bool GameServer::clearDeploymentRuntime(QString* error) {
    if (error) error->clear();
    Scenario emptyScenario = m_runInitialScenario.units.empty()
        ? ScenarioIo::defaultScenario() : m_runInitialScenario;
    emptyScenario.units.clear();
    if (!m_engine.setRemoteScenario(emptyScenario)) {
        if (error) *error = m_engine.lastError();
        return false;
    }
    m_engine.setRunning(false);
    return true;
}

GameServer::RoomStateBackup GameServer::captureRoomState() const {
    RoomStateBackup backup;
    backup.authoritativeRoom = m_authoritativeRoom.toJson();
    backup.scenario = m_engine.scenario();
    backup.runtimeUnits = backup.scenario.units.empty()
        ? QJsonArray{} : m_engine.collectCheckpointState();
    backup.simTime = m_engine.simTime();
    backup.running = m_engine.running();
    backup.speed = m_engine.speedMul();
    backup.phase = m_phase;
    backup.roomStatus = m_roomStatus;
    backup.redReady = m_redReady;
    backup.blueReady = m_blueReady;
    backup.mapMarks = m_mapMarks;
    backup.sharedIntel = m_sharedIntel;
    backup.chatHistory = m_chatHistory;
    backup.chatSequence = m_chatSequence;
    backup.commandResults = m_commandResults;
    backup.commandResultOrder = m_commandResultOrder;
    backup.scenarioRevision = m_scenarioRevision;
    backup.stateRevision = m_stateRevision;
    backup.eventSequence = m_eventSequence;
    return backup;
}

bool GameServer::restoreRoomStateBackup(const RoomStateBackup& backup, QString* error) {
    if (error) error->clear();
    QString localError;
    if (!m_authoritativeRoom.restore(backup.authoritativeRoom, &localError)) {
        if (error) *error = localError;
        return false;
    }
    if (!m_engine.setRemoteScenario(backup.scenario)) {
        if (error) *error = m_engine.lastError();
        return false;
    }
    if (backup.scenario.units.empty()) {
        m_engine.setRunning(false);
        m_engine.setSpeedMul(backup.speed);
    } else if (!m_engine.restoreCheckpointState(
                   backup.runtimeUnits, backup.simTime, backup.running, backup.speed,
                   &localError)) {
        if (error) *error = localError;
        return false;
    }
    m_phase = backup.phase;
    m_roomStatus = backup.roomStatus;
    m_redReady = backup.redReady;
    m_blueReady = backup.blueReady;
    m_mapMarks = backup.mapMarks;
    m_sharedIntel = backup.sharedIntel;
    m_chatHistory = backup.chatHistory;
    m_chatSequence = backup.chatSequence;
    m_commandResults = backup.commandResults;
    m_commandResultOrder = backup.commandResultOrder;
    m_scenarioRevision = backup.scenarioRevision;
    m_stateRevision = backup.stateRevision;
    m_eventSequence = backup.eventSequence;
    syncAuthoritativeSeats();
    return true;
}

bool GameServer::resetAuthoritativeRuntime(const QString& operationId, QString* error) {
    if (error) error->clear();
    const RoomStateBackup backup = captureRoomState();
    m_engine.setRunning(false);
    const AuthoritativeRoom::Result reset = m_authoritativeRoom.applyOperation(
        operationId, QStringLiteral("reset"), m_authoritativeRoom.revision());
    QString localError;
    bool committed = reset.ok && clearDeploymentRuntime(&localError);
    if (committed) {
        m_phase = QStringLiteral("preparing");
        m_redReady = false;
        m_blueReady = false;
        m_mapMarks = {};
        m_mapMarkRateWindows.clear();
        m_sharedIntel.clear();
        m_chatHistory = {};
        m_chatSequence = 0;
        m_commandResults.clear();
        m_commandResultOrder.clear();
        ++m_scenarioRevision;
        syncAuthoritativeSeats();
        committed = persistRoomState(&localError);
    }
    if (committed) return true;

    QString rollbackError;
    if (!restoreRoomStateBackup(backup, &rollbackError) && localError.isEmpty()) {
        localError = QStringLiteral("回滚房间状态失败: %1").arg(rollbackError);
    }
    if (error) *error = localError.isEmpty() ? reset.code : localError;
    return false;
}

bool GameServer::resetRoomIfEmpty(QString* error) {
    if (error) error->clear();
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (it->authenticated && it->roomId == m_roomId) return true;
    }

    const QString operationId = QStringLiteral("internal-empty-%1")
        .arg(m_authoritativeRoom.revision());
    if (!resetAuthoritativeRuntime(operationId, error)) return false;
    m_roomStatus = QStringLiteral("preparing");
    reportRoomStatus(QStringLiteral("preparing"), QStringLiteral("房间已无在线成员，已重置"));
    return true;
}

void GameServer::processRoomOperation(const QJsonObject& operation) {
    if (operation.isEmpty()
        || operation.value(QStringLiteral("state")).toString() != QLatin1String("pending")) return;
    const QString operationId = operation.value(QStringLiteral("operationId")).toString();
    const QString action = operation.value(QStringLiteral("action")).toString();
    if (action != QLatin1String("reset") && action != QLatin1String("redeploy")) return;
    const qint64 requested = operation.value(QStringLiteral("requestedRevision")).toInteger();
    const quint64 currentRevision = m_authoritativeRoom.revision();
    if (requested > 0 && static_cast<quint64>(requested) > currentRevision) {
        acknowledgeRoomOperation(operationId, QStringLiteral("failed"), currentRevision,
                                 QStringLiteral("STALE_REVISION"));
        return;
    }
    const RoomStateBackup backup = captureRoomState();
    const AuthoritativeRoom::Result result = m_authoritativeRoom.applyOperation(
        operationId, action, currentRevision);
    if (!result.ok) {
        acknowledgeRoomOperation(operationId, QStringLiteral("failed"), result.revision,
                                 result.code);
        return;
    }
    QString runtimeError;
    if (!applyDeployedScenario(&runtimeError)) {
        restoreRoomStateBackup(backup);
        acknowledgeRoomOperation(operationId, QStringLiteral("failed"), result.revision,
                                 QStringLiteral("RUNTIME_RESET_FAILED"));
        return;
    }
    m_phase = QStringLiteral("preparing");
    m_mapMarks = {};
    m_mapMarkRateWindows.clear();
    m_sharedIntel.clear();
    syncAuthoritativeSeats();
    QString persistenceError;
    if (!persistRoomState(&persistenceError)) {
        restoreRoomStateBackup(backup);
        acknowledgeRoomOperation(operationId, QStringLiteral("failed"), result.revision,
                                 QStringLiteral("PERSISTENCE_FAILED"));
        return;
    }
    acknowledgeRoomOperation(operationId, QStringLiteral("acknowledged"), result.revision);
    broadcastSnapshots(true);
}

void GameServer::acknowledgeRoomOperation(const QString& operationId, const QString& state,
                                          quint64 revision, const QString& code) {
    if (operationId.isEmpty()) return;
    const QUrl url(m_authServiceUrl + QStringLiteral("/api/internal/rooms/%1/operations/%2/ack")
                       .arg(QUrl::toPercentEncoding(m_roomId),
                            QUrl::toPercentEncoding(operationId)));
    QNetworkRequest request(url);
    request.setRawHeader("X-Internal-Key", m_internalKey.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = m_network.post(
        request, QJsonDocument(QJsonObject{{QStringLiteral("state"), state},
                                            {QStringLiteral("revision"),
                                             static_cast<qint64>(revision)},
                                            {QStringLiteral("code"), code}})
                     .toJson(QJsonDocument::Compact));
    QTimer::singleShot(2500, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

QJsonObject GameServer::roomState() const {
    QJsonObject online;
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (!it->authenticated) continue;
        const QString key = it->seatId.isEmpty() ? QStringLiteral("lobby") : it->seatId;
        online[key] = online.value(key).toInt() + 1;
    }
    QJsonArray seats;
    for (auto it = m_seatLimits.cbegin(); it != m_seatLimits.cend(); ++it) {
        const SeatDescriptor descriptor = describeSeat(it.key());
        const int capacity = qBound(0, it.value(), 64);
        if (capacity == 0) continue;
        const int seatCount = descriptor.type == QLatin1String("commander") ? 1 : capacity;
        for (int index = 1; index <= seatCount; ++index) {
            const QString seatId = canonicalSeatId(descriptor.baseId, index);
            const SeatOccupant occupant = m_seats.value(seatId);
            const AuthoritativeRoom::Seat authoritative = m_authoritativeRoom.seat(seatId);
            seats.append(QJsonObject{{QStringLiteral("seatId"), seatId},
                                     {QStringLiteral("seatType"), descriptor.type},
                                     {QStringLiteral("side"), descriptor.side},
                                     {QStringLiteral("slot"), index},
                                     {QStringLiteral("capacity"), capacity},
                                     {QStringLiteral("occupied"), occupant.userId > 0},
                                     {QStringLiteral("displayName"), occupant.username},
                                     {QStringLiteral("connected"), authoritative.connected},
                                     {QStringLiteral("selectedTemplate"), authoritative.selectedTemplate},
                                     {QStringLiteral("unitId"), authoritative.unitId},
                                     {QStringLiteral("deployed"), authoritative.deployed},
                                     {QStringLiteral("ready"), occupant.ready},
                                     {QStringLiteral("pendingTransfer"), authoritative.pendingTransfer},
                                     {QStringLiteral("redeployRequested"), authoritative.redeployRequested},
                                     {QStringLiteral("unitName"), authoritative.unitName},
                                     {QStringLiteral("state"), occupant.userId > 0
                                          ? QStringLiteral("occupied") : QStringLiteral("vacant")},
                                     {QStringLiteral("revision"),
                                      static_cast<qint64>(authoritative.revision)}});
        }
    }
    const bool readyForStart = m_authoritativeRoom.readiness()
                                   .value(QStringLiteral("ready")).toBool()
        && m_engine.readyForSim();
    return QJsonObject{{QStringLiteral("phase"), m_phase},
                       {QStringLiteral("roomId"), m_roomId},
                       {QStringLiteral("roomName"), m_roomName},
                       {QStringLiteral("roomStatus"), m_roomStatus},
                       {QStringLiteral("redReady"), m_redReady},
                       {QStringLiteral("blueReady"), m_blueReady},
                       {QStringLiteral("readyForStart"), readyForStart},
                       {QStringLiteral("running"), m_engine.running()},
                       {QStringLiteral("readyForSim"), m_engine.readyForSim()},
                       {QStringLiteral("cpIssues"), m_engine.cpIssues()},
                       {QStringLiteral("simTime"), m_engine.simTime()},
                       {QStringLiteral("speed"), m_engine.speedMul()},
                       {QStringLiteral("scenarioRevision"), static_cast<qint64>(m_scenarioRevision)},
                       {QStringLiteral("stateRevision"), static_cast<qint64>(m_stateRevision)},
                       {QStringLiteral("lifecycleRevision"),
                        static_cast<qint64>(m_authoritativeRoom.revision())},
                       {QStringLiteral("readiness"), m_authoritativeRoom.readiness()},
                       {QStringLiteral("deploymentState"),
                        QJsonObject{{QStringLiteral("unitCount"),
                                     m_authoritativeRoom.runtimeUnits().size()},
                                    {QStringLiteral("empty"),
                                     m_authoritativeRoom.runtimeUnits().isEmpty()}}},
                       {QStringLiteral("online"), online},
                       {QStringLiteral("seats"), seats}};
}

QString GameServer::messageSummary(const QString& type, const QJsonObject& payload) const {
    QString summary;
    if (type == QLatin1String("chat")) {
        summary = QStringLiteral("chat message (%1 chars)")
                      .arg(payload.value(QStringLiteral("text")).toString().size());
    }
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
    const int pendingAuthentication = std::count_if(
        m_clients.cbegin(), m_clients.cend(), [](const ClientSession& session) {
            return session.authenticationPending;
        });
    const QString serviceStatus = m_authenticationHealth == QLatin1String("healthy")
        ? QStringLiteral("healthy") : QStringLiteral("degraded");
    const QJsonObject authenticationStatus{
        {QStringLiteral("status"), m_authenticationHealth},
        {QStringLiteral("pending"), pendingAuthentication},
        {QStringLiteral("attempts"), static_cast<qint64>(m_totalAuthenticationAttempts)},
        {QStringLiteral("retries"), static_cast<qint64>(m_totalAuthenticationRetries)},
        {QStringLiteral("transientFailures"),
         static_cast<qint64>(m_totalAuthenticationTransientFailures)},
        {QStringLiteral("credentialFailures"),
         static_cast<qint64>(m_totalAuthenticationCredentialFailures)},
        {QStringLiteral("finalFailures"), static_cast<qint64>(m_totalAuthenticationFinalFailures)},
        {QStringLiteral("lastFailureClass"), m_lastAuthenticationFailureClass}};
    const QJsonObject status{{QStringLiteral("status"), serviceStatus},
                             {QStringLiteral("version"), QStringLiteral(WARGAME_VERSION)},
                             {QStringLiteral("sourceDigest"), QStringLiteral(WARGAME_SOURCE_DIGEST)},
                             {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                             {QStringLiteral("connectedClients"), m_clients.size()},
                             {QStringLiteral("dataPlane"), m_fastDds.backendName()},
                             {QStringLiteral("metrics"), QJsonObject{
                                 {QStringLiteral("uptimeSeconds"), m_uptime.elapsed() / 1000},
                                 {QStringLiteral("totalConnections"), static_cast<qint64>(m_totalConnections)},
                                 {QStringLiteral("totalDisconnects"), static_cast<qint64>(m_totalDisconnects)},
                                 {QStringLiteral("resyncRequests"), static_cast<qint64>(m_totalResyncRequests)}}},
                             {QStringLiteral("authentication"), authenticationStatus},
                             {QStringLiteral("roomState"), roomState()}};
    file.write(QJsonDocument(status).toJson(QJsonDocument::Compact));
    file.commit();
}

QSet<QString> GameServer::visibleUnitIds(const ClientSession& session) const {
    return StateProjector::visibleUnitIds(m_engine, normalizedRole(session),
                                          m_sharedIntel.value(session.seatId),
                                          m_authoritativeRoom.seat(session.seatId).unitId);
}

QJsonArray GameServer::filteredMessages(const ClientSession& session) const {
    return StateProjector::filteredMessages(
        m_engine, normalizedRole(session), m_authoritativeRoom.seat(session.seatId).unitId);
}

QJsonArray GameServer::filteredChatHistory(const ClientSession& session) const {
    QJsonArray output;
    if (session.seatId.isEmpty() || session.roomId != m_roomId) return output;
    for (const QJsonValue& value : m_chatHistory) {
        const QJsonObject message = value.toObject();
        const QString side = message.value(QStringLiteral("side")).toString();
        if (!side.isEmpty() && !session.side.isEmpty() && side != session.side) continue;
        const QJsonArray recipients = message.value(QStringLiteral("recipientSeatIds")).toArray();
        if (!recipients.isEmpty() && !recipients.contains(session.seatId)
            && message.value(QStringLiteral("seatId")).toString() != session.seatId) continue;
        output.append(message);
    }
    return output;
}

QJsonArray GameServer::filteredMapMarks(const ClientSession& session) const {
    QJsonArray output;
    if (session.seatId.isEmpty() || session.roomId != m_roomId || session.side.isEmpty()) {
        return output;
    }
    for (const QJsonValue& value : m_mapMarks) {
        const QJsonObject mark = value.toObject();
        if (mark.value(QStringLiteral("side")).toString() != session.side) continue;
        const QJsonArray visibleTo = mark.value(QStringLiteral("visibleToSeatIds")).toArray();
        const QString markType = mark.value(QStringLiteral("markType")).toString();
        const QString authorSeatId = mark.value(QStringLiteral("seatId")).toString();
        const qint64 authorUserId = mark.value(QStringLiteral("authorUserId")).toInteger();
        const bool participantMark = markType == QLatin1String("self")
            || (markType.isEmpty() && !authorSeatId.endsWith(QLatin1String("_commander")));
        const bool commanderCanInspect = session.seatType == QLatin1String("commander")
            && participantMark;
        const bool participantOwnsMark = participantMark
            && ((authorUserId > 0 && authorUserId == session.userId)
                || (authorUserId <= 0 && authorSeatId == session.seatId));
        const bool explicitlyVisible = visibleTo.contains(session.seatId);
        const bool legacyAuthorOnly = visibleTo.isEmpty() && authorSeatId == session.seatId;
        const bool visible = participantMark
            ? (commanderCanInspect || participantOwnsMark
               || (authorUserId <= 0 && (explicitlyVisible || legacyAuthorOnly)))
            : (explicitlyVisible || legacyAuthorOnly);
        if (visible) {
            QJsonObject projected = mark;
            projected.remove(QStringLiteral("authorUserId"));
            projected.remove(QStringLiteral("visibleToSeatIds"));
            output.append(projected);
        }
    }
    return output;
}

void GameServer::appendMapMark(const QJsonObject& mark) {
    const QString markType = mark.value(QStringLiteral("markType")).toString();
    const QString seatId = mark.value(QStringLiteral("seatId")).toString();
    const qint64 authorUserId = mark.value(QStringLiteral("authorUserId")).toInteger();
    if (markType == QLatin1String("self") && (!seatId.isEmpty() || authorUserId > 0)) {
        QJsonArray retained;
        for (const QJsonValue& value : m_mapMarks) {
            const QJsonObject existing = value.toObject();
            const QString existingType = existing.value(QStringLiteral("markType")).toString();
            const QString existingSeatId = existing.value(QStringLiteral("seatId")).toString();
            const qint64 existingUserId = existing.value(QStringLiteral("authorUserId")).toInteger();
            const bool participantMark = existingType == QLatin1String("self")
                || (existingType.isEmpty()
                    && !existingSeatId.endsWith(QLatin1String("_commander")));
            const bool sameAuthor = authorUserId > 0 && existingUserId > 0
                ? authorUserId == existingUserId : existingSeatId == seatId;
            const bool sameParticipantMark = participantMark && sameAuthor;
            if (!sameParticipantMark) retained.append(existing);
        }
        m_mapMarks = retained;
    }
    m_mapMarks.append(mark);
    while (m_mapMarks.size() > 200) m_mapMarks.removeFirst();
}

void GameServer::removeParticipantMarksForUser(qint64 userId, const QString& legacySeatId) {
    if (userId <= 0 && legacySeatId.isEmpty()) return;
    QJsonArray retained;
    for (const QJsonValue& value : m_mapMarks) {
        const QJsonObject mark = value.toObject();
        const QString type = mark.value(QStringLiteral("markType")).toString();
        const QString seatId = mark.value(QStringLiteral("seatId")).toString();
        const qint64 authorUserId = mark.value(QStringLiteral("authorUserId")).toInteger();
        const bool participantMark = type == QLatin1String("self")
            || (type.isEmpty() && !seatId.endsWith(QLatin1String("_commander")));
        const bool owned = participantMark
            && ((authorUserId > 0 && authorUserId == userId)
                || (authorUserId <= 0 && !legacySeatId.isEmpty() && seatId == legacySeatId));
        if (!owned) retained.append(mark);
    }
    m_mapMarks = retained;
}

QJsonObject GameServer::snapshotFor(const ClientSession& session) const {
    QJsonObject snapshot = StateProjector::snapshotFor(
        m_engine, normalizedRole(session), m_stateRevision, roomState(),
        m_sharedIntel.value(session.seatId), m_authoritativeRoom.seat(session.seatId).unitId);
    if (m_authoritativeRoom.runtimeUnits().isEmpty()) {
        snapshot[QStringLiteral("units")] = QJsonArray{};
        QJsonObject scenario = snapshot.value(QStringLiteral("scenario")).toObject();
        scenario[QStringLiteral("units")] = QJsonArray{};
        snapshot[QStringLiteral("scenario")] = scenario;
    }
    snapshot[QStringLiteral("mapMarks")] = filteredMapMarks(session);
    return snapshot;
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
        || code == QLatin1String("MAP_MARK_RATE_LIMIT")
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

void GameServer::closeRoomSessions(const QString& message) {
    const QJsonObject event{{QStringLiteral("kind"), QStringLiteral("roomClosed")},
                            {QStringLiteral("message"), message}};
    QList<QPair<QWebSocket*, QJsonObject>> notifications;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (!it->authenticated || it->roomId != m_roomId) continue;
        const QJsonObject projected = StateProjector::projectEvent(
            m_engine, normalizedRole(it.value()), event,
            m_authoritativeRoom.seat(it->seatId).unitId);
        if (!projected.isEmpty()) notifications.append({it.key(), projected});
        it->roomId.clear();
        it->seatId.clear();
        it->seatType.clear();
        it->side.clear();
        it->seatReady = false;
        sendSeatDirectory(it.key());
    }
    for (const auto& [socket, projected] : notifications) {
        if (m_clients.contains(socket)) sendEnvelope(socket, QStringLiteral("event"), projected);
    }
}

void GameServer::broadcastSnapshots(bool forceFull) {
    QHash<QWebSocket*, QJsonObject> currentSnapshots;
    bool projectionChanged = false;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (!it->authenticated) continue;
        const QJsonObject current = snapshotFor(it.value());
        currentSnapshots.insert(it.key(), current);
        QJsonObject comparableCurrent = current;
        QJsonObject comparablePrevious = it->lastSnapshot;
        comparableCurrent.remove(QStringLiteral("stateRevision"));
        comparablePrevious.remove(QStringLiteral("stateRevision"));
        QJsonObject currentRoomState = comparableCurrent.value(QStringLiteral("roomState")).toObject();
        QJsonObject previousRoomState = comparablePrevious.value(QStringLiteral("roomState")).toObject();
        currentRoomState.remove(QStringLiteral("stateRevision"));
        previousRoomState.remove(QStringLiteral("stateRevision"));
        comparableCurrent[QStringLiteral("roomState")] = currentRoomState;
        comparablePrevious[QStringLiteral("roomState")] = previousRoomState;
        if (it->lastSnapshot.isEmpty() || comparableCurrent != comparablePrevious) {
            projectionChanged = true;
        }
    }
    if (!projectionChanged && !forceFull) return;
    if (projectionChanged) {
        ++m_stateRevision;
        currentSnapshots.clear();
        for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
            if (it->authenticated) currentSnapshots.insert(it.key(), snapshotFor(it.value()));
        }
    }
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (!it->authenticated) continue;
        const QJsonObject current = currentSnapshots.value(it.key());
        if (!forceFull && !it->lastSnapshot.isEmpty()
            && StateDelta::canCreate(it->lastSnapshot, current)) {
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
        if (!side.isEmpty() && !it->side.isEmpty() && it->side != side) continue;
        const QJsonObject projected = StateProjector::projectEvent(
            m_engine, normalizedRole(it.value()), event,
            m_authoritativeRoom.seat(it.value().seatId).unitId);
        if (!projected.isEmpty()) sendEnvelope(it.key(), QStringLiteral("event"), projected);
    }
}

void GameServer::broadcastChat(const QJsonObject& message) {
    const QString senderSide = message.value(QStringLiteral("side")).toString();
    const QString senderSeat = message.value(QStringLiteral("seatId")).toString();
    QSet<QString> requested;
    for (const QJsonValue& value : message.value(QStringLiteral("recipientSeatIds")).toArray()) {
        if (value.isString()) requested.insert(value.toString());
    }
    const bool preparing = m_phase == QLatin1String("preparing");
    UnitBase* senderUnit = preparing ? nullptr : seatUnit(senderSeat);
    if (!preparing && !senderUnit) return;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (!it->authenticated || (it->roomId != m_roomId && !it->roomId.isEmpty())) continue;
        if (!senderSide.isEmpty() && it->side != senderSide) continue;
        if (!requested.isEmpty() && !requested.contains(it->seatId)
            && it->seatId != senderSeat) continue;
        if (!preparing) {
            UnitBase* recipientUnit = seatUnit(it->seatId);
            if (!recipientUnit) continue;
            if (!StateProjector::canTransmit(m_engine, senderUnit->id(), recipientUnit->id())) continue;
        }
        sendEnvelope(it.key(), QStringLiteral("chat"), message);
    }
}

} // namespace gbr
