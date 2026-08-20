#include "GameServer.h"
#include "StateProjector.h"

#include "core/SnapshotCodec.h"
#include "core/UnitBase.h"
#include "units/AttackUAV.h"
#include "protocol/Protocol.h"
#include "protocol/StateDelta.h"

#include <QCoreApplication>
#include <QByteArray>
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
#include <limits>

namespace gbr {

namespace {

constexpr qsizetype kMaxScenarioUnits = 512;
constexpr qsizetype kMaxSchedulePoints = 512;
constexpr int kMaxMessagesPerSecond = 60;
constexpr int kMaxMapMarksPerSecond = 8;
constexpr qint64 kMaxPendingBytes = 1024 * 1024;
constexpr qint64 kDdsTicketLifetimeMs = 120000;
constexpr int kAiProviderPlanGraceMs = 1000;

QJsonObject payloadForWireVersion(const QString& type, const QJsonObject& payload,
                                  int schemaVersion) {
    if (schemaVersion != Protocol::LegacySchemaVersion) return payload;
    QJsonObject compatible = payload;
    if (type == QLatin1String("snapshot") || type == QLatin1String("delta")) {
        compatible[QStringLiteral("schemaVersion")] = Protocol::LegacySchemaVersion;
        compatible.remove(QStringLiteral("intelState"));
        compatible.remove(QStringLiteral("intelDelta"));
    } else if (type == QLatin1String("intelShare")) {
        // v4 identifies shared contacts by targetId. Newer projections keep
        // intelId as an internal ledger identifier and may also carry it.
        compatible.remove(QStringLiteral("intelId"));
    } else if (type == QLatin1String("intelHistoryPage")) {
        compatible = {};
    }
    return compatible;
}

double planarDistance2(double ax, double ay, double bx, double by) {
    const double dx = ax - bx;
    const double dy = ay - by;
    return dx * dx + dy * dy;
}

QString env(const char* name, const QString& fallback) {
    const QString value = qEnvironmentVariable(name).trimmed();
    return value.isEmpty() ? fallback : value;
}

QString aiConversationStatus(const OllamaResult& result) {
    if (result.ok) return QStringLiteral("completed");
    if (result.failureClass == QLatin1String("cancelled")) {
        return QStringLiteral("cancelled");
    }
    if (result.failureClass == QLatin1String("schema_invalid")
        || result.failureClass == QLatin1String("stale_response")
        || result.failureClass.startsWith(QLatin1String("semantic_"))) {
        return QStringLiteral("rejected");
    }
    return QStringLiteral("failed");
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

bool observerRoomIsOpen(const QJsonObject& room) {
    if (room.isEmpty() || !room.value(QStringLiteral("enabled")).toBool(true)
        || !room.value(QStringLiteral("hostedByGameServer")).toBool(true)) {
        return false;
    }
    static const QSet<QString> openStatuses{
        QStringLiteral("preparing"), QStringLiteral("running"),
        QStringLiteral("paused"), QStringLiteral("finished")};
    return openStatuses.contains(
        room.value(QStringLiteral("status")).toString().trimmed().toLower());
}

bool observerMessageIsReadOnly(const QString& type) {
    static const QSet<QString> allowed{
        QStringLiteral("roomList"), QStringLiteral("leaveRoom"),
        QStringLiteral("heartbeat"), QStringLiteral("resyncRequest"),
        QStringLiteral("ping"), QStringLiteral("setObserverTrajectories"),
        QStringLiteral("setObserverTrails")};
    return allowed.contains(type);
}

bool isRedCommandPost(const UnitBase* unit) {
    return unit && unit->sideStr() == QLatin1String("red")
        && unit->kind() == UnitKind::CommandPost;
}

QString commandCacheKey(const QString& controllerId, const QString& commandId) {
    return QStringLiteral("%1:%2").arg(controllerId, commandId);
}

QString commandCacheKey(qint64 userId, const QString& commandId) {
    return commandCacheKey(QString::number(userId), commandId);
}

QString intelRequestCacheKey(qint64 userId, const QString& action,
                             const QString& requestId) {
    return commandCacheKey(userId, QStringLiteral("intel:%1:%2").arg(action, requestId));
}

bool isIntelRequestType(const QString& type) {
    return type == QLatin1String("shareIntel")
        || type == QLatin1String("createIntelReport")
        || type == QLatin1String("requestIntelHistory");
}

bool buildVmfMessage(const QJsonObject& payload, const QString& messageId,
                     Message* output, QString* error) {
    if (!output) {
        if (error) *error = QStringLiteral("VMF 消息输出参数为空");
        return false;
    }
    Message message;
    message.id = messageId.isEmpty()
        ? payload.value(QStringLiteral("messageId")).toString() : messageId;
    if (message.id.isEmpty()) {
        if (error) *error = QStringLiteral("VMF 消息 ID 缺失");
        return false;
    }
    if (!Message::parseTypeName(payload.value(QStringLiteral("messageType")).toString(),
                                &message.type)
        || message.type == Message::Type::Ack) {
        if (error) *error = QStringLiteral("VMF 消息类型无效");
        return false;
    }
    message.sender = payload.value(QStringLiteral("senderUnitId")).toString();
    message.receiver = payload.value(QStringLiteral("receiverUnitId")).toString();
    message.traceId = payload.value(QStringLiteral("traceId")).toString();
    message.correlationId = payload.value(QStringLiteral("correlationId")).toString();
    message.vmfMessage = payload.value(QStringLiteral("vmfMessage")).toString();
    message.wireFormat = Message::WireFormat::VmfDesignV1;
    const QByteArray encodedWire = payload.value(QStringLiteral("wireBytes")).toString().toLatin1();
    message.wireBytes = QByteArray::fromBase64(encodedWire);
    const QJsonValue bitLengthValue = payload.value(QStringLiteral("wireBitLength"));
    const double bitLengthNumber = bitLengthValue.toDouble(-1.0);
    if (!bitLengthValue.isDouble() || !std::isfinite(bitLengthNumber)
        || bitLengthNumber <= 0.0 || std::floor(bitLengthNumber) != bitLengthNumber
        || bitLengthNumber > static_cast<double>(std::numeric_limits<int>::max())) {
        if (error) *error = QStringLiteral("VMF 位长度无效");
        return false;
    }
    message.wireBitLength = static_cast<int>(bitLengthNumber);
    message.requiresAck = payload.value(QStringLiteral("requiresAck")).toBool(false);
    message.automaticAck = true;
    message.retryCount = payload.value(QStringLiteral("retryCount")).toInt(0);
    message.payload = payload.value(QStringLiteral("payload")).toObject();
    message.timestamp = QDateTime::currentDateTimeUtc();
    message.vmfEncoded = true;
    if (message.wireBytes.isEmpty() || message.wireBytes.toBase64() != encodedWire
        || message.wireBitLength <= 0
        || message.wireBitLength > message.wireBytes.size() * 8) {
        if (error) *error = QStringLiteral("VMF wire 数据无效");
        return false;
    }
    const int remainder = message.wireBitLength % 8;
    if (remainder != 0) {
        const unsigned char unusedMask = static_cast<unsigned char>((1U << (8 - remainder)) - 1U);
        if ((static_cast<unsigned char>(message.wireBytes.at(message.wireBytes.size() - 1))
             & unusedMask) != 0U) {
            if (error) *error = QStringLiteral("VMF padding 位必须为 0");
            return false;
        }
    }
    *output = std::move(message);
    return true;
}

QJsonObject addServerVmfDedupe(const QJsonObject& stateObject,
                               const QSet<QString>& ids) {
    if (stateObject.isEmpty()) return stateObject;
    vmf::RuntimeState state;
    QString error;
    if (!vmf::RuntimeState::fromJson(stateObject, &state, &error)) return stateObject;
    QStringList ordered = ids.values();
    ordered.sort();
    for (const QString& id : ordered) state.rememberMessageId(id);
    return state.toJson();
}

void restoreServerVmfDedupe(const QJsonObject& stateObject, QSet<QString>* ids,
                            QStringList* order) {
    if (!ids || !order || stateObject.isEmpty()) return;
    vmf::RuntimeState state;
    QString error;
    if (!vmf::RuntimeState::fromJson(stateObject, &state, &error)) return;
    ids->clear();
    order->clear();
    for (const QJsonValue& value : state.seenMessageIds) {
        const QString id = value.toString();
        if (id.isEmpty()) continue;
        ids->insert(id);
        order->append(id);
    }
}

QString roomModeFromConfig(const QJsonObject& room) {
    const QString mode = room.value(QStringLiteral("mode")).toString().trimmed().toLower();
    return mode == QLatin1String("pve") ? QStringLiteral("pve") : QStringLiteral("pvp");
}

QString aiDifficultyFromConfig(const QJsonObject& room) {
    const QString difficulty = room.value(QStringLiteral("aiDifficulty")).toString()
                                   .trimmed().toLower();
    if (difficulty == QLatin1String("easy") || difficulty == QLatin1String("hard")) {
        return difficulty;
    }
    return QStringLiteral("normal");
}

quint64 configVersionFromConfig(const QJsonObject& room) {
    const qint64 version = room.value(QStringLiteral("configVersion")).toInteger();
    return version > 0 ? static_cast<quint64>(version) : 1;
}

IntelLedger::Config intelConfigFromRoom(const QJsonObject& room,
                                        const IntelLedger::Config& fallback) {
    const double stale = room.value(QStringLiteral("intelStaleAfterSec"))
                             .toDouble(fallback.staleAfterSec);
    const double archive = room.value(QStringLiteral("intelArchiveAfterSec"))
                               .toDouble(fallback.archiveAfterSec);
    if (!std::isfinite(stale) || !std::isfinite(archive) || stale <= 0.0
        || archive <= stale) {
        return fallback;
    }
    return IntelLedger::Config{stale, archive};
}

bool aiConfigurationFromJson(const QJsonObject& value, const OllamaConfig& current,
                             OllamaConfig* next, QString* error) {
    OllamaConfig candidate = current;
    candidate.deployment = value.value(QStringLiteral("provider")).toString().trimmed().toLower();
    const QString configuredBaseUrl = value.value(QStringLiteral("baseUrl")).toString().trimmed();
    const QString configuredModel = value.value(QStringLiteral("model")).toString().trimmed();
    // Older account-service responses did not include the global Ollama
    // endpoint. Preserve the provider's current endpoint/model in that case;
    // an explicitly supplied value is still validated below.
    if (!configuredBaseUrl.isEmpty()) candidate.baseUrl = configuredBaseUrl;
    if (!configuredModel.isEmpty()) candidate.model = configuredModel;
    if (candidate.deployment != QLatin1String("rules")
        && candidate.deployment != QLatin1String("auto")
        && candidate.deployment != QLatin1String("ollama")) {
        if (error) *error = QStringLiteral("provider 无效");
        return false;
    }
    QString normalizedBaseUrl;
    if (!OllamaProvider::validateBaseUrl(candidate.baseUrl, &normalizedBaseUrl, error)) {
        return false;
    }
    if (candidate.model.isEmpty() || candidate.model.size() > 128) {
        if (error) *error = QStringLiteral("model 无效");
        return false;
    }
    candidate.baseUrl = normalizedBaseUrl;
    if (next) *next = candidate;
    return true;
}

QString roomAiProviderFromConfig(const QJsonObject& room) {
    const QString provider = room.value(QStringLiteral("aiProvider")).toString()
                                 .trimmed().toLower();
    return provider == QLatin1String("ollama")
        ? QStringLiteral("ollama") : QStringLiteral("rules");
}

QString roomAiModelFromConfig(const QJsonObject& room) {
    const QString model = room.value(QStringLiteral("aiModel")).toString().trimmed();
    return model.isEmpty() ? QStringLiteral("auto") : model.left(128);
}

quint64 roomAiConfigVersionFromConfig(const QJsonObject& room) {
    const qint64 value = room.value(QStringLiteral("configVersion")).toInteger();
    return value > 0 ? static_cast<quint64>(value) : 1;
}

QJsonObject roomAiConfiguration(const QJsonObject& room, const QJsonObject& global) {
    QJsonObject result = global;
    const QString provider = roomAiProviderFromConfig(room);
    result[QStringLiteral("provider")] = provider;
    result[QStringLiteral("model")] = roomAiModelFromConfig(room);
    result[QStringLiteral("selectedProvider")] = provider;
    result[QStringLiteral("selectedModel")] = room.value(QStringLiteral("aiModel")).toString();
    result[QStringLiteral("resolvedModel")] =
        room.value(QStringLiteral("aiResolvedModel")).toString();
    result[QStringLiteral("roomConfigVersion")] =
        static_cast<qint64>(roomAiConfigVersionFromConfig(room));
    result[QStringLiteral("ollamaConfigVersion")] =
        global.value(QStringLiteral("configVersion")).toInteger(1);
    return result;
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
        const double maximumSpeed = UnitBase::commandedSpeedLimitMps(kindFromName(unit.kind));
        if (unit.speed > maximumSpeed) {
            return QStringLiteral("单元速度超过 %1 的类型上限: %2")
                .arg(maximumSpeed, 0, 'f', 0).arg(unit.id);
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
      m_aiConversationStore(QDir(m_dataDir).filePath(
          QStringLiteral("ai-planning-history"))),
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
    QString aiConfigurationError;
    const OllamaConfig aiConfiguration = OllamaProvider::fromEnvironment(
        &aiConfigurationError);
    m_aiProviderMode = aiConfiguration.deployment;
    m_aiConnectionStatus = m_aiProviderMode == QLatin1String("rules")
        ? QStringLiteral("disabled") : QStringLiteral("checking");
    m_ollamaProvider = new OllamaProvider(aiConfiguration, &m_network, this);
    if (!m_aiConversationStore.configurationError().isEmpty()) {
        qWarning() << "AI conversation store unavailable:"
                   << m_aiConversationStore.configurationError();
    }
    if (!aiConfigurationError.isEmpty()) {
        qWarning() << "AI provider configuration rejected:" << aiConfigurationError;
        audit(QStringLiteral("ai"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("providerConfigurationRejected")},
                          {QStringLiteral("provider"), m_aiProviderMode}});
    }

    // Register before any scenario is loaded so a simulation-generated VMF
    // observation is durably recorded before the newly-created workflow can
    // be checkpointed. replayDurableEvents() suppresses this callback while
    // applying already-recorded events during recovery.
    if (m_engine.bus()) {
        connect(m_engine.bus(), &MessageBus::messagePosted, this,
                [this](const QJsonObject& posted) { handleGeneratedVmfMessage(posted); });
    }

    const auto bootstrapRoomState = [this]() {
        // A failed restore may have partially applied a checkpoint before an
        // event or runtime validation error. Reset every room-owned value
        // before loading the persisted scenario or the built-in baseline.
        m_engine.setRunning(false);
        m_engine.setSpeedMul(1.0);
        m_phase = QStringLiteral("preparing");
        m_roomMode = m_authoritativeRoom.mode();
        m_aiDifficulty = QStringLiteral("normal");
        m_roomDescription.clear();
        m_scenarioId = QStringLiteral("default");
        m_roomStatus = QStringLiteral("stopped");
        m_configVersion = 1;
        m_lastRoomUpdate.clear();
        m_redReady = false;
        m_blueReady = false;
        m_runInitialScenario = {};
        m_mapMarks = {};
        m_intelLedger.clear();
        m_observerTrajectories.clear();
        m_nextObserverTrajectorySampleAt = 0.0;
        m_commandResults.clear();
        m_commandResultOrder.clear();
        m_vmfMessageIds.clear();
        m_vmfMessageIdOrder.clear();
        m_scenarioRevision = 1;
        m_stateRevision = 1;
        m_eventSequence = 0;
        m_matchGeneration = 1;
        m_aiCommandSequence = 0;
        m_aiPlanningGeneration = 0;
        m_aiRngState = 0xA17A11ULL;
        m_aiNextDecisionAt = 0.0;
        m_aiNextReplanAt = 0.0;
        m_aiPlan = {};
        m_aiStickyRules = false;
        m_aiConsecutiveFailures = 0;
        m_aiSelectedProvider = QStringLiteral("rules");
        m_aiSelectedModel.clear();
        m_aiResolvedModel.clear();
        m_aiRoomConfigVersion = 1;
        m_aiOllamaConfigVersion = 1;
        m_aiFallbackReason.clear();
        clearRoomOperationTracking();

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
                cancelAiPlanRequest();
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
            [this]() {
                if (m_phase != QLatin1String("running") || !m_engine.running()) return;
                sampleObserverTrajectories();
                broadcastSnapshots();
            });
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
    m_aiDecisionTimer.setInterval(250);
    connect(&m_aiDecisionTimer, &QTimer::timeout, this, &GameServer::runAiDecision);
    m_aiProbeTimer.setInterval(10000);
    connect(&m_aiProbeTimer, &QTimer::timeout, this, &GameServer::probeAiProvider);
}

GameServer::~GameServer() {
    m_checkpointTimer.stop();
    m_aiDecisionTimer.stop();
    m_aiProbeTimer.stop();
    cancelAiPlanRequest();
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
    // The WebSocket server transfers ownership of accepted sockets to the
    // caller.  During normal operation removeClient() schedules deletion,
    // but there may be no event loop left while GameServer is being torn
    // down.  Delete the remaining sockets synchronously after disconnecting
    // this object's callbacks so ASan and shutdown both see a complete
    // ownership release.
    const QList<QWebSocket*> clients = m_ownedSockets.values();
    m_clients.clear();
    m_ownedSockets.clear();
    for (QWebSocket* socket : clients) {
        if (!socket) continue;
        socket->disconnect(this);
        socket->close(QWebSocketProtocol::CloseCodeGoingAway,
                      QStringLiteral("服务器正在关闭"));
        delete socket;
    }
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
    m_aiDecisionTimer.start();
    m_aiProbeTimer.start();
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
            m_observerJoinAllowed = false;
        } else {
            m_observerJoinAllowed = observerRoomIsOpen(selected);
            m_intelLedger.setConfig(intelConfigFromRoom(selected, m_intelLedger.config()));
            const QJsonObject globalAi =
                document.object().value(QStringLiteral("aiConfig")).toObject();
            if (!payload.value(QStringLiteral("asObserver")).toBool()) {
                applyAiConfiguration(roomAiConfiguration(selected, globalAi));
            }
            if (payload.value(QStringLiteral("asObserver")).toBool()) {
                m_roomName = selected.value(QStringLiteral("name")).toString(m_roomName);
                m_roomDescription = selected.value(QStringLiteral("description")).toString(
                    m_roomDescription);
                m_scenarioId = selected.value(QStringLiteral("scenarioId")).toString(m_scenarioId);
                m_roomStatus = selected.value(QStringLiteral("status"))
                                   .toString(QStringLiteral("stopped"));
                m_roomMode = roomModeFromConfig(selected);
                m_aiDifficulty = aiDifficultyFromConfig(selected);
                m_configVersion = configVersionFromConfig(selected);
                m_lastRoomUpdate = selected.value(QStringLiteral("updatedAt")).toString();
                completeJoinRoom(guardedSocket, payload, expectedUserId);
                return;
            }
            const QString configuredMode = roomModeFromConfig(selected);
            const QString configuredDifficulty = aiDifficultyFromConfig(selected);
            const quint64 configuredVersion = configVersionFromConfig(selected);
            if (m_phase == QLatin1String("preparing")) {
                if (configuredMode != m_authoritativeRoom.mode()) {
                    const QJsonObject before = m_authoritativeRoom.toJson();
                    const AuthoritativeRoom::Result modeResult =
                        m_authoritativeRoom.setMode(configuredMode);
                    if (!modeResult.ok) {
                        m_authoritativeRoom.restore(before);
                        audit(QStringLiteral("security"),
                              QJsonObject{{QStringLiteral("event"),
                                           QStringLiteral("roomModeChangeRejected")},
                                          {QStringLiteral("mode"), configuredMode},
                                          {QStringLiteral("reason"), modeResult.code}});
                    } else {
                        m_roomMode = configuredMode;
                        m_configVersion = configuredVersion;
                        syncAuthoritativeSeats();
                    }
                } else {
                    m_roomMode = configuredMode;
                    m_configVersion = configuredVersion;
                }
                m_aiDifficulty = configuredDifficulty;
            } else if (configuredMode != m_roomMode
                       || configuredDifficulty != m_aiDifficulty) {
                audit(QStringLiteral("security"),
                      QJsonObject{{QStringLiteral("event"),
                                   QStringLiteral("activeRoomConfigurationChangeRejected")},
                                  {QStringLiteral("mode"), configuredMode},
                                  {QStringLiteral("aiDifficulty"), configuredDifficulty},
                                  {QStringLiteral("configVersion"),
                                   static_cast<qint64>(configuredVersion)},
                                  {QStringLiteral("phase"), m_phase}});
            }
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
            const bool seatParametersChanged = m_phase == QLatin1String("preparing")
                && nextParameters != m_seatParameters;
            if (m_phase == QLatin1String("preparing")) m_seatParameters = nextParameters;
            reconcileSeatConfiguration(seatParametersChanged);
            m_roomName = selected.value(QStringLiteral("name")).toString(m_roomName);
            m_roomDescription = selected.value(QStringLiteral("description")).toString(
                m_roomDescription);
            m_scenarioId = selected.value(QStringLiteral("scenarioId")).toString(m_scenarioId);
            m_roomStatus = selected.value(QStringLiteral("status")).toString(QStringLiteral("stopped"));
            m_lastRoomUpdate = selected.value(QStringLiteral("updatedAt")).toString();
            processRoomOperation(selected.value(QStringLiteral("pendingOperation")).toObject());
        }
        completeJoinRoom(guardedSocket, payload, expectedUserId);
    });
}

void GameServer::reconcileSeatConfiguration(bool resetReadinessForParameterChanges) {
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
    if (!revokedUsers.isEmpty()
        || (parameterChanged && resetReadinessForParameterChanges)) {
        resetReadiness();
    }
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
        m_ownedSockets.insert(socket);
        connect(socket, &QObject::destroyed, this, [this, socket]() {
            m_ownedSockets.remove(socket);
        });
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
        // The server owns the socket through QTcpServer. During server
        // teardown Qt may destroy a child socket before emitting its final
        // disconnected signal, so never retain a dangling raw pointer in the
        // callback.
        QPointer<QWebSocket> guardedSocket(socket);
        connect(socket, &QWebSocket::disconnected, this,
                [this, guardedSocket]() {
                    if (guardedSocket) removeClient(guardedSocket.data());
                });
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
    const int incomingProtocol = envelope.value(QStringLiteral("protocolVersion")).toInt();
    const int incomingSchema = envelope.value(QStringLiteral("schemaVersion")).toInt();
    if (!session.authenticated
        && Protocol::isSupportedWireVersion(incomingProtocol, incomingSchema)) {
        // The auth envelope is the version negotiation. Remember the
        // compatible pair before sending any rejection so even that error is
        // encoded in the client's wire format.
        session.protocolVersion = incomingProtocol;
        session.schemaVersion = incomingSchema;
    }
    const Protocol::ValidationResult validation = Protocol::validateClientEnvelopeForVersion(
        envelope);
    if (!validation.valid) {
        sendError(socket, validation.code, validation.message);
        if (validation.code == QLatin1String("PROTOCOL_MISMATCH")
            || validation.code == QLatin1String("SCHEMA_MISMATCH")) {
            socket->close(QWebSocketProtocol::CloseCodeProtocolError, validation.message);
        }
        return;
    }
    const QString type = envelope.value(QStringLiteral("type")).toString();
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    const QString messageId = envelope.value(QStringLiteral("messageId")).toString();
    if (session.authenticated
        && (incomingProtocol != session.protocolVersion
            || incomingSchema != session.schemaVersion)) {
        sendError(socket, QStringLiteral("PROTOCOL_MISMATCH"),
                  QStringLiteral("当前连接已协商其他协议版本"), messageId);
        socket->close(QWebSocketProtocol::CloseCodeProtocolError,
                      QStringLiteral("协议版本发生变化"));
        return;
    }
    if (!session.authenticated) {
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
        if (type != QLatin1String("auth")) {
            sendError(socket, QStringLiteral("AUTH_REQUIRED"), QStringLiteral("请先完成登录认证"));
            return;
        }
        authenticate(socket, payload.value(QStringLiteral("token")).toString());
        return;
    }

    if (isIntelRequestType(type)) {
        const QString cacheKey = intelRequestCacheKey(session.userId, type, messageId);
        if (m_commandResults.contains(cacheKey)) {
            sendEnvelope(socket, QStringLiteral("commandResult"),
                         m_commandResults.value(cacheKey));
            return;
        }
    }
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

    if (session.observer && !observerMessageIsReadOnly(type)) {
        sendError(socket, QStringLiteral("OBSERVER_READ_ONLY"),
                  QStringLiteral("观察员连接为只读，不能执行此操作"), messageId);
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
    else if (type == QLatin1String("shareIntel")) handleShareIntel(socket, payload, messageId);
    else if (type == QLatin1String("createIntelReport")) handleCreateIntelReport(socket, payload, messageId);
    else if (type == QLatin1String("requestIntelHistory")) handleRequestIntelHistory(socket, payload, messageId);
    else if (type == QLatin1String("mapMark")) handleMapMark(socket, payload);
    else if (type == QLatin1String("setObserverTrajectories")
             || type == QLatin1String("setObserverTrails")) {
        handleSetObserverTrajectories(socket, payload);
    }
    else if (type == QLatin1String("vmfMessage")) handleVmfMessage(socket, payload, messageId);
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
        const QJsonObject globalAiConfig =
            document.object().value(QStringLiteral("aiConfig")).toObject();
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
            m_observerJoinAllowed = false;
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
        const QJsonObject selectedAiConfig = roomAiConfiguration(selected, globalAiConfig);
        applyAiConfiguration(selectedAiConfig);
        m_intelLedger.setConfig(intelConfigFromRoom(selected, m_intelLedger.config()));
        m_observerJoinAllowed = observerRoomIsOpen(selected);
        bool roomHasParticipant = false;
        for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
            if (it->authenticated && !it->observer && it->roomId == m_roomId) {
                roomHasParticipant = true;
                break;
            }
        }
        const bool deferAuthoritativeMode = !roomHasParticipant;
        const QString status = selected.value(QStringLiteral("status")).toString(QStringLiteral("stopped"));
        const QString configuredMode = roomModeFromConfig(selected);
        const QString configuredDifficulty = aiDifficultyFromConfig(selected);
        const quint64 configuredVersion = configVersionFromConfig(selected);
        if (m_phase == QLatin1String("preparing")) {
            if (configuredMode != m_authoritativeRoom.mode() && !deferAuthoritativeMode) {
                const QJsonObject before = m_authoritativeRoom.toJson();
                const AuthoritativeRoom::Result modeResult =
                    m_authoritativeRoom.setMode(configuredMode);
                if (!modeResult.ok) {
                    m_authoritativeRoom.restore(before);
                    audit(QStringLiteral("security"),
                          QJsonObject{{QStringLiteral("event"),
                                       QStringLiteral("roomModeChangeRejected")},
                                      {QStringLiteral("mode"), configuredMode},
                                      {QStringLiteral("reason"), modeResult.code}});
                } else {
                    m_roomMode = configuredMode;
                    m_configVersion = configuredVersion;
                    syncAuthoritativeSeats();
                }
            } else {
                m_roomMode = configuredMode;
                m_configVersion = configuredVersion;
            }
            m_aiDifficulty = configuredDifficulty;
        } else if (configuredMode != m_roomMode
                   || configuredDifficulty != m_aiDifficulty) {
            audit(QStringLiteral("security"),
                  QJsonObject{{QStringLiteral("event"),
                               QStringLiteral("activeRoomConfigurationChangeRejected")},
                              {QStringLiteral("mode"), configuredMode},
                              {QStringLiteral("aiDifficulty"), configuredDifficulty},
                              {QStringLiteral("configVersion"),
                               static_cast<qint64>(configuredVersion)},
                              {QStringLiteral("phase"), m_phase}});
        }
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
        const bool seatParametersChanged = m_phase == QLatin1String("preparing")
            && nextParameters != m_seatParameters;
        if (m_phase == QLatin1String("preparing")) {
            m_seatParameters = nextParameters;
        } else if (nextParameters != m_seatParameters) {
            audit(QStringLiteral("security"),
                  QJsonObject{{QStringLiteral("event"), QStringLiteral("activeSeatParameterChangeRejected")},
                              {QStringLiteral("phase"), m_phase}});
        }
        reconcileSeatConfiguration(seatParametersChanged);
        const QString updated = selected.value(QStringLiteral("updatedAt")).toString();
        const bool changed = !updated.isEmpty() && updated != m_lastRoomUpdate;
        const bool stoppedNow = status == QLatin1String("stopped")
            && m_roomStatus != QLatin1String("stopped");
        m_lastRoomUpdate = updated;
        m_roomName = selected.value(QStringLiteral("name")).toString(m_roomName);
        m_roomDescription = selected.value(QStringLiteral("description")).toString(
            m_roomDescription);
        m_scenarioId = selected.value(QStringLiteral("scenarioId")).toString(m_scenarioId);
        const QJsonObject latestOperation = selected.value(QStringLiteral("operation")).toObject();
        const QJsonObject pendingOperation = selected.value(QStringLiteral("pendingOperation")).toObject();
        const QString pendingAction = pendingOperation.value(QStringLiteral("action")).toString();
        const QString latestAction = latestOperation.value(QStringLiteral("action")).toString();
        const bool deploymentOperation = latestAction == QLatin1String("reset")
            || latestAction == QLatin1String("redeploy");
        const bool operationPending = pendingOperation.value(QStringLiteral("state")).toString()
            == QLatin1String("pending");
        // A pending account operation describes a requested target status, not
        // an authoritative state transition. Keep the locally committed status
        // until the game server has applied and acknowledged the operation.
        if (!operationPending) m_roomStatus = status;
        processRoomOperation(pendingOperation);
        if (operationPending) {
            // The account service deliberately keeps the previous status
            // while an operation is pending.  Do not infer a transition from
            // that stale status; processRoomOperation owns the commit.
            if (roomDirectoryChanged) broadcastRoomDirectory();
            broadcastSnapshots(true);
            flushRoomListWaiters();
            return;
        }
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
            if (m_roomMode == QLatin1String("pve")) {
                bool aiDeploymentRequired = false;
                for (const AuthoritativeRoom::Seat& seat : m_authoritativeRoom.seats()) {
                    if (seat.controllerType == QLatin1String("ai")
                        && (!seat.deployed || !seat.ready)) {
                        aiDeploymentRequired = true;
                        break;
                    }
                }
                if (aiDeploymentRequired) {
                    const RoomStateBackup backup = captureRoomState();
                    const QJsonObject map = m_engine.mapInfo();
                    const AuthoritativeRoom::Result deployed = m_authoritativeRoom.deployAiSeats(
                        map.value(QStringLiteral("widthMeters")).toDouble(),
                        map.value(QStringLiteral("heightMeters")).toDouble(),
                        m_matchGeneration);
                    QString deploymentError;
                    bool committed = deployed.ok && applyDeployedScenario(&deploymentError);
                    if (committed) {
                        syncAuthoritativeSeats();
                        committed = persistRoomState(&deploymentError);
                    }
                    if (!committed) {
                        restoreRoomStateBackup(backup);
                        m_roomStatus = QStringLiteral("preparing");
                        reportRoomStatus(QStringLiteral("preparing"),
                                         deployed.ok ? QStringLiteral("AI 部署失败")
                                                      : deployed.code);
                        audit(QStringLiteral("ai"),
                              QJsonObject{{QStringLiteral("event"),
                                           QStringLiteral("deploymentFailed")},
                                          {QStringLiteral("code"), deployed.ok
                                               ? QStringLiteral("RUNTIME_RESET_FAILED")
                                               : deployed.code}});
                        flushRoomListWaiters();
                        return;
                    }
                }
            }
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
            cancelAiPlanRequest();
            persistRoomState();
            broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("matchEndedByAdmin")},
                                       {QStringLiteral("message"), QStringLiteral("房间管理员已结束推演")} });
        } else if (status == QLatin1String("paused") && m_phase == QLatin1String("running")) {
            m_engine.setRunning(false);
            m_authoritativeRoom.pause();
            m_phase = QStringLiteral("paused");
            cancelAiPlanRequest();
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
        if (!it->authenticated || it->observer || it->roomId != m_roomId) continue;
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
            bool anotherParticipant = false;
            for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
                if (it.key() != target && it->authenticated && !it->observer
                    && it->roomId == m_roomId) {
                    anotherParticipant = true;
                    break;
                }
            }

            AuthoritativeRoom::Result departure;
            QString persistenceError;
            if (!session.observer && anotherParticipant) {
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
                    cancelAiPlanRequest();
                }
                if (committed) {
                    syncAuthoritativeSeats();
                    committed = persistRoomState(&persistenceError);
                }
                if (!committed) {
                    restoreRoomStateBackup(backup);
                    result = QStringLiteral("failed");
                }
            } else if (!session.observer) {
                session.roomId.clear();
                session.seatId.clear();
                session.seatType.clear();
                session.side.clear();
                session.seatReady = false;
                session.observer = false;
                session.role = session.accountRole;
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
                session.observer = false;
                session.role = session.accountRole;
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
    session.username = identity.value(QStringLiteral("username")).toString().trimmed();
    session.displayName = identity.value(QStringLiteral("displayName")).toString().trimmed();
    if (session.displayName.isEmpty()) session.displayName = session.username;
    session.accountRole = identity.value(QStringLiteral("role")).toString().trimmed().toLower();
    if (session.accountRole == QLatin1String("editor")) session.accountRole = QStringLiteral("room_admin");
    if (session.accountRole != QLatin1String("room_admin")) session.accountRole = QStringLiteral("player");
    session.role = session.accountRole;
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
                        {QStringLiteral("role"), session.accountRole},
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
    const bool owned = m_ownedSockets.contains(socket);
    const ClientSession session = m_clients.take(socket);
    bool lastParticipant = session.roomId == m_roomId && !session.observer;
    if (lastParticipant) {
        for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
            if (it->authenticated && !it->observer && it->roomId == m_roomId) {
                lastParticipant = false;
                break;
            }
        }
    }
    if (lastParticipant) {
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
            cancelAiPlanRequest();
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
    if (owned) socket->deleteLater();
    if (session.authenticated) {
        broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("presence")},
                                   {QStringLiteral("message"), QStringLiteral("%1 已离开推演室").arg(session.displayName)}});
    }
}

QString GameServer::normalizedRole(const ClientSession& session) const {
    if (session.observer) return QStringLiteral("observer");
    return session.seatId.isEmpty() ? session.accountRole : session.seatId;
}

bool GameServer::canManageRoom(const ClientSession& session) const {
    return session.authenticated && !session.observer
        && session.accountRole == QLatin1String("room_admin")
        && session.roomId == m_roomId && session.seatId.isEmpty()
        && m_phase == QLatin1String("preparing");
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

bool GameServer::initialPositionForSeat(const QString& seatId, GeoPos* position,
                                        QString* error) const {
    if (error) error->clear();
    if (!position) {
        if (error) *error = QStringLiteral("初始位置输出参数为空");
        return false;
    }
    const SeatDescriptor descriptor = describeSeat(seatId);
    const QString kind = descriptor.type == QLatin1String("commander")
        ? QStringLiteral("commandpost")
        : descriptor.type == QLatin1String("attack") ? QStringLiteral("attackuav")
        : descriptor.type == QLatin1String("recon") ? QStringLiteral("reconuav")
        : descriptor.type == QLatin1String("ground") ? QStringLiteral("groundscout")
                                                       : QStringLiteral("jammeruav");
    QList<ScenarioUnit> candidates;
    const Scenario& baseline = m_runInitialScenario.units.empty()
        ? m_engine.scenario() : m_runInitialScenario;
    for (const ScenarioUnit& unit : baseline.units) {
        if (unit.side == descriptor.side && unit.kind == kind) candidates.append(unit);
    }
    std::sort(candidates.begin(), candidates.end(), [](const ScenarioUnit& left,
                                                       const ScenarioUnit& right) {
        return left.id < right.id;
    });
    const int requestedIndex = descriptor.type == QLatin1String("commander")
        ? 0 : descriptor.index - 1;
    if (requestedIndex < 0 || requestedIndex >= candidates.size()) {
        if (error) *error = QStringLiteral("战位 %1 缺少对应的初始单位，请房间管理员补齐 %2")
                                  .arg(seatId, kind);
        return false;
    }
    *position = candidates.at(requestedIndex).pos;
    return true;
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
                     {QStringLiteral("description"), m_roomDescription},
                     {QStringLiteral("status"), m_roomStatus},
                     {QStringLiteral("mode"), m_roomMode},
                     {QStringLiteral("aiDifficulty"), m_aiDifficulty},
                     {QStringLiteral("configVersion"), static_cast<qint64>(m_configVersion)},
                     {QStringLiteral("intelStaleAfterSec"), m_intelLedger.config().staleAfterSec},
                     {QStringLiteral("intelArchiveAfterSec"), m_intelLedger.config().archiveAfterSec},
                     {QStringLiteral("hostedByGameServer"), true},
                     {QStringLiteral("scenarioId"), m_scenarioId}};
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
    if (!m_clients.contains(socket) || m_clients.value(socket).observer
        || m_clients.value(socket).roomId.isEmpty()) {
        return;
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
            const bool ai = authoritative.controllerType == QLatin1String("ai");
            const bool occupied = ai || occupant.userId > 0;
            seats.append(QJsonObject{{QStringLiteral("seatId"), seatId},
                                     {QStringLiteral("seatType"), descriptor.type},
                                     {QStringLiteral("side"), descriptor.side},
                                     {QStringLiteral("slot"), index},
                                     {QStringLiteral("capacity"), capacity},
                                     {QStringLiteral("occupied"), occupied},
                                     {QStringLiteral("displayName"), ai ? QStringLiteral("AI")
                                                                         : occupant.username},
                                     {QStringLiteral("connected"), ai || authoritative.connected},
                                     {QStringLiteral("commander"), descriptor.type == QLatin1String("commander")},
                                     {QStringLiteral("controllerType"),
                                      ai ? QStringLiteral("ai") : QStringLiteral("human")},
                                     {QStringLiteral("selectedTemplate"), authoritative.selectedTemplate},
                                     {QStringLiteral("unitId"), authoritative.unitId},
                                     {QStringLiteral("deployed"), authoritative.deployed},
                                     {QStringLiteral("ready"), occupant.ready},
                                     {QStringLiteral("pendingTransfer"), authoritative.pendingTransfer},
                                     {QStringLiteral("redeployRequested"), authoritative.redeployRequested},
                                     {QStringLiteral("unitName"), authoritative.unitName},
                                     {QStringLiteral("state"), occupied
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
    if (payload.value(QStringLiteral("asObserver")).toBool()
        || m_roomStatus == QLatin1String("stopped")) {
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
    const bool observerJoin = payload.value(QStringLiteral("asObserver")).toBool();
    if (observerJoin && !m_observerJoinAllowed) {
        sendError(socket, QStringLiteral("ROOM_CLOSED"), QStringLiteral("房间当前未开启"));
        return;
    }
    if (!observerJoin && m_roomStatus == QLatin1String("stopped")) {
        sendError(socket, QStringLiteral("ROOM_CLOSED"), QStringLiteral("房间当前未开启"));
        return;
    }
    if (!observerJoin && m_roomStatus == QLatin1String("finished")) {
        sendError(socket, QStringLiteral("ROOM_FINISHED"), QStringLiteral("本局推演已结束，请等待管理员开启下一局"));
        return;
    }
    if (!observerJoin && sessionIt->accountRole == QLatin1String("room_admin")
        && (m_roomStatus != QLatin1String("preparing")
            || m_phase != QLatin1String("preparing"))) {
        sendError(socket, QStringLiteral("ROOM_ADMIN_PREPARING_ONLY"),
                  QStringLiteral("房间管理员只能进入准备阶段的房间"));
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
    session.observer = observerJoin;
    session.role = observerJoin ? QStringLiteral("observer") : session.accountRole;
    if (observerJoin) {
        session.observerTrajectorySelection = m_observerSelectionCache.value(session.userId);
        for (auto it = session.observerTrajectorySelection.begin();
             it != session.observerTrajectorySelection.end();) {
            if (!m_engine.unit(*it)) it = session.observerTrajectorySelection.erase(it);
            else ++it;
        }
        m_observerSelectionCache.insert(session.userId,
                                        session.observerTrajectorySelection);
        sendFullSnapshot(socket);
        return;
    }
    sendSeatDirectory(socket);
    sendFullSnapshot(socket);
    broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("roomPresence")},
                               {QStringLiteral("message"), QStringLiteral("%1 已进入房间大厅").arg(session.displayName)}});
}

void GameServer::handleLeaveRoom(QWebSocket* socket, const QJsonObject& payload) {
    ClientSession& session = m_clients[socket];
    if (session.observer) {
        m_observerSelectionCache.remove(session.userId);
        session.observerTrajectorySelection.clear();
        session.roomId.clear();
        session.seatId.clear();
        session.seatType.clear();
        session.side.clear();
        session.seatReady = false;
        session.observer = false;
        session.role = session.accountRole;
        handleRoomList(socket);
        return;
    }
    if (session.seatId.isEmpty()) {
        const ClientSession originalSession = session;
        session.roomId.clear();
        session.seatType.clear();
        session.side.clear();
        session.seatReady = false;
        session.observer = false;
        session.role = session.accountRole;
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
    bool lastParticipant = session.roomId == m_roomId;
    if (lastParticipant) {
        for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
            if (it.key() != socket && it->authenticated && !it->observer
                && it->roomId == m_roomId) {
                lastParticipant = false;
                break;
            }
        }
    }
    if (lastParticipant) {
        const ClientSession originalSession = session;
        session.roomId.clear();
        session.seatId.clear();
        session.seatType.clear();
        session.side.clear();
        session.seatReady = false;
        session.observer = false;
        session.role = session.accountRole;
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
    session.observer = false;
    session.role = session.accountRole;
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
    if (session.accountRole == QLatin1String("room_admin")) {
        sendError(socket, QStringLiteral("ROOM_ADMIN_NO_SEAT"),
                  QStringLiteral("房间管理员不占用战位，请在房间管理界面编辑配置"));
        return;
    }
    if (m_phase == QLatin1String("preparing")
        && m_authoritativeRoom.mode() != m_roomMode) {
        const AuthoritativeRoom::Result modeResult = m_authoritativeRoom.setMode(m_roomMode);
        if (!modeResult.ok) {
            sendError(socket, modeResult.code, QStringLiteral("房间模式当前无法切换"));
            return;
        }
        syncAuthoritativeSeats();
        if (!persistRoomState()) {
            sendError(socket, QStringLiteral("PERSISTENCE_FAILED"),
                      QStringLiteral("房间模式切换未能持久化"));
            return;
        }
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
        GeoPos initialPosition;
        QString initialPositionError;
        if (!initialPositionForSeat(transfer.value(QStringLiteral("targetSeatId")).toString(),
                                    &initialPosition, &initialPositionError)
            || !m_authoritativeRoom.deployInitial(
                   transfer.value(QStringLiteral("targetSeatId")).toString(), initialPosition).ok) {
            restoreRoomStateBackup(backup);
            sendTransferOutcome(QStringLiteral("transferRejected"), transfer, approval.revision,
                                QStringLiteral("INITIAL_UNIT_MISSING"));
            sendError(socket, QStringLiteral("INITIAL_UNIT_MISSING"), initialPositionError);
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
    GeoPos initialPosition;
    QString initialPositionError;
    if (!initialPositionForSeat(seatId, &initialPosition, &initialPositionError)) {
        sendError(socket, QStringLiteral("INITIAL_UNIT_MISSING"), initialPositionError);
        return;
    }
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
    const AuthoritativeRoom::Result initialDeployment =
        m_authoritativeRoom.deployInitial(seatId, initialPosition);
    if (!initialDeployment.ok) {
        m_authoritativeRoom.restore(before);
        sendError(socket, initialDeployment.code, QStringLiteral("初始部署位置无效"));
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
    SeatOccupant occupant;
    occupant.seatId = seatId;
    occupant.seatType = seatType;
    occupant.side = side;
    occupant.userId = session.userId;
    occupant.username = session.username;
    occupant.controllerType = QStringLiteral("human");
    occupant.controllerId = QString::number(session.userId);
    occupant.ready = false;
    m_seats.insert(seatId, occupant);
    syncAuthoritativeSeats();
    QString deploymentError;
    if (!applyDeployedScenario(&deploymentError)) {
        m_authoritativeRoom.restore(before);
        syncAuthoritativeSeats();
        sendError(socket, QStringLiteral("INITIAL_DEPLOYMENT_FAILED"), deploymentError);
        return;
    }
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

    if (ready && m_roomMode == QLatin1String("pve")
        && session.side == QLatin1String("red")
        && session.seatType == QLatin1String("commander")) {
        bool aiDeploymentRequired = false;
        for (const AuthoritativeRoom::Seat& seat : m_authoritativeRoom.seats()) {
            if (seat.controllerType == QLatin1String("ai")
                && (!seat.deployed || !seat.ready)) {
                aiDeploymentRequired = true;
                break;
            }
        }
        if (aiDeploymentRequired) {
            const RoomStateBackup backup = captureRoomState();
            const QJsonObject map = m_engine.mapInfo();
            const AuthoritativeRoom::Result deployed = m_authoritativeRoom.deployAiSeats(
                map.value(QStringLiteral("widthMeters")).toDouble(),
                map.value(QStringLiteral("heightMeters")).toDouble(), m_matchGeneration);
            QString deploymentError;
            bool committed = deployed.ok && applyDeployedScenario(&deploymentError);
            if (committed) {
                syncAuthoritativeSeats();
                committed = persistRoomState(&deploymentError);
            }
            if (!committed) {
                restoreRoomStateBackup(backup);
                sendError(socket, QStringLiteral("AI_DEPLOYMENT_FAILED"),
                          deployed.ok ? deploymentError : deployed.code);
                return;
            }
        }
    }

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
    const double alt = point.value(QStringLiteral("alt")).toDouble(0.0);
    const QJsonObject map = m_engine.mapInfo();
    UnitBase* replacing = m_engine.unit(targetSeat.unitId);
    UnitKind targetKind = replacing ? replacing->kind() : UnitKind::GroundScout;
    if (!replacing) {
        const QString kind = targetSeat.seatType == QLatin1String("commander")
            ? QStringLiteral("commandpost")
            : targetSeat.seatType == QLatin1String("attack") ? QStringLiteral("attackuav")
            : targetSeat.seatType == QLatin1String("recon") ? QStringLiteral("reconuav")
            : targetSeat.seatType == QLatin1String("jammer") ? QStringLiteral("jammeruav")
            : QStringLiteral("groundscout");
        targetKind = kindFromName(kind);
    }
    const double candidateRadius = replacing ? replacing->collisionRadius()
        : UnitBase::defaultCollisionRadiusM(targetKind);
    const double candidateHalfHeight = replacing ? replacing->collisionHalfHeight()
        : UnitBase::defaultCollisionHalfHeightM(targetKind);
    const double width = map.value(QStringLiteral("widthMeters")).toDouble();
    const double height = map.value(QStringLiteral("heightMeters")).toDouble();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(alt)
        || x < candidateRadius || y < candidateRadius
        || x > width - candidateRadius || y > height - candidateRadius) {
        sendError(socket, QStringLiteral("INVALID_DEPLOYMENT"), QStringLiteral("部署位置超出地图范围"));
        return;
    }
    for (const QString& existingId : m_engine.unitIds()) {
        if (existingId == targetSeat.unitId) continue;
        UnitBase* existing = m_engine.unit(existingId);
        if (!existing || !existing->alive()) continue;
        const double dx = x - existing->pos().x;
        const double dy = y - existing->pos().y;
        const double dz = std::abs(alt - existing->pos().alt);
        if (std::hypot(dx, dy) < candidateRadius + existing->collisionRadius() - 1e-9
            && dz < candidateHalfHeight + existing->collisionHalfHeight() - 1e-9) {
            sendError(socket, QStringLiteral("DEPLOYMENT_COLLISION"),
                      QStringLiteral("部署位置与已有单位碰撞体积重叠"));
            return;
        }
    }
    const QJsonObject before = m_authoritativeRoom.toJson();
    const AuthoritativeRoom::Result deployed = m_authoritativeRoom.deploy(
        session.userId, targetSeatId,
        GeoPos{x, y, alt});
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

void GameServer::handleShareIntel(QWebSocket* socket, const QJsonObject& payload,
                                  const QString& requestId) {
    const ClientSession& sender = m_clients.value(socket);
    const auto reject = [this, socket, requestId](const QString& code,
                                                   const QString& message) {
        sendIntelCommandResult(socket, QStringLiteral("shareIntel"), requestId,
                               CommandResult::reject(code, message));
    };
    QJsonObject normalizedPayload = payload;
    if (sender.schemaVersion == Protocol::LegacySchemaVersion
        && payload.contains(QStringLiteral("targetId"))) {
        if (payload.value(QStringLiteral("note")).toString().size()
            > Protocol::MaxIntelNoteLength) {
            reject(QStringLiteral("INTEL_NOTE_TOO_LONG"),
                   QStringLiteral("当前服务器兼容模式下情报备注最多 %1 字")
                       .arg(Protocol::MaxIntelNoteLength));
            return;
        }
        const QString targetId = payload.value(QStringLiteral("targetId")).toString();
        normalizedPayload.remove(QStringLiteral("targetId"));
        normalizedPayload[QStringLiteral("intelId")] =
            QStringLiteral("sensor_%1_%2").arg(sender.seatId, targetId);
    }
    Protocol::IntelShareRequest request;
    const Protocol::ValidationResult requestValidation = Protocol::fromJson(
        normalizedPayload, &request);
    if (!requestValidation.valid) {
        reject(requestValidation.code, requestValidation.message);
        return;
    }
    if (m_roomStatus == QLatin1String("stopped") || sender.seatId.isEmpty()
        || sender.roomId != m_roomId || sender.side.isEmpty()) {
        reject(QStringLiteral("SEAT_REQUIRED"), QStringLiteral("请先选择战位"));
        return;
    }
    const QString intelId = request.intelId;
    const QStringList requestedRecipients = request.recipientSeatIds;
    const QString note = request.note;
    const Protocol::IntelState source = m_intelLedger.state(sender.seatId);
    const Protocol::IntelContact* contact = nullptr;
    for (const auto& candidate : source.records) {
        if (candidate.intelId == intelId) { contact = &candidate; break; }
    }
    if (!contact) {
        reject(QStringLiteral("INTEL_NOT_FOUND"), QStringLiteral("当前战位没有该情报"));
        return;
    }
    if (contact->freshness == QLatin1String("archived")) {
        reject(QStringLiteral("INTEL_ARCHIVED"), QStringLiteral("归档情报不能直接共享"));
        return;
    }
    UnitBase* senderUnit = seatUnit(sender.seatId);
    if (!senderUnit) {
        reject(QStringLiteral("SEAT_UNIT_NOT_FOUND"), QStringLiteral("当前战位没有对应的仿真单位"));
        return;
    }
    QStringList acceptedRecipients;
    for (const QString& recipientSeat : requestedRecipients) {
        const AuthoritativeRoom::Seat recipient = m_authoritativeRoom.seat(recipientSeat);
        if (recipient.seatId.isEmpty() || recipient.side != sender.side
            || recipient.unitId.isEmpty() || !recipient.connected) continue;
        UnitBase* recipientUnit = seatUnit(recipientSeat);
        if (!recipientUnit || !StateProjector::canTransmit(m_engine, senderUnit->id(), recipientUnit->id())) continue;
        acceptedRecipients.append(recipientSeat);
    }
    acceptedRecipients.removeDuplicates();
    if (acceptedRecipients.isEmpty()) {
        reject(QStringLiteral("COMMUNICATION_LOST"), QStringLiteral("没有当前可达的同阵营战位"));
        return;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QJsonArray targetSeats = [&]() { QJsonArray a; for (const auto& id : acceptedRecipients) a.append(id); return a; }();
    QString persistenceError;
    if (!recordDurableEvent(QStringLiteral("intelShare"),
                            QJsonObject{{QStringLiteral("senderSeatId"), sender.seatId},
                                        {QStringLiteral("senderUserId"), sender.userId},
                                        {QStringLiteral("requestId"), requestId},
                                        {QStringLiteral("recipientSeatIds"), targetSeats},
                                        {QStringLiteral("intelId"), intelId},
                                        {QStringLiteral("note"), note},
                                        {QStringLiteral("receivedAt"), now.toString(Qt::ISODateWithMs)}},
                            &persistenceError)) {
        reject(QStringLiteral("PERSISTENCE_FAILED"), persistenceError);
        return;
    }
    QList<QPair<QWebSocket*, QJsonObject>> notifications;
    for (const QString& recipientSeat : acceptedRecipients) {
        const AuthoritativeRoom::Seat recipient = m_authoritativeRoom.seat(recipientSeat);
        const IntelLedger::Result result = m_intelLedger.share(
            sender.seatId, recipientSeat, sender.side, recipient.side, true, intelId, note, now);
        if (!result.ok) continue;
        if (contact->type == QLatin1String("sensorContact")
            && contact->freshness == QLatin1String("live") && contact->actionable
            && !contact->targetId.isEmpty()) {
            m_sharedIntel[recipientSeat].insert(contact->targetId);
        }
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            if (!it->authenticated || it->seatId != recipientSeat) continue;
            QJsonObject notification{
                {QStringLiteral("senderSeatId"), sender.seatId},
                {QStringLiteral("intelId"), intelId},
                {QStringLiteral("sharedAt"), now.toString(Qt::ISODateWithMs)},
                {QStringLiteral("note"), note}};
            if (!contact->targetId.isEmpty()) {
                notification[QStringLiteral("targetId")] = contact->targetId;
            }
            notifications.append({it.key(), notification});
        }
    }
    const CommandResult accepted = CommandResult::ok(QStringLiteral("情报共享已确认"));
    cacheIntelCommandResult(sender.userId, QStringLiteral("shareIntel"), requestId, accepted);
    if (!persistRoomState(&persistenceError)) {
        audit(QStringLiteral("persistence"),
              QJsonObject{{QStringLiteral("event"),
                           QStringLiteral("intelCheckpointDeferred")},
                          {QStringLiteral("action"), QStringLiteral("shareIntel")},
                          {QStringLiteral("requestId"), requestId},
                          {QStringLiteral("message"), persistenceError}});
    }
    sendIntelCommandResult(socket, QStringLiteral("shareIntel"), requestId, accepted);
    for (const auto& notification : notifications) {
        if (m_clients.contains(notification.first)) {
            sendEnvelope(notification.first, QStringLiteral("intelShare"), notification.second);
        }
    }
    broadcastSnapshots();
}

void GameServer::handleCreateIntelReport(QWebSocket* socket, const QJsonObject& payload,
                                         const QString& requestId) {
    ClientSession& sender = m_clients[socket];
    const auto reject = [this, socket, requestId](const QString& code,
                                                   const QString& message) {
        sendIntelCommandResult(socket, QStringLiteral("createIntelReport"), requestId,
                               CommandResult::reject(code, message));
    };
    const Protocol::ValidationResult requestValidation =
        Protocol::validateClientPayload(QStringLiteral("createIntelReport"), payload);
    if (!requestValidation.valid) {
        reject(requestValidation.code, requestValidation.message);
        return;
    }
    if (m_roomStatus == QLatin1String("stopped") || sender.seatId.isEmpty()
        || sender.roomId != m_roomId || sender.side.isEmpty()) {
        reject(QStringLiteral("SEAT_REQUIRED"), QStringLiteral("请先选择战位"));
        return;
    }
    const QJsonObject position = payload.value(QStringLiteral("position")).toObject();
    const double width = m_engine.mapInfo().value(QStringLiteral("widthMeters")).toDouble();
    const double height = m_engine.mapInfo().value(QStringLiteral("heightMeters")).toDouble();
    const double x = position.value(QStringLiteral("x")).toDouble(qQNaN());
    const double y = position.value(QStringLiteral("y")).toDouble(qQNaN());
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || y < 0.0 || x > width || y > height) {
        reject(QStringLiteral("MAP_BOUNDS"), QStringLiteral("情报位置超出权威地图范围"));
        return;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString category = payload.value(QStringLiteral("type")).toString();
    const QString title = payload.value(QStringLiteral("title")).toString();
    const QString note = payload.value(QStringLiteral("note")).toString();
    QString persistenceError;
    if (!recordDurableEvent(QStringLiteral("intelReport"),
                            QJsonObject{{QStringLiteral("seatId"), sender.seatId},
                                        {QStringLiteral("userId"), sender.userId},
                                        {QStringLiteral("requestId"), requestId},
                                        {QStringLiteral("type"), category},
                                        {QStringLiteral("title"), title},
                                        {QStringLiteral("position"), position},
                                        {QStringLiteral("note"), note},
                                        {QStringLiteral("receivedAt"), now.toString(Qt::ISODateWithMs)}},
                            &persistenceError)) {
        reject(QStringLiteral("PERSISTENCE_FAILED"), persistenceError);
        return;
    }
    const IntelLedger::Result result = m_intelLedger.createManualReport(
        sender.seatId, category, title, position, note, now);
    if (!result.ok) {
        reject(result.code, QStringLiteral("人工情报报告被拒绝"));
        return;
    }
    const CommandResult accepted = CommandResult::ok(QStringLiteral("人工情报报告已确认"));
    cacheIntelCommandResult(sender.userId, QStringLiteral("createIntelReport"), requestId,
                            accepted);
    if (!persistRoomState(&persistenceError)) {
        audit(QStringLiteral("persistence"),
              QJsonObject{{QStringLiteral("event"),
                           QStringLiteral("intelCheckpointDeferred")},
                          {QStringLiteral("action"),
                           QStringLiteral("createIntelReport")},
                          {QStringLiteral("requestId"), requestId},
                          {QStringLiteral("message"), persistenceError}});
    }
    sendIntelCommandResult(socket, QStringLiteral("createIntelReport"), requestId, accepted);
    broadcastSnapshots();
}

void GameServer::handleRequestIntelHistory(QWebSocket* socket, const QJsonObject& payload,
                                           const QString& requestId) {
    const ClientSession& session = m_clients.value(socket);
    const auto reject = [this, socket, requestId](const QString& code,
                                                   const QString& message) {
        sendIntelCommandResult(socket, QStringLiteral("requestIntelHistory"), requestId,
                               CommandResult::reject(code, message));
    };
    if (session.seatId.isEmpty() || session.observer) {
        reject(QStringLiteral("PERMISSION_DENIED"), QStringLiteral("观察员不能查询战位私有情报台账"));
        return;
    }
    Protocol::IntelHistoryQuery query;
    const Protocol::ValidationResult validation = Protocol::fromJson(payload, &query);
    if (!validation.valid) {
        reject(validation.code, validation.message);
        return;
    }
    const Protocol::IntelHistoryPage page = m_intelLedger.historyPage(session.seatId, query);
    sendEnvelope(socket, QStringLiteral("intelHistoryPage"), page.toJson());
    sendIntelCommandResult(socket, QStringLiteral("requestIntelHistory"), requestId,
                           CommandResult::ok(QStringLiteral("情报历史查询已确认")));
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

void GameServer::handleSetObserverTrajectories(QWebSocket* socket,
                                               const QJsonObject& payload) {
    if (!m_clients.contains(socket)) return;
    ClientSession& session = m_clients[socket];
    if (!session.observer || session.roomId != m_roomId) {
        sendError(socket, QStringLiteral("PERMISSION_DENIED"),
                  QStringLiteral("只有旁观者可以选择单位轨迹"));
        return;
    }
    const QJsonArray requested = payload.value(QStringLiteral("unitIds")).toArray();
    if (requested.size() > Protocol::MaxObserverTrajectoryUnits) {
        sendError(socket, QStringLiteral("INVALID_TRAJECTORY_SELECTION"),
                  QStringLiteral("最多选择 8 个单位轨迹"));
        return;
    }
    QSet<QString> selected;
    for (const QJsonValue& value : requested) {
        const QString unitId = value.toString();
        if (selected.contains(unitId) || !m_engine.unit(unitId)) {
            sendError(socket, QStringLiteral("INVALID_TRAJECTORY_SELECTION"),
                      QStringLiteral("轨迹选择包含不存在或重复的单位"));
            return;
        }
        selected.insert(unitId);
    }
    session.observerTrajectorySelection = selected;
    if (session.userId > 0) m_observerSelectionCache.insert(session.userId, selected);
    const double now = m_engine.simTime();
    for (const QString& unitId : selected) {
        if (!m_observerTrajectories.contains(unitId)) {
            const UnitBase* unit = m_engine.unit(unitId);
            if (!unit) continue;
            m_observerTrajectories.insert(unitId, QJsonArray{QJsonObject{
                {QStringLiteral("time"), now},
                {QStringLiteral("x"), unit->pos().x},
                {QStringLiteral("y"), unit->pos().y},
                {QStringLiteral("alt"), unit->pos().alt}}});
        }
    }
    broadcastSnapshots(true);
}

void GameServer::sampleObserverTrajectories() {
    bool hasObserver = false;
    QSet<QString> selected;
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (!it->authenticated || !it->observer || it->roomId != m_roomId) continue;
        hasObserver = true;
        selected.unite(it->observerTrajectorySelection);
    }
    if (!hasObserver || selected.isEmpty()) return;

    const double now = m_engine.simTime();
    if (!std::isfinite(now)) return;
    if (m_nextObserverTrajectorySampleAt > 1.0
        && now + 1e-9 < m_nextObserverTrajectorySampleAt - 1.0) {
        m_observerTrajectories.clear();
        m_nextObserverTrajectorySampleAt = 0.0;
    }
    if (now + 1e-9 < m_nextObserverTrajectorySampleAt) return;
    m_nextObserverTrajectorySampleAt = now + 1.0;
    for (const QString& unitId : selected) {
        const UnitBase* unit = m_engine.unit(unitId);
        if (!unit) continue;
        QJsonArray points = m_observerTrajectories.value(unitId);
        if (!points.isEmpty()
            && points.last().toObject().value(QStringLiteral("time")).toDouble(-1.0)
                >= now - 1e-9) continue;
        points.append(QJsonObject{{QStringLiteral("time"), now},
                                  {QStringLiteral("x"), unit->pos().x},
                                  {QStringLiteral("y"), unit->pos().y},
                                  {QStringLiteral("alt"), unit->pos().alt}});
        while (points.size() > Protocol::MaxObserverTrajectoryPoints) points.removeFirst();
        m_observerTrajectories.insert(unitId, points);
    }
}

QJsonObject GameServer::observerTrajectoriesFor(const ClientSession& session) const {
    if (!session.observer) return {};
    QStringList ids = session.observerTrajectorySelection.values();
    ids.sort();
    QJsonArray selected;
    QJsonArray trails;
    for (const QString& id : ids) {
        selected.append(id);
        trails.append(QJsonObject{{QStringLiteral("unitId"), id},
                                  {QStringLiteral("points"),
                                   m_observerTrajectories.value(id)}});
    }
    return QJsonObject{{QStringLiteral("selectedUnitIds"), selected},
                       {QStringLiteral("trails"), trails}};
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
        QStringLiteral("setRoe"), QStringLiteral("activateCountermeasure"),
        QStringLiteral("activateScan"), QStringLiteral("attemptFieldRepair"),
        QStringLiteral("cancelService")};
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
    const bool stationaryAction = action == QLatin1String("unitOrder")
        || action == QLatin1String("activateCountermeasure")
        || action == QLatin1String("attemptFieldRepair");
    if (!stationaryAction && !unit->movable()) {
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
    if (action == QLatin1String("activateCountermeasure")) {
        if (!unit->countermeasureState().supported()) {
            return reject(QStringLiteral("INVALID_UNIT_KIND"),
                          QStringLiteral("该单元不支持干扰弹"));
        }
        if (!unit->countermeasureState().available()) {
            return reject(QStringLiteral("WEAPON_UNAVAILABLE"),
                          QStringLiteral("干扰弹冷却中或次数已用尽"));
        }
    }
    if (action == QLatin1String("activateScan")) {
        if (unit->kind() != UnitKind::ReconUAV || !unit->scanState().supported()) {
            return reject(QStringLiteral("INVALID_UNIT_KIND"),
                          QStringLiteral("该操作仅适用于侦察无人机"));
        }
        if (!unit->scanState().available()) {
            return reject(QStringLiteral("WEAPON_UNAVAILABLE"),
                          QStringLiteral("扫描技能冷却中"));
        }
    }
    if (action == QLatin1String("attemptFieldRepair")) {
        const QJsonObject subsystems = unit->subsystemStateJson();
        const bool damaged = subsystems.value(QStringLiteral("sensor")).toDouble() < 1.0 - 1e-9
            || subsystems.value(QStringLiteral("comms")).toDouble() < 1.0 - 1e-9
            || subsystems.value(QStringLiteral("mobility")).toDouble() < 1.0 - 1e-9
            || subsystems.value(QStringLiteral("weapon")).toDouble() < 1.0 - 1e-9;
        if (!damaged || unit->repairCooldownRemaining() > 1e-9) {
            return reject(QStringLiteral("WEAPON_UNAVAILABLE"),
                          damaged ? QStringLiteral("战场修理冷却中")
                                  : QStringLiteral("没有可修复的受损部位"));
        }
    }
    if (action == QLatin1String("service")) {
        UnitBase* commandPost = m_engine.unit(unit->cpId());
        if (unit->kind() == UnitKind::CommandPost || !commandPost || !commandPost->alive()
            || commandPost->kind() != UnitKind::CommandPost
            || commandPost->side() != unit->side()
            || unit->pos().distanceTo2D(commandPost->pos())
                > SimulationEngine::kServiceRadiusMeters) {
            return reject(QStringLiteral("INVALID_ARGUMENT"),
                          QStringLiteral("单元必须在活指挥所 750 米内才能开始补充"));
        }
    }
    if (action == QLatin1String("cancelService") && !unit->serviceRequested()) {
        return reject(QStringLiteral("INVALID_ARGUMENT"),
                      QStringLiteral("单元当前未在补充"));
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
        const double maximum = unit->maxCommandedSpeed();
        if (!ok || !std::isfinite(speed) || speed <= 0.0
            || maximum <= 0.0 || speed > maximum) {
            return reject(QStringLiteral("INVALID_ARGUMENT"),
                          QStringLiteral("速度必须大于 0 且不超过 %1")
                              .arg(maximum,
                                   0, 'f', 0));
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
    if (action == QLatin1String("updateRoomConfig")) {
        handleRoomConfigCommand(socket, payload);
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

void GameServer::handleRoomConfigCommand(QWebSocket* socket, const QJsonObject& payload) {
    if (!m_clients.contains(socket)) return;
    const ClientSession session = m_clients.value(socket);
    const QString commandId = payload.value(QStringLiteral("commandId")).toString();
    const QString cacheKey = commandCacheKey(session.userId, commandId);
    const QJsonObject args = payload.value(QStringLiteral("args")).toObject();
    const auto cacheResult = [this, session, commandId](const CommandResult& result) {
        QJsonObject resultPayload = result.toJson();
        resultPayload[QStringLiteral("commandId")] = commandId;
        resultPayload[QStringLiteral("serverTime")] = m_engine.simTime();
        const QString key = commandCacheKey(session.userId, commandId);
        if (!m_commandResults.contains(key)) m_commandResultOrder.append(key);
        m_commandResults.insert(key, resultPayload);
        while (m_commandResultOrder.size() > 2048) {
            m_commandResults.remove(m_commandResultOrder.takeFirst());
        }
        return resultPayload;
    };
    const auto reject = [this, socket, commandId, cacheResult](const QString& code,
                                                                const QString& message) {
        sendCommandResult(socket, commandId, CommandResult::reject(code, message));
    };

    if (!canManageRoom(session)) {
        reject(QStringLiteral("PERMISSION_DENIED"),
               QStringLiteral("只有准备阶段的房间管理员可以编辑房间配置"));
        return;
    }
    const quint64 expectedVersion = static_cast<quint64>(
        args.value(QStringLiteral("expectedConfigVersion")).toInteger());
    if (expectedVersion != m_configVersion) {
        reject(QStringLiteral("ROOM_CONFIG_VERSION_CONFLICT"),
               QStringLiteral("房间配置已经更新，请重新载入后再保存"));
        return;
    }
    if (m_roomConfigCommandsInFlight.contains(cacheKey)) return;

    QJsonObject request{
        {QStringLiteral("expected_config_version"), static_cast<qint64>(expectedVersion)},
        {QStringLiteral("name"), args.value(QStringLiteral("name"))},
        {QStringLiteral("description"), args.value(QStringLiteral("description"))},
        {QStringLiteral("scenario_id"), args.value(QStringLiteral("scenarioId"))},
        {QStringLiteral("seat_limits"), args.value(QStringLiteral("seatLimits"))},
        {QStringLiteral("seat_parameters"), args.value(QStringLiteral("seatParameters"))}};
    const QUrl url(m_authServiceUrl + QStringLiteral("/api/internal/rooms/%1/config")
                       .arg(QUrl::toPercentEncoding(m_roomId)));
    QNetworkRequest networkRequest(url);
    networkRequest.setRawHeader("X-Internal-Key", m_internalKey.toUtf8());
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = m_network.put(
        networkRequest, QJsonDocument(request).toJson(QJsonDocument::Compact));
    m_roomConfigCommandsInFlight.insert(cacheKey);
    QTimer::singleShot(5000, reply, [reply]() {
        if (reply->isRunning()) {
            reply->setProperty("roomConfigTimedOut", true);
            reply->abort();
        }
    });
    const QPointer<QWebSocket> guardedSocket(socket);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, guardedSocket, commandId, cacheKey, session, cacheResult]() {
        m_roomConfigCommandsInFlight.remove(cacheKey);
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool timedOut = reply->property("roomConfigTimedOut").toBool();
        const QString networkError = reply->errorString();
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();

        const auto finish = [this, guardedSocket, commandId, cacheResult](
                                const CommandResult& result) {
            // Cache even when the requesting socket has gone away; a reconnect
            // with the same command id must receive the definitive result.
            cacheResult(result);
            if (guardedSocket && m_clients.contains(guardedSocket)) {
                sendCommandResult(guardedSocket, commandId, result);
            }
        };

        if (statusCode != 200 || !document.isObject()) {
            const QString code = statusCode == 409
                ? QStringLiteral("ROOM_CONFIG_VERSION_CONFLICT")
                : QStringLiteral("ROOM_CONFIG_UPDATE_FAILED");
            const QString message = statusCode == 409
                ? QStringLiteral("房间配置版本冲突，请重新载入后保存")
                : timedOut
                    ? QStringLiteral("房间配置保存超时，请稍后重试")
                    : QStringLiteral("房间配置保存失败: %1")
                          .arg(networkError.isEmpty()
                                   ? QStringLiteral("HTTP %1").arg(statusCode)
                                   : networkError);
            finish(CommandResult::reject(code, message));
            return;
        }

        const QJsonObject room = document.object().value(QStringLiteral("room")).toObject();
        const quint64 nextVersion = static_cast<quint64>(
            room.value(QStringLiteral("configVersion")).toInteger());
        const QJsonObject limitsObject = room.value(QStringLiteral("seatLimits")).toObject();
        const QJsonObject parametersObject = room.value(QStringLiteral("seatParameters")).toObject();
        if (room.value(QStringLiteral("roomId")).toString() != m_roomId
            || nextVersion == 0 || limitsObject.isEmpty()) {
            finish(CommandResult::reject(QStringLiteral("ROOM_CONFIG_UPDATE_FAILED"),
                                         QStringLiteral("账号服务返回的房间配置无效")));
            return;
        }

        QHash<QString, int> nextLimits;
        for (auto it = limitsObject.constBegin(); it != limitsObject.constEnd(); ++it) {
            const QString key = it.key();
            const SeatDescriptor descriptor = describeSeat(key);
            if (!it.value().isDouble() || descriptor.baseId != key
                || (descriptor.side != QLatin1String("red")
                    && descriptor.side != QLatin1String("blue"))) {
                finish(CommandResult::reject(QStringLiteral("ROOM_CONFIG_UPDATE_FAILED"),
                                             QStringLiteral("账号服务返回的战位容量无效")));
                return;
            }
            const int limit = it.value().toInt(-1);
            if (limit < 0 || limit > 64
                || (descriptor.type == QLatin1String("commander") && limit != 1)) {
                finish(CommandResult::reject(QStringLiteral("ROOM_CONFIG_UPDATE_FAILED"),
                                             QStringLiteral("账号服务返回的战位容量超出范围")));
                return;
            }
            nextLimits.insert(key, limit);
        }
        if (nextLimits.size() != 10) {
            finish(CommandResult::reject(QStringLiteral("ROOM_CONFIG_UPDATE_FAILED"),
                                         QStringLiteral("账号服务返回的战位配置不完整")));
            return;
        }
        QHash<QString, QJsonObject> nextParameters;
        for (auto it = parametersObject.constBegin(); it != parametersObject.constEnd(); ++it) {
            const SeatDescriptor descriptor = describeSeat(it.key());
            if (descriptor.baseId.isEmpty() || !it.value().isObject()) {
                finish(CommandResult::reject(QStringLiteral("ROOM_CONFIG_UPDATE_FAILED"),
                                             QStringLiteral("账号服务返回的战位参数无效")));
                return;
            }
            const QJsonObject parameters = it.value().toObject();
            for (auto parameter = parameters.constBegin(); parameter != parameters.constEnd(); ++parameter) {
                if ((parameter.key() != QLatin1String("communicationRange")
                     && parameter.key() != QLatin1String("detectRange"))
                    || !parameter.value().isDouble()
                    || !std::isfinite(parameter.value().toDouble())
                    || parameter.value().toDouble() < 0.0
                    || parameter.value().toDouble() > 1000000.0) {
                    finish(CommandResult::reject(QStringLiteral("ROOM_CONFIG_UPDATE_FAILED"),
                                                 QStringLiteral("账号服务返回的战位参数超出范围")));
                    return;
                }
            }
            nextParameters.insert(it.key(), parameters);
        }

        if (m_phase != QLatin1String("preparing")) {
            finish(CommandResult::reject(QStringLiteral("ROOM_NOT_PREPARING"),
                                         QStringLiteral("房间已经离开准备阶段，配置未应用")));
            return;
        }
        const RoomStateBackup backup = captureRoomState();
        m_roomName = room.value(QStringLiteral("name")).toString(m_roomName);
        m_roomDescription = room.value(QStringLiteral("description")).toString(m_roomDescription);
        m_scenarioId = room.value(QStringLiteral("scenarioId")).toString(m_scenarioId);
        m_configVersion = nextVersion;
        m_lastRoomUpdate = room.value(QStringLiteral("updatedAt")).toString(m_lastRoomUpdate);
        m_seatLimits = nextLimits;
        m_seatParameters = nextParameters;
        reconcileSeatConfiguration(true);
        const CommandResult success = CommandResult::ok(QStringLiteral("房间配置已保存"));
        // Include the result in the checkpoint written below.  This keeps a
        // retried command idempotent across a server restart as well as across
        // a live reconnect.
        cacheResult(success);
        QString persistenceError;
        if (!persistRoomState(&persistenceError)) {
            QString rollbackError;
            restoreRoomStateBackup(backup, &rollbackError);
            finish(CommandResult::reject(
                QStringLiteral("PERSISTENCE_FAILED"),
                QStringLiteral("房间配置已收到但本地检查点写入失败: %1")
                    .arg(persistenceError.isEmpty() ? rollbackError : persistenceError)));
            return;
        }
        syncAuthoritativeSeats();
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            if (it->authenticated && it->roomId == m_roomId) sendSeatDirectory(it.key());
        }
        broadcastRoomDirectory();
        broadcastSnapshots(true);
        finish(success);
    });
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

void GameServer::handleVmfMessage(QWebSocket* socket, const QJsonObject& payload,
                                  const QString& requestId) {
    if (!m_clients.contains(socket)) return;
    ClientSession& session = m_clients[socket];
    if (session.roomId != m_roomId || session.seatId.isEmpty() || session.observer) {
        sendError(socket, QStringLiteral("SEAT_REQUIRED"),
                  QStringLiteral("选择可用战位后才能发送 VMF 消息"), requestId);
        return;
    }
    if (!m_engine.vmfEnabled()
        || m_engine.scenario().communicationPolicy.format != QLatin1String("vmf-design-v1")) {
        sendError(socket, QStringLiteral("VMF_DISABLED"),
                  QStringLiteral("当前房间未启用 VMF profile"), requestId);
        return;
    }
    const AuthoritativeRoom::Seat seat = m_authoritativeRoom.seat(session.seatId);
    Message::Type requestedType;
    if (!Message::parseTypeName(payload.value(QStringLiteral("messageType")).toString(),
                                &requestedType)) {
        sendError(socket, QStringLiteral("VMF_ROLE_FORBIDDEN"),
                  QStringLiteral("当前战位无权发送该 VMF 消息类型"), requestId);
        return;
    }
    const QString senderId = payload.value(QStringLiteral("senderUnitId")).toString();
    if (senderId.isEmpty() || seat.unitId != senderId) {
        sendError(socket, QStringLiteral("UNIT_NOT_OWNED"),
                  QStringLiteral("VMF 发送单元不属于当前战位"), requestId);
        return;
    }
    UnitBase* sender = m_engine.unit(senderId);
    if (!sender || !sender->alive() || sender->sideStr() != session.side) {
        sendError(socket, QStringLiteral("UNIT_NOT_AVAILABLE"),
                  QStringLiteral("VMF 发送单元当前不可用"), requestId);
        return;
    }
    const QString receiverId = payload.value(QStringLiteral("receiverUnitId")).toString();
    if (receiverId != QLatin1String("*")) {
        UnitBase* receiver = m_engine.unit(receiverId);
        if (!receiver || !receiver->alive() || receiver->sideStr() != session.side) {
            sendError(socket, QStringLiteral("INVALID_RECEIVER"),
                      QStringLiteral("VMF 接收单元必须是同阵营有效单元"), requestId);
            return;
        }
        if (!StateProjector::canTransmit(m_engine, senderId, receiverId)) {
            sendError(socket, QStringLiteral("COMMUNICATION_LOST"),
                      QStringLiteral("发送方与接收方当前不在有向通信链路内"), requestId);
            return;
        }
    } else {
        bool reachable = false;
        for (const QString& id : m_engine.unitIds()) {
            const UnitBase* candidate = m_engine.unit(id);
            if (candidate && candidate->alive() && candidate->sideStr() == session.side
                && id != senderId && StateProjector::canTransmit(m_engine, senderId, id)) {
                reachable = true;
                break;
            }
        }
        if (!reachable) {
            sendError(socket, QStringLiteral("COMMUNICATION_LOST"),
                      QStringLiteral("当前没有可达的同阵营 VMF 接收单元"), requestId);
            return;
        }
    }

    const QString traceId = payload.value(QStringLiteral("traceId")).toString();
    const QString dedupeKey = QString::number(session.userId) + QLatin1Char(':') + traceId;
    if (m_vmfMessageIds.contains(dedupeKey)) {
        sendError(socket, QStringLiteral("DUPLICATE_MESSAGE"),
                  QStringLiteral("VMF trace 已处理"), requestId);
        return;
    }
    Message message;
    QString buildError;
    const QString messageId = QStringLiteral("vmf_%1_%2").arg(session.userId).arg(traceId);
    if (!buildVmfMessage(payload, messageId, &message, &buildError)) {
        sendError(socket, QStringLiteral("INVALID_VMF"), buildError, requestId);
        return;
    }
    QString validationError;
    if (!m_engine.validateVmfMessage(message, &validationError)) {
        sendError(socket, QStringLiteral("INVALID_VMF"), validationError, requestId);
        return;
    }
    const QJsonObject catalogSummary = m_engine.vmfMessageCatalogSummary(message);
    if (catalogSummary.isEmpty()) {
        sendError(socket, QStringLiteral("INVALID_VMF"),
                  QStringLiteral("VMF 消息目录摘要缺失"), requestId);
        return;
    }
    QString receiverRole;
    if (receiverId != QLatin1String("*")) {
        bool receiverSeatFound = false;
        for (const AuthoritativeRoom::Seat& candidate : m_authoritativeRoom.seats()) {
            if (candidate.unitId == receiverId) {
                receiverRole = candidate.seatType;
                receiverSeatFound = true;
                break;
            }
        }
        if (!receiverSeatFound) {
            sendError(socket, QStringLiteral("INVALID_RECEIVER"),
                      QStringLiteral("VMF 接收单元不属于当前权威战位"), requestId);
            return;
        }
    } else {
        const QJsonArray receiverRoles = catalogSummary.value(QStringLiteral("receiverRoles"))
                                             .toArray();
        if (!std::any_of(receiverRoles.cbegin(), receiverRoles.cend(), [](const QJsonValue& value) {
                return value.toString() == QLatin1String("any");
            })) {
            sendError(socket, QStringLiteral("INVALID_RECEIVER"),
                      QStringLiteral("该 VMF 消息必须指定具有目录角色的接收战位"), requestId);
            return;
        }
    }
    if (!m_engine.validateVmfMessageForRoles(message, seat.seatType, receiverRole,
                                             &validationError)) {
        sendError(socket, QStringLiteral("VMF_ROLE_FORBIDDEN"), validationError, requestId);
        return;
    }
    message.requiresAck = catalogSummary.value(QStringLiteral("requiresAck")).toBool();
    message.automaticAck = catalogSummary.value(QStringLiteral("automaticAck")).toBool();
    message.payload.insert(QStringLiteral("vmfCatalogId"),
                           catalogSummary.value(QStringLiteral("catalogId")));
    message.payload.insert(QStringLiteral("vmfTrigger"),
                           catalogSummary.value(QStringLiteral("trigger")));
    message.payload.insert(QStringLiteral("vmfInformationValue"),
                           catalogSummary.value(QStringLiteral("informationValue")));
    if (!m_engine.validateVmfWorkflowMessage(message, &validationError)) {
        sendError(socket, QStringLiteral("VMF_SEQUENCE_INVALID"), validationError, requestId);
        return;
    }
    QJsonObject durable = payload;
    durable.insert(QStringLiteral("messageId"), message.id);
    durable.insert(QStringLiteral("userId"), session.userId);
    durable.insert(QStringLiteral("seatId"), session.seatId);
    QString persistenceError;
    if (!recordDurableEvent(QStringLiteral("vmfMessage"), durable, &persistenceError)) {
        sendError(socket, QStringLiteral("PERSISTENCE_FAILED"),
                  QStringLiteral("VMF 消息日志写入失败: %1").arg(persistenceError), requestId);
        return;
    }
    QString postError;
    if (!m_engine.postVmfMessage(message, &postError)) {
        sendError(socket, QStringLiteral("INVALID_VMF"), postError, requestId);
        return;
    }
    bool acknowledged = !message.requiresAck;
    if (!acknowledged && m_engine.bus()) {
        const QList<Message> pending = m_engine.bus()->pendingAcks();
        acknowledged = std::none_of(
            pending.cbegin(), pending.cend(), [&message](const Message& candidate) {
                return candidate.id == message.id;
            });
    }
    m_vmfMessageIds.insert(dedupeKey);
    m_vmfMessageIdOrder.append(dedupeKey);
    while (m_vmfMessageIdOrder.size() > 4096) {
        m_vmfMessageIds.remove(m_vmfMessageIdOrder.takeFirst());
    }
    ++m_stateRevision;

    QJsonObject workflowState;
    if (const GuidedStrikeWorkflow* workflow = m_engine.guidedStrikeWorkflow(session.side)) {
        workflowState = workflow->snapshot();
    }
    const QJsonObject event{
        {QStringLiteral("kind"), QStringLiteral("vmfMessage")},
        {QStringLiteral("messageId"), message.id},
        {QStringLiteral("traceId"), message.traceId},
        {QStringLiteral("correlationId"), message.correlationId},
        {QStringLiteral("vmfMessage"), message.vmfMessage},
        {QStringLiteral("wireFormat"), Message::wireFormatName(message.wireFormat)},
        {QStringLiteral("wireBitLength"), message.wireBitLength},
        {QStringLiteral("senderUnitId"), message.sender},
        {QStringLiteral("receiverUnitId"), message.receiver},
        {QStringLiteral("messageType"), Message::typeName(message.type)},
        {QStringLiteral("validated"), true},
        {QStringLiteral("fieldCount"), payload.value(QStringLiteral("fieldCount")).toInt()},
        {QStringLiteral("catalogId"), catalogSummary.value(QStringLiteral("catalogId"))},
        {QStringLiteral("trigger"), catalogSummary.value(QStringLiteral("trigger"))},
        {QStringLiteral("informationValue"), catalogSummary.value(QStringLiteral("informationValue"))},
        {QStringLiteral("retryCount"), message.retryCount},
        {QStringLiteral("acked"), acknowledged},
        {QStringLiteral("senderSide"), session.side},
        {QStringLiteral("receiverSide"), session.side},
        {QStringLiteral("direction"), QStringLiteral("friendly")},
        {QStringLiteral("workflow"), workflowState},
        {QStringLiteral("summary"), messageSummary(QStringLiteral("vmfMessage"), payload)}};
    broadcastVmfEvent(event);
    persistRoomState();
}

void GameServer::handleGeneratedVmfMessage(const QJsonObject& posted) {
    // Client-originated VMF envelopes are recorded by handleVmfMessage before
    // they enter the bus.  This path is intentionally limited to the
    // simulation-generated reconnaissance confirmation; recording every bus
    // message would persist position reports, ACKs and native traffic that
    // are not part of the VMF control-plane contract.
    if (m_replayingDurableEvents) return;
    if (posted.value(QStringLiteral("type")).toString()
            != QLatin1String("TargetDestroyed")
        || posted.value(QStringLiteral("payload")).toObject()
               .value(QStringLiteral("confirmationSource")).toString()
            != QLatin1String("recon-observation")) {
        return;
    }
    if (!m_engine.vmfEnabled()
        || posted.value(QStringLiteral("wireFormat")).toString()
            != QLatin1String("vmf-design-v1")) {
        audit(QStringLiteral("vmf"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("generatedMessageRejected")},
                          {QStringLiteral("reason"), QStringLiteral("missing-vmf-envelope")},
                          {QStringLiteral("messageId"), posted.value(QStringLiteral("id"))}});
        return;
    }
    const QString messageId = posted.value(QStringLiteral("id")).toString();
    if (messageId.trimmed().isEmpty()) {
        audit(QStringLiteral("vmf"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("generatedMessageRejected")},
                          {QStringLiteral("reason"), QStringLiteral("missing-message-id")}});
        return;
    }

    const QString senderId = posted.value(QStringLiteral("sender")).toString();
    const UnitBase* sender = m_engine.unit(senderId);
    if (!sender || !sender->alive() || sender->kind() != UnitKind::ReconUAV) {
        audit(QStringLiteral("vmf"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("generatedMessageRejected")},
                          {QStringLiteral("reason"), QStringLiteral("invalid-recon-sender")},
                          {QStringLiteral("senderUnitId"), senderId}});
        return;
    }

    QString seatId;
    for (const AuthoritativeRoom::Seat& candidate : m_authoritativeRoom.seats()) {
        if (candidate.unitId == senderId && candidate.side == sender->sideStr()) {
            seatId = candidate.seatId;
            break;
        }
    }
    if (seatId.isEmpty()) {
        audit(QStringLiteral("vmf"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("generatedMessageRejected")},
                          {QStringLiteral("reason"), QStringLiteral("recon-seat-not-found")},
                          {QStringLiteral("senderUnitId"), senderId}});
        return;
    }

    // Convert MessageBus' internal JSON shape back to the durable VMF event
    // shape consumed by applyDurableEvent().  The exact encoded bytes are
    // retained for deterministic replay and are never included in projected
    // observer events.
    QJsonObject durable{
        {QStringLiteral("messageId"), messageId},
        {QStringLiteral("messageType"), posted.value(QStringLiteral("type"))},
        {QStringLiteral("senderUnitId"), posted.value(QStringLiteral("sender"))},
        {QStringLiteral("receiverUnitId"), posted.value(QStringLiteral("receiver"))},
        {QStringLiteral("traceId"), posted.value(QStringLiteral("traceId"))},
        {QStringLiteral("correlationId"), posted.value(QStringLiteral("correlationId"))},
        {QStringLiteral("vmfMessage"), posted.value(QStringLiteral("vmfMessage"))},
        {QStringLiteral("wireFormat"), posted.value(QStringLiteral("wireFormat"))},
        {QStringLiteral("wireBytes"), posted.value(QStringLiteral("wireBytes"))},
        {QStringLiteral("wireBitLength"), posted.value(QStringLiteral("wireBitLength"))},
        {QStringLiteral("requiresAck"), posted.value(QStringLiteral("requiresAck"))},
        {QStringLiteral("retryCount"), posted.value(QStringLiteral("retryCount"))},
        {QStringLiteral("payload"), posted.value(QStringLiteral("payload"))},
        {QStringLiteral("userId"), 0},
        {QStringLiteral("seatId"), seatId},
        {QStringLiteral("generatedBy"), QStringLiteral("simulation")}};

    QString persistenceError;
    if (!recordDurableEvent(QStringLiteral("vmfMessage"), durable, &persistenceError)) {
        audit(QStringLiteral("persistence"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("generatedVmfEventFailed")},
                          {QStringLiteral("messageId"), durable.value(QStringLiteral("messageId"))},
                          {QStringLiteral("message"), persistenceError}});
    }
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
    const ClientSession& session = m_clients.value(socket);
    if (!canManageRoom(session)) {
        sendError(socket, QStringLiteral("PERMISSION_DENIED"),
                  QStringLiteral("只有准备阶段的房间管理员可以编辑初始阵容"));
        return;
    }
    const QJsonObject unitObject = payload.value(QStringLiteral("unit")).toObject();
    const QString unitId = unitObject.value(QStringLiteral("id")).toString().trimmed();
    if (unitId.isEmpty()) {
        sendError(socket, QStringLiteral("INVALID_SCENARIO"), QStringLiteral("初始单位必须包含 ID"));
        return;
    }
    Scenario replacement = m_runInitialScenario.units.empty()
        ? m_engine.scenario() : m_runInitialScenario;
    QJsonObject wrapper = ScenarioIo::toJson(replacement);
    wrapper[QStringLiteral("units")] = QJsonArray{unitObject};
    QString parseError;
    const Scenario parsed = ScenarioIo::fromJson(wrapper, &parseError);
    if (!parseError.isEmpty() || parsed.units.size() != 1) {
        sendError(socket, QStringLiteral("INVALID_SCENARIO"),
                  parseError.isEmpty() ? QStringLiteral("初始单位数据无效") : parseError);
        return;
    }
    bool replaced = false;
    for (ScenarioUnit& unit : replacement.units) {
        if (unit.id != unitId) continue;
        unit = parsed.units.front();
        replaced = true;
        break;
    }
    if (!replaced) replacement.units.push_back(parsed.units.front());
    QString error;
    if (!replaceInitialScenario(replacement, &error)) {
        sendError(socket, QStringLiteral("SCENARIO_UPDATE_FAILED"), error);
        return;
    }
    broadcastSnapshots(true);
}

void GameServer::handleScenarioRemove(QWebSocket* socket, const QJsonObject& payload) {
    const ClientSession& session = m_clients.value(socket);
    if (!canManageRoom(session)) {
        sendError(socket, QStringLiteral("PERMISSION_DENIED"),
                  QStringLiteral("只有准备阶段的房间管理员可以编辑初始阵容"));
        return;
    }
    const QString unitId = payload.value(QStringLiteral("unitId")).toString().trimmed();
    Scenario replacement = m_runInitialScenario.units.empty()
        ? m_engine.scenario() : m_runInitialScenario;
    const qsizetype beforeSize = static_cast<qsizetype>(replacement.units.size());
    std::erase_if(replacement.units, [&unitId](const ScenarioUnit& unit) {
        return unit.id == unitId;
    });
    if (static_cast<qsizetype>(replacement.units.size()) == beforeSize) {
        sendError(socket, QStringLiteral("UNIT_NOT_FOUND"), QStringLiteral("初始单位不存在"));
        return;
    }
    QString error;
    if (!replaceInitialScenario(replacement, &error)) {
        sendError(socket, QStringLiteral("SCENARIO_UPDATE_FAILED"), error);
        return;
    }
    broadcastSnapshots(true);
}

void GameServer::handleScenarioReplace(QWebSocket* socket, const QJsonObject& payload) {
    const ClientSession& session = m_clients.value(socket);
    if (!canManageRoom(session)) {
        sendError(socket, QStringLiteral("PERMISSION_DENIED"),
                  QStringLiteral("只有准备阶段的房间管理员可以编辑初始阵容"));
        return;
    }
    QString parseError;
    const Scenario replacement = ScenarioIo::fromJson(
        payload.value(QStringLiteral("scenario")).toObject(), &parseError);
    if (!parseError.isEmpty()) {
        sendError(socket, QStringLiteral("INVALID_SCENARIO"), parseError);
        return;
    }
    QString error;
    if (!replaceInitialScenario(replacement, &error)) {
        sendError(socket, QStringLiteral("SCENARIO_UPDATE_FAILED"), error);
        return;
    }
    broadcastSnapshots(true);
}

bool GameServer::applyInitialScenarioState(const Scenario& initialScenario,
                                           const Scenario& runtimeScenario,
                                           quint64 scenarioRevision,
                                           QString* error) {
    if (error) error->clear();
    if (scenarioRevision == 0) {
        if (error) *error = QStringLiteral("初始场景版本无效");
        return false;
    }
    const QString initialValidation = validateNetworkScenario(initialScenario);
    if (!initialValidation.isEmpty()) {
        if (error) *error = initialValidation;
        return false;
    }
    if (!runtimeScenario.units.empty()) {
        const QString runtimeValidation = validateNetworkScenario(runtimeScenario);
        if (!runtimeValidation.isEmpty()) {
            if (error) *error = runtimeValidation;
            return false;
        }
    }

    QHash<QString, ScenarioUnit> catalog = AuthoritativeRoom::defaultTemplateCatalog();
    for (const ScenarioUnit& unit : initialScenario.units) {
        if (catalog.contains(unit.kind)) catalog.insert(unit.kind, unit);
    }
    QString catalogError;
    if (!m_authoritativeRoom.setTemplateCatalog(catalog, &catalogError)) {
        if (error) *error = catalogError;
        return false;
    }
    const bool applied = runtimeScenario.units.empty()
        ? m_engine.setRemoteScenario(runtimeScenario) : m_engine.setScenario(runtimeScenario);
    if (!applied) {
        if (error) *error = m_engine.lastError();
        return false;
    }
    m_runInitialScenario = initialScenario;
    m_scenarioRevision = scenarioRevision;
    resetReadiness();
    return true;
}

bool GameServer::replaceInitialScenario(const Scenario& scenario, QString* error) {
    if (error) error->clear();
    if (m_phase != QLatin1String("preparing")) {
        if (error) *error = QStringLiteral("只有准备阶段可以编辑初始阵容");
        return false;
    }
    if (!m_authoritativeRoom.seats().isEmpty()) {
        if (error) *error = QStringLiteral("房间已有战位占用，请先清空战位后再编辑初始阵容");
        return false;
    }
    const QString validationError = validateNetworkScenario(scenario);
    if (!validationError.isEmpty()) {
        if (error) *error = validationError;
        return false;
    }

    const RoomStateBackup backup = captureRoomState();
    const quint64 nextRevision = m_scenarioRevision + 1;
    QString persistenceError;
    if (!recordDurableEvent(
            QStringLiteral("scenario"),
            QJsonObject{{QStringLiteral("scenario"), ScenarioIo::toJson(scenario)},
                        {QStringLiteral("scenarioRevision"), static_cast<qint64>(nextRevision)}},
            &persistenceError)) {
        if (error) *error = QStringLiteral("场景日志写入失败: %1").arg(persistenceError);
        return false;
    }

    const quint64 scenarioEventSequence = m_eventSequence;
    const auto rollback = [this, &backup, scenarioEventSequence, error](const QString& reason) {
        QString restoreError;
        if (!restoreRoomStateBackup(backup, &restoreError)) {
            if (error) {
                *error = QStringLiteral("%1；场景回滚失败: %2")
                    .arg(reason, restoreError);
            }
            return false;
        }

        // The scenario event was already appended. Restore the previous event
        // sequence before appending a compensating event, otherwise recovery
        // would replay a change that never committed to a checkpoint.
        m_eventSequence = scenarioEventSequence;
        const Scenario initialScenario = backup.runInitialScenario.units.empty()
            ? backup.scenario : backup.runInitialScenario;
        QString rollbackEventError;
        const bool rollbackRecorded = recordDurableEvent(
            QStringLiteral("scenarioRollback"),
            QJsonObject{{QStringLiteral("scenario"), ScenarioIo::toJson(backup.scenario)},
                        {QStringLiteral("initialScenario"), ScenarioIo::toJson(initialScenario)},
                        {QStringLiteral("scenarioRevision"),
                         static_cast<qint64>(backup.scenarioRevision)}},
            &rollbackEventError);
        QString persistError;
        const bool scenarioSaved = rollbackRecorded && persistScenario(&persistError);
        const bool checkpointSaved = scenarioSaved && persistRoomState(&persistError);
        if (error) {
            QStringList details{reason};
            if (!rollbackRecorded) {
                details.append(QStringLiteral("回滚日志写入失败: %1").arg(rollbackEventError));
            }
            if (!scenarioSaved || !checkpointSaved) {
                details.append(QStringLiteral("旧场景持久化失败: %1").arg(persistError));
            }
            *error = details.join(QStringLiteral("；"));
        }
        return false;
    };

    QString applyError;
    if (!applyInitialScenarioState(scenario, scenario, nextRevision, &applyError)) {
        return rollback(QStringLiteral("场景应用失败: %1").arg(applyError));
    }
    ++m_stateRevision;
    if (!persistScenario(&persistenceError)) {
        return rollback(QStringLiteral("场景文件保存失败: %1").arg(persistenceError));
    }
    if (!persistRoomState(&persistenceError)) {
        return rollback(QStringLiteral("房间检查点保存失败: %1").arg(persistenceError));
    }
    return true;
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
    checkpoint.engineState = checkpoint.scenario.units.empty()
        ? QJsonObject{} : m_engine.collectGlobalCheckpointState();
    checkpoint.vmfState = addServerVmfDedupe(m_engine.collectVmfRuntimeState(),
                                             m_vmfMessageIds);
    for (const QString& key : m_commandResultOrder) {
    checkpoint.commandHistory.append(
            QJsonObject{{QStringLiteral("key"), key},
                        {QStringLiteral("result"), m_commandResults.value(key)}});
    }
    checkpoint.mapMarks = m_mapMarks;
    checkpoint.intelLedger = m_intelLedger.toJson();
    checkpoint.authoritativeRoom = m_authoritativeRoom.toJson();
    checkpoint.phase = m_phase;
    checkpoint.roomStatus = m_roomStatus;
    checkpoint.roomName = m_roomName;
    checkpoint.roomDescription = m_roomDescription;
    checkpoint.scenarioId = m_scenarioId;
    for (auto it = m_seatLimits.cbegin(); it != m_seatLimits.cend(); ++it) {
        checkpoint.seatLimits[it.key()] = it.value();
    }
    for (auto it = m_seatParameters.cbegin(); it != m_seatParameters.cend(); ++it) {
        checkpoint.seatParameters[it.key()] = it.value();
    }
    checkpoint.configVersion = m_configVersion;
    checkpoint.lastRoomUpdate = m_lastRoomUpdate;
    checkpoint.redReady = m_redReady;
    checkpoint.blueReady = m_blueReady;
    checkpoint.running = m_engine.running();
    checkpoint.simTime = m_engine.simTime();
    checkpoint.speed = m_engine.speedMul();
    checkpoint.scenarioRevision = m_scenarioRevision;
    checkpoint.stateRevision = m_stateRevision;
    checkpoint.eventSequence = m_eventSequence;
    if (m_roomMode == QLatin1String("pve")) {
        AiCheckpointState ai;
        ai.matchGeneration = m_matchGeneration;
        ai.commandSequence = m_aiCommandSequence;
        ai.planningGeneration = m_aiPlanningGeneration;
        ai.rngState = m_aiRngState;
        ai.aiDifficulty = m_aiDifficulty;
        ai.providerMode = m_aiProviderMode;
        ai.providerModel = m_ollamaProvider ? m_ollamaProvider->model()
                                             : QStringLiteral("qwen3:4b");
        ai.selectedProvider = m_aiSelectedProvider;
        ai.selectedModel = m_aiSelectedModel;
        ai.resolvedModel = m_aiResolvedModel;
        ai.roomConfigVersion = m_aiRoomConfigVersion;
        ai.ollamaConfigVersion = m_aiOllamaConfigVersion;
        ai.fallbackReason = m_aiFallbackReason;
        ai.nextDecisionAt = m_aiNextDecisionAt;
        ai.nextReplanAt = m_aiNextReplanAt;
        if (!m_aiPlan.requestId.isEmpty()) ai.currentPlan = m_aiPlan;
        ai.consecutiveFailures = m_aiConsecutiveFailures;
        ai.stickyRules = m_aiStickyRules;
        ai.effectiveEngine = m_aiEffectiveEngine;
        ai.lastFailureClass = m_aiLastFailureClass;
        ai.providerRequests = m_aiProviderRequests;
        ai.providerSuccesses = m_aiProviderSuccesses;
        ai.providerFailures = m_aiProviderFailures;
        ai.lastLatencyMs = m_aiLastLatencyMs;
        ai.averageLatencyMs = m_aiAverageLatencyMs;
        ai.strategyPhase = m_aiStrategyPhase;
        ai.replanReason = m_aiReplanReason;
        QStringList contactIds = m_aiContactMemory.keys();
        contactIds.sort();
        for (const QString& contactId : contactIds) {
            ai.contactMemory.append(m_aiContactMemory.value(contactId).toJson());
        }
        ai.nextPrivilegedSampleAt = m_aiNextPrivilegedSampleAt;
        ai.privilegedSampleSequence = m_aiPrivilegedSampleSequence;
        checkpoint.aiState = ai;
    }
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
    if (checkpoint.sourceSchemaVersion == 2) {
        QString eventError;
        const QJsonArray tailEvents = m_persistence.eventsAfter(checkpoint.eventSequence,
                                                                 &eventError);
        if (!eventError.isEmpty()) {
            if (error) *error = eventError;
            return false;
        }
        const bool hasLegacyCommand = std::any_of(
            tailEvents.cbegin(), tailEvents.cend(), [](const QJsonValue& value) {
                return value.toObject().value(QStringLiteral("kind")).toString()
                    == QLatin1String("command");
            });
        if (hasLegacyCommand) {
            if (error) {
                *error = QStringLiteral(
                    "schema 2 检查点之后存在旧规则命令日志；请先用旧版本生成最新检查点再升级");
            }
            return false;
        }

        // Schema 2 used serviceRequested for an automatic return-to-base action.
        // Schema 3 interprets it as an in-place, interruptible service timer, so
        // carrying the old FSM state forward would silently change the result.
        QJsonArray migratedRuntime;
        for (const QJsonValue& value : checkpoint.runtimeUnits) {
            QJsonObject state = value.toObject();
            if (state.value(QStringLiteral("checkpointType")).toString()
                != QLatin1String("engine")
                && state.value(QStringLiteral("serviceRequested")).toBool()) {
                state[QStringLiteral("serviceRequested")] = false;
                QJsonObject behavior = state.value(QStringLiteral("behavior")).toObject();
                behavior[QStringLiteral("fsmState")] = QStringLiteral("idle");
                behavior[QStringLiteral("waypoints")] = QJsonArray{};
                behavior[QStringLiteral("waypointIndex")] = 0;
                state[QStringLiteral("behavior")] = behavior;
            }
            migratedRuntime.append(state);
        }
        checkpoint.runtimeUnits = migratedRuntime;
    }
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
    cancelAiPlanRequest();
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
        if (!m_engine.restoreCheckpointState(checkpoint.runtimeUnits,
                                             checkpoint.engineState,
                                             checkpoint.simTime,
                                             checkpoint.running, checkpoint.speed,
                                             &runtimeError)) {
            if (error) *error = QStringLiteral("运行态恢复失败: %1").arg(runtimeError);
            return false;
        }
    }
    if (!checkpoint.vmfState.isEmpty()) {
        QString vmfError;
        if (!m_engine.restoreVmfRuntimeState(checkpoint.vmfState, &vmfError)) {
            if (error) *error = QStringLiteral("VMF 运行态恢复失败: %1").arg(vmfError);
            return false;
        }
        restoreServerVmfDedupe(checkpoint.vmfState, &m_vmfMessageIds,
                               &m_vmfMessageIdOrder);
    }
    m_runInitialScenario = checkpoint.runInitialScenario;
    m_phase = checkpoint.phase;
    if (!checkpoint.roomName.isEmpty()) m_roomName = checkpoint.roomName;
    m_roomDescription = checkpoint.roomDescription;
    if (!checkpoint.scenarioId.isEmpty()) m_scenarioId = checkpoint.scenarioId;
    if (!checkpoint.roomStatus.isEmpty()) m_roomStatus = checkpoint.roomStatus;
    m_configVersion = checkpoint.configVersion > 0 ? checkpoint.configVersion : 1;
    m_lastRoomUpdate = checkpoint.lastRoomUpdate;
    if (!checkpoint.seatLimits.isEmpty()) {
        QHash<QString, int> restoredLimits;
        for (auto it = checkpoint.seatLimits.constBegin(); it != checkpoint.seatLimits.constEnd(); ++it) {
            const SeatDescriptor descriptor = describeSeat(it.key());
            if (descriptor.baseId != it.key() || !it.value().isDouble()) {
                if (error) *error = QStringLiteral("检查点战位容量无效");
                return false;
            }
            const int limit = it.value().toInt(-1);
            if (limit < 0 || limit > 64
                || (descriptor.type == QLatin1String("commander") && limit != 1)) {
                if (error) *error = QStringLiteral("检查点战位容量超出范围");
                return false;
            }
            restoredLimits.insert(it.key(), limit);
        }
        if (restoredLimits.size() == 10) m_seatLimits = restoredLimits;
    }
    if (!checkpoint.seatParameters.isEmpty()) {
        QHash<QString, QJsonObject> restoredParameters;
        for (auto it = checkpoint.seatParameters.constBegin(); it != checkpoint.seatParameters.constEnd(); ++it) {
            const SeatDescriptor descriptor = describeSeat(it.key());
            if (descriptor.baseId.isEmpty() || !it.value().isObject()) {
                if (error) *error = QStringLiteral("检查点战位参数无效");
                return false;
            }
            restoredParameters.insert(it.key(), it.value().toObject());
        }
        m_seatParameters = restoredParameters;
    }
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
    if (!m_intelLedger.restore(checkpoint.intelLedger, error)) return false;
    if (!checkpoint.authoritativeRoom.isEmpty()
        && !m_authoritativeRoom.restore(checkpoint.authoritativeRoom, error)) return false;
    m_roomMode = m_authoritativeRoom.mode();
    if (m_authoritativeRoom.phase() != checkpoint.phase) {
        if (error) *error = QStringLiteral("检查点权威房间阶段与运行阶段不一致");
        return false;
    }
    syncAuthoritativeSeats();
    if (checkpoint.aiState.has_value()) {
        if (m_roomMode != QLatin1String("pve")) {
            if (error) *error = QStringLiteral("PVP 检查点不应包含 AI 状态");
            return false;
        }
        const AiCheckpointState& ai = *checkpoint.aiState;
        m_matchGeneration = ai.matchGeneration;
        m_aiCommandSequence = ai.commandSequence;
        m_aiPlanningGeneration = ai.planningGeneration;
        m_aiRngState = ai.rngState;
        m_aiDifficulty = ai.aiDifficulty;
        m_aiProviderMode = ai.providerMode;
        m_aiSelectedProvider = ai.selectedProvider.isEmpty()
            ? (ai.providerMode == QLatin1String("rules")
                   ? QStringLiteral("rules") : QStringLiteral("ollama"))
            : ai.selectedProvider;
        m_aiSelectedModel = ai.selectedModel;
        m_aiResolvedModel = ai.resolvedModel;
        m_aiRoomConfigVersion = ai.roomConfigVersion;
        m_aiOllamaConfigVersion = ai.ollamaConfigVersion;
        m_aiFallbackReason = ai.fallbackReason;
        m_aiNextDecisionAt = ai.nextDecisionAt;
        m_aiNextReplanAt = ai.nextReplanAt;
        m_aiPlan = ai.currentPlan.value_or(AiPlanV1{});
        m_aiConsecutiveFailures = ai.consecutiveFailures;
        m_aiStickyRules = ai.stickyRules;
        m_aiEffectiveEngine = ai.effectiveEngine;
        m_aiLastFailureClass = ai.lastFailureClass;
        m_aiProviderRequests = ai.providerRequests;
        m_aiProviderSuccesses = ai.providerSuccesses;
        m_aiProviderFailures = ai.providerFailures;
        m_aiLastLatencyMs = ai.lastLatencyMs;
        m_aiAverageLatencyMs = ai.averageLatencyMs;
        m_aiStrategyPhase = ai.strategyPhase;
        m_aiReplanReason = ai.replanReason;
        m_aiContactMemory.clear();
        for (const QJsonValue& value : ai.contactMemory) {
            AiObservedTarget contact;
            QString contactError;
            if (AiObservedTarget::fromJson(value.toObject(), &contact, &contactError)) {
                m_aiContactMemory.insert(contact.targetId, contact);
            }
        }
        m_aiNextPrivilegedSampleAt = ai.nextPrivilegedSampleAt;
        m_aiPrivilegedSampleSequence = ai.privilegedSampleSequence;
    }
    if (!replayDurableEvents(error)) return false;
    if (!m_aiPlan.requestId.isEmpty()
        && (m_aiPlan.matchGeneration != m_matchGeneration
            || m_aiPlan.sourceStateRevision != m_stateRevision)) {
        m_aiPlan = {};
        m_aiNextReplanAt = 0.0;
    }
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
    const bool wasReplaying = m_replayingDurableEvents;
    m_replayingDurableEvents = true;
    for (const QJsonValue& value : events) {
        const QJsonObject event = value.toObject();
        const quint64 sequence = static_cast<quint64>(
            event.value(QStringLiteral("sequence")).toInteger());
        QString applyError;
        if (!applyDurableEvent(event.value(QStringLiteral("kind")).toString(),
                               event.value(QStringLiteral("payload")).toObject(),
                               &applyError)) {
            m_replayingDurableEvents = wasReplaying;
            if (error) {
                *error = QStringLiteral("重放事件 %1 失败: %2").arg(sequence).arg(applyError);
            }
            return false;
        }
        m_eventSequence = sequence;
        ++m_stateRevision;
    }
    m_replayingDurableEvents = wasReplaying;
    return true;
}

bool GameServer::applyDurableEvent(const QString& kind, const QJsonObject& payload,
                                   QString* error) {
    if (error) error->clear();
    if (kind == QLatin1String("vmfMessage")) {
        const QString messageId = payload.value(QStringLiteral("messageId")).toString();
        Message message;
        QString buildError;
        if (!buildVmfMessage(payload, messageId, &message, &buildError)) {
            if (error) *error = buildError;
            return false;
        }
        QString validationError;
        if (!m_engine.validateVmfMessage(message, &validationError)) {
            if (error) *error = validationError;
            return false;
        }
        const QString sourceSeatId = payload.value(QStringLiteral("seatId")).toString();
        if (sourceSeatId.isEmpty() || !m_authoritativeRoom.hasSeat(sourceSeatId)) {
            if (error) *error = QStringLiteral("VMF 重放事件缺少有效发送战位");
            return false;
        }
        const AuthoritativeRoom::Seat sourceSeat = m_authoritativeRoom.seat(sourceSeatId);
        if (sourceSeat.unitId.isEmpty() || sourceSeat.unitId != message.sender) {
            if (error) *error = QStringLiteral("VMF 重放发送单元不属于记录战位");
            return false;
        }
        const UnitBase* sourceUnit = m_engine.unit(message.sender);
        if (!sourceUnit || !sourceUnit->alive()
            || sourceUnit->sideStr() != sourceSeat.side) {
            if (error) *error = QStringLiteral("VMF 重放发送单元无效或阵营不一致");
            return false;
        }
        const QJsonObject catalogSummary = m_engine.vmfMessageCatalogSummary(message);
        if (catalogSummary.isEmpty()) {
            if (error) *error = QStringLiteral("VMF 重放消息目录摘要缺失");
            return false;
        }
        QString receiverRole;
        if (message.receiver != QLatin1String("*")) {
            bool receiverFound = false;
            for (const AuthoritativeRoom::Seat& candidate : m_authoritativeRoom.seats()) {
                if (candidate.unitId == message.receiver) {
                    receiverRole = candidate.seatType;
                    if (candidate.side != sourceSeat.side) {
                        if (error) *error = QStringLiteral("VMF 重放接收单元阵营不一致");
                        return false;
                    }
                    receiverFound = true;
                    break;
                }
            }
            if (!receiverFound) {
                if (error) *error = QStringLiteral("VMF 重放接收单元不存在");
                return false;
            }
        } else {
            const QJsonArray receiverRoles = catalogSummary
                .value(QStringLiteral("receiverRoles")).toArray();
            if (!std::any_of(receiverRoles.cbegin(), receiverRoles.cend(),
                             [](const QJsonValue& value) {
                                 return value.toString() == QLatin1String("any");
                             })) {
                if (error) *error = QStringLiteral("VMF 重放通配接收方不满足目录角色约束");
                return false;
            }
        }
        if (!m_engine.validateVmfMessageForRoles(message, sourceSeat.seatType,
                                                 receiverRole, &validationError)) {
            if (error) *error = validationError;
            return false;
        }
        message.requiresAck = catalogSummary.value(QStringLiteral("requiresAck")).toBool();
        message.automaticAck = catalogSummary.value(QStringLiteral("automaticAck")).toBool();
        message.payload.insert(QStringLiteral("vmfCatalogId"),
                               catalogSummary.value(QStringLiteral("catalogId")));
        message.payload.insert(QStringLiteral("vmfTrigger"),
                               catalogSummary.value(QStringLiteral("trigger")));
        message.payload.insert(QStringLiteral("vmfInformationValue"),
                               catalogSummary.value(QStringLiteral("informationValue")));
        const bool generatedSimulationMessage =
            payload.value(QStringLiteral("generatedBy")).toString()
                == QLatin1String("simulation")
            && payload.value(QStringLiteral("userId")).toInteger() == 0
            && message.type == Message::Type::TargetDestroyed
            && message.payload.value(QStringLiteral("confirmationSource")).toString()
                == QLatin1String("recon-observation");
        const GuidedStrikeWorkflow* workflow =
            m_engine.guidedStrikeWorkflow(sourceSeat.side);
        const bool unboundObservation = generatedSimulationMessage
            && (!workflow || workflow->stage() == GuidedStrikeWorkflow::Stage::Idle);
        if (!unboundObservation
            && !m_engine.validateVmfWorkflowMessage(message, &validationError)) {
            if (error) *error = validationError;
            return false;
        }
        if (!m_engine.postVmfMessage(message, &validationError)) {
            if (error) *error = validationError;
            return false;
        }
        const QString userId = QString::number(payload.value(QStringLiteral("userId")).toInteger());
        const QString traceId = payload.value(QStringLiteral("traceId")).toString();
        const QString dedupeKey = userId + QLatin1Char(':') + traceId;
        if (!dedupeKey.startsWith(QLatin1String("0:"))) {
            m_vmfMessageIds.insert(dedupeKey);
            m_vmfMessageIdOrder.append(dedupeKey);
            while (m_vmfMessageIdOrder.size() > 4096) {
                m_vmfMessageIds.remove(m_vmfMessageIdOrder.takeFirst());
            }
        }
        return true;
    }
    if (kind == QLatin1String("command")) {
        const CommandResult result = m_engine.executeCommand(
            payload.value(QStringLiteral("action")).toString(),
            payload.value(QStringLiteral("args")).toObject().toVariantMap());
        const QString commandId = payload.value(QStringLiteral("commandId")).toString();
        const qint64 userId = payload.value(QStringLiteral("userId")).toInteger();
        const QString controllerId = payload.value(QStringLiteral("controllerId")).toString();
        if (!commandId.isEmpty() && (userId > 0 || !controllerId.isEmpty())) {
            QJsonObject resultPayload = result.toJson();
            resultPayload[QStringLiteral("commandId")] = commandId;
            resultPayload[QStringLiteral("serverTime")] = m_engine.simTime();
            const QString key = controllerId.isEmpty()
                ? commandCacheKey(userId, commandId)
                : commandCacheKey(controllerId, commandId);
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
    if (kind == QLatin1String("scenario")
        || kind == QLatin1String("scenarioRollback")) {
        const QJsonObject runtimeObject = payload.value(QStringLiteral("scenario")).toObject();
        const Scenario runtimeScenario = ScenarioIo::fromJson(runtimeObject);
        const qint64 revision = payload.value(QStringLiteral("scenarioRevision")).toInteger();
        Scenario initialScenario = runtimeScenario;
        if (kind == QLatin1String("scenarioRollback")) {
            const QJsonValue initialValue = payload.value(QStringLiteral("initialScenario"));
            // Rollback events written before initialScenario was added only
            // carried the runtime scenario. At replay time the checkpoint
            // already contains the previous initial scene, so preserve it.
            initialScenario = initialValue.isObject()
                ? ScenarioIo::fromJson(initialValue.toObject())
                : (m_runInitialScenario.units.empty() ? runtimeScenario : m_runInitialScenario);
        }
        QString applyError;
        if (revision <= 0 || !applyInitialScenarioState(initialScenario, runtimeScenario,
                                                        static_cast<quint64>(revision),
                                                        &applyError)) {
            if (error) *error = revision <= 0
                ? QStringLiteral("场景版本无效") : applyError;
            return false;
        }
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
            m_phase = QStringLiteral("paused");
            cancelAiPlanRequest();
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
            resetAiMatchState();
            ++m_scenarioRevision;
        } else {
            if (error) *error = QStringLiteral("未知控制事件: %1").arg(action);
            return false;
        }
        return true;
    }
    if (kind == QLatin1String("intelReport")) {
        const QDateTime received = QDateTime::fromString(
            payload.value(QStringLiteral("receivedAt")).toString(), Qt::ISODateWithMs);
        const IntelLedger::Result result = m_intelLedger.createManualReport(
            payload.value(QStringLiteral("seatId")).toString(),
            payload.value(QStringLiteral("type")).toString(),
            payload.value(QStringLiteral("title")).toString(),
            payload.value(QStringLiteral("position")).toObject(),
            payload.value(QStringLiteral("note")).toString(), received.isValid()
                ? received : QDateTime::currentDateTimeUtc());
        if (!result.ok) {
            if (error) *error = QStringLiteral("情报报告重放失败: %1").arg(result.code);
            return false;
        }
        const qint64 userId = payload.value(QStringLiteral("userId")).toInteger();
        const QString requestId = payload.value(QStringLiteral("requestId")).toString();
        if (userId > 0 && !requestId.isEmpty()) {
            cacheIntelCommandResult(userId, QStringLiteral("createIntelReport"), requestId,
                                    CommandResult::ok(QStringLiteral("人工情报报告已确认")));
        }
        return true;
    }
    if (kind == QLatin1String("intelShare")) {
        const QString senderSeat = payload.value(QStringLiteral("senderSeatId")).toString();
        const QDateTime received = QDateTime::fromString(
            payload.value(QStringLiteral("receivedAt")).toString(), Qt::ISODateWithMs);
        const AuthoritativeRoom::Seat sourceSeat = m_authoritativeRoom.seat(senderSeat);
        for (const QJsonValue& value : payload.value(QStringLiteral("recipientSeatIds")).toArray()) {
            const QString recipientSeat = value.toString();
            const AuthoritativeRoom::Seat targetSeat = m_authoritativeRoom.seat(recipientSeat);
            const UnitBase* sourceUnit = seatUnit(senderSeat);
            const UnitBase* targetUnit = seatUnit(recipientSeat);
            const bool reachable = sourceUnit && targetUnit
                && StateProjector::canTransmit(m_engine, sourceUnit->id(), targetUnit->id());
            const IntelLedger::Result result = m_intelLedger.share(
                senderSeat, recipientSeat, sourceSeat.side, targetSeat.side, reachable,
                payload.value(QStringLiteral("intelId")).toString(),
                payload.value(QStringLiteral("note")).toString(),
                received.isValid() ? received : QDateTime::currentDateTimeUtc());
            if (!result.ok) {
                if (error) *error = QStringLiteral("情报共享重放失败: %1").arg(result.code);
                return false;
            }
        }
        const qint64 userId = payload.value(QStringLiteral("senderUserId")).toInteger();
        const QString requestId = payload.value(QStringLiteral("requestId")).toString();
        if (userId > 0 && !requestId.isEmpty()) {
            cacheIntelCommandResult(userId, QStringLiteral("shareIntel"), requestId,
                                    CommandResult::ok(QStringLiteral("情报共享已确认")));
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
        SeatOccupant occupant;
        occupant.seatId = seat.seatId;
        occupant.seatType = seat.seatType;
        occupant.side = seat.side;
        occupant.userId = seat.userId;
        occupant.username = seat.username;
        occupant.controllerType = seat.controllerType;
        occupant.controllerId = seat.controllerId;
        occupant.ready = seat.ready;
        m_seats.insert(seat.seatId, occupant);
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
    QString parseError;
    Scenario scenario = ScenarioIo::fromJson(scenarioJson, &parseError);
    if (!parseError.isEmpty()) {
        if (error) *error = parseError;
        return false;
    }
    const auto applyRangeParameter = [error](const QJsonObject& parameters,
                                             const QString& name, double* target) {
        if (!parameters.contains(name) || !target) return true;
        const QJsonValue value = parameters.value(name);
        const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!value.isDouble() || !std::isfinite(number) || number < 0.0
            || number > 1'000'000.0) {
            if (error) *error = QStringLiteral("战位参数无效: %1").arg(name);
            return false;
        }
        *target = number;
        return true;
    };
    for (const AuthoritativeRoom::Seat& seat : m_authoritativeRoom.seats()) {
        auto unit = std::find_if(scenario.units.begin(), scenario.units.end(),
                                 [&seat](const ScenarioUnit& candidate) {
                                     return candidate.id == seat.unitId;
                                 });
        if (unit == scenario.units.end()) continue;
        QJsonObject parameters = m_seatParameters.value(seat.seatId);
        if (parameters.isEmpty()) parameters = m_seatParameters.value(seatParameterKey(seat.seatId));
        if (!applyRangeParameter(parameters, QStringLiteral("communicationRange"),
                                 &unit->commRange)
            || !applyRangeParameter(parameters, QStringLiteral("detectRange"),
                                    &unit->detectRange)) {
            return false;
        }
    }
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
    backup.runInitialScenario = m_runInitialScenario;
    backup.runtimeUnits = backup.scenario.units.empty()
        ? QJsonArray{} : m_engine.collectCheckpointState();
    backup.engineState = backup.scenario.units.empty()
        ? QJsonObject{} : m_engine.collectGlobalCheckpointState();
    backup.vmfState = addServerVmfDedupe(m_engine.collectVmfRuntimeState(),
                                         m_vmfMessageIds);
    backup.simTime = m_engine.simTime();
    backup.running = m_engine.running();
    backup.speed = m_engine.speedMul();
    backup.phase = m_phase;
    backup.roomStatus = m_roomStatus;
    backup.roomName = m_roomName;
    backup.roomDescription = m_roomDescription;
    backup.scenarioId = m_scenarioId;
    backup.seatLimits = m_seatLimits;
    backup.seatParameters = m_seatParameters;
    backup.configVersion = m_configVersion;
    backup.lastRoomUpdate = m_lastRoomUpdate;
    backup.redReady = m_redReady;
    backup.blueReady = m_blueReady;
    backup.mapMarks = m_mapMarks;
    backup.sharedIntel = m_sharedIntel;
    backup.intelLedger = m_intelLedger.toJson();
    backup.chatHistory = m_chatHistory;
    backup.chatSequence = m_chatSequence;
    backup.commandResults = m_commandResults;
    backup.commandResultOrder = m_commandResultOrder;
    backup.scenarioRevision = m_scenarioRevision;
    backup.stateRevision = m_stateRevision;
    backup.eventSequence = m_eventSequence;
    backup.matchGeneration = m_matchGeneration;
    backup.aiCommandSequence = m_aiCommandSequence;
    backup.aiPlanningGeneration = m_aiPlanningGeneration;
    backup.aiRngState = m_aiRngState;
    backup.aiNextDecisionAt = m_aiNextDecisionAt;
    backup.aiNextReplanAt = m_aiNextReplanAt;
    backup.aiPlan = m_aiPlan;
    backup.aiStickyRules = m_aiStickyRules;
    backup.aiConsecutiveFailures = m_aiConsecutiveFailures;
    backup.aiContactMemory = m_aiContactMemory;
    backup.aiStrategyPhase = m_aiStrategyPhase;
    backup.aiReplanReason = m_aiReplanReason;
    backup.aiNextPrivilegedSampleAt = m_aiNextPrivilegedSampleAt;
    backup.aiPrivilegedSampleSequence = m_aiPrivilegedSampleSequence;
    backup.aiSelectedProvider = m_aiSelectedProvider;
    backup.aiSelectedModel = m_aiSelectedModel;
    backup.aiResolvedModel = m_aiResolvedModel;
    backup.aiRoomConfigVersion = m_aiRoomConfigVersion;
    backup.aiOllamaConfigVersion = m_aiOllamaConfigVersion;
    backup.aiFallbackReason = m_aiFallbackReason;
    return backup;
}

bool GameServer::restoreRoomStateBackup(const RoomStateBackup& backup, QString* error) {
    if (error) error->clear();
    QString localError;
    if (!m_authoritativeRoom.restore(backup.authoritativeRoom, &localError)) {
        if (error) *error = localError;
        return false;
    }
    m_roomMode = m_authoritativeRoom.mode();
    if (!m_engine.setRemoteScenario(backup.scenario)) {
        if (error) *error = m_engine.lastError();
        return false;
    }
    if (backup.scenario.units.empty()) {
        m_engine.setRunning(false);
        m_engine.setSpeedMul(backup.speed);
    } else if (!m_engine.restoreCheckpointState(
                   backup.runtimeUnits, backup.engineState, backup.simTime,
                   backup.running, backup.speed, &localError)) {
        if (error) *error = localError;
        return false;
    }
    if (!backup.vmfState.isEmpty()) {
        if (!m_engine.restoreVmfRuntimeState(backup.vmfState, &localError)) {
            if (error) *error = localError;
            return false;
        }
        restoreServerVmfDedupe(backup.vmfState, &m_vmfMessageIds,
                               &m_vmfMessageIdOrder);
    }
    m_phase = backup.phase;
    m_roomStatus = backup.roomStatus;
    m_roomName = backup.roomName;
    m_roomDescription = backup.roomDescription;
    m_scenarioId = backup.scenarioId;
    m_seatLimits = backup.seatLimits;
    m_seatParameters = backup.seatParameters;
    m_configVersion = backup.configVersion;
    m_lastRoomUpdate = backup.lastRoomUpdate;
    m_runInitialScenario = backup.runInitialScenario;
    m_redReady = backup.redReady;
    m_blueReady = backup.blueReady;
    m_mapMarks = backup.mapMarks;
    m_sharedIntel = backup.sharedIntel;
    if (!m_intelLedger.restore(backup.intelLedger, &localError)) {
        if (error) *error = localError;
        return false;
    }
    m_chatHistory = backup.chatHistory;
    m_chatSequence = backup.chatSequence;
    m_commandResults = backup.commandResults;
    m_commandResultOrder = backup.commandResultOrder;
    m_scenarioRevision = backup.scenarioRevision;
    m_stateRevision = backup.stateRevision;
    m_eventSequence = backup.eventSequence;
    m_matchGeneration = backup.matchGeneration;
    m_aiCommandSequence = backup.aiCommandSequence;
    m_aiPlanningGeneration = backup.aiPlanningGeneration;
    m_aiRngState = backup.aiRngState;
    m_aiNextDecisionAt = backup.aiNextDecisionAt;
    m_aiNextReplanAt = backup.aiNextReplanAt;
    m_aiPlan = backup.aiPlan;
    m_aiStickyRules = backup.aiStickyRules;
    m_aiConsecutiveFailures = backup.aiConsecutiveFailures;
    m_aiContactMemory = backup.aiContactMemory;
    m_aiStrategyPhase = backup.aiStrategyPhase;
    m_aiReplanReason = backup.aiReplanReason;
    m_aiNextPrivilegedSampleAt = backup.aiNextPrivilegedSampleAt;
    m_aiPrivilegedSampleSequence = backup.aiPrivilegedSampleSequence;
    m_aiSelectedProvider = backup.aiSelectedProvider;
    m_aiSelectedModel = backup.aiSelectedModel;
    m_aiResolvedModel = backup.aiResolvedModel;
    m_aiRoomConfigVersion = backup.aiRoomConfigVersion;
    m_aiOllamaConfigVersion = backup.aiOllamaConfigVersion;
    m_aiFallbackReason = backup.aiFallbackReason;
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
        m_intelLedger.clear();
        m_observerTrajectories.clear();
        m_nextObserverTrajectorySampleAt = 0.0;
        m_chatHistory = {};
        m_chatSequence = 0;
        m_commandResults.clear();
        m_commandResultOrder.clear();
        resetAiMatchState();
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
        if (it->authenticated && !it->observer && it->roomId == m_roomId) return true;
    }

    const QString operationId = QStringLiteral("internal-empty-%1")
        .arg(m_authoritativeRoom.revision());
    if (!resetAuthoritativeRuntime(operationId, error)) return false;
    m_roomStatus = QStringLiteral("preparing");
    reportRoomStatus(QStringLiteral("preparing"), QStringLiteral("房间已无在线成员，已重置"));
    broadcastSnapshots(true);
    return true;
}

void GameServer::clearRoomOperationTracking() {
    m_lifecycleOperationInFlight.clear();
    m_lifecycleOperationAction.clear();
    m_lifecycleOperationRequestedRevision = 0;
    m_lifecycleOperationRequestedConfigVersion = 0;
    m_lifecycleOperationRequestedOllamaVersion = 0;
    m_lifecycleOperationAckState.clear();
    m_lifecycleOperationAckCode.clear();
    m_lifecycleOperationAckRevision = 0;
    m_lifecycleOperationAckInFlight = false;
}

void GameServer::processRoomOperation(const QJsonObject& operation) {
    if (operation.isEmpty()
        || operation.value(QStringLiteral("state")).toString() != QLatin1String("pending")) return;
    const QString operationId = operation.value(QStringLiteral("operationId")).toString();
    const QString action = operation.value(QStringLiteral("action")).toString();
    if (operationId.isEmpty()) return;
    const qint64 requested = operation.value(QStringLiteral("requestedRevision")).toInteger();
    const quint64 currentRevision = m_authoritativeRoom.revision();
    const qint64 requestedConfig = operation.value(QStringLiteral("requestedConfigVersion"))
                                       .toInteger();
    const qint64 requestedEndpoint = operation.value(
        QStringLiteral("requestedOllamaConfigVersion")).toInteger();

    const bool trackedOperation = m_lifecycleOperationInFlight == operationId;
    if (!m_lifecycleOperationInFlight.isEmpty() && !trackedOperation) {
        audit(QStringLiteral("lifecycle"),
              QJsonObject{{QStringLiteral("event"),
                           QStringLiteral("pendingOperationSuperseded")},
                          {QStringLiteral("operationId"),
                           m_lifecycleOperationInFlight},
                          {QStringLiteral("replacementOperationId"), operationId}});
        clearRoomOperationTracking();
    }
    if (trackedOperation && !m_lifecycleOperationAckState.isEmpty()) {
        acknowledgeRoomOperation(operationId, m_lifecycleOperationAckState,
                                 m_lifecycleOperationAckRevision,
                                 m_lifecycleOperationAckCode);
        return;
    }
    if (trackedOperation) {
        // Only an open operation can legitimately be waiting without a final
        // ACK: its model probe may have been started by the configuration sync.
        if (action != QLatin1String("open") || m_aiProbeInFlight
            || (m_ollamaProvider && m_ollamaProvider->inFlight())) {
            return;
        }
    } else {
        m_lifecycleOperationInFlight = operationId;
        m_lifecycleOperationAction = action;
        m_lifecycleOperationRequestedRevision = requested > 0
            ? static_cast<quint64>(requested) : 0;
        m_lifecycleOperationRequestedConfigVersion = requestedConfig > 0
            ? static_cast<quint64>(requestedConfig) : 0;
        m_lifecycleOperationRequestedOllamaVersion = requestedEndpoint > 0
            ? static_cast<quint64>(requestedEndpoint) : 0;
    }
    const auto fail = [this, operationId](const QString& code) {
        // The operation may have changed the authoritative room revision before
        // a validation/persistence failure is detected. ACK the revision that
        // is current at the point of failure rather than a stale entry snapshot.
        acknowledgeRoomOperation(operationId, QStringLiteral("failed"),
                                 m_authoritativeRoom.revision(), code);
    };
    if (requested > 0 && static_cast<quint64>(requested) > currentRevision) {
        fail(QStringLiteral("STALE_REVISION"));
        return;
    }
    if (requestedConfig > 0 && static_cast<quint64>(requestedConfig) != m_configVersion
        && action != QLatin1String("stop") && action != QLatin1String("force-stop")) {
        fail(QStringLiteral("STALE_CONFIG_VERSION"));
        return;
    }
    if (requestedEndpoint > 0
        && static_cast<quint64>(requestedEndpoint) != m_aiOllamaConfigVersion
        && action == QLatin1String("open")) {
        fail(QStringLiteral("STALE_AI_CONFIG_VERSION"));
        return;
    }

    if (action == QLatin1String("open")) {
        if (m_roomStatus != QLatin1String("stopped")
            && m_roomStatus != QLatin1String("finished")) {
            fail(QStringLiteral("INVALID_STATE"));
            return;
        }
        if (!m_aiConfigApplied) {
            m_roomStatus = QStringLiteral("stopped");
            fail(QStringLiteral("AI_CONFIGURATION_INVALID"));
            return;
        }
        if (m_aiSelectedProvider == QLatin1String("ollama")
            && m_aiConnectionStatus != QLatin1String("connected")) {
            if (m_aiProbeInFlight || m_ollamaProvider->inFlight()) {
                return;
            }
            m_aiProbeInFlight = true;
            m_aiConnectionStatus = QStringLiteral("checking");
            writeMonitorStatus();
            m_ollamaProvider->probe(m_matchGeneration,
                [this, operation, operationId](OllamaResult result) {
                    m_aiProbeInFlight = false;
                    if (m_lifecycleOperationInFlight != operationId
                        || !m_lifecycleOperationAckState.isEmpty()) {
                        return;
                    }
                    m_aiLastProbeAt = QDateTime::currentDateTimeUtc()
                                          .toString(Qt::ISODateWithMs);
                    if (!result.ok) {
                        m_aiConnectionStatus = result.failureClass.isEmpty()
                            ? QStringLiteral("unavailable") : result.failureClass;
                        m_aiProbeFailureClass = result.failureClass;
                        m_aiResolvedModel.clear();
                        m_aiFallbackReason = result.failureClass;
                        acknowledgeRoomOperation(
                            operationId, QStringLiteral("failed"),
                            m_authoritativeRoom.revision(),
                            result.failureClass.isEmpty()
                                ? QStringLiteral("AI_PROBE_FAILED") : result.failureClass);
                        m_roomStatus = QStringLiteral("stopped");
                        writeMonitorStatus();
                        broadcastSnapshots(true);
                        return;
                    }
                    m_aiConnectionStatus = QStringLiteral("connected");
                    m_aiProbeFailureClass.clear();
                    m_aiResolvedModel = m_ollamaProvider
                        ? (m_ollamaProvider->resolvedModel().isEmpty()
                               ? m_ollamaProvider->configuredModel()
                               : m_ollamaProvider->resolvedModel())
                        : result.resolvedModel;
                    processRoomOperation(operation);
                });
            return;
        }
        const RoomStateBackup backup = captureRoomState();
        QString runtimeError;
        if (!resetAuthoritativeRuntime(
                QStringLiteral("internal-open-%1").arg(operationId), &runtimeError)) {
            m_roomStatus = QStringLiteral("stopped");
            fail(QStringLiteral("RUNTIME_RESET_FAILED"));
            return;
        }
        m_phase = QStringLiteral("preparing");
        m_roomStatus = QStringLiteral("preparing");
        m_aiFallbackReason.clear();
        syncAuthoritativeSeats();
        if (!persistRoomState(&runtimeError)) {
            QString rollbackError;
            if (!restoreRoomStateBackup(backup, &rollbackError)) {
                audit(QStringLiteral("persistence"),
                      QJsonObject{{QStringLiteral("event"),
                                   QStringLiteral("lifecycleRollbackFailed")},
                                  {QStringLiteral("message"), rollbackError}});
            }
            m_roomStatus = QStringLiteral("stopped");
            persistRoomState(nullptr);
            fail(QStringLiteral("PERSISTENCE_FAILED"));
            return;
        }
        const quint64 revision = m_authoritativeRoom.revision();
        acknowledgeRoomOperation(operationId, QStringLiteral("acknowledged"), revision);
        broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("matchReset")},
                                   {QStringLiteral("message"), QStringLiteral("房间已锁定配置并进入准备阶段")}});
        broadcastSnapshots(true);
        return;
    }

    if (action == QLatin1String("start") || action == QLatin1String("resume")) {
        const QString expectedPhase = action == QLatin1String("start")
            ? QStringLiteral("preparing") : QStringLiteral("paused");
        if (m_phase != expectedPhase) {
            fail(QStringLiteral("INVALID_STATE"));
            return;
        }
        const RoomStateBackup backup = captureRoomState();
        const auto rollback = [this, &backup, &fail](const QString& code) {
            QString rollbackError;
            if (!restoreRoomStateBackup(backup, &rollbackError)) {
                audit(QStringLiteral("persistence"),
                      QJsonObject{{QStringLiteral("event"),
                                   QStringLiteral("lifecycleRollbackFailed")},
                                  {QStringLiteral("message"), rollbackError}});
            }
            fail(code);
        };
        if (m_roomMode == QLatin1String("pve")) {
            bool aiDeploymentRequired = false;
            for (const AuthoritativeRoom::Seat& seat : m_authoritativeRoom.seats()) {
                if (seat.controllerType == QLatin1String("ai")
                    && (!seat.deployed || !seat.ready)) {
                    aiDeploymentRequired = true;
                    break;
                }
            }
            if (aiDeploymentRequired) {
                const QJsonObject map = m_engine.mapInfo();
                const auto deployed = m_authoritativeRoom.deployAiSeats(
                    map.value(QStringLiteral("widthMeters")).toDouble(),
                    map.value(QStringLiteral("heightMeters")).toDouble(), m_matchGeneration);
                QString deploymentError;
                if (!deployed.ok || !applyDeployedScenario(&deploymentError)) {
                    rollback(deployed.ok ? QStringLiteral("AI_DEPLOYMENT_FAILED")
                                         : deployed.code);
                    return;
                }
                syncAuthoritativeSeats();
            }
        }
        if (!m_engine.readyForSim()) {
            rollback(QStringLiteral("ENGINE_NOT_READY"));
            return;
        }
        const auto started = m_authoritativeRoom.start();
        if (!started.ok) {
            rollback(started.code);
            return;
        }
        m_runInitialScenario = m_engine.scenario();
        m_phase = QStringLiteral("running");
        m_roomStatus = QStringLiteral("running");
        m_engine.setRunning(true);
        if (!persistRoomState()) {
            rollback(QStringLiteral("PERSISTENCE_FAILED"));
            return;
        }
        acknowledgeRoomOperation(operationId, QStringLiteral("acknowledged"),
                                 started.revision);
        broadcastEvent(QJsonObject{{QStringLiteral("kind"), QStringLiteral("matchStarted")},
                                   {QStringLiteral("message"), QStringLiteral("房间管理员已开启推演")}});
        broadcastSnapshots(true);
        return;
    }

    if (action == QLatin1String("pause")) {
        if (m_phase != QLatin1String("running")) {
            fail(QStringLiteral("INVALID_STATE"));
            return;
        }
        const RoomStateBackup backup = captureRoomState();
        const auto paused = m_authoritativeRoom.pause();
        if (!paused.ok) {
            fail(paused.code);
            return;
        }
        m_engine.setRunning(false);
        m_phase = QStringLiteral("paused");
        m_roomStatus = QStringLiteral("paused");
        cancelAiPlanRequest();
        if (!persistRoomState()) {
            QString rollbackError;
            if (!restoreRoomStateBackup(backup, &rollbackError)) {
                audit(QStringLiteral("persistence"),
                      QJsonObject{{QStringLiteral("event"),
                                   QStringLiteral("lifecycleRollbackFailed")},
                                  {QStringLiteral("message"), rollbackError}});
            }
            fail(QStringLiteral("PERSISTENCE_FAILED"));
            return;
        }
        acknowledgeRoomOperation(operationId, QStringLiteral("acknowledged"), paused.revision);
        broadcastSnapshots(true);
        return;
    }

    if (action == QLatin1String("finish")) {
        if (m_phase != QLatin1String("running")
            && m_phase != QLatin1String("preparing")
            && m_phase != QLatin1String("paused")) {
            fail(QStringLiteral("INVALID_STATE"));
            return;
        }
        const RoomStateBackup backup = captureRoomState();
        const auto finished = m_authoritativeRoom.finish(QStringLiteral("draw"));
        if (!finished.ok) {
            fail(finished.code);
            return;
        }
        m_engine.setRunning(false);
        m_phase = QStringLiteral("finished");
        m_roomStatus = QStringLiteral("finished");
        cancelAiPlanRequest();
        if (!persistRoomState()) {
            QString rollbackError;
            if (!restoreRoomStateBackup(backup, &rollbackError)) {
                audit(QStringLiteral("persistence"),
                      QJsonObject{{QStringLiteral("event"),
                                   QStringLiteral("lifecycleRollbackFailed")},
                                  {QStringLiteral("message"), rollbackError}});
            }
            fail(QStringLiteral("PERSISTENCE_FAILED"));
            return;
        }
        acknowledgeRoomOperation(operationId, QStringLiteral("acknowledged"), finished.revision);
        broadcastSnapshots(true);
        return;
    }

    if (action == QLatin1String("stop") || action == QLatin1String("force-stop")) {
        const RoomStateBackup backup = captureRoomState();
        QString runtimeError;
        if (!resetAuthoritativeRuntime(
                QStringLiteral("internal-stop-%1").arg(operationId), &runtimeError)) {
            fail(QStringLiteral("RUNTIME_RESET_FAILED"));
            return;
        }
        m_engine.setRunning(false);
        m_phase = QStringLiteral("preparing");
        m_roomStatus = QStringLiteral("stopped");
        if (!persistRoomState()) {
            QString rollbackError;
            if (!restoreRoomStateBackup(backup, &rollbackError)) {
                audit(QStringLiteral("persistence"),
                      QJsonObject{{QStringLiteral("event"),
                                   QStringLiteral("lifecycleRollbackFailed")},
                                  {QStringLiteral("message"), rollbackError}});
            } else {
                persistRoomState(nullptr);
            }
            fail(QStringLiteral("PERSISTENCE_FAILED"));
            return;
        }
        closeRoomSessions(QStringLiteral("房间已停止"));
        acknowledgeRoomOperation(operationId, QStringLiteral("acknowledged"),
                                 m_authoritativeRoom.revision());
        broadcastSnapshots(true);
        return;
    }

    if (action != QLatin1String("reset") && action != QLatin1String("redeploy")) return;
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
    m_observerTrajectories.clear();
    m_nextObserverTrajectorySampleAt = 0.0;
    resetAiMatchState();
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
    if (m_lifecycleOperationInFlight != operationId) {
        clearRoomOperationTracking();
        m_lifecycleOperationInFlight = operationId;
    }
    m_lifecycleOperationAckState = state;
    m_lifecycleOperationAckRevision = revision;
    m_lifecycleOperationAckCode = code;
    if (m_lifecycleOperationAckInFlight) return;
    m_lifecycleOperationAckInFlight = true;
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
                                            {QStringLiteral("code"), code},
                                            {QStringLiteral("ai_resolved_model"),
                                             m_aiResolvedModel}})
                     .toJson(QJsonDocument::Compact));
    QTimer::singleShot(2500, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, operationId, state, revision, code]() {
        const int statusCode = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError networkError = reply->error();
        const QByteArray responseBody = reply->readAll();
        reply->deleteLater();
        if (m_lifecycleOperationInFlight != operationId) return;
        m_lifecycleOperationAckInFlight = false;
        const bool accepted = networkError == QNetworkReply::NoError
            && statusCode >= 200 && statusCode < 300;
        if (!accepted) {
            audit(QStringLiteral("lifecycle"),
                  QJsonObject{{QStringLiteral("event"),
                               QStringLiteral("operationAckFailed")},
                              {QStringLiteral("operationId"), operationId},
                              {QStringLiteral("state"), state},
                              {QStringLiteral("revision"), static_cast<qint64>(revision)},
                              {QStringLiteral("code"), code},
                              {QStringLiteral("httpStatus"), statusCode},
                              {QStringLiteral("networkError"),
                               static_cast<int>(networkError)},
                              {QStringLiteral("response"),
                               QString::fromUtf8(responseBody).left(256)}});
            return;
        }
        clearRoomOperationTracking();
        QTimer::singleShot(0, this, [this]() { syncRoomControl(); });
    });
}

QList<AiSeatState> GameServer::aiSeatStates() const {
    QList<AiSeatState> states;
    QHash<QString, double> cruiseSpeeds;
    for (const ScenarioUnit& scenarioUnit : m_engine.scenario().units) {
        if (std::isfinite(scenarioUnit.speed) && scenarioUnit.speed > 0.0) {
            cruiseSpeeds.insert(scenarioUnit.id, scenarioUnit.speed);
        }
    }
    QStringList seatIds;
    for (const AuthoritativeRoom::Seat& seat : m_authoritativeRoom.seats()) {
        if (seat.controllerType == QLatin1String("ai") && seat.side == QLatin1String("blue")
            && seat.deployed) seatIds.append(seat.seatId);
    }
    std::sort(seatIds.begin(), seatIds.end());
    const AuthoritativeRoom::Seat commanderSeat =
        m_authoritativeRoom.seat(QStringLiteral("blue_commander"));
    UnitBase* commandPost = commanderSeat.unitId.isEmpty()
        ? nullptr : m_engine.unit(commanderSeat.unitId);
    for (const QString& seatId : seatIds) {
        const AuthoritativeRoom::Seat seat = m_authoritativeRoom.seat(seatId);
        UnitBase* unit = m_engine.unit(seat.unitId);
        if (!unit || !unit->alive() || !unit->movable()) continue;
        AiSeatState state;
        state.seatId = seatId;
        state.unitId = seat.unitId;
        state.kind = unit->kindStr();
        state.alive = unit->alive();
        state.movable = unit->movable();
        state.x = unit->pos().x;
        state.y = unit->pos().y;
        state.collisionRadius = unit->collisionRadius();
        state.collisionHalfHeight = unit->collisionHalfHeight();
        state.speed = unit->speed();
        state.commandedSpeed = unit->baseSpeed();
        state.cruiseSpeed = cruiseSpeeds.value(seat.unitId, state.commandedSpeed);
        state.maxCommandedSpeed = unit->maxCommandedSpeed();
        state.hpRatio = unit->maxHp() > 0.0
            ? std::clamp(unit->hp() / unit->maxHp(), 0.0, 1.0) : 0.0;
        state.sensorHealth = std::clamp(unit->sensorHealth(), 0.0, 1.0);
        state.commsHealth = std::clamp(unit->commsHealth(), 0.0, 1.0);
        state.mobilityHealth = std::clamp(unit->mobilityHealth(), 0.0, 1.0);
        state.weaponHealth = std::clamp(unit->weaponHealth(), 0.0, 1.0);
        state.detectRange = unit->detectRange();
        state.commRange = unit->commRange();
        state.fuelRemaining = unit->fuelRemaining();
        state.fuelCapacity = unit->fuelCapacity();
        state.fuelBurnRate = unit->fuelBurnRate();
        const double fuelEndurance = unit->estimatedFuelEndurance();
        state.estimatedFuelEndurance = std::isfinite(fuelEndurance)
            ? fuelEndurance : -1.0;
        state.lowestSubsystemHealth = std::min(
            {state.sensorHealth, state.commsHealth,
             state.mobilityHealth, state.weaponHealth});
        state.serviceRequested = unit->serviceRequested();
        state.serviceProgress = unit->serviceProgress();
        state.commandPostAlive = commandPost && commandPost->alive();
        if (commandPost) {
            state.commandPostX = commandPost->pos().x;
            state.commandPostY = commandPost->pos().y;
            state.serviceEligible = unit->kind() != UnitKind::CommandPost
                && state.commandPostAlive
                && unit->pos().distanceTo2D(commandPost->pos())
                    <= SimulationEngine::kServiceRadiusMeters;
        }
        const UnitBase::AbilityState& countermeasure = unit->countermeasureState();
        state.countermeasureSupported = countermeasure.supported();
        state.countermeasureAvailable = countermeasure.available();
        state.countermeasureRange = countermeasure.range;
        state.countermeasureCooldownRemaining = countermeasure.cooldownRemaining;
        state.countermeasureRemaining = countermeasure.remaining;
        state.countermeasureCapacity = countermeasure.capacity;
        const UnitBase::AbilityState& scan = unit->scanState();
        state.scanSupported = scan.supported();
        state.scanAvailable = scan.available();
        state.scanCooldownRemaining = scan.cooldownRemaining;
        state.repairCooldownRemaining = unit->repairCooldownRemaining();
        state.repairAvailable = state.lowestSubsystemHealth < 1.0 - 1e-9
            && state.repairCooldownRemaining <= 1e-9;
        state.communicationAvailable = commandPost && commandPost->alive()
            && StateProjector::canTransmit(m_engine, commandPost->id(), unit->id())
            && StateProjector::canTransmit(m_engine, unit->id(), commandPost->id());
        if (const auto* attack = qobject_cast<const AttackUAV*>(unit)) {
            state.ammoRemaining = attack->ammoRemaining();
            state.ammoCapacity = attack->ammoCapacity();
            state.cooldownRemaining = attack->cooldownRemaining();
            state.minimumAttackRange = attack->minimumAttackRange();
            state.optimalAttackRange = attack->optimalAttackRange();
            state.maximumAttackRange = attack->attackRange();
        }
        const QJsonObject projected = StateProjector::snapshotFor(
            m_engine, seatId, m_stateRevision, {}, {}, seat.unitId);
        for (const QJsonValue& value : projected.value(QStringLiteral("projectiles")).toArray()) {
            if (!value.isObject()) continue;
            const QJsonObject projectile = value.toObject();
            const QJsonArray position = projectile.value(QStringLiteral("position")).toArray();
            if (position.size() < 2) continue;
            AiObservedProjectile observedProjectile;
            observedProjectile.projectileId = projectile.value(QStringLiteral("id")).toString();
            observedProjectile.side = projectile.value(QStringLiteral("side")).toString();
            observedProjectile.targetId = projectile.value(QStringLiteral("targetId")).toString();
            observedProjectile.x = position.at(0).toDouble();
            observedProjectile.y = position.at(1).toDouble();
            observedProjectile.headingRad =
                projectile.value(QStringLiteral("headingRad")).toDouble();
            observedProjectile.speed = projectile.value(QStringLiteral("speed")).toDouble();
            observedProjectile.age = projectile.value(QStringLiteral("age")).toDouble();
            observedProjectile.lifetime = projectile.value(QStringLiteral("lifetime")).toDouble();
            observedProjectile.active = projectile.value(QStringLiteral("active")).toBool(true);
            const QString attackerId = projectile.value(QStringLiteral("attackerId")).toString();
            const QJsonObject attackerRuntime = m_engine.unitSnapshot(attackerId);
            const double averageDamage = (attackerRuntime.value(QStringLiteral("damageMin")).toDouble()
                                          + attackerRuntime.value(QStringLiteral("damageMax")).toDouble())
                * 0.5;
            observedProjectile.expectedDamage = averageDamage
                * attackerRuntime.value(QStringLiteral("hitProbability")).toDouble()
                * std::clamp(attackerRuntime.value(QStringLiteral("jamFactor")).toDouble(1.0)
                                 * attackerRuntime.value(QStringLiteral("subsystems")).toObject()
                                       .value(QStringLiteral("weapon")).toDouble(1.0),
                             0.0, 1.0);
            state.visibleProjectiles.append(observedProjectile);
        }
        const QSet<QString> visible = StateProjector::visibleUnitIds(
            m_engine, seatId, {}, seat.unitId);
        QStringList targets;
        for (const QString& targetId : visible) {
            UnitBase* target = m_engine.unit(targetId);
            if (!target || !target->alive() || target->sideStr() == QLatin1String("blue")) continue;
            targets.append(targetId);
        }
        std::sort(targets.begin(), targets.end(), [&unit, this](const QString& left,
                                                                 const QString& right) {
            const UnitBase* leftUnit = m_engine.unit(left);
            const UnitBase* rightUnit = m_engine.unit(right);
            const double leftDistance = leftUnit ? unit->pos().distanceTo2D(leftUnit->pos())
                                                  : std::numeric_limits<double>::max();
            const double rightDistance = rightUnit ? unit->pos().distanceTo2D(rightUnit->pos())
                                                    : std::numeric_limits<double>::max();
            if (std::abs(leftDistance - rightDistance) > 1e-9) {
                return leftDistance < rightDistance;
            }
            return left < right;
        });
        for (const QString& targetId : targets) {
            UnitBase* target = m_engine.unit(targetId);
            if (!target) continue;
            AiObservedTarget observed;
            observed.targetId = target->id();
            observed.targetKind = target->kindStr();
            observed.x = target->pos().x;
            observed.y = target->pos().y;
            observed.confidence = 1.0;
            observed.lastSeenAt = m_engine.simTime();
            observed.hp = target->hp();
            observed.maxHp = target->maxHp();
            observed.visible = true;
            observed.commandPost = isRedCommandPost(target);
            state.visibleTargets.append(observed);
        }
        if (!targets.isEmpty()) {
            UnitBase* target = m_engine.unit(targets.first());
            state.targetId = targets.first();
            state.targetVisible = target != nullptr;
            if (target) {
                state.targetX = target->pos().x;
                state.targetY = target->pos().y;
                state.targetKind = target->kindStr();
            }
        }
        states.append(state);
    }
    return states;
}

void GameServer::updateAiContactMemory(const QList<AiSeatState>& states, double now,
                                       const AiDifficultyParameters& parameters,
                                       double mapWidth, double mapHeight) {
    QSet<QString> visibleIds;
    for (const AiSeatState& state : states) {
        for (const AiObservedTarget& observed : state.visibleTargets) {
            if (observed.targetId.isEmpty()) continue;
            visibleIds.insert(observed.targetId);
            AiObservedTarget current = observed;
            const AiObservedTarget previous = m_aiContactMemory.value(observed.targetId);
            const double elapsed = now - previous.lastSeenAt;
            if (previous.targetId == current.targetId && elapsed > 0.05 && elapsed < 30.0
                && !previous.privileged) {
                current.velocityX = std::clamp((current.x - previous.x) / elapsed, -500.0, 500.0);
                current.velocityY = std::clamp((current.y - previous.y) / elapsed, -500.0, 500.0);
            }
            current.confidence = 1.0;
            current.lastSeenAt = now;
            current.visible = true;
            current.privileged = false;
            m_aiContactMemory.insert(current.targetId, current);
        }
    }
    for (auto it = m_aiContactMemory.begin(); it != m_aiContactMemory.end();) {
        if (visibleIds.contains(it.key())) {
            ++it;
            continue;
        }
        it->visible = false;
        it->confidence = std::max(0.05, it->confidence * 0.94);
        ++it;
    }

    // Hard mode receives sparse strategic reconnaissance only. Samples are
    // delayed, spatially noisy, capped in confidence and never marked visible;
    // the normal target-visibility gate therefore still blocks direct fire.
    if (m_aiDifficulty == QLatin1String("hard")
        && now + 1e-9 >= m_aiNextPrivilegedSampleAt) {
        m_aiNextPrivilegedSampleAt = now + 12.0;
        ++m_aiPrivilegedSampleSequence;
        const double errorRadius = std::max(400.0, std::min(mapWidth, mapHeight) * 0.03);
        for (const ScenarioUnit& scenarioUnit : m_engine.scenario().units) {
            if (scenarioUnit.side != QLatin1String("red")) continue;
            const UnitBase* target = m_engine.unit(scenarioUnit.id);
            if (!target || !target->alive()) continue;
            const double angle = static_cast<double>(RulesAi::nextRandom(&m_aiRngState)
                                                     % 6283ULL) / 1000.0;
            const double radius = errorRadius * (0.7
                + static_cast<double>(RulesAi::nextRandom(&m_aiRngState) % 600ULL) / 1000.0);
            AiObservedTarget contact;
            contact.targetId = target->id();
            contact.targetKind = target->kindStr();
            contact.x = std::clamp(target->pos().x + std::cos(angle) * radius,
                                   0.0, std::max(0.0, mapWidth));
            contact.y = std::clamp(target->pos().y + std::sin(angle) * radius,
                                   0.0, std::max(0.0, mapHeight));
            contact.confidence = 0.65;
            contact.lastSeenAt = std::max(0.0, now - 5.0);
            contact.visible = false;
            contact.privileged = true;
            contact.commandPost = isRedCommandPost(target);
            if (!visibleIds.contains(contact.targetId)) {
                m_aiContactMemory.insert(contact.targetId, contact);
            }
        }
    } else if (m_aiDifficulty != QLatin1String("hard")) {
        for (auto it = m_aiContactMemory.begin(); it != m_aiContactMemory.end();) {
            if (it->privileged) it = m_aiContactMemory.erase(it);
            else ++it;
        }
        m_aiNextPrivilegedSampleAt = 0.0;
    }

    const double memoryLimit = std::max(8, parameters.contactMemorySeconds) + 5.0;
    for (auto it = m_aiContactMemory.begin(); it != m_aiContactMemory.end();) {
        if (now - it->lastSeenAt > memoryLimit || !std::isfinite(it->x)
            || !std::isfinite(it->y)) {
            it = m_aiContactMemory.erase(it);
        } else {
            ++it;
        }
    }
}

AiKnowledgeState GameServer::buildAiKnowledge(const QList<AiSeatState>& states, double now,
                                              const AiDifficultyParameters& parameters) const {
    AiKnowledgeState knowledge;
    knowledge.seats = states;
    knowledge.now = now;
    const QJsonObject map = m_engine.mapInfo();
    knowledge.mapWidth = map.value(QStringLiteral("widthMeters")).toDouble(20000.0);
    knowledge.mapHeight = map.value(QStringLiteral("heightMeters")).toDouble(15000.0);
    knowledge.phase = m_aiStrategyPhase;
    knowledge.commandPostAlive = false;
    const AuthoritativeRoom::Seat commander =
        m_authoritativeRoom.seat(QStringLiteral("blue_commander"));
    const UnitBase* commandPost = commander.unitId.isEmpty()
        ? nullptr : m_engine.unit(commander.unitId);
    if (commandPost && commandPost->alive()) knowledge.commandPostAlive = true;

    QStringList contactIds = m_aiContactMemory.keys();
    contactIds.sort();
    for (const QString& contactId : contactIds) {
        const AiObservedTarget contact = m_aiContactMemory.value(contactId);
        if (now - contact.lastSeenAt <= std::max(8, parameters.contactMemorySeconds)
            && contact.confidence > 0.05) {
            knowledge.contacts.append(contact);
        }
    }
    if (commandPost && commandPost->alive()) {
        for (const AiObservedTarget& contact : knowledge.contacts) {
            if (contact.targetId == commandPost->id() || contact.commandPost
                || contact.targetKind == QLatin1String("commandpost")) {
                continue;
            }
            if (planarDistance2(commandPost->pos().x, commandPost->pos().y,
                                contact.x, contact.y) < 4000.0 * 4000.0) {
                knowledge.commandPostThreat = true;
                break;
            }
        }
        if (!knowledge.commandPostThreat) {
            for (const AiSeatState& state : states) {
                const bool projectedIncoming = std::any_of(
                    state.visibleProjectiles.cbegin(), state.visibleProjectiles.cend(),
                    [commandPost](const AiObservedProjectile& projectile) {
                        return projectile.active
                            && projectile.side == QLatin1String("red")
                            && projectile.targetId == commandPost->id();
                    });
                if (projectedIncoming) {
                    knowledge.commandPostThreat = true;
                    break;
                }
            }
        }
    }
    if (!knowledge.commandPostAlive) knowledge.phase = QStringLiteral("regroup");
    else if (knowledge.commandPostThreat) knowledge.phase = QStringLiteral("defend_cp");
    else if (knowledge.contacts.isEmpty()) knowledge.phase = QStringLiteral("recon");
    else if (std::any_of(knowledge.contacts.cbegin(), knowledge.contacts.cend(),
                         [](const AiObservedTarget& contact) { return contact.commandPost; })) {
        knowledge.phase = QStringLiteral("strike");
    } else if (now < 120.0) {
        knowledge.phase = QStringLiteral("shape");
    } else {
        knowledge.phase = QStringLiteral("exploit");
    }
    return knowledge;
}

bool GameServer::executeAiCommand(const AiCommand& command) {
    if (m_phase != QLatin1String("running") || m_roomMode != QLatin1String("pve")) return false;
    const AuthoritativeRoom::Seat seat = m_authoritativeRoom.seat(command.seatId);
    if (seat.seatId.isEmpty() || seat.controllerType != QLatin1String("ai")) return false;
    const QString commandId = QStringLiteral("ai:%1:%2:%3")
        .arg(m_matchGeneration).arg(command.seatId).arg(++m_aiCommandSequence);
    const QString cacheKey = commandCacheKey(seat.controllerId, commandId);
    if (m_commandResults.contains(cacheKey)) return true;
    ClientSession session;
    session.authenticated = true;
    session.roomId = m_roomId;
    session.seatId = seat.seatId;
    session.seatType = seat.seatType;
    session.side = seat.side;
    session.role = seat.seatId;
    session.username = QStringLiteral("AI");
    QString code;
    QString reason;
    if (!validateCommandOwnership(session, command.action, command.args, &code, &reason)) {
        ++m_aiCommandRejected;
        audit(QStringLiteral("ai"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("commandRejected")},
                          {QStringLiteral("controllerId"), seat.controllerId},
                          {QStringLiteral("commandId"), commandId},
                          {QStringLiteral("action"), command.action},
                          {QStringLiteral("code"), code}});
        return false;
    }
    const QJsonObject args = QJsonObject::fromVariantMap(command.args);
    QString persistenceError;
    if (!recordDurableEvent(QStringLiteral("command"),
                            QJsonObject{{QStringLiteral("commandId"), commandId},
                                        {QStringLiteral("controllerType"), QStringLiteral("ai")},
                                        {QStringLiteral("controllerId"), seat.controllerId},
                                        {QStringLiteral("seatId"), seat.seatId},
                                        {QStringLiteral("action"), command.action},
                                        {QStringLiteral("args"), args}},
                            &persistenceError)) {
        ++m_aiCommandRejected;
        audit(QStringLiteral("ai"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("persistenceFailed")},
                          {QStringLiteral("controllerId"), seat.controllerId}});
        return false;
    }
    const CommandResult result = m_engine.executeCommand(command.action, command.args);
    QJsonObject resultPayload = result.toJson();
    resultPayload[QStringLiteral("commandId")] = commandId;
    resultPayload[QStringLiteral("serverTime")] = m_engine.simTime();
    m_commandResultOrder.append(cacheKey);
    m_commandResults.insert(cacheKey, resultPayload);
    while (m_commandResultOrder.size() > 2048) {
        m_commandResults.remove(m_commandResultOrder.takeFirst());
    }
    audit(QStringLiteral("ai"),
          QJsonObject{{QStringLiteral("event"), QStringLiteral("commandResult")},
                      {QStringLiteral("controllerId"), seat.controllerId},
                      {QStringLiteral("commandId"), commandId},
                      {QStringLiteral("action"), command.action},
                      {QStringLiteral("accepted"), result.accepted},
                      {QStringLiteral("code"), result.code}});
    if (result.accepted) {
        ++m_aiCommandAccepted;
        if (command.action == QLatin1String("withdraw")) ++m_aiResourceWithdrawals;
    } else {
        ++m_aiCommandRejected;
    }
    return result.accepted;
}

void GameServer::cancelAiPlanRequest() {
    if (m_ollamaProvider) {
        const std::optional<OllamaResult> cancelled = m_ollamaProvider->cancel();
        if (cancelled.has_value()) {
            recordAiConversation(cancelled.value(), m_aiPlanRequestPlanningGeneration,
                                 QStringLiteral("cancelled"), m_aiPlan,
                                 QStringLiteral("request cancelled"));
        }
    }
    m_aiProbeInFlight = false;
    m_aiPlanRequestInFlight = false;
    m_aiPlanRequestGeneration = 0;
    m_aiPlanRequestPlanningGeneration = 0;
}

void GameServer::recordAiConversation(const OllamaResult& result,
                                      quint64 planningGeneration,
                                      const QString& status,
                                      const AiPlanV1& finalPlan,
                                      const QString& fallbackReason) {
    if (result.requestId.isEmpty()) return;
    OllamaConversationRecord record;
    record.conversationId = result.requestId;
    record.requestId = result.requestId;
    record.roomId = m_roomId;
    record.generation = planningGeneration;
    record.time = QDateTime::currentDateTimeUtc();
    record.configuredModel = result.configuredModel;
    record.resolvedModel = result.resolvedModel;
    record.model = result.resolvedModel.isEmpty() ? result.configuredModel
                                                  : result.resolvedModel;
    record.status = status;
    record.failure = result.failureClass;
    record.latencyMs = std::max<qint64>(0, result.latencyMs);
    record.messages = result.messages;
    record.raw = result.rawResponse;
    record.parsed = result.parsedPlan;
    const QString engine = status == QLatin1String("completed")
        ? QStringLiteral("ollama") : QStringLiteral("rules");
    record.final = QJsonObject{{QStringLiteral("engine"), engine},
                               {QStringLiteral("plan"), finalPlan.toJson()}};
    if (!fallbackReason.isEmpty()) {
        record.fallback = QJsonObject{{QStringLiteral("engine"), QStringLiteral("rules")},
                                      {QStringLiteral("reason"), fallbackReason},
                                      {QStringLiteral("plan"), finalPlan.toJson()}};
    }
    QString error;
    if (!m_aiConversationStore.appendFinalRecord(record, &error)) {
        qWarning() << "AI conversation record write failed:" << error;
        audit(QStringLiteral("ai"),
              QJsonObject{{QStringLiteral("event"),
                           QStringLiteral("conversationRecordFailed")},
                          {QStringLiteral("requestId"), result.requestId},
                          {QStringLiteral("message"), error}});
    }
}

void GameServer::applyAiConfiguration(const QJsonObject& config) {
    const qint64 versionValue = config.value(QStringLiteral("roomConfigVersion")).toInteger(
        config.value(QStringLiteral("configVersion")).toInteger());
    if (versionValue <= 0 || !m_ollamaProvider) return;
    const quint64 version = static_cast<quint64>(versionValue);
    const QString selectedProvider = config.value(QStringLiteral("selectedProvider"))
                                         .toString(config.value(QStringLiteral("provider"))
                                                       .toString())
                                         .trimmed().toLower();
    const QString selectedModel = config.value(QStringLiteral("selectedModel"))
                                      .toString(config.value(QStringLiteral("model")).toString())
                                      .trimmed();
    const quint64 endpointVersion = qMax<qint64>(
        1, config.value(QStringLiteral("ollamaConfigVersion")).toInteger(
               config.value(QStringLiteral("configVersion")).toInteger(1)));
    const bool sameSelection = selectedProvider == m_aiSelectedProvider
        && selectedModel == m_aiSelectedModel
        && version == m_aiRoomConfigVersion
        && endpointVersion == m_aiOllamaConfigVersion;
    // Room AI is frozen once preparation has begun.  The account service
    // rejects edits too, but this guard protects the authority if an internal
    // caller or a stale response attempts a hot update.
    if (m_roomStatus != QLatin1String("stopped") && !sameSelection) {
        audit(QStringLiteral("security"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("activeAiConfigurationChangeRejected")},
                          {QStringLiteral("phase"), m_phase},
                          {QStringLiteral("roomConfigVersion"), static_cast<qint64>(version)}});
        return;
    }
    OllamaConfig next;
    QString configurationError;
    if (!aiConfigurationFromJson(config, m_ollamaProvider->config(), &next,
                                 &configurationError)) {
        if (version == m_aiConfigVersion && !m_aiConfigApplied) return;
        m_aiConfigVersion = version;
        m_aiConfigApplied = false;
        m_aiProviderMode = selectedProvider;
        m_aiSelectedProvider = selectedProvider == QLatin1String("ollama")
            ? QStringLiteral("ollama") : QStringLiteral("rules");
        m_aiSelectedModel = selectedModel;
        m_aiRoomConfigVersion = version;
        m_aiOllamaConfigVersion = endpointVersion;
        m_aiConnectionStatus = QStringLiteral("configuration_error");
        m_aiProbeFailureClass = QStringLiteral("configuration_error");
        audit(QStringLiteral("ai"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("configurationRejected")},
                          {QStringLiteral("configVersion"), static_cast<qint64>(version)},
                          {QStringLiteral("message"), configurationError}});
        writeMonitorStatus();
        return;
    }
    const bool changed = version != m_aiRoomConfigVersion
        || endpointVersion != m_aiOllamaConfigVersion
        || selectedProvider != m_aiSelectedProvider
        || selectedModel != m_aiSelectedModel
        || next.deployment != m_ollamaProvider->deployment()
        || next.baseUrl != m_ollamaProvider->baseUrl()
        || next.model != m_ollamaProvider->model();
    if (!changed && m_aiConfigApplied) return;
    m_aiConfigVersion = version;
    m_aiRoomConfigVersion = version;
    m_aiOllamaConfigVersion = endpointVersion;
    m_aiSelectedProvider = selectedProvider == QLatin1String("ollama")
        ? QStringLiteral("ollama") : QStringLiteral("rules");
    m_aiSelectedModel = selectedModel;
    m_aiResolvedModel.clear();
    m_aiFallbackReason.clear();
    m_aiConfigApplied = true;
    cancelAiPlanRequest();
    m_ollamaProvider->reconfigure(next);
    m_aiProviderMode = next.deployment;
    m_aiConnectionStatus = next.deployment == QLatin1String("rules")
        ? QStringLiteral("disabled") : QStringLiteral("checking");
    m_aiProbeFailureClass.clear();
    m_aiLastProbeAt.clear();
    m_aiStickyRules = false;
    m_aiConsecutiveFailures = 0;
    m_aiPlan = {};
    m_aiNextReplanAt = 0.0;
    audit(QStringLiteral("ai"),
          QJsonObject{{QStringLiteral("event"), QStringLiteral("configurationApplied")},
                      {QStringLiteral("provider"), next.deployment},
                      {QStringLiteral("model"), next.model},
                      {QStringLiteral("configVersion"), static_cast<qint64>(version)}});
    if (next.deployment != QLatin1String("rules")) probeAiProvider();
    else writeMonitorStatus();
}

void GameServer::probeAiProvider() {
    if (!m_ollamaProvider || m_aiProviderMode == QLatin1String("rules")) {
        m_aiConnectionStatus = QStringLiteral("disabled");
        m_aiProbeFailureClass.clear();
        writeMonitorStatus();
        return;
    }
    if (m_aiProbeInFlight || m_aiPlanRequestInFlight || m_ollamaProvider->inFlight()) return;
    m_aiProbeInFlight = true;
    m_aiConnectionStatus = QStringLiteral("checking");
    m_aiProbeFailureClass.clear();
    writeMonitorStatus();
    m_ollamaProvider->probe(m_matchGeneration, [this](OllamaResult result) {
        m_aiProbeInFlight = false;
        m_aiLastProbeAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        if (result.ok) {
            m_aiConnectionStatus = QStringLiteral("connected");
            m_aiProbeFailureClass.clear();
            m_aiResolvedModel = m_ollamaProvider
                ? (m_ollamaProvider->resolvedModel().isEmpty()
                       ? m_ollamaProvider->configuredModel()
                       : m_ollamaProvider->resolvedModel())
                : QString();
        } else {
            m_aiConnectionStatus = result.failureClass.isEmpty()
                ? QStringLiteral("unavailable") : result.failureClass;
            m_aiProbeFailureClass = result.failureClass;
            m_aiResolvedModel.clear();
        }
        writeMonitorStatus();
    });
}

void GameServer::resetAiMatchState() {
    cancelAiPlanRequest();
    ++m_matchGeneration;
    m_aiCommandSequence = 0;
    m_aiPlanningGeneration = 0;
    m_aiRngState = 0xA17A11ULL ^ m_matchGeneration;
    m_aiNextDecisionAt = 0.0;
    m_aiNextReplanAt = 0.0;
    m_aiPlan = {};
    m_aiStickyRules = false;
    m_aiConsecutiveFailures = 0;
    m_aiEffectiveEngine = QStringLiteral("rules");
    m_aiLastFailureClass.clear();
    m_aiContactMemory.clear();
    m_aiStrategyPhase = QStringLiteral("recon");
    m_aiReplanReason = QStringLiteral("match_start");
    m_aiNextPrivilegedSampleAt = 0.0;
    m_aiPrivilegedSampleSequence = 0;
}

void GameServer::handleAiPlanResult(const AiPlanRequestContext& context,
                                    OllamaResult result) {
    const bool trackedRequest = m_aiPlanRequestInFlight
        && context.matchGeneration == m_aiPlanRequestGeneration
        && context.planningGeneration == m_aiPlanRequestPlanningGeneration;
    if (!trackedRequest) {
        audit(QStringLiteral("ai"),
              QJsonObject{{QStringLiteral("event"),
                           QStringLiteral("staleProviderResponseDiscarded")}});
        return;
    }
    m_aiPlanRequestInFlight = false;
    m_aiPlanRequestGeneration = 0;
    m_aiPlanRequestPlanningGeneration = 0;
    if (context.matchGeneration != m_matchGeneration) {
        result.ok = false;
        result.failureClass = QStringLiteral("stale_response");
        recordAiConversation(result, context.planningGeneration,
                             QStringLiteral("rejected"), m_aiPlan,
                             QStringLiteral("stale response discarded"));
        audit(QStringLiteral("ai"),
              QJsonObject{{QStringLiteral("event"),
                           QStringLiteral("staleProviderResponseDiscarded")}});
        return;
    }
    m_aiLastLatencyMs = result.latencyMs;
    if (result.latencyMs > 0) {
        const quint64 samples = m_aiProviderSuccesses + m_aiProviderFailures;
        m_aiAverageLatencyMs = static_cast<qint64>(
            (m_aiAverageLatencyMs * samples + result.latencyMs) / (samples + 1));
    }
    if (result.ok) {
        const QList<AiSeatState> latestStates = aiSeatStates();
        QHash<QString, AiSeatState> latestBySeat;
        for (const AiSeatState& state : latestStates) latestBySeat.insert(state.seatId, state);
        const QJsonObject map = m_engine.mapInfo();
        const double mapWidth = map.value(QStringLiteral("widthMeters")).toDouble();
        const double mapHeight = map.value(QStringLiteral("heightMeters")).toDouble();
        const AiKnowledgeState currentKnowledge = buildAiKnowledge(
            latestStates, m_engine.simTime(), RulesAi::parameters(m_aiDifficulty));
        bool valid = result.plan.sourceStateRevision > 0
            && result.plan.sourceStateRevision == context.sourceStateRevision
            && result.plan.matchGeneration == context.matchGeneration
            && result.plan.planningGeneration == context.planningGeneration;
        QSet<QString> objectiveSeats;
        if (valid) {
            for (const AiObjectiveV1& objective : result.plan.objectives) {
                const auto state = latestBySeat.constFind(objective.seatId);
                if (state == latestBySeat.cend()
                    || !state->alive || !state->movable
                    || objectiveSeats.contains(objective.seatId)
                    || objective.validUntil + 1e-9 < m_engine.simTime()) {
                    valid = false;
                    break;
                }
                if (state->kind == QLatin1String("commandpost")
                    && (objective.action != QLatin1String("relocate")
                        || !currentKnowledge.commandPostThreat)) {
                    valid = false;
                    break;
                }
                objectiveSeats.insert(objective.seatId);
                const bool movement = objective.action == QLatin1String("search")
                    || objective.action == QLatin1String("patrol")
                    || objective.action == QLatin1String("guard")
                    || objective.action == QLatin1String("jam")
                    || objective.action == QLatin1String("relocate");
                if (objective.action == QLatin1String("attack")
                    && state->kind != QLatin1String("attackuav")) {
                    valid = false;
                    break;
                }
                if (objective.action == QLatin1String("relocate")
                    && state->kind != QLatin1String("commandpost")) {
                    valid = false;
                    break;
                }
                if (movement && objective.region.isEmpty()) {
                    valid = false;
                    break;
                }
                if (objective.action == QLatin1String("attack")
                    && objective.targetId.isEmpty()) {
                    valid = false;
                    break;
                }
                if (!objective.targetId.isEmpty()) {
                    const QSet<QString> visible = StateProjector::visibleUnitIds(
                        m_engine, objective.seatId, {}, state->unitId);
                    if (!visible.contains(objective.targetId)) {
                        valid = false;
                        break;
                    }
                }
                if (!objective.region.isEmpty()) {
                    const double x = objective.region.value(QStringLiteral("x"))
                                         .toDouble(std::numeric_limits<double>::quiet_NaN());
                    const double y = objective.region.value(QStringLiteral("y"))
                                         .toDouble(std::numeric_limits<double>::quiet_NaN());
                    if (!std::isfinite(x) || !std::isfinite(y)
                        || x < 0.0 || y < 0.0 || x > mapWidth || y > mapHeight) {
                        valid = false;
                        break;
                    }
                    if (movement) {
                        const double dx = x - state->x;
                        const double dy = y - state->y;
                        const double minimumDisplacement = std::max(
                            50.0, std::min(mapWidth, mapHeight) * 0.01);
                        if (dx * dx + dy * dy < minimumDisplacement * minimumDisplacement) {
                            valid = false;
                            break;
                        }
                    }
                }
            }
        }
        if (valid) {
            const bool rebasedAfterRulesReplan =
                context.planningGeneration != m_aiPlanningGeneration;
            result.plan.sourceStateRevision = m_stateRevision;
            m_aiPlan = result.plan;
            double earliestPlanExpiry = std::numeric_limits<double>::infinity();
            for (const AiObjectiveV1& objective : m_aiPlan.objectives) {
                earliestPlanExpiry = std::min(earliestPlanExpiry, objective.validUntil);
            }
            if (std::isfinite(earliestPlanExpiry)) {
                m_aiNextReplanAt = std::min(m_aiNextReplanAt, earliestPlanExpiry);
            }
            m_aiEffectiveEngine = QStringLiteral("ollama");
            m_aiLastFailureClass.clear();
            m_aiFallbackReason.clear();
            m_aiConsecutiveFailures = 0;
            ++m_aiProviderSuccesses;
            recordAiConversation(result, context.planningGeneration,
                                 QStringLiteral("completed"), m_aiPlan);
            if (rebasedAfterRulesReplan) {
                audit(QStringLiteral("ai"),
                      QJsonObject{{QStringLiteral("event"),
                                   QStringLiteral("providerPlanRebased")},
                                  {QStringLiteral("requestPlanningGeneration"),
                                   static_cast<qint64>(context.planningGeneration)},
                                  {QStringLiteral("currentPlanningGeneration"),
                                   static_cast<qint64>(m_aiPlanningGeneration)}});
            }
            return;
        }
        result.ok = false;
        result.failureClass = QStringLiteral("stale_response");
    }
    ++m_aiProviderFailures;
    ++m_aiConsecutiveFailures;
    m_aiEffectiveEngine = QStringLiteral("rules");
    m_aiLastFailureClass = result.failureClass;
    if (m_aiConsecutiveFailures >= 2) {
        m_aiStickyRules = true;
        m_aiFallbackReason = result.failureClass.isEmpty()
            ? QStringLiteral("连续两次规划失败") : result.failureClass;
    }
    recordAiConversation(result, context.planningGeneration,
                         aiConversationStatus(result), m_aiPlan,
                         result.failureClass.isEmpty()
                             ? QStringLiteral("provider failure") : result.failureClass);
    audit(QStringLiteral("ai"),
          QJsonObject{{QStringLiteral("event"), QStringLiteral("providerFailure")},
                      {QStringLiteral("failureClass"), result.failureClass},
                      {QStringLiteral("consecutiveFailures"), m_aiConsecutiveFailures},
                      {QStringLiteral("stickyRules"), m_aiStickyRules}});
}

void GameServer::runAiDecision() {
    if (m_roomMode != QLatin1String("pve") || m_phase != QLatin1String("running")) return;
    const AiDifficultyParameters parameters = RulesAi::parameters(m_aiDifficulty);
    const double now = m_engine.simTime();
    if (now + 1e-9 < m_aiNextDecisionAt) return;
    const QList<AiSeatState> states = aiSeatStates();
    if (states.isEmpty()) return;
    const QJsonObject map = m_engine.mapInfo();
    const double mapWidth = map.value(QStringLiteral("widthMeters")).toDouble();
    const double mapHeight = map.value(QStringLiteral("heightMeters")).toDouble();
    updateAiContactMemory(states, now, parameters, mapWidth, mapHeight);
    const AiKnowledgeState knowledge = buildAiKnowledge(states, now, parameters);
    const bool replanDue = now + 1e-9 >= m_aiNextReplanAt || m_aiPlan.objectives.isEmpty();
    if (replanDue) {
        if (knowledge.commandPostThreat) m_aiReplanReason = QStringLiteral("command_post_threat");
        else if (m_aiPlan.objectives.isEmpty()) m_aiReplanReason = QStringLiteral("initial_plan");
        else if (knowledge.contacts.isEmpty()) m_aiReplanReason = QStringLiteral("contact_search");
        else m_aiReplanReason = QStringLiteral("scheduled_replan");
        m_aiStrategyPhase = knowledge.phase;
    }
    if (replanDue) {
        ++m_aiPlanningGeneration;
        const QString requestId = QStringLiteral("ai-plan:%1:%2")
            .arg(m_matchGeneration).arg(m_aiPlanningGeneration);
        QElapsedTimer plannerTimer;
        plannerTimer.start();
        m_aiPlan = RulesAi::makeStrategicPlan(
            knowledge, requestId, m_matchGeneration, m_stateRevision,
            now + parameters.enhancedReplanIntervalMs / 1000.0, &m_aiRngState,
            parameters, m_aiPlanningGeneration);
        m_aiStrategyPlannerLatencyMs = plannerTimer.elapsed();
        // The rules plan is immediately executable while an optional provider
        // request is in flight. A successful provider result flips this back.
        m_aiEffectiveEngine = QStringLiteral("rules");
        m_aiNextReplanAt = now + parameters.enhancedReplanIntervalMs / 1000.0;
        if (m_ollamaProvider && !m_aiStickyRules
            && m_aiProviderMode != QLatin1String("rules") && !m_aiPlanRequestInFlight) {
            const QJsonObject commanderSnapshot = StateProjector::snapshotFor(
                m_engine, QStringLiteral("blue_commander"), m_stateRevision,
                QJsonObject{}, {}, QStringLiteral("blue_cp"));
            const QSet<QString> commanderVisible = StateProjector::visibleUnitIds(
                m_engine, QStringLiteral("blue_commander"), {}, QStringLiteral("blue_cp"));
            const QJsonObject projection{
                {QStringLiteral("simTime"), now},
                {QStringLiteral("stateRevision"), static_cast<qint64>(m_stateRevision)},
                {QStringLiteral("units"), commanderSnapshot.value(QStringLiteral("units"))},
                {QStringLiteral("projectiles"),
                 commanderSnapshot.value(QStringLiteral("projectiles"))}};
            const quint64 requestGeneration = m_matchGeneration;
            const quint64 requestPlanningGeneration = m_aiPlanningGeneration;
            const quint64 requestSourceRevision = m_stateRevision;
            const AiPlanRequestContext context{
                requestGeneration, requestPlanningGeneration, requestSourceRevision};
            m_aiPlanRequestInFlight = true;
            m_aiPlanRequestGeneration = requestGeneration;
            m_aiPlanRequestPlanningGeneration = requestPlanningGeneration;
            ++m_aiProviderRequests;
            OllamaPlanRequest request;
            request.projection = projection;
            request.requestId = requestId;
            request.matchGeneration = requestGeneration;
            request.sourceStateRevision = requestSourceRevision;
            request.planningGeneration = requestPlanningGeneration;
            request.validUntil = std::max(
                m_aiNextReplanAt,
                now + static_cast<double>(m_ollamaProvider->config().totalTimeoutMs
                                           + kAiProviderPlanGraceMs) / 1000.0);
            request.mapWidth = map.value(QStringLiteral("widthMeters")).toDouble();
            request.mapHeight = map.value(QStringLiteral("heightMeters")).toDouble();
            for (const AiSeatState& state : states) {
                if (!state.alive || !state.movable
                    || !state.seatId.startsWith(QLatin1String("blue_"))) {
                    continue;
                }
                QSet<QString> visibleTargets = StateProjector::visibleUnitIds(
                    m_engine, state.seatId, {}, state.unitId);
                visibleTargets.intersect(commanderVisible);
                QStringList targetIds;
                for (const QString& targetId : visibleTargets) {
                    const UnitBase* target = m_engine.unit(targetId);
                    if (target && target->alive()
                        && target->sideStr() != QLatin1String("blue")) {
                        targetIds.append(targetId);
                    }
                }
                targetIds.sort();
                request.mobileSeats.append(OllamaSeatConstraint{
                    state.seatId, state.unitId, state.kind, state.x, state.y, targetIds});
            }
            m_ollamaProvider->requestPlan(
                request,
                [this, context](OllamaResult result) {
                    handleAiPlanResult(context, std::move(result));
                });
        }
    }
    const QList<AiCommand> commands = RulesAi::commandsForPlan(
        m_aiPlan, states, now, mapWidth, mapHeight, &knowledge);
    QHash<QString, int> attackUnitsPerTarget;
    QSet<QString> targeted;
    for (const AiCommand& command : commands) {
        const bool attackCommand = command.action == QLatin1String("engageTarget")
            || (command.action == QLatin1String("moveTo")
                && command.args.contains(QStringLiteral("attackTargetId")));
        if (attackCommand) {
            const QString targetId = command.action == QLatin1String("engageTarget")
                ? command.args.value(QStringLiteral("targetId")).toString()
                : command.args.value(QStringLiteral("attackTargetId")).toString();
            if (targetId.isEmpty()) continue;
            if (!targeted.contains(targetId) && targeted.size() >= parameters.maxTargets) continue;
            if (attackUnitsPerTarget.value(targetId) >= parameters.coordinatedUnitsPerTarget) {
                continue;
            }
            if (executeAiCommand(command)) {
                targeted.insert(targetId);
                ++attackUnitsPerTarget[targetId];
            }
            continue;
        }
        executeAiCommand(command);
    }
    m_aiNextDecisionAt = now + static_cast<double>(
        parameters.enhancedDecisionIntervalMs + parameters.reactionDelayMs) / 1000.0;
}

QJsonObject GameServer::roomState() const {
    QJsonObject online;
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (!it->authenticated || it->observer) continue;
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
            const bool ai = authoritative.controllerType == QLatin1String("ai");
            const bool occupied = ai || occupant.userId > 0;
            seats.append(QJsonObject{{QStringLiteral("seatId"), seatId},
                                     {QStringLiteral("seatType"), descriptor.type},
                                     {QStringLiteral("side"), descriptor.side},
                                     {QStringLiteral("slot"), index},
                                     {QStringLiteral("capacity"), capacity},
                                     {QStringLiteral("occupied"), occupied},
                                     {QStringLiteral("displayName"), ai ? QStringLiteral("AI")
                                                                         : occupant.username},
                                     {QStringLiteral("connected"), ai || authoritative.connected},
                                     {QStringLiteral("controllerType"),
                                      ai ? QStringLiteral("ai") : QStringLiteral("human")},
                                     {QStringLiteral("selectedTemplate"), authoritative.selectedTemplate},
                                     {QStringLiteral("unitId"), authoritative.unitId},
                                     {QStringLiteral("deployed"), authoritative.deployed},
                                     {QStringLiteral("ready"), occupant.ready},
                                     {QStringLiteral("pendingTransfer"), authoritative.pendingTransfer},
                                     {QStringLiteral("redeployRequested"), authoritative.redeployRequested},
                                     {QStringLiteral("unitName"), authoritative.unitName},
                                     {QStringLiteral("state"), occupied
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
                       {QStringLiteral("roomDescription"), m_roomDescription},
                       {QStringLiteral("scenarioId"), m_scenarioId},
                       {QStringLiteral("roomStatus"), m_roomStatus},
                       {QStringLiteral("roomMode"), m_roomMode},
                       {QStringLiteral("aiDifficulty"), m_aiDifficulty},
                       {QStringLiteral("aiProvider"), m_aiSelectedProvider},
                       {QStringLiteral("aiModel"), m_aiSelectedModel},
                       {QStringLiteral("aiResolvedModel"), m_aiResolvedModel},
                       {QStringLiteral("aiEngine"), m_aiEffectiveEngine},
                       {QStringLiteral("aiFallbackReason"), m_aiFallbackReason},
                       {QStringLiteral("configVersion"), static_cast<qint64>(m_configVersion)},
                       {QStringLiteral("seatLimits"), [&]() {
                           QJsonObject limits;
                           for (auto it = m_seatLimits.cbegin(); it != m_seatLimits.cend(); ++it) {
                               limits[it.key()] = it.value();
                           }
                           return limits;
                       }()},
                       {QStringLiteral("seatParameters"), [&]() {
                           QJsonObject parameters;
                           for (auto it = m_seatParameters.cbegin(); it != m_seatParameters.cend(); ++it) {
                               parameters[it.key()] = it.value();
                           }
                           return parameters;
                       }()},
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
    QJsonObject objectiveCounts;
    for (const AiObjectiveV1& objective : m_aiPlan.objectives) {
        objectiveCounts[objective.action] = objectiveCounts.value(objective.action).toInt() + 1;
    }
    const QJsonObject aiStatus{
        {QStringLiteral("provider"), m_aiProviderMode},
        {QStringLiteral("selectedProvider"), m_aiSelectedProvider},
        {QStringLiteral("selectedModel"), m_aiSelectedModel},
        {QStringLiteral("model"), m_ollamaProvider
             ? (m_ollamaProvider->resolvedModel().isEmpty()
                    ? m_ollamaProvider->configuredModel()
                    : m_ollamaProvider->resolvedModel()) : QString()},
        {QStringLiteral("configuredModel"),
         m_ollamaProvider ? m_ollamaProvider->configuredModel() : QString()},
        {QStringLiteral("resolvedModel"),
         m_aiResolvedModel.isEmpty()
             ? (m_ollamaProvider ? m_ollamaProvider->resolvedModel() : QString())
             : m_aiResolvedModel},
        {QStringLiteral("baseUrl"), m_ollamaProvider ? m_ollamaProvider->baseUrl() : QString()},
        {QStringLiteral("connectionStatus"), m_aiConnectionStatus},
        {QStringLiteral("configVersion"), static_cast<qint64>(m_aiConfigVersion)},
        {QStringLiteral("configApplied"), m_aiConfigApplied},
        {QStringLiteral("lastProbeAt"), m_aiLastProbeAt},
        {QStringLiteral("probeFailureClass"), m_aiProbeFailureClass},
        {QStringLiteral("effectiveEngine"), m_aiEffectiveEngine},
        {QStringLiteral("requests"), static_cast<qint64>(m_aiProviderRequests)},
        {QStringLiteral("successes"), static_cast<qint64>(m_aiProviderSuccesses)},
        {QStringLiteral("failures"), static_cast<qint64>(m_aiProviderFailures)},
        {QStringLiteral("lastLatencyMs"), m_aiLastLatencyMs},
        {QStringLiteral("averageLatencyMs"), m_aiAverageLatencyMs},
        {QStringLiteral("lastFailureClass"), m_aiLastFailureClass},
        {QStringLiteral("consecutiveFailures"), m_aiConsecutiveFailures},
        {QStringLiteral("stickyRules"), m_aiStickyRules},
        {QStringLiteral("fallbackReason"), m_aiFallbackReason},
        {QStringLiteral("roomConfigVersion"), static_cast<qint64>(m_aiRoomConfigVersion)},
        {QStringLiteral("ollamaConfigVersion"), static_cast<qint64>(m_aiOllamaConfigVersion)},
        {QStringLiteral("strategyPhase"), m_aiStrategyPhase},
        {QStringLiteral("replanReason"), m_aiReplanReason},
        {QStringLiteral("planningGeneration"), static_cast<qint64>(m_aiPlanningGeneration)},
        {QStringLiteral("knownContacts"), m_aiContactMemory.size()},
        {QStringLiteral("privilegedContacts"), static_cast<int>(std::count_if(
             m_aiContactMemory.cbegin(), m_aiContactMemory.cend(),
             [](const AiObservedTarget& contact) { return contact.privileged; }))},
        {QStringLiteral("commandAccepted"), static_cast<qint64>(m_aiCommandAccepted)},
        {QStringLiteral("commandRejected"), static_cast<qint64>(m_aiCommandRejected)},
        {QStringLiteral("resourceWithdrawals"), static_cast<qint64>(m_aiResourceWithdrawals)},
        {QStringLiteral("strategyPlannerLatencyMs"), m_aiStrategyPlannerLatencyMs},
        {QStringLiteral("lastDecisionSimTime"), m_aiNextDecisionAt},
        {QStringLiteral("missionCounts"), objectiveCounts},
        {QStringLiteral("objectiveCounts"), objectiveCounts}};
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
                             {QStringLiteral("ai"), aiStatus},
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

QJsonObject GameServer::snapshotFor(const ClientSession& session, quint64 projectedRevision) const {
    const quint64 revision = projectedRevision > 0 ? projectedRevision : m_stateRevision;
    QJsonObject projectedRoomState = roomState();
    projectedRoomState[QStringLiteral("stateRevision")] = static_cast<qint64>(revision);
    if (session.observer) {
        QJsonObject workflows;
        for (const QString& side : {QStringLiteral("red"), QStringLiteral("blue")}) {
            if (const GuidedStrikeWorkflow* workflow = m_engine.guidedStrikeWorkflow(side)) {
                workflows.insert(side, workflow->snapshot());
            }
        }
        if (!workflows.isEmpty()) {
            projectedRoomState.insert(QStringLiteral("vmfWorkflows"), workflows);
        }
    } else if (!session.side.isEmpty()) {
        if (const GuidedStrikeWorkflow* workflow = m_engine.guidedStrikeWorkflow(session.side)) {
            projectedRoomState.insert(QStringLiteral("vmfWorkflow"),
                                      StateProjector::projectWorkflow(workflow->snapshot()));
        }
    }
    QJsonObject snapshot = StateProjector::snapshotFor(
        m_engine, normalizedRole(session), revision, projectedRoomState,
        m_sharedIntel.value(session.seatId), m_authoritativeRoom.seat(session.seatId).unitId,
        observerTrajectoriesFor(session));
    if (m_authoritativeRoom.runtimeUnits().isEmpty()) {
        snapshot[QStringLiteral("units")] = QJsonArray{};
        // Before deployment there is no runtime roster, but the room admin
        // still needs the initial scenario to edit it. Participant clients
        // receive an empty scenario until they claim a seat.
        if (normalizedRole(session) != QLatin1String("room_admin")) {
            QJsonObject scenario = snapshot.value(QStringLiteral("scenario")).toObject();
            scenario[QStringLiteral("units")] = QJsonArray{};
            snapshot[QStringLiteral("scenario")] = scenario;
        }
    }
    // Observer snapshots use a strict, read-only wire shape. Map marks are
    // participant-scoped and intentionally absent rather than represented by
    // an empty field, so the projection remains valid under that contract.
    if (!session.observer) {
        snapshot[QStringLiteral("mapMarks")] = filteredMapMarks(session);
        // Unseated authenticated clients have no seat-scoped intelligence
        // projection. Omitting the field keeps their snapshot in the same
        // valid wire shape as the pre-seat lifecycle.
        if (!session.seatId.isEmpty()) {
            snapshot[QStringLiteral("intelState")] = projectedIntelState(session);
        }
    }
    if (session.schemaVersion == Protocol::LegacySchemaVersion) {
        snapshot[QStringLiteral("schemaVersion")] = Protocol::LegacySchemaVersion;
        snapshot.remove(QStringLiteral("intelState"));
    }
    return snapshot;
}

QStringList GameServer::intelShareTargets(const ClientSession& session) const {
    QStringList targets;
    if (session.seatId.isEmpty() || session.side.isEmpty()) return targets;
    const AuthoritativeRoom::Seat source = m_authoritativeRoom.seat(session.seatId);
    UnitBase* sourceUnit = seatUnit(session.seatId);
    if (!sourceUnit || !source.connected) return targets;
    QStringList seatIds = m_authoritativeRoom.seats().keys();
    seatIds.sort();
    for (const QString& seatId : seatIds) {
        const AuthoritativeRoom::Seat candidate = m_authoritativeRoom.seat(seatId);
        if (candidate.seatId.isEmpty() || candidate.seatId == session.seatId
            || candidate.side != source.side || !candidate.connected || candidate.unitId.isEmpty()) continue;
        UnitBase* destination = seatUnit(seatId);
        if (destination && StateProjector::canTransmit(m_engine, sourceUnit->id(), destination->id())) {
            targets.append(seatId);
        }
    }
    return targets;
}

QJsonObject GameServer::projectedIntelState(const ClientSession& session) const {
    if (session.observer || session.seatId.isEmpty()) return {};
    return m_intelLedger.projectedState(session.seatId, intelShareTargets(session)).toJson();
}

void GameServer::refreshIntelLedger() {
    if (m_roomId.isEmpty()) return;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    bool changed = m_intelLedger.advance(now) > 0;
    QStringList seatIds = m_authoritativeRoom.seats().keys();
    seatIds.sort();
    for (const QString& seatId : seatIds) {
        const AuthoritativeRoom::Seat seat = m_authoritativeRoom.seat(seatId);
        if (seat.side.isEmpty() || seat.unitId.isEmpty() || !seat.connected) continue;
        const QSet<QString> visible = StateProjector::sensorVisibleUnitIds(
            m_engine, seatId, seat.unitId);
        for (const QString& targetId : visible) {
            UnitBase* target = m_engine.unit(targetId);
            UnitBase* source = m_engine.unit(seat.unitId);
            if (!target || !source || !target->alive() || target->sideStr() == seat.side) continue;
            const QJsonObject runtime = m_engine.unitSnapshot(targetId);
            const QJsonArray coordinates = runtime.value(QStringLiteral("position")).toArray();
            if (coordinates.size() < 2) continue;
            const QJsonObject position{{QStringLiteral("x"), coordinates.at(0)},
                                       {QStringLiteral("y"), coordinates.at(1)},
                                       {QStringLiteral("alt"), coordinates.size() > 2 ? coordinates.at(2) : QJsonValue(0.0)}};
            const QJsonObject known{{QStringLiteral("callsign"), runtime.value(QStringLiteral("callsign"))},
                                    {QStringLiteral("kind"), runtime.value(QStringLiteral("kind"))},
                                    {QStringLiteral("side"), runtime.value(QStringLiteral("side"))}};
            const IntelLedger::Result result = m_intelLedger.observeSensor(
                seatId, targetId, known, position, source->id(), now);
            changed = changed || result.changed;
        }
    }
    m_sharedIntel.clear();
    for (const QString& seatId : seatIds) {
        for (const auto& contact : m_intelLedger.state(seatId).records) {
            if (contact.type == QLatin1String("sensorContact")
                && contact.freshness == QLatin1String("live") && contact.actionable
                && !contact.targetId.isEmpty()) {
                m_sharedIntel[seatId].insert(contact.targetId);
            }
        }
    }
    if (changed) persistRoomState();
}

bool GameServer::sendEnvelope(QWebSocket* socket, const QString& type,
                              const QJsonObject& payload) {
    if (!socket || socket->state() != QAbstractSocket::ConnectedState
        || !m_clients.contains(socket)) return false;
    if (socket->bytesToWrite() > kMaxPendingBytes) {
        socket->close(QWebSocketProtocol::CloseCodeTooMuchData,
                      QStringLiteral("客户端接收速度过慢"));
        return false;
    }
    ClientSession& session = m_clients[socket];
    const QJsonObject wirePayload = payloadForWireVersion(type, payload, session.schemaVersion);
    if (wirePayload.isEmpty() && type == QLatin1String("intelHistoryPage")) return false;
    const QJsonObject envelope = Protocol::makeServerEnvelopeForVersion(
        type, session.sequence + 1, wirePayload, session.protocolVersion, session.schemaVersion);
    const Protocol::ValidationResult validation = Protocol::validateServerEnvelopeForVersion(
        envelope);
    if (!validation.valid) {
        audit(QStringLiteral("protocol"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("outgoingMessageRejected")},
                          {QStringLiteral("type"), type},
                          {QStringLiteral("code"), validation.code},
                          {QStringLiteral("message"), validation.message},
                          {QStringLiteral("protocolVersion"), session.protocolVersion},
                          {QStringLiteral("schemaVersion"), session.schemaVersion},
                          {QStringLiteral("user"), session.username}});
        return false;
    }
    const QByteArray encoded = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (encoded.size() > Protocol::MaxServerMessageBytes) {
        audit(QStringLiteral("security"),
              QJsonObject{{QStringLiteral("event"), QStringLiteral("outgoingMessageTooLarge")},
                          {QStringLiteral("type"), type},
                          {QStringLiteral("bytes"), encoded.size()},
                          {QStringLiteral("user"), session.username}});
        socket->close(QWebSocketProtocol::CloseCodeTooMuchData,
                      QStringLiteral("服务器状态快照超过大小限制"));
        return false;
    }
    ++session.sequence;
    socket->sendTextMessage(QString::fromUtf8(encoded));
    if (type != QLatin1String("snapshot") && type != QLatin1String("delta")
        && type != QLatin1String("pong")) {
        audit(QStringLiteral("message"), QJsonObject{{QStringLiteral("direction"), QStringLiteral("out")},
                                                       {QStringLiteral("type"), type},
                                                       {QStringLiteral("user"), session.username},
                                                       {QStringLiteral("role"), session.role},
                                                       {QStringLiteral("summary"), messageSummary(type, payload)}});
    }
    return true;
}

void GameServer::sendError(QWebSocket* socket, const QString& code, const QString& message,
                           const QString& requestId) {
    if (code == QLatin1String("PERMISSION_DENIED")
        || code == QLatin1String("OBSERVER_READ_ONLY")
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

void GameServer::cacheIntelCommandResult(qint64 userId, const QString& action,
                                         const QString& requestId,
                                         const CommandResult& result) {
    if (userId <= 0 || requestId.isEmpty() || !result.accepted) return;
    const QString key = intelRequestCacheKey(userId, action, requestId);
    if (m_commandResults.contains(key)) return;
    QJsonObject payload = result.toJson();
    payload[QStringLiteral("commandId")] = requestId;
    payload[QStringLiteral("serverTime")] = m_engine.simTime();
    m_commandResultOrder.append(key);
    m_commandResults.insert(key, payload);
    while (m_commandResultOrder.size() > 2048) {
        m_commandResults.remove(m_commandResultOrder.takeFirst());
    }
}

void GameServer::sendIntelCommandResult(QWebSocket* socket, const QString& action,
                                        const QString& requestId,
                                        const CommandResult& result) {
    if (requestId.isEmpty()) return;
    const qint64 userId = m_clients.contains(socket) ? m_clients.value(socket).userId : 0;
    if (result.accepted) cacheIntelCommandResult(userId, action, requestId, result);
    const QString key = intelRequestCacheKey(userId, action, requestId);
    const QJsonObject payload = result.accepted && m_commandResults.contains(key)
        ? m_commandResults.value(key)
        : QJsonObject{{QStringLiteral("accepted"), result.accepted},
                      {QStringLiteral("code"), result.code},
                      {QStringLiteral("message"), result.message},
                      {QStringLiteral("commandId"), requestId},
                      {QStringLiteral("serverTime"), m_engine.simTime()}};
    sendEnvelope(socket, QStringLiteral("commandResult"), payload);
}

bool GameServer::sendFullSnapshot(QWebSocket* socket) {
    if (!socket || !m_clients.contains(socket)) return false;
    ClientSession& session = m_clients[socket];
    const QJsonObject snapshot = snapshotFor(session);
    if (!sendEnvelope(socket, QStringLiteral("snapshot"), snapshot)) return false;
    session.lastSnapshot = snapshot;
    return true;
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
        it->observer = false;
        it->observerTrajectorySelection.clear();
        it->role = QStringLiteral("player");
        sendSeatDirectory(it.key());
    }
    for (const auto& [socket, projected] : notifications) {
        if (m_clients.contains(socket)) sendEnvelope(socket, QStringLiteral("event"), projected);
    }
    m_observerSelectionCache.clear();
    m_observerTrajectories.clear();
    m_nextObserverTrajectorySampleAt = 0.0;
}

void GameServer::broadcastSnapshots(bool forceFull) {
    refreshIntelLedger();
    const quint64 candidateRevision = m_stateRevision + 1;
    QHash<QString, QJsonObject> projectedByGroup;
    QHash<QWebSocket*, QJsonObject> currentSnapshots;
    bool projectionChanged = false;
    const auto projectionGroupKey = [this](const ClientSession& session) {
        QStringList shared = m_sharedIntel.value(session.seatId).values();
        QStringList trajectories = session.observerTrajectorySelection.values();
        std::sort(shared.begin(), shared.end());
        std::sort(trajectories.begin(), trajectories.end());
        QJsonArray sharedIds;
        for (const QString& id : shared) sharedIds.append(id);
        QJsonArray trajectoryIds;
        for (const QString& id : trajectories) trajectoryIds.append(id);
        const QJsonObject key{{QStringLiteral("role"), normalizedRole(session)},
                              {QStringLiteral("userId"), session.userId},
                              {QStringLiteral("seatId"), session.seatId},
                              {QStringLiteral("side"), session.side},
                              {QStringLiteral("ownedUnitId"),
                               m_authoritativeRoom.seat(session.seatId).unitId},
                              {QStringLiteral("sharedIntel"), sharedIds},
                              {QStringLiteral("trajectories"), trajectoryIds}};
        return QString::fromUtf8(QJsonDocument(key).toJson(QJsonDocument::Compact));
    };
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (!it->authenticated) continue;
        const QString groupKey = projectionGroupKey(it.value());
        QJsonObject current = projectedByGroup.value(groupKey);
        if (current.isEmpty()) {
            current = snapshotFor(it.value(), candidateRevision);
            projectedByGroup.insert(groupKey, current);
        }
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
        m_stateRevision = candidateRevision;
    } else {
        // The candidate revision was only needed to build a fresh projection.
        // A forced full snapshot without a state change must keep the current
        // contiguous revision expected by clients.
        for (auto it = currentSnapshots.begin(); it != currentSnapshots.end(); ++it) {
            it->insert(QStringLiteral("stateRevision"), static_cast<qint64>(m_stateRevision));
            QJsonObject room = it->value(QStringLiteral("roomState")).toObject();
            room.insert(QStringLiteral("stateRevision"), static_cast<qint64>(m_stateRevision));
            it->insert(QStringLiteral("roomState"), room);
        }
    }
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (!it->authenticated) continue;
        const QJsonObject current = currentSnapshots.value(it.key());
        bool sent = false;
        if (!forceFull && !it->lastSnapshot.isEmpty()
            && StateDelta::canCreate(it->lastSnapshot, current)) {
            const QJsonObject delta = StateDelta::create(it->lastSnapshot, current);
            const QJsonObject candidateEnvelope = Protocol::makeServerEnvelopeForVersion(
                QStringLiteral("delta"), 1, delta, it->protocolVersion, it->schemaVersion);
            if (!delta.isEmpty()
                && Protocol::validateServerEnvelopeForVersion(candidateEnvelope).valid) {
                sent = sendEnvelope(it.key(), QStringLiteral("delta"), delta);
                if (!sent) sent = sendFullSnapshot(it.key());
            } else {
                // A projection that cannot be encoded as a valid delta must
                // never reach the client. Re-establish its complete baseline.
                sent = sendFullSnapshot(it.key());
            }
        } else {
            sent = sendEnvelope(it.key(), QStringLiteral("snapshot"), current);
        }
        // A failed send must leave the previous baseline intact so the next
        // broadcast can retry a full snapshot or a delta from known state.
        if (sent) it->lastSnapshot = current;
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

void GameServer::broadcastVmfEvent(const QJsonObject& event) {
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (!it->authenticated || it->roomId != m_roomId) continue;
        const QString role = normalizedRole(it.value());
        if (it->observer) {
            const QJsonObject summary = StateProjector::projectEvent(
                m_engine, role, event,
                m_authoritativeRoom.seat(it.value().seatId).unitId);
            if (!summary.isEmpty()) sendEnvelope(it.key(), QStringLiteral("event"), summary);
            continue;
        }
        const QJsonObject projected = StateProjector::projectEvent(
            m_engine, role, event,
            m_authoritativeRoom.seat(it.value().seatId).unitId);
        if (!projected.isEmpty()) sendEnvelope(it.key(), QStringLiteral("vmfEvent"), projected);
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
