#include "SimulationEngine.h"
#include "UnitBase.h"
#include "SnapshotCodec.h"
#include "LocalTransport.h"
#include "vmf/VmfProfile.h"
#include "vmf/VmfRuntimeState.h"
#include "../units/AttackUAV.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QFileInfo>
#include <QDir>
#include <QSet>
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>

namespace gbr {

namespace {

constexpr size_t kMaxScenarioUnits = 512;
constexpr size_t kMaxSchedulePoints = 512;
constexpr qsizetype kMaxUnitIdLength = 64;
constexpr qsizetype kMaxCallsignLength = 128;
constexpr double kMaxMapExtentMeters = 1'000'000.0;
constexpr double kMaxRealtimeStepSeconds = 0.5;
constexpr double kMaxRealtimeDebtSeconds = 10.0;

bool isKnownKind(const QString& kind) {
    return kind == QLatin1String("commandpost")
        || kind == QLatin1String("reconuav")
        || kind == QLatin1String("attackuav")
        || kind == QLatin1String("groundscout")
        || kind == QLatin1String("jammeruav")
        || kind == QLatin1String("groundtarget");
}

bool finiteNonNegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

QString validateScenarioUnit(const ScenarioUnit& u, const ScenarioMap& map) {
    if (u.id.trimmed().isEmpty()) return QStringLiteral("单元ID不能为空");
    if (u.id.size() > kMaxUnitIdLength || u.callsign.size() > kMaxCallsignLength) {
        return QStringLiteral("单元 ID 或名称过长: %1").arg(u.id.left(kMaxUnitIdLength));
    }
    if (!isKnownKind(u.kind)) {
        return QStringLiteral("未知单元类型: %1 (%2)").arg(u.kind, u.id);
    }
    if (u.side != QLatin1String("red") && u.side != QLatin1String("blue")) {
        return QStringLiteral("单元阵营无效: %1 (%2)").arg(u.side, u.id);
    }
    const bool validPosition = std::isfinite(u.pos.x) && std::isfinite(u.pos.y)
        && std::isfinite(u.pos.alt) && u.pos.x >= 0.0 && u.pos.y >= 0.0
        && u.pos.x <= map.widthMeters && u.pos.y <= map.heightMeters;
    const bool validParams = std::isfinite(u.detectRange) && u.detectRange >= 0.0
        && std::isfinite(u.attackRange) && u.attackRange >= 0.0
        && std::isfinite(u.commRange) && u.commRange >= 0.0
        && std::isfinite(u.speed) && u.speed >= 0.0
        && u.speed <= UnitBase::commandedSpeedLimitMps(kindFromName(u.kind))
        && std::isfinite(u.collisionRadius) && u.collisionRadius > 0.0
        && u.collisionRadius <= 1000.0
        && std::isfinite(u.collisionHalfHeight) && u.collisionHalfHeight > 0.0
        && u.collisionHalfHeight <= 500.0
        && std::isfinite(u.maxHp) && u.maxHp > 0.0
        && std::isfinite(u.attackPower) && u.attackPower >= 0.0
        && std::isfinite(u.armor) && u.armor >= 0.0 && u.armor <= 0.9
        && std::isfinite(u.repairRate) && u.repairRate >= 0.0
        && std::isfinite(u.subsystemRepairRate) && u.subsystemRepairRate >= 0.0;
    if (!validPosition || !validParams) {
        if (!std::isfinite(u.speed) || u.speed < 0.0
            || u.speed > UnitBase::commandedSpeedLimitMps(kindFromName(u.kind))) {
            return QStringLiteral("单元速度超过 %1 的类型上限: %2")
                .arg(UnitBase::commandedSpeedLimitMps(kindFromName(u.kind)), 0, 'f', 0)
                .arg(u.id);
        }
        return QStringLiteral("单元参数无效: %1").arg(u.id);
    }
    if (u.kind != QLatin1String("commandpost")
        && (!std::isfinite(u.fuelCapacitySec) || u.fuelCapacitySec <= 0.0
            || !std::isfinite(u.initialFuelSec) || u.initialFuelSec < 0.0
            || u.initialFuelSec > u.fuelCapacitySec)) {
        return QStringLiteral("移动单元燃油参数无效: %1").arg(u.id);
    }
    if (u.kind == QLatin1String("attackuav")) {
        const bool validWeapon = u.ammoCapacity >= 0 && u.ammoCapacity <= 100000
            && u.initialAmmo >= 0 && u.initialAmmo <= u.ammoCapacity
            && std::isfinite(u.hitProbability) && u.hitProbability >= 0.0
            && u.hitProbability <= 1.0
            && std::isfinite(u.minAttackRange) && u.minAttackRange >= 0.0
            && std::isfinite(u.optimalRange) && u.optimalRange >= u.minAttackRange
            && u.optimalRange <= u.attackRange
            && std::isfinite(u.cooldownSec) && u.cooldownSec >= 0.0
            && std::isfinite(u.damageMin) && u.damageMin >= 0.0
            && std::isfinite(u.damageMax) && u.damageMax >= u.damageMin
            && std::isfinite(u.rangeFalloff) && u.rangeFalloff >= 0.0
            && u.rangeFalloff <= 1.0
            && std::isfinite(u.fuelCapacitySec) && u.fuelCapacitySec > 0.0
            && std::isfinite(u.initialFuelSec) && u.initialFuelSec >= 0.0
            && u.initialFuelSec <= u.fuelCapacitySec
            && std::isfinite(u.rearmDurationSec) && u.rearmDurationSec >= 0.0;
        if (!validWeapon) return QStringLiteral("攻击单元武器参数无效: %1").arg(u.id);
    }
    if (u.schedule.size() > kMaxSchedulePoints) {
        return QStringLiteral("单元计划点不能超过 %1 个: %2")
            .arg(kMaxSchedulePoints).arg(u.id);
    }
    for (const auto& point : u.schedule) {
        if (!std::isfinite(point.time) || point.time < 0.0
            || !std::isfinite(point.x) || !std::isfinite(point.y)
            || point.x < 0.0 || point.y < 0.0
            || point.x > map.widthMeters || point.y > map.heightMeters) {
            return QStringLiteral("单元计划点无效: %1").arg(u.id);
        }
    }
    return {};
}

// ScenarioUnit is also the editor-facing mutation DTO. Older callers and
// lightweight fixtures do not carry the collision fields introduced in schema
// 4, so fill only the new required values before validation. Explicit negative
// or non-finite values remain invalid and are never silently repaired.
ScenarioUnit normalizeScenarioUnitDefaults(ScenarioUnit unit) {
    const UnitKind kind = kindFromName(unit.kind);
    if (std::isfinite(unit.collisionRadius) && unit.collisionRadius == 0.0) {
        unit.collisionRadius = UnitBase::defaultCollisionRadiusM(kind);
    }
    if (std::isfinite(unit.collisionHalfHeight) && unit.collisionHalfHeight == 0.0) {
        unit.collisionHalfHeight = UnitBase::defaultCollisionHalfHeightM(kind);
    }
    // A copied editor DTO may carry the generated URN of its old id.  Only
    // regenerate the repository-owned default; an explicit external URN is
    // stable identity and must survive an id rename.
    const QString generatedPrefix = QStringLiteral("urn:gbr:wargame:unit:");
    if (!unit.id.isEmpty()
        && (unit.vmfUrn.trimmed().isEmpty()
            || (unit.vmfUrn.startsWith(generatedPrefix)
                && unit.vmfUrn.mid(generatedPrefix.size()) != unit.id))) {
        unit.vmfUrn = QStringLiteral("urn:gbr:wargame:unit:%1").arg(unit.id);
    }
    return unit;
}

bool isHostileTarget(const UnitBase* attacker, const UnitBase* target) {
    return attacker && target && attacker->alive() && target->alive()
        && attacker->side() != target->side();
}

QString defaultVmfProfileRoot() {
    const QString configured = qEnvironmentVariable("WARGAME_VMF_ROOT").trimmed();
    if (!configured.isEmpty()) return configured;
    const QStringList candidates{
#ifdef WARGAME_DESIGN_ROOT
        QStringLiteral(WARGAME_DESIGN_ROOT),
#endif
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("design/EncoderDecoder")),
        QDir::current().filePath(QStringLiteral("design/EncoderDecoder")),
        QDir::current().filePath(QStringLiteral("../design/EncoderDecoder")),
        QDir::current().filePath(QStringLiteral("../../design/EncoderDecoder"))};
    for (const QString& candidate : candidates) {
        if (QFileInfo(QDir(candidate).filePath(QStringLiteral("msgStruct/msg0_1.xml"))).exists()) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return {};
}

} // namespace

SimulationEngine::SimulationEngine(QObject* parent)
    : SimulationEngine(nullptr, parent) {}

SimulationEngine::SimulationEngine(ITransport* transport, QObject* parent)
    : QObject(parent),
      m_ownedTransport(transport ? nullptr : std::make_unique<LocalTransport>(this)),
      m_transport(transport ? transport : m_ownedTransport.get()),
      m_map(std::make_unique<MapProvider>()),
      m_clock(std::make_unique<RealTimeClock>()),
      m_recorder(std::make_unique<MessageLogRecorder>()) {
    const QString mapRoot = qEnvironmentVariableIsSet("WARGAME_MAP_DIR")
        ? qEnvironmentVariable("WARGAME_MAP_DIR")
        : QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("map"));
    QString metadataError;
    if (!m_map->loadMetadataFile(QDir(mapRoot).filePath(QStringLiteral("metadata.json")),
                                 &metadataError)) {
        qWarning() << "SimulationEngine: using built-in map fallback:" << metadataError;
    }

    m_timer.setInterval(50);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this](){ onTickInternal(false, 0.0); });
    // Forward bus emissions through the transport sink so the engine's
    // message cache and QML signals stay in sync regardless of which
    // transport is in use.
    m_transport->setMessageSink([this](const QJsonObject& obj) { onMessagePosted(obj); });
    if (m_transport->bus()) {
        connect(m_transport->bus(), &MessageBus::ackStateChanged, this,
                [this](const QString& messageId, bool acknowledged, int retryCount,
                       const QString& reason) {
                    updateMessageAckState(messageId, acknowledged, retryCount, reason);
                });
    }

    m_dirtyTimer.setInterval(16);
    m_dirtyTimer.setSingleShot(true);
    connect(&m_dirtyTimer, &QTimer::timeout, this, &SimulationEngine::flushDirtyUnits);
    initCommandDispatch();
}

SimulationEngine::~SimulationEngine() {
    m_timer.stop();
    m_dirtyTimer.stop();
    // An externally-owned transport can outlive the engine. Do not leave its
    // message sink pointing at this destroyed object.
    if (m_transport) {
        m_transport->setMessageSink({});
        // The encoder captures this engine through the VMF gateway.  Clear it
        // before an externally owned transport can outlive the engine.
        if (m_transport->bus()) m_transport->bus()->setVmfEncoder({});
    }
}

void SimulationEngine::loadDefaultScenario() {
    setScenario(ScenarioIo::defaultScenario());
}

QString SimulationEngine::locateVmfProfileRoot() {
    return defaultVmfProfileRoot();
}

bool SimulationEngine::configureCommunication(const Scenario& scenario, QString* error) {
    if (error) error->clear();
    if (scenario.communicationPolicy.format == QLatin1String("native")) {
        if (m_transport && m_transport->bus()) m_transport->bus()->setVmfEncoder({});
        m_vmfGateway.reset();
        m_vmfDictionaries.reset();
        m_vmfCatalog.reset();
        m_vmfProfileError.clear();
        m_redGuidedStrikeWorkflow.reset();
        m_blueGuidedStrikeWorkflow.reset();
        emit vmfWorkflowChanged();
        return true;
    }
    QList<vmf::Diagnostic> diagnostics;
    const QString root = locateVmfProfileRoot();
    const auto dictionaries = vmf::VmfProfile::load(
        scenario.communicationPolicy.vmfProfile.isEmpty()
            ? QString::fromLatin1(vmf::VmfProfile::DesignV1)
            : scenario.communicationPolicy.vmfProfile,
        root, &diagnostics);
    if (!dictionaries) {
        m_vmfProfileError = vmf::Codec::diagnosticsToString(diagnostics);
        if (error) *error = m_vmfProfileError.isEmpty()
            ? QStringLiteral("无法加载 VMF profile: %1").arg(root) : m_vmfProfileError;
        return false;
    }
    QList<vmf::Diagnostic> catalogDiagnostics;
    const auto catalog = vmf::VmfProfile::loadCatalog(
        scenario.communicationPolicy.vmfProfile.isEmpty()
            ? QString::fromLatin1(vmf::VmfProfile::DesignV1)
            : scenario.communicationPolicy.vmfProfile,
        root, &catalogDiagnostics);
    if (!catalog) {
        m_vmfProfileError = vmf::Codec::diagnosticsToString(catalogDiagnostics);
        if (error) *error = m_vmfProfileError.isEmpty()
            ? QStringLiteral("无法加载 VMF 消息目录: %1").arg(root) : m_vmfProfileError;
        return false;
    }
    m_vmfDictionaries = dictionaries;
    m_vmfCatalog = catalog;
    m_vmfGateway = std::make_unique<vmf::VmfMessageGateway>(
        m_transport ? m_transport->bus() : nullptr, m_vmfDictionaries, m_vmfCatalog, this);
    m_vmfGateway->setAutomaticAckEnabled(scenario.communicationPolicy.automaticAck);
    m_vmfGateway->setCoordinateResolver([this](double x, double y)
        -> std::optional<GeoCoord> {
        if (!m_map || m_map->metadataRevision() <= 0
            || !m_map->contains(GeoPos{x, y, 0.0})) {
            return std::nullopt;
        }
        const GeoCoord coordinate = m_map->logicalToGeo(GeoPos{x, y, 0.0});
        if (!std::isfinite(coordinate.lat) || !std::isfinite(coordinate.lon)
            || coordinate.lat < -90.0 || coordinate.lat > 90.0
            || coordinate.lon < -180.0 || coordinate.lon > 180.0) {
            return std::nullopt;
        }
        return coordinate;
    });
    if (m_transport && m_transport->bus()) {
        m_transport->bus()->setVmfEncoder(
            [this](const Message& input, Message* output, QString* encodeError) {
                return m_vmfGateway && m_vmfGateway->prepareDomainMessage(
                    input, output, encodeError);
            });
    }
    m_vmfProfileError.clear();
    return true;
}

QJsonObject SimulationEngine::collectVmfRuntimeState() const {
    if (!m_vmfGateway || !m_transport || !m_transport->bus()) return {};

    vmf::RuntimeState state;
    state.profileId = m_scenario.communicationPolicy.vmfProfile.isEmpty()
        ? QString::fromLatin1(vmf::RuntimeState::ProfileId)
        : m_scenario.communicationPolicy.vmfProfile;
    for (GuidedStrikeWorkflow* workflow : {m_redGuidedStrikeWorkflow.get(),
                                            m_blueGuidedStrikeWorkflow.get()}) {
        if (workflow && workflow->stage() != GuidedStrikeWorkflow::Stage::Idle) {
            state.activeTasks.append(workflow->snapshot());
        }
    }
    const QJsonArray pending = m_transport->bus()->pendingAckState();
    for (const QJsonValue& value : pending) {
        const QJsonObject entry = value.toObject();
        const QJsonObject message = entry.value(QStringLiteral("message")).toObject();
        QJsonObject ack{
            {QStringLiteral("messageId"), message.value(QStringLiteral("id"))},
            {QStringLiteral("type"), message.value(QStringLiteral("type"))},
            {QStringLiteral("traceId"), message.value(QStringLiteral("traceId"))},
            {QStringLiteral("correlationId"), message.value(QStringLiteral("correlationId"))},
            {QStringLiteral("sender"), message.value(QStringLiteral("sender"))},
            {QStringLiteral("receiver"), message.value(QStringLiteral("receiver"))},
            {QStringLiteral("sentAt"), entry.value(QStringLiteral("sentAt"))},
            {QStringLiteral("retries"), entry.value(QStringLiteral("retries"))},
            {QStringLiteral("retryCount"), message.value(QStringLiteral("retryCount"))},
            {QStringLiteral("requiresAck"), message.value(QStringLiteral("requiresAck"))},
            {QStringLiteral("automaticAck"), message.value(QStringLiteral("automaticAck"))},
            {QStringLiteral("payload"), message.value(QStringLiteral("payload"))},
            {QStringLiteral("vmfMessage"), message.value(QStringLiteral("vmfMessage"))},
            {QStringLiteral("wireFormat"), message.value(QStringLiteral("wireFormat"))}};
        if (message.contains(QStringLiteral("wireBytes"))) {
            ack.insert(QStringLiteral("wireBytes"), message.value(QStringLiteral("wireBytes")));
            ack.insert(QStringLiteral("wireBitLength"), message.value(QStringLiteral("wireBitLength")));
        }
        state.pendingAcks.append(ack);
    }
    state.seenMessageIds = m_transport->bus()->automaticMessageState();
    state.traceSummaries = m_vmfGateway->recentTraceSummaries();
    QString validationError;
    if (!state.validate(&validationError)) {
        qWarning() << "SimulationEngine: refusing to export invalid VMF runtime state"
                   << validationError;
        return {};
    }
    return state.toJson();
}

bool SimulationEngine::restoreVmfRuntimeState(const QJsonObject& json, QString* error) {
    if (error) error->clear();
    if (json.isEmpty()) return true;
    if (!m_vmfGateway || !m_transport || !m_transport->bus()) {
        if (error) *error = QStringLiteral("当前场景未启用 VMF，不能恢复 VMF 状态");
        return false;
    }
    vmf::RuntimeState state;
    QString stateError;
    if (!vmf::RuntimeState::fromJson(json, &state, &stateError)) {
        if (error) *error = stateError;
        return false;
    }
    const QString expectedProfile = m_scenario.communicationPolicy.vmfProfile.isEmpty()
        ? QString::fromLatin1(vmf::RuntimeState::ProfileId)
        : m_scenario.communicationPolicy.vmfProfile;
    if (state.profileId != expectedProfile) {
        if (error) *error = QStringLiteral("VMF 运行时状态 profile 与场景不一致");
        return false;
    }

    // Validate every workflow against a detached instance before touching the
    // live workflows.  A malformed second task must not leave the first side
    // half-restored.
    QSet<QString> restoredSides;
    for (const QJsonValue& value : state.activeTasks) {
        const QJsonObject task = value.toObject();
        const QString side = task.value(QStringLiteral("side")).toString();
        GuidedStrikeWorkflow* workflow = guidedStrikeWorkflow(side);
        if (!workflow) {
            if (error) *error = QStringLiteral("VMF 活动任务阵营无对应工作流");
            return false;
        }
        if (restoredSides.contains(side)) {
            if (error) *error = QStringLiteral("VMF 活动任务同一阵营重复");
            return false;
        }
        const QJsonObject current = workflow->snapshot();
        if (task.value(QStringLiteral("commandPostId")).toString()
                != current.value(QStringLiteral("commandPostId")).toString()) {
            if (error) *error = QStringLiteral("VMF 活动任务指挥所与场景不一致");
            return false;
        }
        GuidedStrikeWorkflow candidate(nullptr, side,
                                       current.value(QStringLiteral("commandPostId")).toString());
        QString workflowError;
        if (!candidate.restoreSnapshot(task, &workflowError)) {
            if (error) *error = workflowError;
            return false;
        }
        restoredSides.insert(side);
    }

    QJsonArray pending;
    for (const QJsonValue& value : state.pendingAcks) {
        const QJsonObject ack = value.toObject();
        QJsonObject message{
            {QStringLiteral("id"), ack.value(QStringLiteral("messageId"))},
            {QStringLiteral("type"), ack.value(QStringLiteral("type"))},
            {QStringLiteral("sender"), ack.value(QStringLiteral("sender"))},
            {QStringLiteral("receiver"), ack.value(QStringLiteral("receiver"))},
            {QStringLiteral("requiresAck"), ack.value(QStringLiteral("requiresAck"))},
            {QStringLiteral("automaticAck"), ack.value(QStringLiteral("automaticAck"))},
            {QStringLiteral("retryCount"), ack.value(QStringLiteral("retryCount"))},
            {QStringLiteral("traceId"), ack.value(QStringLiteral("traceId"))},
            {QStringLiteral("correlationId"), ack.value(QStringLiteral("correlationId"))},
            {QStringLiteral("wireFormat"), ack.value(QStringLiteral("wireFormat"))},
            {QStringLiteral("payload"), ack.value(QStringLiteral("payload"))}};
        if (ack.contains(QStringLiteral("vmfMessage"))) {
            message.insert(QStringLiteral("vmfMessage"), ack.value(QStringLiteral("vmfMessage")));
        }
        if (ack.contains(QStringLiteral("wireBytes"))) {
            message.insert(QStringLiteral("wireBytes"), ack.value(QStringLiteral("wireBytes")));
            message.insert(QStringLiteral("wireBitLength"), ack.value(QStringLiteral("wireBitLength")));
        }
        pending.append(QJsonObject{{QStringLiteral("message"), message},
                                   {QStringLiteral("sentAt"), ack.value(QStringLiteral("sentAt"))},
                                   {QStringLiteral("retries"), ack.value(QStringLiteral("retries"))}});
    }
    // Run the exact bus decoders on detached state first.  This keeps restore
    // atomic even when a future MessageBus validation rule is stricter than
    // RuntimeState's structural validation.
    MessageBus pendingValidator;
    QString restoreError;
    if (!pendingValidator.restorePendingAckState(pending, &restoreError)) {
        if (error) *error = restoreError;
        return false;
    }
    MessageBus automaticValidator;
    if (!automaticValidator.restoreAutomaticMessageState(state.seenMessageIds,
                                                         &restoreError)) {
        if (error) *error = restoreError;
        return false;
    }
    for (const QJsonValue& value : state.activeTasks) {
        const QJsonObject task = value.toObject();
        GuidedStrikeWorkflow* workflow = guidedStrikeWorkflow(
            task.value(QStringLiteral("side")).toString());
        QString workflowError;
        if (!workflow->restoreSnapshot(task, &workflowError)) {
            if (error) *error = workflowError;
            return false;
        }
    }
    if (!m_transport->bus()->restorePendingAckState(pending, &restoreError)
        || !m_transport->bus()->restoreAutomaticMessageState(state.seenMessageIds, &restoreError)
        || !m_vmfGateway->restoreTraceSummaries(state.traceSummaries, &restoreError)) {
        if (error) *error = restoreError;
        return false;
    }
    return true;
}

bool SimulationEngine::validateVmfMessage(const Message& message, QString* error) const {
    if (error) error->clear();
    if (!m_vmfGateway) {
        if (error) *error = QStringLiteral("当前场景未启用 VMF");
        return false;
    }
    if (message.wireFormat != Message::WireFormat::VmfDesignV1
        || message.wireBytes.isEmpty() || message.wireBitLength <= 0) {
        if (error) *error = QStringLiteral("VMF 消息缺少完整 wire 数据");
        return false;
    }
    if (!vmf::VmfMessageGateway::isMessageNameCompatible(message)) {
        if (error) *error = QStringLiteral("VMF 消息名与领域消息类型不匹配");
        return false;
    }
    if (!m_vmfGateway->validateCatalog(message, {}, {}, error)) return false;
    vmf::DecodedMessage decoded;
    QList<vmf::Diagnostic> diagnostics;
    if (!m_vmfGateway->decode(message, &decoded, &diagnostics)) {
        if (error) *error = vmf::Codec::diagnosticsToString(diagnostics);
        return false;
    }
    return true;
}

bool SimulationEngine::validateVmfMessageForRoles(const Message& message,
                                                  const QString& senderRole,
                                                  const QString& receiverRole,
                                                  QString* error) const {
    if (!validateVmfMessage(message, error)) return false;
    return m_vmfGateway && m_vmfGateway->validateCatalog(message, senderRole,
                                                          receiverRole, error);
}

bool SimulationEngine::validateVmfWorkflowMessage(const Message& message,
                                                   QString* error) const {
    if (error) error->clear();
    const UnitBase* sender = unit(message.sender);
    if (!sender) {
        if (error) *error = QStringLiteral("VMF 发送单元不存在");
        return false;
    }
    GuidedStrikeWorkflow* workflow = guidedStrikeWorkflow(sender->sideStr());
    return !workflow || workflow->validateIncomingMessage(message, error);
}

QJsonObject SimulationEngine::vmfMessageCatalogSummary(const Message& message) const {
    return m_vmfGateway ? m_vmfGateway->catalogSummary(message) : QJsonObject{};
}

GuidedStrikeWorkflow* SimulationEngine::guidedStrikeWorkflow(const QString& side) const {
    if (side == QLatin1String("red")) return m_redGuidedStrikeWorkflow.get();
    if (side == QLatin1String("blue")) return m_blueGuidedStrikeWorkflow.get();
    return nullptr;
}

bool SimulationEngine::postVmfMessage(Message message, QString* error) {
    if (error) error->clear();
    if (!m_vmfGateway || !m_transport || !m_transport->bus()) {
        if (error) *error = QStringLiteral("当前场景未启用 VMF");
        return false;
    }
    if (!validateVmfMessage(message, error)) return false;
    message.vmfEncoded = true;
    return m_transport->bus()->send(message);
}

bool SimulationEngine::prepareVmfMessage(const Message& input, Message* output,
                                         QString* error) const {
    if (error) error->clear();
    if (!m_vmfGateway) {
        if (error) *error = QStringLiteral("当前场景未启用 VMF");
        return false;
    }
    return m_vmfGateway->prepareDomainMessage(input, output, error);
}

bool SimulationEngine::prepareVmfMessageWithTrace(const Message& input, Message* output,
                                                  QJsonObject* trace,
                                                  QString* error) const {
    if (!m_vmfGateway) {
        if (error) *error = QStringLiteral("VMF profile 未启用");
        return false;
    }
    return m_vmfGateway->prepareDomainMessage(input, output, error, trace);
}

bool SimulationEngine::prepareVmfXmlMessage(const QString& messageName,
                                            const QByteArray& xml,
                                            const Message& input, Message* output,
                                            QJsonObject* trace,
                                            QString* error) const {
    if (!m_vmfGateway) {
        if (error) *error = QStringLiteral("VMF profile 未启用");
        return false;
    }
    return m_vmfGateway->prepareXmlMessage(messageName, xml, input, output, trace, error);
}

bool SimulationEngine::setScenario(const Scenario& s) {
    return applyScenario(s, false);
}

bool SimulationEngine::setRemoteScenario(const Scenario& s) {
    return applyScenario(s, true);
}

bool SimulationEngine::applyScenario(const Scenario& s, bool allowEmpty) {
    if (s.units.empty() && !allowEmpty) {
        m_lastError = QStringLiteral("场景单元为空，未应用");
        emit errorOccurred(m_lastError);
        return false;
    }
    if (!std::isfinite(s.map.widthMeters) || s.map.widthMeters <= 0.0
        || !std::isfinite(s.map.heightMeters) || s.map.heightMeters <= 0.0
        || s.map.widthMeters > kMaxMapExtentMeters
        || s.map.heightMeters > kMaxMapExtentMeters) {
        m_lastError = QStringLiteral("场景地图尺寸无效，未应用");
        emit errorOccurred(m_lastError);
        return false;
    }
    if (s.communicationPolicy.format != QLatin1String("native")
        && s.communicationPolicy.format != QLatin1String("vmf-design-v1")) {
        m_lastError = QStringLiteral("通信格式无效，场景未应用");
        emit errorOccurred(m_lastError);
        return false;
    }
    if (!std::isfinite(s.communicationPolicy.ackTimeoutSec)
        || s.communicationPolicy.ackTimeoutSec <= 0.0
        || s.communicationPolicy.maxRetries < 0
        || s.communicationPolicy.maxRetries > 16) {
        m_lastError = QStringLiteral("通信 ACK 策略无效，场景未应用");
        emit errorOccurred(m_lastError);
        return false;
    }
    QString communicationError;
    if (!configureCommunication(s, &communicationError)) {
        m_lastError = communicationError;
        emit errorOccurred(m_lastError);
        return false;
    }
    if (s.units.size() > kMaxScenarioUnits) {
        m_lastError = QStringLiteral("场景单元数量不能超过 %1，未应用").arg(kMaxScenarioUnits);
        emit errorOccurred(m_lastError);
        return false;
    }
    Scenario normalized = s;
    for (ScenarioUnit& unit : normalized.units) {
        unit = normalizeScenarioUnitDefaults(unit);
    }
    QSet<QString> ids;
    QSet<QString> vmfUrns;
    for (const auto& unit : normalized.units) {
        const QString validationError = validateScenarioUnit(unit, normalized.map);
        if (!validationError.isEmpty()) {
            m_lastError = validationError + QStringLiteral("，场景未应用");
            emit errorOccurred(m_lastError);
            return false;
        }
        if (ids.contains(unit.id)) {
            m_lastError = QStringLiteral("单元ID重复: %1，场景未应用").arg(unit.id);
            emit errorOccurred(m_lastError);
            return false;
        }
        ids.insert(unit.id);
        if (vmfUrns.contains(unit.vmfUrn)) {
            m_lastError = QStringLiteral("VMF URN重复: %1，场景未应用").arg(unit.vmfUrn);
            emit errorOccurred(m_lastError);
            return false;
        }
        vmfUrns.insert(unit.vmfUrn);
    }
    setRunning(false);
    m_scenario = std::move(normalized);
    if (m_transport && m_transport->bus()) {
        // Native unit messages retain their existing explicit ACK handlers;
        // VMF envelopes opt into automatic ACK on the gateway itself.
        m_transport->bus()->setAckPolicy(m_scenario.communicationPolicy.ackTimeoutSec,
                                         m_scenario.communicationPolicy.maxRetries, false);
        m_transport->bus()->setSimulationTime(0.0);
    }
    rebuildScenarioIndex();
    m_map->setLogicalSizeMeters(s.map.widthMeters, s.map.heightMeters);
    m_map->setName(s.map.name);
    m_map->setZoom(1.0);
    // Reset clock state in-place: keep speedMul, reset simTime.
    m_clock->setSpeedMul(m_clock->speedMul());
    // Reset simTime by swapping the clock to a fresh one with the same speedMul
    // (simTime is part of clock state; recreating is the simplest correct way
    // without expanding the IClock interface).
    const double carrySpeed = m_clock->speedMul();
    m_clock = std::make_unique<RealTimeClock>();
    m_clock->setSpeedMul(carrySpeed);
    emit speedMulChanged();
    m_messageCache.clear();
    m_lastError.clear();
    m_scanAccum = 0.0;
    m_reportCounter = 0;
    m_outcomeReported = false;
    m_destroyedReported.clear();
    m_cachedDetections.clear();
    m_remoteRuntimeProjection.clear();
    m_pendingCombatRequests.clear();
    m_projectiles.clear();
    m_collisionCooldowns.clear();
    m_scanContacts.clear();
    m_unitIdentityCatalog = {};
    m_unitIdentityOrder.clear();
    do {
        m_battleSeed = QRandomGenerator::system()->generate64();
    } while (m_battleSeed == 0);
    rebuildUnitsFromScenario();
    applyFrozenSidePolicy();
    m_redGuidedStrikeWorkflow.reset();
    m_blueGuidedStrikeWorkflow.reset();
    if (m_vmfGateway) {
        m_redGuidedStrikeWorkflow = std::make_unique<GuidedStrikeWorkflow>(
            m_transport ? m_transport->bus() : nullptr, QStringLiteral("red"),
            m_units.find(QStringLiteral("red_cp")) != m_units.end()
                ? QStringLiteral("red_cp") : QString());
        m_blueGuidedStrikeWorkflow = std::make_unique<GuidedStrikeWorkflow>(
            m_transport ? m_transport->bus() : nullptr, QStringLiteral("blue"),
            m_units.find(QStringLiteral("blue_cp")) != m_units.end()
                ? QStringLiteral("blue_cp") : QString());
        const auto connectWorkflow = [this](GuidedStrikeWorkflow* workflow) {
            if (!workflow) return;
            connect(workflow, &GuidedStrikeWorkflow::stageChanged, this,
                    [this](GuidedStrikeWorkflow::Stage) { emit vmfWorkflowChanged(); });
            connect(workflow, &GuidedStrikeWorkflow::workflowEvent, this,
                    [this](const QJsonObject&) { emit vmfWorkflowChanged(); });
        };
        connectWorkflow(m_redGuidedStrikeWorkflow.get());
        connectWorkflow(m_blueGuidedStrikeWorkflow.get());
    }
    emit vmfWorkflowChanged();
    if (!m_replaying) {
        m_timeline = {};
        m_timelineSequence = 0;
        m_replayCommandSequence = 0;
        m_replayCommands.clear();
        m_replaySteps.clear();
        m_replayCheckpoints.clear();
        m_replayInitialScenario = m_replayRecordingEnabled ? m_scenario : Scenario{};
        m_replayInitialSeed = m_replayRecordingEnabled ? m_battleSeed : 0;
        m_lastReplayCheckpointTime = 0.0;
        m_recordedDuration = 0.0;
        m_recordedFinalSnapshot = m_replayRecordingEnabled
            ? collectAllUnitsSnapshot() : QJsonArray{};
        m_recordedFinalProjectiles = m_replayRecordingEnabled
            ? projectilesSnapshot() : QJsonArray{};
        if (m_replayRecordingEnabled) captureReplayCheckpoint();
        appendTimeline(QStringLiteral("system"), QStringLiteral("场景已加载"),
                       QJsonObject{{QStringLiteral("unitCount"),
                                    static_cast<qint64>(m_units.size())}});
    }
    emit mapChanged();
    emit unitsChanged();
    emit projectilesChanged();
    emit scanContactsChanged();
    emit messagesChanged();
    emit simTimeChanged();
    recomputeReadyForSim();
    return true;
}

void SimulationEngine::rebuildScenarioIndex() {
    m_scenarioIndex.clear();
    for (size_t i = 0; i < m_scenario.units.size(); ++i) {
        m_scenarioIndex.emplace(m_scenario.units[i].id, i);
    }
}

ScenarioUnit* SimulationEngine::findScenarioUnit(const QString& id) {
    auto it = m_scenarioIndex.find(id);
    if (it == m_scenarioIndex.end()) return nullptr;
    return &m_scenario.units[it->second];
}

void SimulationEngine::connectUnitSignals(UnitBase* unit, const QString& id) {
    connect(unit, &UnitBase::notifyEvent, this, [this, id](const QString& t, const QString& b, const QString& lvl){
        appendTimeline(QStringLiteral("unit"), t,
                       QJsonObject{{QStringLiteral("body"), b},
                                   {QStringLiteral("unitId"), id}}, lvl);
        emit eventPosted(t, b, lvl, id);
    });
    connect(unit, &UnitBase::perceptionChanged, this, [this, id]() {
        emit perceptionUpdated(id, unitSnapshot(id));
    });
    connect(unit, &UnitBase::hpChanged, this, [this, id]() {
        recomputeReadyForSim();
        auto* changed = this->unit(id);
        if (!m_inTick && changed && changed->kind() == UnitKind::CommandPost) {
            checkWinLoseCondition();
        }
        markUnitsDirty();
    });
    connect(unit, &UnitBase::runtimeStateChanged, this,
            [this]() { markUnitsDirty(); });
}

void SimulationEngine::rememberUnitIdentity(const ScenarioUnit& unit) {
    if (!m_unitIdentityCatalog.contains(unit.id)) m_unitIdentityOrder.append(unit.id);
    m_unitIdentityCatalog[unit.id] = QJsonObject{
        {QStringLiteral("side"), unit.side},
        {QStringLiteral("callsign"), unit.callsign}};
    constexpr qsizetype kMaxRememberedUnitIdentities = 2048;
    while (m_unitIdentityOrder.size() > kMaxRememberedUnitIdentities) {
        m_unitIdentityCatalog.remove(m_unitIdentityOrder.takeFirst());
    }
}

void SimulationEngine::createSingleUnit(const ScenarioUnit& u) {
    if (u.id.isEmpty()) {
        m_lastError = QStringLiteral("单元ID不能为空");
        emit errorOccurred(m_lastError);
        return;
    }
    if (m_units.find(u.id) != m_units.end()) {
        // Duplicate id: silently dropping (via emplace returning false) would orphan the
        // scenario entry; surface it so the user can fix the JSON.
        m_lastError = QStringLiteral("单元ID重复: %1，已跳过").arg(u.id);
        emit errorOccurred(m_lastError);
        return;
    }
    UnitBase::Params p;
    p.detectRange = u.detectRange;
    p.attackRange = u.attackRange;
    p.commRange = u.commRange;
    p.speed = u.speed;
    p.collisionRadius = u.collisionRadius;
    p.collisionHalfHeight = u.collisionHalfHeight;
    p.maxHp = u.maxHp;
    p.attackPower = u.attackPower;
    p.armor = u.armor;
    p.repairRate = u.repairRate;
    p.subsystemRepairRate = u.subsystemRepairRate;
    p.pos = u.pos;
    auto unit = UnitBase::create(u.id, kindFromName(u.kind), sideFromName(u.side), m_transport->bus(), this);
    if (!unit) {
        m_lastError = QStringLiteral("无法创建单元类型: %1").arg(u.kind);
        emit errorOccurred(m_lastError);
        return;
    }
    rememberUnitIdentity(u);
    const bool isCp = (unit->kind() == UnitKind::CommandPost);
    unit->setCallsign(u.callsign);
    unit->setParams(p);
    if (unit->movable()) {
        unit->configureFuel(std::max(1.0, u.fuelCapacitySec),
                            std::clamp(u.initialFuelSec, 0.0, u.fuelCapacitySec),
                            std::max(1.0, u.speed));
    }
    if (auto* attacker = qobject_cast<AttackUAV*>(unit.get())) attacker->configureWeapon(u);
    unit->setHp(p.maxHp);
    unit->setSchedule(u.schedule);
    connectUnitSignals(unit.get(), u.id);
    unit->setUnitLookup([this](const QString& uid) -> UnitBase* {
        auto it = m_units.find(uid);
        return it != m_units.end() ? it->second.get() : nullptr;
    });
    m_units.emplace(u.id, std::move(unit));
    if (isCp) {
        m_transport->setUnitCommandPost(u.id, true);
    }
}

void SimulationEngine::rebuildUnitsFromScenario() {
    m_units.clear();
    for (const auto& u : m_scenario.units) {
        createSingleUnit(u);
    }
    for (auto& [id, unit] : m_units) {
        unit->setCpId(commandSenderIdFor(unit.get()));
    }
}

void SimulationEngine::setRunning(bool r) {
    if (r && !m_readyForSim) {
        m_lastError = QStringLiteral("场景未就绪，无法开始推演: %1").arg(m_cpIssues);
        emit errorOccurred(m_lastError);
        return;
    }
    if (m_running == r) return;
    m_running = r;
    if (m_running) {
        m_realtimeDebtSeconds = 0.0;
        m_realtimeTick.start();
        m_timer.start();
    } else {
        m_timer.stop();
        m_realtimeTick.invalidate();
        m_realtimeDebtSeconds = 0.0;
    }
    emit runningChanged();
    if (!m_replaying) {
        appendTimeline(QStringLiteral("control"),
                       r ? QStringLiteral("推演开始/继续")
                         : QStringLiteral("推演暂停"));
    }
}

void SimulationEngine::setReplayRecordingEnabled(bool enabled) {
    if (m_replayRecordingEnabled == enabled) return;
    m_replayRecordingEnabled = enabled;
    if (enabled) {
        m_replayInitialScenario = m_scenario;
        m_replayInitialSeed = m_battleSeed;
        m_recordedDuration = simTime();
        m_recordedFinalSnapshot = collectAllUnitsSnapshot();
        m_recordedFinalProjectiles = projectilesSnapshot();
        captureReplayCheckpoint();
        return;
    }
    m_replayInitialScenario = {};
    m_replayInitialSeed = 0;
    m_replayCommands.clear();
    m_replaySteps.clear();
    m_replayCheckpoints.clear();
    m_recordedDuration = 0.0;
    m_recordedFinalSnapshot = {};
    m_recordedFinalProjectiles = {};
}

void SimulationEngine::setSideFrozen(const QString& side, bool frozen) {
    if (side != QLatin1String("red") && side != QLatin1String("blue")) return;
    if (frozen) m_frozenSides.insert(side);
    else m_frozenSides.remove(side);
    applyFrozenSidePolicy();
}

void SimulationEngine::applyFrozenSidePolicy() {
    for (auto& [id, unit] : m_units) {
        Q_UNUSED(id);
        if (!m_frozenSides.contains(unit->sideStr())) continue;
        unit->clearSchedule();
        unit->cancelWaypointMotion();
        unit->cancelService();
        unit->setStatus(QStringLiteral("固定靶"));
    }
}

void SimulationEngine::setSpeedMul(double m) {
    if (!std::isfinite(m)) return;
    m = std::clamp(m, 0.0, 8.0);
    if (m_clock->speedMul() == m) return;
    m_realtimeDebtSeconds = 0.0;
    if (m_running && m_realtimeTick.isValid()) m_realtimeTick.restart();
    m_clock->setSpeedMul(m);
    emit speedMulChanged();
}

void SimulationEngine::stepOnce(double simSeconds) {
    if (m_running || !m_readyForSim || !std::isfinite(simSeconds) || simSeconds <= 0.0) return;
    onTickInternal(true, simSeconds);
}

void SimulationEngine::markUnitsDirty() {
    if (!m_unitsDirty) {
        m_unitsDirty = true;
        if (!m_dirtyTimer.isActive()) m_dirtyTimer.start();
    }
}

void SimulationEngine::flushDirtyUnits() {
    if (m_unitsDirty) {
        m_unitsDirty = false;
        emit unitsChanged();
    }
}

void SimulationEngine::onTickInternal(bool manual, double manualDt) {
    double dt = manualDt;
    if (!manual) {
        if (!m_realtimeTick.isValid()) {
            m_realtimeTick.start();
            return;
        }
        const double wallSeconds = static_cast<double>(m_realtimeTick.nsecsElapsed()) / 1e9;
        m_realtimeTick.restart();
        // A busy projection or durable write must not permanently slow the
        // authoritative clock. Retain bounded debt while limiting each step,
        // so a suspended process cannot jump several seconds in one iteration.
        m_realtimeDebtSeconds = std::min(
            kMaxRealtimeDebtSeconds,
            m_realtimeDebtSeconds + wallSeconds * m_clock->speedMul());
        dt = std::min(kMaxRealtimeStepSeconds, m_realtimeDebtSeconds);
        m_realtimeDebtSeconds = std::max(0.0, m_realtimeDebtSeconds - dt);
    }
    if (!manual && dt <= 0.0) return;
    m_clock->advance(dt);
    if (m_transport && m_transport->bus()) {
        m_transport->bus()->advanceSimulationTime(dt);
    }

    // Resolve all combat in the same simulation step before deciding the
    // winner. Otherwise unordered unit iteration can turn a simultaneous
    // command-post kill into an arbitrary red/blue victory.
    m_inTick = true;
    QHash<QString, GeoPos> previousPositions;
    for (const auto& [id, unit] : m_units) previousPositions.insert(id, unit->pos());
    applySchedules(m_clock->simTime(), dt);
    tickUnits(dt, previousPositions);
    resolveUnitCollisions(dt, previousPositions);
    applyEcmJamming();
    resolveCombatRequests();
    advanceProjectiles(dt, previousPositions);
    advanceServices(dt);
    expireScanContacts();
    scanReconDetections(dt);
    broadcastPositionReports(manual);
    refreshDetectionCache();
    m_inTick = false;

    checkWinLoseCondition();

    if (!m_replaying && m_replayRecordingEnabled) {
        m_replaySteps.push_back(ReplayStep{simTime() - dt, dt});
        m_recordedDuration = std::max(m_recordedDuration, simTime());
        m_recordedFinalSnapshot = collectAllUnitsSnapshot();
        m_recordedFinalProjectiles = projectilesSnapshot();
        if (simTime() - m_lastReplayCheckpointTime >= 10.0) captureReplayCheckpoint();
    }

    emit simTimeChanged();
    markUnitsDirty();
}

void SimulationEngine::tickUnits(double dt,
                                 const QHash<QString, GeoPos>& previousPositions) {
    for (auto& [id, u] : m_units) {
        // Remote-owned units are mirrored state; the local process does not
        // tick them — the peer does, and we receive the new state via the
        // transport. Single-process mode never has Remote units.
        if (u->owner() == UnitOwner::Remote || m_frozenSides.contains(u->sideStr())) continue;
        u->onTick(dt);
        if (auto* attacker = qobject_cast<AttackUAV*>(u.get())) {
            if (auto request = attacker->takePendingShot()) {
                m_pendingCombatRequests.push_back(std::move(*request));
            }
        }
        const GeoPos before = previousPositions.value(id, u->pos());
        const double actualSpeed = dt > 0.0 ? before.distanceTo2D(u->pos()) / dt : 0.0;
        u->advanceRuntimeState(dt, actualSpeed);
        // Don't sample path for dead units — they don't move and we'd be
        // pushing the same position into recentPath forever.
        if (u->alive()) u->sampleRecentPath(m_clock->simTime());
    }
}

void SimulationEngine::resolveUnitCollisions(
    double dt, const QHash<QString, GeoPos>& previousPositions) {
    if (!std::isfinite(dt) || dt <= 0.0 || m_units.size() < 2) return;

    // Cooldowns are deliberately advanced before pair evaluation so a
    // restored checkpoint behaves exactly like an uninterrupted simulation.
    for (auto it = m_collisionCooldowns.begin(); it != m_collisionCooldowns.end();) {
        it.value() = std::max(0.0, it.value() - dt);
        if (it.value() <= 1e-9) it = m_collisionCooldowns.erase(it);
        else ++it;
    }

    struct PendingImpact {
        UnitBase* first = nullptr;
        UnitBase* second = nullptr;
        double damageToFirst = 0.0;
        double damageToSecond = 0.0;
        QPointF normal;
        QPointF contact;
        double relativeSpeed = 0.0;
        bool friendly = false;
    };
    std::vector<PendingImpact> impacts;
    std::vector<UnitBase*> units;
    units.reserve(m_units.size());
    for (const auto& [id, value] : m_units) {
        Q_UNUSED(id);
        if (value->alive()) units.push_back(value.get());
    }

    const auto layerOverlap = [](const UnitBase* a, const UnitBase* b) {
        if (UnitBase::isGroundCollisionLayer(a->kind())
            && UnitBase::isGroundCollisionLayer(b->kind())) {
            return true;
        }
        return std::abs(a->pos().alt - b->pos().alt)
            <= a->collisionHalfHeight() + b->collisionHalfHeight();
    };
    const auto pairKey = [](const UnitBase* a, const UnitBase* b) {
        return a->id() < b->id() ? a->id() + QLatin1Char('|') + b->id()
                                 : b->id() + QLatin1Char('|') + a->id();
    };

    for (size_t i = 0; i < units.size(); ++i) {
        UnitBase* first = units[i];
        for (size_t j = i + 1; j < units.size(); ++j) {
            UnitBase* second = units[j];
            if (!layerOverlap(first, second)) continue;

            const GeoPos firstPrevious = previousPositions.value(first->id(), first->pos());
            const GeoPos secondPrevious = previousPositions.value(second->id(), second->pos());
            const QPointF start(firstPrevious.x - secondPrevious.x,
                                firstPrevious.y - secondPrevious.y);
            const QPointF firstDelta(first->pos().x - firstPrevious.x,
                                     first->pos().y - firstPrevious.y);
            const QPointF secondDelta(second->pos().x - secondPrevious.x,
                                      second->pos().y - secondPrevious.y);
            const QPointF relativeDelta(firstDelta.x() - secondDelta.x(),
                                         firstDelta.y() - secondDelta.y());
            // Units placed at the same deployment point are not considered
            // colliding until at least one body moves. This keeps a static
            // service/deployment overlap stable while still catching every
            // swept crossing during simulation.
            if (std::hypot(relativeDelta.x(), relativeDelta.y()) <= 1e-6) continue;
            const double deltaSquared = QPointF::dotProduct(relativeDelta, relativeDelta);
            double contactT = 0.0;
            if (deltaSquared > 1e-9) {
                contactT = std::clamp(
                    -QPointF::dotProduct(start, relativeDelta) / deltaSquared, 0.0, 1.0);
            }
            const QPointF closest(start.x() + relativeDelta.x() * contactT,
                                  start.y() + relativeDelta.y() * contactT);
            const double radius = first->collisionRadius() + second->collisionRadius();
            const double distance = std::hypot(closest.x(), closest.y());
            if (distance > radius + 1e-6) continue;

            QPointF normal(closest.x(), closest.y());
            if (std::hypot(normal.x(), normal.y()) <= 1e-6) {
                normal = QPointF(first->pos().x - second->pos().x,
                                 first->pos().y - second->pos().y);
            }
            if (std::hypot(normal.x(), normal.y()) <= 1e-6) {
                // Stable fallback for exact co-location, independent of map
                // iteration order or pointer addresses.
                normal = (first->id() < second->id())
                    ? QPointF(1.0, 0.0) : QPointF(-1.0, 0.0);
            }
            const double normalLength = std::hypot(normal.x(), normal.y());
            normal /= normalLength;

            const QPointF firstVelocity(firstDelta.x() / dt, firstDelta.y() / dt);
            const QPointF secondVelocity(secondDelta.x() / dt, secondDelta.y() / dt);
            const QPointF relativeVelocity(firstVelocity.x() - secondVelocity.x(),
                                            firstVelocity.y() - secondVelocity.y());
            const double closingSpeed = std::max(
                0.0, -QPointF::dotProduct(relativeVelocity, normal));
            const double relativeSpeed = std::hypot(relativeVelocity.x(), relativeVelocity.y());
            const QString key = pairKey(first, second);
            const bool damageReady = !m_collisionCooldowns.contains(key)
                || m_collisionCooldowns.value(key) <= 1e-9;

            PendingImpact impact;
            impact.first = first;
            impact.second = second;
            impact.normal = normal;
            impact.contact = QPointF(second->pos().x + normal.x() * second->collisionRadius(),
                                     second->pos().y + normal.y() * second->collisionRadius());
            impact.relativeSpeed = relativeSpeed;
            impact.friendly = first->side() == second->side();
            if (damageReady) {
                const double severity = std::clamp(
                    std::max(0.25, (closingSpeed - 2.0) / 20.0), 0.25, 3.0);
                const double base = 8.0 * severity;
                const double firstDamage = std::min(
                    impact.friendly ? first->maxHp() * 0.10 : first->maxHp() * 0.35,
                    base * UnitBase::collisionImpactPower(second->kind())
                        / UnitBase::collisionResistance(first->kind())
                        * (impact.friendly ? 0.25 : 1.0));
                const double secondDamage = std::min(
                    impact.friendly ? second->maxHp() * 0.10 : second->maxHp() * 0.35,
                    base * UnitBase::collisionImpactPower(first->kind())
                        / UnitBase::collisionResistance(second->kind())
                        * (impact.friendly ? 0.25 : 1.0));
                impact.damageToFirst = firstDamage;
                impact.damageToSecond = secondDamage;
                m_collisionCooldowns.insert(key, 0.75);
            }
            impacts.push_back(impact);
        }
    }

    for (const PendingImpact& impact : impacts) {
        UnitBase* first = impact.first;
        UnitBase* second = impact.second;
        if (!first->alive() || !second->alive()) continue;

        // Resolve overlap after recording the contact. Moving both bodies is
        // deterministic and prevents a route from repeatedly tunnelling
        // through the same unit on subsequent 50ms ticks.
        const double currentDistance = first->pos().distanceTo2D(second->pos());
        const double minimumDistance = first->collisionRadius() + second->collisionRadius();
        if (currentDistance < minimumDistance - 1e-6) {
            const double correction = minimumDistance - currentDistance + 0.01;
            const bool firstCanMove = first->movable()
                && !m_frozenSides.contains(first->sideStr());
            const bool secondCanMove = second->movable()
                && !m_frozenSides.contains(second->sideStr());
            const double firstShare = firstCanMove && secondCanMove ? 0.5
                : (firstCanMove ? 1.0 : 0.0);
            const double secondShare = secondCanMove && firstCanMove ? 0.5
                : (secondCanMove ? 1.0 : 0.0);
            const GeoPos firstPos = first->pos();
            const GeoPos secondPos = second->pos();
            if (firstCanMove) first->setPosition(GeoPos{
                firstPos.x + impact.normal.x() * correction * firstShare,
                firstPos.y + impact.normal.y() * correction * firstShare,
                firstPos.alt});
            if (secondCanMove) second->setPosition(GeoPos{
                secondPos.x - impact.normal.x() * correction * secondShare,
                secondPos.y - impact.normal.y() * correction * secondShare,
                secondPos.alt});
        }
        if (impact.damageToFirst > 0.0 || impact.damageToSecond > 0.0) {
            first->applyDamageDelta(first->assessDamage(impact.damageToFirst, 2));
            second->applyDamageDelta(second->assessDamage(impact.damageToSecond, 2));
            appendTimeline(QStringLiteral("collision"), QStringLiteral("单位碰撞"),
                           QJsonObject{{QStringLiteral("unitAId"), first->id()},
                                       {QStringLiteral("unitBId"), second->id()},
                                       {QStringLiteral("damageToA"), impact.damageToFirst},
                                       {QStringLiteral("damageToB"), impact.damageToSecond},
                                       {QStringLiteral("relativeSpeed"), impact.relativeSpeed},
                                       {QStringLiteral("friendly"), impact.friendly},
                                       {QStringLiteral("x"), impact.contact.x()},
                                       {QStringLiteral("y"), impact.contact.y()}},
                           impact.friendly ? QStringLiteral("info")
                                            : QStringLiteral("warn"));
        }
    }
}

void SimulationEngine::resolveCombatRequests() {
    if (m_pendingCombatRequests.empty()) return;
    std::sort(m_pendingCombatRequests.begin(), m_pendingCombatRequests.end(),
              [](const CombatRequest& left, const CombatRequest& right) {
                  if (left.attackerId != right.attackerId) {
                      return left.attackerId < right.attackerId;
                  }
                  return left.shotSequence < right.shotSequence;
              });

    bool changed = false;
    for (const CombatRequest& request : m_pendingCombatRequests) {
        auto* attacker = qobject_cast<AttackUAV*>(unit(request.attackerId));
        UnitBase* target = unit(request.targetId);
        if (!attacker || !attacker->alive() || !isHostileTarget(attacker, target)) {
            if (attacker) attacker->rejectProjectileLaunch(QStringLiteral("invalid_target"));
            continue;
        }
        if (activeProjectileCount() >= kMaximumProjectiles) {
            attacker->rejectProjectileLaunch(QStringLiteral("projectile_limit"));
            continue;
        }
        ProjectileState projectile;
        projectile.id = QStringLiteral("%1:%2").arg(request.attackerId)
                            .arg(request.shotSequence);
        projectile.attackerId = request.attackerId;
        projectile.targetId = request.targetId;
        projectile.side = attacker->side();
        projectile.position = attacker->pos();
        projectile.previousPosition = projectile.position;
        projectile.headingRad = std::atan2(target->pos().y - attacker->pos().y,
                                           target->pos().x - attacker->pos().x);
        projectile.launchTime = simTime();
        projectile.request = request;
        const auto [_, inserted] = m_projectiles.emplace(projectile.id, projectile);
        if (!inserted) {
            attacker->rejectProjectileLaunch(QStringLiteral("duplicate_projectile"));
            continue;
        }
        attacker->markProjectileLaunched(projectile.id);
        appendTimeline(QStringLiteral("projectile"), QStringLiteral("导弹发射"),
                       QJsonObject{{QStringLiteral("shotId"), projectile.id},
                                   {QStringLiteral("attackerId"), projectile.attackerId},
                                   {QStringLiteral("targetId"), projectile.targetId}});
        changed = true;
    }
    m_pendingCombatRequests.clear();
    if (changed) emit projectilesChanged();
}

void SimulationEngine::advanceProjectiles(
    double dt, const QHash<QString, GeoPos>& previousPositions) {
    if (!std::isfinite(dt) || dt <= 0.0 || m_projectiles.empty()) return;

    bool changed = false;
    QStringList terminalIds;
    for (auto it = m_projectiles.begin(); it != m_projectiles.end();) {
        if (!it->second.active()) {
            if (!it->second.resultSettled) {
                terminalIds.append(it->first);
            } else {
                it->second.terminalAge += dt;
            }
            if (it->second.resultSettled && it->second.terminalAge >= 0.75) {
                it = m_projectiles.erase(it);
                changed = true;
                continue;
            }
        }
        ++it;
    }

    auto normalizeAngle = [](double angle) {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kTwoPi = 2.0 * kPi;
        while (angle > kPi) angle -= kTwoPi;
        while (angle < -kPi) angle += kTwoPi;
        return angle;
    };
    auto sweptDistance = [](const GeoPos& missileStart, const GeoPos& missileEnd,
                            const GeoPos& targetStart, const GeoPos& targetEnd) {
        const double rx = missileStart.x - targetStart.x;
        const double ry = missileStart.y - targetStart.y;
        const double vx = (missileEnd.x - missileStart.x)
            - (targetEnd.x - targetStart.x);
        const double vy = (missileEnd.y - missileStart.y)
            - (targetEnd.y - targetStart.y);
        const double vv = vx * vx + vy * vy;
        const double t = vv > 1e-12
            ? std::clamp(-(rx * vx + ry * vy) / vv, 0.0, 1.0) : 0.0;
        return std::hypot(rx + vx * t, ry + vy * t);
    };

    for (auto& [id, projectile] : m_projectiles) {
        if (!projectile.active()) continue;
        UnitBase* target = unit(projectile.targetId);
        if (!target || !target->alive() || target->side() == projectile.side) {
            projectile.terminalReason = QStringLiteral("target_lost");
            projectile.terminalAge = 0.0;
            terminalIds.append(id);
            changed = true;
            continue;
        }

        projectile.previousPosition = projectile.position;
        const double desiredHeading = std::atan2(target->pos().y - projectile.position.y,
                                                 target->pos().x - projectile.position.x);
        const double maxTurn = projectile.speed / kProjectileTurnRadiusMeters * dt;
        projectile.headingRad += std::clamp(
            normalizeAngle(desiredHeading - projectile.headingRad), -maxTurn, maxTurn);
        projectile.headingRad = normalizeAngle(projectile.headingRad);
        projectile.position.x += std::cos(projectile.headingRad) * projectile.speed * dt;
        projectile.position.y += std::sin(projectile.headingRad) * projectile.speed * dt;
        projectile.age += dt;

        const bool launchedThisTick = std::abs(projectile.launchTime - simTime()) <= 1e-9;
        const GeoPos targetStart = launchedThisTick
            ? target->pos() : previousPositions.value(projectile.targetId, target->pos());
        if (sweptDistance(projectile.previousPosition, projectile.position,
                          targetStart, target->pos()) <= kProjectileCollisionRadiusMeters) {
            projectile.terminalReason = QStringLiteral("contact");
        } else if (projectile.position.x < 0.0 || projectile.position.y < 0.0
                   || projectile.position.x > m_scenario.map.widthMeters
                   || projectile.position.y > m_scenario.map.heightMeters) {
            projectile.terminalReason = QStringLiteral("out_of_bounds");
        } else if (projectile.age + 1e-9 >= projectile.lifetime) {
            projectile.terminalReason = QStringLiteral("expired");
        }
        if (!projectile.active()) {
            projectile.terminalAge = 0.0;
            terminalIds.append(id);
        }
        changed = true;
    }

    settleTerminalProjectiles(terminalIds);
    if (!terminalIds.isEmpty()) changed = true;
    if (changed) emit projectilesChanged();
}

void SimulationEngine::settleTerminalProjectiles(const QStringList& projectileIds) {
    if (projectileIds.isEmpty()) return;

    QStringList orderedIds = projectileIds;
    orderedIds.removeDuplicates();
    orderedIds.sort();
    struct ResolvedProjectile {
        QString projectileId;
        AttackUAV* attacker = nullptr;
        CombatOutcome outcome;
        bool killCredit = false;
    };
    QHash<QString, double> initialHp;
    QHash<QString, UnitBase::DamageDelta> accumulatedDamage;
    std::vector<ResolvedProjectile> resolved;
    resolved.reserve(orderedIds.size());

    for (const QString& projectileId : orderedIds) {
        auto projectileIt = m_projectiles.find(projectileId);
        if (projectileIt == m_projectiles.end()) continue;
        ProjectileState& projectile = projectileIt->second;
        if (projectile.active() || projectile.resultSettled) continue;
        auto* attacker = qobject_cast<AttackUAV*>(unit(projectile.attackerId));
        CombatOutcome outcome;
        outcome.shotId = projectile.id;
        outcome.attackerId = projectile.attackerId;
        outcome.targetId = projectile.targetId;
        outcome.shotSequence = projectile.request.shotSequence;
        outcome.distance = projectile.request.distance;

        UnitBase* target = unit(projectile.targetId);
        if (projectile.terminalReason != QLatin1String("contact")
            || !target || !target->alive()) {
            outcome.result = projectile.terminalReason == QLatin1String("contact")
                ? QStringLiteral("target_lost") : projectile.terminalReason;
            if (target) outcome.hpBefore = outcome.hpAfter = target->hp();
            projectile.terminalReason = outcome.result;
            projectile.resultSettled = true;
            resolved.push_back({projectileId, attacker, outcome, false});
            continue;
        }

        CombatRequest request = projectile.request;
        outcome = CombatResolver::resolve(request, m_battleSeed);
        projectile.terminalReason = outcome.result;
        projectile.resultSettled = true;

        if (!outcome.hit()) {
            outcome.hpBefore = outcome.hpAfter = target->hp();
            resolved.push_back({projectileId, attacker, outcome, false});
            continue;
        }

        if (!initialHp.contains(projectile.targetId)) {
            initialHp.insert(projectile.targetId, target->hp());
        }
        UnitBase::DamageDelta& total = accumulatedDamage[projectile.targetId];
        outcome.hpBefore = std::max(0.0, initialHp.value(projectile.targetId)
                                             - total.hullDamage);
        int subsystemIndex = static_cast<int>(request.shotSequence % 4ULL);
        for (const char ch : request.attackerId.toUtf8()) {
            subsystemIndex += static_cast<unsigned char>(ch);
        }
        UnitBase::DamageDelta delta = target->assessDamage(outcome.damage, subsystemIndex);
        const double assessedHullDamage = delta.hullDamage;
        const double appliedDamage = std::min(assessedHullDamage, outcome.hpBefore);
        const double appliedFraction = assessedHullDamage > 0.0
            ? appliedDamage / assessedHullDamage : 0.0;
        delta.hullDamage = appliedDamage;
        delta.sensorLoss *= appliedFraction;
        delta.commsLoss *= appliedFraction;
        delta.mobilityLoss *= appliedFraction;
        delta.weaponLoss *= appliedFraction;
        const QJsonObject subsystemState = target->subsystemStateJson();
        const auto capLoss = [](double loss, double health, double accumulated) {
            return std::min(loss, std::max(0.0, health - accumulated));
        };
        delta.sensorLoss = capLoss(delta.sensorLoss,
                                   subsystemState.value(QStringLiteral("sensor")).toDouble(),
                                   total.sensorLoss);
        delta.commsLoss = capLoss(delta.commsLoss,
                                  subsystemState.value(QStringLiteral("comms")).toDouble(),
                                  total.commsLoss);
        delta.mobilityLoss = capLoss(delta.mobilityLoss,
                                     subsystemState.value(QStringLiteral("mobility")).toDouble(),
                                     total.mobilityLoss);
        delta.weaponLoss = capLoss(delta.weaponLoss,
                                   subsystemState.value(QStringLiteral("weapon")).toDouble(),
                                   total.weaponLoss);
        outcome.damage = appliedDamage;
        outcome.hpAfter = std::max(0.0, outcome.hpBefore - appliedDamage);
        const bool killCredit = outcome.hpBefore > 0.0 && outcome.hpAfter <= 0.0;
        total.hullDamage += delta.hullDamage;
        total.sensorLoss += delta.sensorLoss;
        total.commsLoss += delta.commsLoss;
        total.mobilityLoss += delta.mobilityLoss;
        total.weaponLoss += delta.weaponLoss;
        resolved.push_back({projectileId, attacker, outcome, killCredit});
    }

    QStringList damagedIds = accumulatedDamage.keys();
    damagedIds.sort();
    for (const QString& targetId : damagedIds) {
        if (UnitBase* target = unit(targetId); target && target->alive()) {
            target->applyDamageDelta(accumulatedDamage.value(targetId));
        }
    }
    for (ResolvedProjectile& result : resolved) {
        if (result.attacker) result.attacker->applyCombatOutcome(result.outcome,
                                                                 result.killCredit);
        appendTimeline(QStringLiteral("combat"),
                       result.outcome.hit() ? QStringLiteral("导弹命中")
                                            : QStringLiteral("攻击无效"),
                       QJsonObject{{QStringLiteral("shotId"), result.outcome.shotId},
                                   {QStringLiteral("attackerId"), result.outcome.attackerId},
                                   {QStringLiteral("targetId"), result.outcome.targetId},
                                   {QStringLiteral("result"), result.outcome.result},
                                   {QStringLiteral("hit"), result.outcome.hit()},
                                   {QStringLiteral("damage"), result.outcome.damage},
                                   {QStringLiteral("kill"), result.killCredit}},
                       result.killCredit ? QStringLiteral("warn") : QStringLiteral("info"));
    }
}

void SimulationEngine::advanceServices(double dt) {
    for (auto& [id, controlled] : m_units) {
        if (m_frozenSides.contains(controlled->sideStr())) continue;
        if (!controlled->serviceRequested()) continue;
        UnitBase* cp = unit(controlled->serviceCpId());
        const bool eligible = controlled->alive() && cp && cp->alive()
            && cp->kind() == UnitKind::CommandPost && cp->side() == controlled->side()
            && controlled->pos().distanceTo2D(cp->pos()) <= kServiceRadiusMeters;
        if (!eligible) {
            controlled->cancelService();
            controlled->setStatus(QStringLiteral("补充已中断"));
            appendTimeline(QStringLiteral("service"), QStringLiteral("补充中断"),
                           QJsonObject{{QStringLiteral("unitId"), id}});
            continue;
        }
        if (controlled->advanceService(dt)) {
            controlled->setStatus(QStringLiteral("补充完成"));
            appendTimeline(QStringLiteral("service"), QStringLiteral("补充完成"),
                           QJsonObject{{QStringLiteral("unitId"), id}},
                           QStringLiteral("success"));
        } else {
            controlled->setStatus(QStringLiteral("补充中 %1%")
                                      .arg(qRound(controlled->serviceProgress() * 100.0)));
        }
    }
}

void SimulationEngine::expireScanContacts() {
    const auto previousSize = m_scanContacts.size();
    std::erase_if(m_scanContacts, [this](const ScanContact& contact) {
        const UnitBase* scanner = unit(contact.scannerId);
        const UnitBase* target = unit(contact.targetId);
        return contact.expiresAt <= simTime() || !scanner || !scanner->alive()
            || !target || !target->alive();
    });
    if (m_scanContacts.size() != previousSize) emit scanContactsChanged();
}

void SimulationEngine::applyEcmJamming() {
    constexpr double kJammedFactor = 0.5;
    std::vector<std::pair<QString, UnitBase*>> jammers;
    jammers.reserve(m_units.size());
    for (auto& [id, unit] : m_units) {
        if (unit->kind() == UnitKind::JammerUAV && unit->alive()
            && !m_frozenSides.contains(unit->sideStr())) {
            jammers.emplace_back(id, unit.get());
        }
    }

    // Resolve jammer-on-jammer effects from base ranges as one simultaneous
    // phase. This avoids unordered_map iteration order deciding which jammer
    // gets to transmit at full range first.
    QSet<QString> jammedJammers;
    for (const auto& [sourceId, source] : jammers) {
        for (const auto& [targetId, target] : jammers) {
            if (sourceId == targetId || source->side() == target->side()) continue;
            if (source->pos().distanceTo2D(target->pos()) <= source->baseDetectRange()) {
                jammedJammers.insert(targetId);
            }
        }
    }

    std::unordered_map<QString, double> desiredFactors;
    desiredFactors.reserve(m_units.size());
    for (const auto& [id, _] : m_units) {
        desiredFactors.emplace(id, jammedJammers.contains(id) ? kJammedFactor : 1.0);
    }

    // A jammed jammer uses its reduced emission range against ordinary units.
    for (const auto& [sourceId, source] : jammers) {
        const double sourceFactor = jammedJammers.contains(sourceId) ? kJammedFactor : 1.0;
        const double jamRange = source->baseDetectRange() * sourceFactor;
        for (const auto& [targetId, target] : m_units) {
            if (!target->alive() || target->kind() == UnitKind::JammerUAV
                || target->side() == source->side()) {
                continue;
            }
            if (source->pos().distanceTo2D(target->pos()) <= jamRange) {
                desiredFactors[targetId] = kJammedFactor;
            }
        }
    }

    // Apply each result once so a continuously jammed unit does not emit a
    // reset-to-1.0 update followed by a second update on every tick.
    for (auto& [id, unit] : m_units) unit->applyJamming(desiredFactors[id]);
}

void SimulationEngine::scanReconDetections(double dt) {
    m_scanAccum += dt;
    constexpr double kScanInterval = 1.5;
    if (m_scanAccum < kScanInterval) return;
    m_scanAccum = 0.0;

    for (auto& [reconId, recon] : m_units) {
        if (recon->kind() != UnitKind::ReconUAV) continue;
        if (!recon->alive()) continue;
        if (m_frozenSides.contains(recon->sideStr())) continue;
        const auto center = recon->pos();
        const double dr = recon->detectRange();
        const QString myCpId = commandSenderIdFor(recon.get());
        for (auto& [tid, target] : m_units) {
            if (target->side() == recon->side()) continue;
            if (!target->alive()) continue;
            const double d = center.distanceTo2D(target->pos());
            if (d > dr) continue;
            if (!myCpId.isEmpty()) {
                Message dm;
                dm.type = Message::Type::TargetDetect;
                dm.sender = reconId;
                dm.receiver = myCpId;
                dm.requiresAck = true;
                dm.payload["targetId"] = tid;
                dm.payload["callsign"] = target->callsign();
                dm.payload["x"] = target->pos().x;
                dm.payload["y"] = target->pos().y;
                dm.payload["alt"] = target->pos().alt;
                dm.payload["distance"] = d;
                m_transport->send(dm);
            }
            // Communication range, not sensor range, determines which allies
            // receive the shared track. One broadcast also avoids one logged
            // message per ally.
            Message sm;
            sm.type = Message::Type::SharedDetect;
            sm.sender = reconId;
            sm.receiver = "*";
            sm.payload["targetId"] = tid;
            sm.payload["callsign"] = target->callsign();
            sm.payload["x"] = target->pos().x;
            sm.payload["y"] = target->pos().y;
            sm.payload["alt"] = target->pos().alt;
            sm.payload["distance"] = d;
            m_transport->send(sm);
        }
    }
}

void SimulationEngine::broadcastPositionReports(bool manual) {
    m_reportCounter++;
    if (m_reportCounter % 4 != 0 && !manual) return;
    for (auto& [id, u] : m_units) {
        if (!u->alive()) continue;
        if (m_frozenSides.contains(u->sideStr())) continue;
        Message m;
        m.type = Message::Type::PositionReport;
        m.sender = id;
        m.receiver = "*";
        m.payload["x"] = u->pos().x;
        m.payload["y"] = u->pos().y;
        m.payload["alt"] = u->pos().alt;
        m.payload["side"] = u->sideStr();
        m_transport->send(m);
    }
}

void SimulationEngine::refreshDetectionCache() {
    m_cachedDetections.clear();
    for (const auto& [id, u] : m_units) {
        if (!u->alive()) {
            m_cachedDetections[id] = {};
            continue;
        }
        QJsonArray dets;
        const auto center = u->pos();
        const double dr = u->detectRange();
        for (const auto& [oid, ou] : m_units) {
            if (oid == id) continue;
            if (ou->side() == u->side()) continue;
            if (!ou->alive()) continue;
            const double d = center.distanceTo2D(ou->pos());
            if (d <= dr) {
                QJsonObject det;
                det["id"] = oid;
                det["callsign"] = ou->callsign();
                det["kind"] = ou->kindStr();
                det["side"] = ou->sideStr();
                det["distance"] = d;
                QJsonArray p;
                p.append(ou->pos().x);
                p.append(ou->pos().y);
                p.append(ou->pos().alt);
                det["position"] = p;
                dets.append(det);
            }
        }
        m_cachedDetections[id] = dets;
    }
}

void SimulationEngine::applySchedules(double simTime, double dt) {
    for (auto& [id, u] : m_units) {
        if (m_frozenSides.contains(u->sideStr())) continue;
        if (!u->movable()) continue;
        if (!u->alive()) continue;
        if (!u->hasUsableFuel() || u->serviceRequested()) continue;
        if (u->hasActiveWaypoints()) continue;
        const auto& sched = u->schedule();
        if (sched.empty()) continue;

        GeoPos target;
        if (simTime <= sched.front().time) {
            target = GeoPos{sched.front().x, sched.front().y, u->pos().alt};
        } else if (simTime >= sched.back().time) {
            target = GeoPos{sched.back().x, sched.back().y, u->pos().alt};
        } else {
            for (size_t i = 0; i + 1 < sched.size(); i++) {
                const auto& a = sched[i];
                const auto& b = sched[i + 1];
                if (simTime >= a.time && simTime <= b.time) {
                    const double span = b.time - a.time;
                    const double t = span > 1e-6 ? (simTime - a.time) / span : 1.0;
                    target = GeoPos{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, u->pos().alt};
                    break;
                }
            }
        }

        const double dist = u->pos().distanceTo2D(target);
        constexpr double kSnapThreshold = 1.0;

        if (dist <= kSnapThreshold) {
            u->setPosition(target);
        } else {
            const double t = std::min(1.0, u->speed() * dt / std::max(1.0, dist));
            const GeoPos lerped = u->pos().lerp(target, t);
            u->setPosition(lerped);
        }
    }
}

void SimulationEngine::onMessagePosted(const QJsonObject& msg) {
    updateMessageCache(msg);
    if (m_recorder) m_recorder->record(msg);
    emit messagesChanged();

    if (msg.value("type").toString() == "TargetDestroyed") {
        const QString targetId = msg.value("payload").toObject().value("targetId").toString();
        const QString attackerId = msg.value("payload").toObject().value("attackerId").toString();
        if (targetId.isEmpty() || m_destroyedReported.contains(targetId)) return;
        auto it = m_units.find(targetId);
        // messagePosted also contains messages that failed communication
        // checks. A notification must never become an authoritative damage
        // command; local combat applies HP before posting this event.
        if (it != m_units.end() && !it->second->alive()) {
            m_destroyedReported.insert(targetId);
            const double dx = it->second->pos().x;
            const double dy = it->second->pos().y;
            emit targetDestroyedVisual(targetId, dx, dy);
            emit unitDestroyed(targetId);
            QString targetSide = it->second->sideStr();
            QString sideLabel = (targetSide == "red") ? QString::fromUtf8("红方") : QString::fromUtf8("蓝方");
            emit eventPosted(
                QString::fromUtf8("单元被摧毁"),
                QString::fromUtf8("%1单元 %2 被 %3 摧毁").arg(sideLabel, targetId, attackerId),
                "warn",
                targetId
            );
            sendReconDestructionConfirmations(targetId, attackerId);
            recomputeReadyForSim();
        }
    }
}

void SimulationEngine::sendReconDestructionConfirmations(const QString& targetId,
                                                          const QString& attackerId) {
    // The design scenario requires the reconnaissance aircraft to confirm a
    // kill before the command post orders withdrawal.  This is an observation
    // message only: combat has already set the target HP to zero, and the
    // message cannot create or alter damage.
    if (!m_vmfGateway || !m_transport || !m_transport->bus()
        || targetId.isEmpty() || attackerId.isEmpty()) return;
    UnitBase* target = unit(targetId);
    UnitBase* attacker = unit(attackerId);
    if (!target || !attacker || !attacker->alive()
        || target->side() == attacker->side()) return;
    const QString commandPostId = commandSenderIdFor(attacker);
    UnitBase* commandPost = unit(commandPostId);
    if (!commandPost || !commandPost->alive()) return;
    const GuidedStrikeWorkflow* workflow = guidedStrikeWorkflow(attacker->sideStr());
    const QString correlationId = workflow ? workflow->correlationId() : QString();
    const QString registeredReconId = workflow ? workflow->reconId() : QString();

    for (const auto& [id, candidate] : m_units) {
        if (!candidate || candidate->kind() != UnitKind::ReconUAV
            || candidate->side() != attacker->side() || !candidate->alive()) continue;
        if (!registeredReconId.isEmpty() && id != registeredReconId) continue;
        if (candidate->pos().distanceTo2D(target->pos()) > candidate->detectRange()) continue;
        if (!m_transport->canCommunicate(candidate->id(), commandPostId)) continue;

        Message confirmation;
        confirmation.type = Message::Type::TargetDestroyed;
        confirmation.sender = candidate->id();
        confirmation.receiver = commandPostId;
        confirmation.correlationId = correlationId;
        confirmation.requiresAck = true;
        confirmation.automaticAck = true;
        confirmation.payload = QJsonObject{
            {QStringLiteral("targetId"), targetId},
            {QStringLiteral("attackerId"), attackerId},
            {QStringLiteral("x"), target->pos().x},
            {QStringLiteral("y"), target->pos().y},
            {QStringLiteral("alt"), target->pos().alt},
            {QStringLiteral("targetType"), target->kindStr()},
            {QStringLiteral("targetCount"), 1},
            {QStringLiteral("friendFoe"), QStringLiteral("enemy")},
            {QStringLiteral("status"), QStringLiteral("destroyed")},
            {QStringLiteral("confirmationSource"), QStringLiteral("recon-observation")}};
        m_transport->send(confirmation);
        // One authoritative observation is sufficient.  A second recon
        // aircraft must not create duplicate workflow edges or ACK traffic.
        break;
    }
}

void SimulationEngine::recomputeReadyForSim() {
    int redLive = 0, blueLive = 0;
    int redDead = 0, blueDead = 0;
    QStringList issueList;
    QString redFirstDead, blueFirstDead;
    for (const auto& [id, u] : m_units) {
        if (u->kind() != UnitKind::CommandPost) continue;
        if (!u->alive()) {
            if (u->side() == Side::Red) { redDead++; if (redFirstDead.isEmpty()) redFirstDead = id; }
            else { blueDead++; if (blueFirstDead.isEmpty()) blueFirstDead = id; }
            issueList << id + " (已摧毁)";
            continue;
        }
        if (u->side() == Side::Red) redLive++;
        else if (u->side() == Side::Blue) blueLive++;
    }
    if (redLive == 0) {
        if (redDead > 0) issueList << "红方指挥所已全部被摧毁";
        else issueList << "红方缺失指挥所";
    } else if (redLive > 1) issueList << "红方指挥所重复 (x" + QString::number(redLive) + ")";
    if (blueLive == 0) {
        if (blueDead > 0) issueList << "蓝方指挥所已全部被摧毁";
        else issueList << "蓝方缺失指挥所";
    } else if (blueLive > 1) issueList << "蓝方指挥所重复 (x" + QString::number(blueLive) + ")";
    const bool ok = (redLive == 1) && (blueLive == 1);
    const QString issues = issueList.join("; ");
    if (ok != m_readyForSim || issues != m_cpIssues) {
        const bool wasReady = m_readyForSim;
        m_readyForSim = ok;
        m_cpIssues = issues;
        if (!wasReady && ok) m_outcomeReported = false;
        emit readyForSimChanged();
        // CP-death path is owned by checkWinLoseCondition() (it emits
        // simulationEnded once and stops once). For non-death readiness loss
        // (duplicate CP, missing CP via addOrUpdateUnit), pause here so the
        // user notices the broken state.
        if (wasReady && !ok && m_running && !issues.contains(QStringLiteral("已全部被摧毁"))) {
            setRunning(false);
        }
    }
}

void SimulationEngine::updateMessageCache(const QJsonObject& msg) {
    m_messageCache.prepend(msg);
    if (m_messageCache.size() > 200) m_messageCache.removeLast();
}

void SimulationEngine::updateMessageAckState(const QString& messageId, bool acknowledged,
                                             int retryCount, const QString& reason) {
    if (messageId.isEmpty()) return;
    bool changed = false;
    for (QVariant& value : m_messageCache) {
        QVariantMap message = value.toMap();
        if (message.value(QStringLiteral("id")).toString() != messageId) continue;
        if (message.value(QStringLiteral("acked")).toBool() != acknowledged) {
            message.insert(QStringLiteral("acked"), acknowledged);
            changed = true;
        }
        if (message.value(QStringLiteral("retryCount")).toInt() != retryCount) {
            message.insert(QStringLiteral("retryCount"), retryCount);
            changed = true;
        }
        if (message.value(QStringLiteral("ackReason")).toString() != reason) {
            message.insert(QStringLiteral("ackReason"), reason);
            changed = true;
        }
        value = message;
    }
    if (changed) emit messagesChanged();
}

QJsonObject SimulationEngine::unitSnapshot(const QString& id) const {
    auto it = m_units.find(id);
    if (it == m_units.end()) return {};
    auto& u = it->second;
    QJsonObject o;
    o["id"] = u->id();
    o["callsign"] = u->callsign();
    o["kind"] = u->kindStr();
    o["side"] = u->sideStr();
    o["movable"] = u->movable();
    QJsonArray pos;
    pos.append(u->pos().x);
    pos.append(u->pos().y);
    pos.append(u->pos().alt);
    o["position"] = pos;
    o["detectRange"] = u->detectRange();
    o["attackRange"] = u->attackRange();
    o["commRange"] = u->commRange();
    o["speed"] = u->speed();
    o["baseSpeed"] = u->baseSpeed();
    o["maxCommandedSpeed"] = u->maxCommandedSpeed();
    o["collisionRadius"] = u->collisionRadius();
    o["collisionHalfHeight"] = u->collisionHalfHeight();
    o["maxHp"] = u->maxHp();
    o["attackPower"] = u->attackPower();
    o["armor"] = u->armor();
    o["hp"] = u->hp();
    o["alive"] = u->alive();
    o["disabled"] = u->disabled();
    o["subsystems"] = u->subsystemStateJson();
    o["serviceRequested"] = u->serviceRequested();
    o["serviceProgress"] = u->serviceProgress();
    o["serviceDuration"] = u->serviceDuration();
    o["serviceElapsed"] = u->serviceElapsed();
    o["serviceCpId"] = u->serviceCpId();
    const UnitBase* serviceCp = unit(commandSenderIdFor(u.get()));
    o["serviceEligible"] = u->kind() != UnitKind::CommandPost && u->movable() && u->alive()
        && serviceCp && serviceCp->alive()
        && u->pos().distanceTo2D(serviceCp->pos()) <= kServiceRadiusMeters;
    o["fuelRemaining"] = u->fuelRemaining();
    o["fuelCapacity"] = u->fuelCapacity();
    o["fuelBurnRate"] = u->fuelBurnRate();
    const double endurance = u->estimatedFuelEndurance();
    o["estimatedEnduranceSec"] = std::isfinite(endurance) ? endurance : -1.0;
    o["economyCruiseSpeed"] = u->economyCruiseSpeed();
    o["abilities"] = u->abilityStateJson();
    o["countermeasureRange"] = u->countermeasureState().range;
    o["countermeasureCooldownRemaining"] = u->countermeasureState().cooldownRemaining;
    o["countermeasureRemaining"] = u->countermeasureState().remaining;
    o["countermeasureCapacity"] = u->countermeasureState().capacity;
    o["scanCooldownRemaining"] = u->scanState().cooldownRemaining;
    o["repairCooldownRemaining"] = u->repairCooldownRemaining();
    int incomingThreatCount = 0;
    double nearestThreatDistance = std::numeric_limits<double>::infinity();
    double minimumThreatEta = std::numeric_limits<double>::infinity();
    for (const auto& [projectileId, projectile] : m_projectiles) {
        Q_UNUSED(projectileId);
        if (!projectile.active() || projectile.side == u->side()
            || projectile.targetId != id) continue;
        const double distance = u->pos().distanceTo2D(projectile.position);
        if (distance > kProjectileThreatRadiusMeters) continue;
        ++incomingThreatCount;
        nearestThreatDistance = std::min(nearestThreatDistance, distance);
        minimumThreatEta = std::min(minimumThreatEta, distance / projectile.speed);
    }
    o["incomingThreatCount"] = incomingThreatCount;
    o["nearestThreatDistance"] = incomingThreatCount > 0 ? nearestThreatDistance : -1.0;
    o["minimumThreatEta"] = incomingThreatCount > 0 ? minimumThreatEta : -1.0;
    o["status"] = u->statusText();
    o["sharedKnowledge"] = u->sharedKnowledgeJson();
    QJsonArray rp;
    const auto rpv = u->recentPath();
    for (const auto& rppi : rpv) {
        const auto rpm = rppi.toMap();
        QJsonObject r;
        r["x"] = rpm.value("x").toDouble();
        r["y"] = rpm.value("y").toDouble();
        rp.append(r);
    }
    o["recentPath"] = rp;
    QJsonArray dets;
    auto dit = m_cachedDetections.find(id);
    if (dit != m_cachedDetections.end()) dets = dit->second;
    o["detections"] = dets;
    QJsonArray sched;
    for (const auto& sp : u->schedule()) {
        QJsonObject p;
        p["time"] = sp.time;
        p["x"] = sp.x;
        p["y"] = sp.y;
        sched.append(p);
    }
    o["schedule"] = sched;
    if (u->kind() == UnitKind::JammerUAV) {
        o["jammer"] = true;
        o["jamFactor"] = u->jamFactor();
    }
    if (auto* attacker = qobject_cast<AttackUAV*>(u.get())) {
        const ScenarioUnit* configured = nullptr;
        auto index = m_scenarioIndex.find(id);
        if (index != m_scenarioIndex.end()) configured = &m_scenario.units[index->second];
        o["ammoRemaining"] = attacker->ammoRemaining();
        o["ammoCapacity"] = attacker->ammoCapacity();
        o["cooldownRemaining"] = attacker->cooldownRemaining();
        o["lastShotOutcome"] = attacker->lastShotOutcome();
        o["fuelRemaining"] = attacker->fuelRemaining();
        o["fuelCapacity"] = attacker->fuelCapacity();
        o["turnaroundProgress"] = attacker->turnaroundProgress();
        o["turnaroundElapsed"] = attacker->turnaroundElapsed();
        o["rulesOfEngagement"] = attacker->rulesOfEngagement();
        o["targetId"] = attacker->targetId();
        o["armed"] = attacker->armed();
        o["activeProjectileId"] = attacker->activeProjectileId();
        o["activeProjectileCount"] = attacker->activeProjectileCount();
        if (configured) {
            o["initialAmmo"] = configured->initialAmmo;
            o["hitProbability"] = configured->hitProbability;
            o["optimalRange"] = configured->optimalRange;
            o["minAttackRange"] = configured->minAttackRange;
            o["cooldownSec"] = configured->cooldownSec;
            o["damageMin"] = configured->damageMin;
            o["damageMax"] = configured->damageMax;
            o["rangeFalloff"] = configured->rangeFalloff;
            o["rearmDurationSec"] = configured->rearmDurationSec;
        }
    }
    if (const auto projected = m_remoteRuntimeProjection.constFind(id);
        projected != m_remoteRuntimeProjection.constEnd()) {
        // The server has already validated and permission-trimmed this object.
        // Overlay it last so fields absent from the local mirror (actions,
        // threat summaries, and projected private state) survive the decode.
        for (auto field = projected->constBegin(); field != projected->constEnd(); ++field) {
            o.insert(field.key(), field.value());
        }
    }
    return o;
}

/// @brief Resolve the actual registered CommandPost id for a unit's side.
/// @details Avoids hardcoding "red_cp"/"blue_cp" so user-renamed CPs keep working.
QString SimulationEngine::commandSenderIdFor(const UnitBase* u) const {
    if (!u) return QString();
    for (const auto& [id, other] : m_units) {
        if (other->kind() == UnitKind::CommandPost && other->side() == u->side() && other->alive()) {
            return id;
        }
    }
    // No alive CP for this side. Caller (command dispatch) treats empty sender as "no comm available"
    // and surfaces a user-visible error instead of silently dropping the order.
    return QString();
}

void SimulationEngine::initCommandDispatch() {
    m_dispatch["assignTarget"]  = [this](auto& a){ cmdAssignTarget(a); };
    m_dispatch["setFlightPlan"] = [this](auto& a){ cmdSetFlightPlan(a); };
    m_dispatch["unitOrder"]     = [this](auto& a){ cmdUnitOrder(a); };
    m_dispatch["attackAt"]      = [this](auto& a){ cmdAttackAt(a); };
    m_dispatch["engageTarget"]  = [this](auto& a){ cmdEngageTarget(a); };
    m_dispatch["moveTo"]        = [this](auto& a){ cmdMoveTo(a); };
    m_dispatch["withdraw"]      = [this](auto& a){ cmdWithdraw(a); };
    m_dispatch["setSpeed"]      = [this](auto& a){ cmdSetSpeed(a); };
    m_dispatch["pursue"]        = [this](auto& a){ cmdPursue(a); };
    m_dispatch["guideAttack"]   = [this](auto& a){ cmdGuideAttack(a); };
    m_dispatch["setSchedule"]   = [this](auto& a){ cmdSetSchedule(a); };
    m_dispatch["halt"]          = [this](auto& a){ cmdHalt(a); };
    m_dispatch["service"]       = [this](auto& a){ cmdService(a); };
    m_dispatch["cancelService"] = [this](auto& a){ cmdCancelService(a); };
    m_dispatch["activateCountermeasure"] = [this](auto& a){ cmdActivateCountermeasure(a); };
    m_dispatch["activateScan"] = [this](auto& a){ cmdActivateScan(a); };
    m_dispatch["attemptFieldRepair"] = [this](auto& a){ cmdAttemptFieldRepair(a); };
    m_dispatch["cancelEngagement"] = [this](auto& a){ cmdCancelEngagement(a); };
    m_dispatch["setRoe"]        = [this](auto& a){ cmdSetRulesOfEngagement(a); };
}

QVariantMap SimulationEngine::command(const QString& action, const QVariantMap& args) {
    return executeCommand(action, args).toVariantMap();
}

CommandResult SimulationEngine::executeCommand(const QString& action, const QVariantMap& args) {
    const CommandResult validation = validateCommand(action, args);
    if (!validation.accepted) {
        m_lastError = validation.message;
        emit errorOccurred(m_lastError);
        return validation;
    }
    auto it = m_dispatch.find(action);
    // Command handlers are intentionally void because most commands enqueue
    // asynchronous bus messages.  Clear the previous diagnostic and treat a
    // handler-reported error as a rejected dispatch so callers cannot commit a
    // durable VMF task whose engine action was silently dropped.
    m_lastError.clear();
    it->second(args);
    if (!m_lastError.isEmpty()) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::DispatchFailed),
                                     m_lastError);
    }
    if (!m_replaying) {
        if (m_replayRecordingEnabled) {
            m_replayCommands.push_back(ReplayCommand{simTime(), ++m_replayCommandSequence,
                                                      action, args});
        }
        appendTimeline(QStringLiteral("command"), QStringLiteral("命令已接受"),
                       QJsonObject{{QStringLiteral("action"), action},
                                   {QStringLiteral("args"),
                                    QJsonObject::fromVariantMap(args)}});
    }
    return CommandResult::ok();
}

CommandResult SimulationEngine::validateCommand(const QString& action,
                                                const QVariantMap& args) const {
    const auto dispatchIt = m_dispatch.find(action);
    if (dispatchIt == m_dispatch.end()) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::UnknownAction),
                                     QStringLiteral("未知操作: %1").arg(action));
    }

    QString unitId;
    if (action == QLatin1String("guideAttack")) {
        unitId = args.value(QStringLiteral("guideId")).toString();
    } else if (action == QLatin1String("assignTarget")
               || action == QLatin1String("setFlightPlan")
               || action == QLatin1String("engageTarget")
               || action == QLatin1String("pursue")) {
        unitId = args.value(QStringLiteral("attackerId")).toString();
    } else {
        unitId = args.value(QStringLiteral("unitId")).toString();
    }

    UnitBase* controlled = unit(unitId);
    if (!controlled) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::UnitNotFound),
                                     QStringLiteral("未知单元: %1").arg(unitId));
    }
    if (!controlled->alive()) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::UnitDestroyed),
                                     QStringLiteral("单元已摧毁: %1").arg(unitId));
    }
    if (m_frozenSides.contains(controlled->sideStr())) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::UnitFrozen),
                                     QStringLiteral("固定靶阵营不能执行命令: %1").arg(unitId));
    }
    const bool stationaryAction = action == QLatin1String("unitOrder")
        || action == QLatin1String("activateCountermeasure")
        || action == QLatin1String("attemptFieldRepair");
    if (!stationaryAction && !controlled->movable()) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::UnitNotMovable),
                                     QStringLiteral("该操作仅适用于可移动单元: %1").arg(unitId));
    }

    auto validPoint = [this](const QVariantMap& point) {
        if (!point.contains(QStringLiteral("x")) || !point.contains(QStringLiteral("y"))) {
            return false;
        }
        bool xOk = false;
        bool yOk = false;
        const double x = point.value(QStringLiteral("x")).toDouble(&xOk);
        const double y = point.value(QStringLiteral("y")).toDouble(&yOk);
        return xOk && yOk && std::isfinite(x) && std::isfinite(y)
            && x >= 0.0 && y >= 0.0
            && x <= m_scenario.map.widthMeters && y <= m_scenario.map.heightMeters;
    };

    const bool attackAction = action == QLatin1String("assignTarget")
        || action == QLatin1String("engageTarget")
        || action == QLatin1String("pursue");
    if ((attackAction || action == QLatin1String("attackAt")
         || action == QLatin1String("setFlightPlan"))
        && controlled->kind() != UnitKind::AttackUAV) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidUnitKind),
                                     QStringLiteral("该操作仅适用于攻击无人机"));
    }
    if ((action == QLatin1String("cancelEngagement") || action == QLatin1String("setRoe"))
        && controlled->kind() != UnitKind::AttackUAV) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidUnitKind),
                                         QStringLiteral("该操作仅适用于攻击无人机"));
    }

    AttackUAV* reloadingAttacker = nullptr;
    if (attackAction || action == QLatin1String("attackAt")) {
        reloadingAttacker = dynamic_cast<AttackUAV*>(controlled);
    } else if (action == QLatin1String("guideAttack")) {
        reloadingAttacker = dynamic_cast<AttackUAV*>(
            unit(args.value(QStringLiteral("attackerId")).toString()));
    }
    if (reloadingAttacker && reloadingAttacker->cooldownRemaining() > 0.0) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::WeaponReloading),
                                     QStringLiteral("攻击机正在换弹，请等待换弹完成后重新攻击"));
    }
    if (reloadingAttacker
        && (reloadingAttacker->ammoRemaining() <= 0 || reloadingAttacker->serviceRequested())) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::WeaponUnavailable),
                                     QStringLiteral("攻击机没有可用弹药或正在补给"));
    }
    if (action == QLatin1String("engageTarget")
        && activeProjectileCount() >= kMaximumProjectiles) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::WeaponUnavailable),
                                     QStringLiteral("全局在途导弹已达到 512 枚安全上限"));
    }
    if (action == QLatin1String("setRoe")) {
        const QString roe = args.value(QStringLiteral("roe")).toString();
        if (roe != QLatin1String("hold") && roe != QLatin1String("free")) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                         QStringLiteral("交战规则必须为 hold 或 free"));
        }
    }

    if (attackAction) {
        const QString targetId = args.value(QStringLiteral("targetId")).toString();
        UnitBase* target = unit(targetId);
        if (!isHostileTarget(controlled, target)) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidTarget),
                                         QStringLiteral("敌方目标不存在、已摧毁或阵营无效"));
        }
    }

    if (action == QLatin1String("guideAttack")) {
        UnitBase* attacker = unit(args.value(QStringLiteral("attackerId")).toString());
        UnitBase* target = unit(args.value(QStringLiteral("targetId")).toString());
        if (controlled->kind() != UnitKind::GroundScout || !attacker || !attacker->alive()
            || attacker->kind() != UnitKind::AttackUAV
            || controlled->side() != attacker->side()
            || !isHostileTarget(attacker, target)) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidTarget),
                                         QStringLiteral("引导单元、攻击单元或敌方目标无效"));
        }
        if (!validPoint(args.value(QStringLiteral("targetPos")).toMap())) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                         QStringLiteral("引导目标位置无效或超出地图边界"));
        }
        if (!m_transport->canCommunicate(controlled->id(), attacker->id())) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::CommunicationLost),
                                         QStringLiteral("引导单元与攻击机之间没有可用通信链路"));
        }
    }

    if ((action == QLatin1String("attackAt") || action == QLatin1String("moveTo")
         || (action == QLatin1String("withdraw")
             && args.contains(QStringLiteral("pos"))))
        && !validPoint(args.value(QStringLiteral("pos")).toMap())) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                     QStringLiteral("命令目标无效或超出地图边界"));
    }
    if (action == QLatin1String("unitOrder")) {
        const QString text = args.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty() || text.size() > 420) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                         QStringLiteral("文本命令不能为空且不能超过 420 字"));
        }
    }
    if (action == QLatin1String("setFlightPlan")) {
        const QVariantList waypoints = args.value(QStringLiteral("waypoints")).toList();
        if (waypoints.isEmpty()) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                         QStringLiteral("航路至少需要一个有效航点"));
        }
        if (waypoints.size() > 512) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                         QStringLiteral("航路最多支持 512 个航点"));
        }
        for (const QVariant& waypoint : waypoints) {
            if (!validPoint(waypoint.toMap())) {
                return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                             QStringLiteral("航路点无效或超出地图边界"));
            }
        }
    }
    if (action == QLatin1String("setSpeed")) {
        bool ok = false;
        const double speed = args.value(QStringLiteral("speed")).toDouble(&ok);
        const double maximum = controlled->maxCommandedSpeed();
        if (!ok || !std::isfinite(speed) || speed <= 0.0
            || maximum <= 0.0 || speed > maximum) {
            return CommandResult::reject(
                QString::fromLatin1(CommandCode::InvalidArgument),
                QStringLiteral("单元速度必须大于 0 且不超过 %1")
                    .arg(maximum, 0, 'f', 0));
        }
    }
    if (action == QLatin1String("setSchedule")) {
        const QVariantList schedule = args.value(QStringLiteral("schedule")).toList();
        if (schedule.size() > 512) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                         QStringLiteral("计划点不能超过 512 个"));
        }
        for (const QVariant& value : schedule) {
            const QVariantMap point = value.toMap();
            bool timeOk = false;
            const double time = point.value(QStringLiteral("time")).toDouble(&timeOk);
            if (!timeOk || !std::isfinite(time) || time < 0.0 || !validPoint(point)) {
                return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                             QStringLiteral("计划点时间或位置无效"));
            }
        }
    }

    const bool movementAction = action == QLatin1String("moveTo")
        || action == QLatin1String("withdraw") || action == QLatin1String("attackAt")
        || action == QLatin1String("setFlightPlan") || action == QLatin1String("pursue")
        || action == QLatin1String("guideAttack") || action == QLatin1String("setSchedule");
    if (movementAction && !controlled->hasUsableFuel()) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::WeaponUnavailable),
                                     QStringLiteral("燃油已耗尽，无法执行新的移动命令"));
    }
    if (action == QLatin1String("activateCountermeasure")) {
        if (!controlled->countermeasureState().supported()) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidUnitKind),
                                         QStringLiteral("该单元不支持干扰弹"));
        }
        if (!controlled->countermeasureState().available()) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::WeaponUnavailable),
                                         QStringLiteral("干扰弹冷却中或次数已用尽"));
        }
    }
    if (action == QLatin1String("activateScan")) {
        if (controlled->kind() != UnitKind::ReconUAV || !controlled->scanState().supported()) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidUnitKind),
                                         QStringLiteral("该操作仅适用于侦察无人机"));
        }
        if (!controlled->scanState().available()) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::WeaponUnavailable),
                                         QStringLiteral("扫描技能冷却中"));
        }
    }
    if (action == QLatin1String("attemptFieldRepair")) {
        const QJsonObject subsystems = controlled->subsystemStateJson();
        const bool damaged = subsystems.value(QStringLiteral("sensor")).toDouble() < 1.0 - 1e-9
            || subsystems.value(QStringLiteral("comms")).toDouble() < 1.0 - 1e-9
            || subsystems.value(QStringLiteral("mobility")).toDouble() < 1.0 - 1e-9
            || subsystems.value(QStringLiteral("weapon")).toDouble() < 1.0 - 1e-9;
        if (!damaged || controlled->repairCooldownRemaining() > 1e-9) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::WeaponUnavailable),
                                         damaged ? QStringLiteral("战场修理冷却中")
                                                 : QStringLiteral("没有可修复的受损部位"));
        }
    }
    if (action == QLatin1String("service")) {
        UnitBase* cp = unit(commandSenderIdFor(controlled));
        if (controlled->kind() == UnitKind::CommandPost || !cp || !cp->alive()
            || controlled->pos().distanceTo2D(cp->pos()) > kServiceRadiusMeters) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                         QStringLiteral("单元必须在活指挥所 750 米内才能开始补充"));
        }
    }
    if (action == QLatin1String("cancelService") && !controlled->serviceRequested()) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                     QStringLiteral("单元当前未在补充"));
    }

    // Scanning is a local sensor action. It intentionally remains usable when
    // the recon unit is outside the command-post network; the resulting
    // contact is still filtered by the directed communication graph.
    const bool needsCommandPost = action != QLatin1String("activateScan")
        && action != QLatin1String("setSpeed")
        && action != QLatin1String("setSchedule")
        && action != QLatin1String("guideAttack")
        && action != QLatin1String("cancelService");
    if (needsCommandPost) {
        const QString commandPostId = commandSenderIdFor(controlled);
        if (commandPostId.isEmpty()) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::CommandPostUnavailable),
                                         QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(unitId));
        }
        if (commandPostId != controlled->id()
            && !m_transport->canCommunicate(commandPostId, controlled->id())) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::CommunicationLost),
                                         QStringLiteral("指挥所与单元之间没有可用通信链路: %1")
                                             .arg(unitId));
        }
    }
    return CommandResult::ok();
}

void SimulationEngine::cmdAssignTarget(const QVariantMap& args) {
    const auto attackerId = args.value("attackerId").toString();
    const auto targetId = args.value("targetId").toString();
    auto* u = unit(attackerId);
    auto* target = unit(targetId);
    if (!isHostileTarget(u, target) || u->kind() != UnitKind::AttackUAV) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(attackerId);
        emit errorOccurred(m_lastError);
        return;
    }
    Message m;
    m.type = Message::Type::AttackOrder;
    m.sender = cpId;
    m.receiver = attackerId;
    m.requiresAck = !args.value(QStringLiteral("notificationOnly")).toBool();
    m.payload["targetId"] = targetId;
    if (args.value(QStringLiteral("notificationOnly")).toBool()) {
        m.payload[QStringLiteral("notificationOnly")] = true;
    }
    if (!m_transport->send(m)) {
        m_lastError = QStringLiteral("攻击指令消息编码失败: %1").arg(attackerId);
        emit errorOccurred(m_lastError);
    }
}

void SimulationEngine::cmdSetFlightPlan(const QVariantMap& args) {
    const auto attackerId = args.value("attackerId").toString();
    auto* u = unit(attackerId);
    if (!u || !u->alive() || !u->movable()) return;
    Message m;
    m.type = Message::Type::FlightPlan;
    m.receiver = attackerId;
    QJsonArray wp;
    for (const auto& v : args.value("waypoints").toList()) {
        const auto wpt = v.toMap();
        if (!wpt.contains("x") || !wpt.contains("y")) return;
        const double x = wpt.value("x").toDouble();
        const double y = wpt.value("y").toDouble();
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        wp.append(QJsonObject{{"x", x}, {"y", y}, {"alt", 2000.0}});
    }
    if (wp.isEmpty()) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(attackerId);
        emit errorOccurred(m_lastError);
        return;
    }
    m.sender = cpId;
    m.payload["waypoints"] = wp;
    if (!m_transport->send(m)) {
        m_lastError = QStringLiteral("VMF 航路消息编码失败: %1").arg(attackerId);
        emit errorOccurred(m_lastError);
    }
}

void SimulationEngine::cmdEngageTarget(const QVariantMap& args) {
    const auto attackerId = args.value("attackerId").toString();
    const auto targetId = args.value("targetId").toString();
    auto* u = unit(attackerId);
    auto* target = unit(targetId);
    if (!isHostileTarget(u, target) || u->kind() != UnitKind::AttackUAV) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(attackerId);
        emit errorOccurred(m_lastError);
        return;
    }
    Message m;
    m.type = Message::Type::AttackOrder;
    m.sender = cpId;
    m.receiver = attackerId;
    m.payload["fireNow"] = true;
    m.payload["targetId"] = targetId;
    if (!m_transport->send(m)) {
        m_lastError = QStringLiteral("VMF 攻击消息编码失败: %1").arg(attackerId);
        emit errorOccurred(m_lastError);
    }
}

void SimulationEngine::cmdUnitOrder(const QVariantMap& args) {
    const QString unitId = args.value(QStringLiteral("unitId")).toString();
    UnitBase* unit = this->unit(unitId);
    if (!unit || !unit->alive()) return;
    const QString commandPostId = commandSenderIdFor(unit);
    if (commandPostId.isEmpty()) return;
    Message message;
    message.type = Message::Type::UnitOrder;
    message.sender = commandPostId;
    message.receiver = unitId;
    message.payload[QStringLiteral("text")] = args.value(QStringLiteral("text")).toString().trimmed();
    if (args.value(QStringLiteral("notificationOnly")).toBool()) {
        message.payload[QStringLiteral("notificationOnly")] = true;
    }
    m_transport->send(message);
}

void SimulationEngine::cmdAttackAt(const QVariantMap& args) {
    const QString unitId = args.value(QStringLiteral("unitId")).toString();
    const QVariantMap position = args.value(QStringLiteral("pos")).toMap();
    UnitBase* unit = this->unit(unitId);
    if (!unit || !unit->alive() || unit->kind() != UnitKind::AttackUAV) return;
    const QString commandPostId = commandSenderIdFor(unit);
    if (commandPostId.isEmpty()) return;
    Message message;
    message.type = Message::Type::AttackOrder;
    message.sender = commandPostId;
    message.receiver = unitId;
    message.requiresAck = true;
    message.payload[QStringLiteral("x")] = position.value(QStringLiteral("x")).toDouble();
    message.payload[QStringLiteral("y")] = position.value(QStringLiteral("y")).toDouble();
    m_transport->send(message);
}

void SimulationEngine::cmdMoveTo(const QVariantMap& args) {
    const auto uid = args.value("unitId").toString();
    const auto posMap = args.value("pos").toMap();
    auto* u = unit(uid);
    if (!u || !u->alive() || !u->movable()) return;
    if (!posMap.contains("x") || !posMap.contains("y")) return;
    if (!std::isfinite(posMap.value("x").toDouble())
        || !std::isfinite(posMap.value("y").toDouble())) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(uid);
        emit errorOccurred(m_lastError);
        return;
    }
    Message m;
    m.type = Message::Type::Guidance;
    m.sender = cpId;
    m.receiver = uid;
    m.payload["x"] = posMap.value("x").toDouble();
    m.payload["y"] = posMap.value("y").toDouble();
    m.payload["kind"] = QString("moveTo");
    if (args.value(QStringLiteral("notificationOnly")).toBool()) {
        m.payload[QStringLiteral("notificationOnly")] = true;
    }
    m_transport->send(m);
}

void SimulationEngine::cmdWithdraw(const QVariantMap& args) {
    const auto uid = args.value("unitId").toString();
    const QVariantMap position = args.value(QStringLiteral("pos")).toMap();
    auto* u = unit(uid);
    if (!u || !u->alive() || !u->movable()) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(uid);
        emit errorOccurred(m_lastError);
        return;
    }
    Message m;
    m.type = Message::Type::Withdraw;
    m.sender = cpId;
    m.receiver = uid;
    const bool notificationOnly = args.value(QStringLiteral("notificationOnly")).toBool();
    m.requiresAck = !notificationOnly;
    if (notificationOnly) {
        m.payload[QStringLiteral("notificationOnly")] = true;
    }
    if (position.contains(QStringLiteral("x")) && position.contains(QStringLiteral("y"))) {
        m.payload["homeX"] = position.value(QStringLiteral("x")).toDouble();
        m.payload["homeY"] = position.value(QStringLiteral("y")).toDouble();
        m.payload["x"] = position.value(QStringLiteral("x")).toDouble();
        m.payload["y"] = position.value(QStringLiteral("y")).toDouble();
    } else if (!notificationOnly) {
        if (auto* cp = unit(cpId)) {
            m.payload["homeX"] = cp->pos().x;
            m.payload["homeY"] = cp->pos().y;
        } else {
            m.payload["homeX"] = u->pos().x;
            m.payload["homeY"] = u->pos().y;
        }
    }
    if (!m_transport->send(m)) {
        m_lastError = QStringLiteral("VMF 撤离消息编码失败: %1").arg(uid);
        emit errorOccurred(m_lastError);
    }
}

void SimulationEngine::cmdSetSpeed(const QVariantMap& args) {
    const auto uid = args.value("unitId").toString();
    const auto v = args.value("speed").toDouble();
    auto* u = unit(uid);
    if (!u) return;
    if (!u->movable() || !u->alive()) return;
    if (!std::isfinite(v) || v <= 0.0) return;
    u->setSpeed(std::min(v, u->maxCommandedSpeed()));
}

void SimulationEngine::cmdPursue(const QVariantMap& args) {
    const auto attackerId = args.value("attackerId").toString();
    const auto targetId = args.value("targetId").toString();
    auto* u = unit(attackerId);
    auto* t = unit(targetId);
    if (!isHostileTarget(u, t) || u->kind() != UnitKind::AttackUAV) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(attackerId);
        emit errorOccurred(m_lastError);
        return;
    }
    Message m;
    m.type = Message::Type::Pursue;
    m.sender = cpId;
    m.receiver = attackerId;
    m.payload["targetId"] = targetId;
    m.payload["x"] = t->pos().x;
    m.payload["y"] = t->pos().y;
    m_transport->send(m);
}

void SimulationEngine::cmdGuideAttack(const QVariantMap& args) {
    const auto guideId = args.value("guideId").toString();
    const auto attackerId = args.value("attackerId").toString();
    const auto targetId = args.value("targetId").toString();
    const auto tpMap = args.value("targetPos").toMap();
    auto* guide = unit(guideId);
    auto* attacker = unit(attackerId);
    auto* target = unit(targetId);
    const double targetX = tpMap.value("x").toDouble();
    const double targetY = tpMap.value("y").toDouble();
    if (!guide || !guide->alive() || guide->kind() != UnitKind::GroundScout
        || !attacker || attacker->kind() != UnitKind::AttackUAV
        || guide->side() != attacker->side()
        || !isHostileTarget(attacker, target)
        || !tpMap.contains("x") || !tpMap.contains("y")
        || !std::isfinite(targetX) || !std::isfinite(targetY)) return;
    {
        Message m;
        m.type = Message::Type::FlightPlan;
        m.sender = guideId;
        m.receiver = attackerId;
        QJsonArray wp;
        QJsonObject w0; w0["x"] = targetX; w0["y"] = targetY; w0["alt"] = 2000.0;
        wp.append(w0);
        m.payload["waypoints"] = wp;
        m.payload["targetId"] = targetId;
        m_transport->send(m);
    }
    {
        Message m;
        m.type = Message::Type::AttackOrder;
        m.sender = guideId;
        m.receiver = attackerId;
        m.payload["fireNow"] = true;
        m.payload["targetId"] = targetId;
        m_transport->send(m);
    }
}

void SimulationEngine::cmdSetSchedule(const QVariantMap& args) {
    const auto uid = args.value("unitId").toString();
    auto* u = unit(uid);
    if (!u || !u->alive() || !u->movable()) {
        if (uid.isEmpty() || !unit(uid)) {
            m_lastError = QStringLiteral("未知单元: %1").arg(uid);
            emit errorOccurred(m_lastError);
        }
        return;
    }
    std::vector<SchedulePoint> sched;
    const auto list = args.value("schedule").toList();
    for (int i = 0; i < list.size(); ++i) {
        const auto m = list[i].toMap();
        if (!m.contains("time") || !m.contains("x") || !m.contains("y")) {
            m_lastError = QStringLiteral("计划点 #%1 缺少字段 (time/x/y): %2").arg(i).arg(uid);
            emit errorOccurred(m_lastError);
            return;
        }
        SchedulePoint p;
        p.time = m.value("time").toDouble();
        p.x = m.value("x").toDouble();
        p.y = m.value("y").toDouble();
        if (!std::isfinite(p.time) || !std::isfinite(p.x) || !std::isfinite(p.y)) {
            m_lastError = QStringLiteral("计划点 #%1 含 NaN/Inf: %2").arg(i).arg(uid);
            emit errorOccurred(m_lastError);
            return;
        }
        sched.push_back(p);
    }
    std::sort(sched.begin(), sched.end(),
              [](const SchedulePoint& a, const SchedulePoint& b) {
                  return a.time < b.time;
              });
    u->cancelService();
    u->cancelWaypointMotion();
    u->setSchedule(sched);
    if (auto* su = findScenarioUnit(uid)) su->schedule = sched;
    emit unitsChanged();
}

void SimulationEngine::cmdHalt(const QVariantMap& args) {
    const auto unitId = args.value("unitId").toString();
    auto* u = unit(unitId);
    if (!u || !u->alive() || !u->movable()) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(unitId);
        emit errorOccurred(m_lastError);
        return;
    }
    Message m;
    m.type = Message::Type::Halt;
    m.sender = cpId;
    m.receiver = unitId;
    m_transport->send(m);
}

void SimulationEngine::cmdService(const QVariantMap& args) {
    const QString unitId = args.value(QStringLiteral("unitId")).toString();
    UnitBase* controlled = unit(unitId);
    if (!controlled || !controlled->alive() || !controlled->movable()) return;
    const QString cpId = commandSenderIdFor(controlled);
    UnitBase* cp = unit(cpId);
    if (!cp || !cp->alive()) return;
    controlled->cancelWaypointMotion();
    controlled->clearSchedule();
    if (auto* attacker = qobject_cast<AttackUAV*>(controlled)) attacker->cancelEngagement();
    if (controlled->beginService(cpId)) {
        appendTimeline(QStringLiteral("service"), QStringLiteral("开始补充"),
                       QJsonObject{{QStringLiteral("unitId"), unitId},
                                   {QStringLiteral("cpId"), cpId},
                                   {QStringLiteral("duration"), controlled->serviceDuration()}});
    }
}

void SimulationEngine::cmdCancelService(const QVariantMap& args) {
    UnitBase* controlled = unit(args.value(QStringLiteral("unitId")).toString());
    if (!controlled || !controlled->serviceRequested()) return;
    const QString unitId = controlled->id();
    controlled->cancelService();
    controlled->setStatus(QStringLiteral("补充已取消"));
    appendTimeline(QStringLiteral("service"), QStringLiteral("取消补充"),
                   QJsonObject{{QStringLiteral("unitId"), unitId}});
}

void SimulationEngine::cmdActivateCountermeasure(const QVariantMap& args) {
    UnitBase* source = unit(args.value(QStringLiteral("unitId")).toString());
    if (!source || !source->activateCountermeasure()) return;
    int affected = 0;
    QStringList terminalIds;
    for (auto& [projectileId, projectile] : m_projectiles) {
        if (!projectile.active() || projectile.side == source->side()) continue;
        if (source->pos().distanceTo2D(projectile.position)
            > source->countermeasureState().range) continue;
        projectile.terminalReason = QStringLiteral("countermeasured");
        projectile.terminalAge = 0.0;
        terminalIds.append(projectileId);
        ++affected;
    }
    settleTerminalProjectiles(terminalIds);
    source->setStatus(QStringLiteral("已释放干扰弹"));
    appendTimeline(QStringLiteral("ability"), QStringLiteral("干扰弹释放"),
                   QJsonObject{{QStringLiteral("unitId"), source->id()},
                               {QStringLiteral("affectedProjectiles"), affected}});
    emit projectilesChanged();
}

void SimulationEngine::cmdActivateScan(const QVariantMap& args) {
    UnitBase* scanner = unit(args.value(QStringLiteral("unitId")).toString());
    if (!scanner || !scanner->activateScan()) return;
    std::erase_if(m_scanContacts, [scanner](const ScanContact& contact) {
        return contact.scannerId == scanner->id();
    });
    const double range = scanner->scanState().range
        * scanner->sensorHealth() * scanner->jamFactor();
    int count = 0;
    for (const auto& [targetId, target] : m_units) {
        if (!target->alive() || target->side() == scanner->side()
            || scanner->pos().distanceTo2D(target->pos()) > range) continue;
        m_scanContacts.push_back(ScanContact{scanner->id(), targetId, scanner->side(),
                                             simTime() + 18.0});
        ++count;
    }
    const bool foundTarget = count > 0;
    scanner->setStatus(foundTarget
                           ? QStringLiteral("超视距扫描完成，锁定 %1 个目标").arg(count)
                           : QStringLiteral("超视距扫描完成，扫描范围内未发现敌方目标"));
    appendTimeline(QStringLiteral("ability"), QStringLiteral("侦察扫描"),
                   QJsonObject{{QStringLiteral("unitId"), scanner->id()},
                               {QStringLiteral("contactCount"), count},
                               {QStringLiteral("expiresAt"), simTime() + 18.0}});
    emit eventPosted(QStringLiteral("超视距扫描"),
                     foundTarget
                         ? QStringLiteral("%1 锁定 %2 个敌方目标").arg(scanner->id()).arg(count)
                         : QStringLiteral("%1 扫描范围内未发现敌方目标").arg(scanner->id()),
                     foundTarget ? QStringLiteral("success") : QStringLiteral("warn"),
                     scanner->id());
    emit scanContactsChanged();
}

void SimulationEngine::cmdAttemptFieldRepair(const QVariantMap& args) {
    UnitBase* controlled = unit(args.value(QStringLiteral("unitId")).toString());
    if (!controlled) return;
    const bool success = controlled->attemptFieldRepair(m_battleSeed);
    controlled->setStatus(success ? QStringLiteral("战场修理成功")
                                  : QStringLiteral("战场修理失败"));
    appendTimeline(QStringLiteral("ability"), QStringLiteral("战场修理"),
                   QJsonObject{{QStringLiteral("unitId"), controlled->id()},
                               {QStringLiteral("success"), success},
                               {QStringLiteral("attemptSequence"),
                                QString::number(controlled->repairAttemptSequence() - 1)}},
                   success ? QStringLiteral("success") : QStringLiteral("info"));
}

void SimulationEngine::cmdCancelEngagement(const QVariantMap& args) {
    const QString unitId = args.value(QStringLiteral("unitId")).toString();
    UnitBase* controlled = unit(unitId);
    const QString cpId = commandSenderIdFor(controlled);
    if (!controlled || cpId.isEmpty()) return;
    Message message;
    message.type = Message::Type::CancelEngagement;
    message.sender = cpId;
    message.receiver = unitId;
    m_transport->send(message);
}

void SimulationEngine::cmdSetRulesOfEngagement(const QVariantMap& args) {
    const QString unitId = args.value(QStringLiteral("unitId")).toString();
    UnitBase* controlled = unit(unitId);
    const QString cpId = commandSenderIdFor(controlled);
    if (!controlled || cpId.isEmpty()) return;
    Message message;
    message.type = Message::Type::SetRulesOfEngagement;
    message.sender = cpId;
    message.receiver = unitId;
    message.payload[QStringLiteral("roe")] = args.value(QStringLiteral("roe")).toString();
    m_transport->send(message);
}

void SimulationEngine::addOrUpdateUnit(const ScenarioUnit& su) {
    const ScenarioUnit normalized = normalizeScenarioUnitDefaults(su);
    const QString validationError = validateScenarioUnit(normalized, m_scenario.map);
    if (!validationError.isEmpty()) {
        m_lastError = validationError;
        emit errorOccurred(m_lastError);
        return;
    }
    if (m_scenarioIndex.find(normalized.id) == m_scenarioIndex.end()
        && m_scenario.units.size() >= kMaxScenarioUnits) {
        m_lastError = QStringLiteral("场景单元数量不能超过 %1").arg(kMaxScenarioUnits);
        emit errorOccurred(m_lastError);
        return;
    }

    // Update scenario store via O(1) index
    auto idxIt = m_scenarioIndex.find(normalized.id);
    if (idxIt != m_scenarioIndex.end()) {
        m_scenario.units[idxIt->second] = normalized;
    } else {
        m_scenarioIndex[normalized.id] = m_scenario.units.size();
        m_scenario.units.push_back(normalized);
    }
    rememberUnitIdentity(normalized);

    // Incrementally update runtime units
    auto it = m_units.find(normalized.id);
    if (it != m_units.end()) {
        const bool runtimeTypeChanged = it->second->kindStr() != normalized.kind
            || it->second->sideStr() != normalized.side;
        if (runtimeTypeChanged) {
            m_units.erase(it);
            createSingleUnit(normalized);
        } else {
            UnitBase::Params p;
            p.detectRange = normalized.detectRange;
            p.attackRange = normalized.attackRange;
            p.commRange = normalized.commRange;
            p.speed = normalized.speed;
            p.collisionRadius = normalized.collisionRadius;
            p.collisionHalfHeight = normalized.collisionHalfHeight;
            p.maxHp = normalized.maxHp;
            p.attackPower = normalized.attackPower;
            p.armor = normalized.armor;
            p.repairRate = normalized.repairRate;
            p.subsystemRepairRate = normalized.subsystemRepairRate;
            p.pos = normalized.pos;
            it->second->setCallsign(normalized.callsign);
            it->second->setParams(p);
            if (it->second->movable()) {
                it->second->configureFuel(std::max(1.0, normalized.fuelCapacitySec),
                                          std::clamp(normalized.initialFuelSec, 0.0,
                                                     normalized.fuelCapacitySec),
                                          std::max(1.0, normalized.speed));
            }
            if (auto* attacker = qobject_cast<AttackUAV*>(it->second.get())) {
                attacker->configureWeapon(normalized);
            }
            it->second->setSchedule(normalized.schedule);
        }
    } else {
        createSingleUnit(normalized);
    }

    for (auto& [oid, ou] : m_units) {
        ou->setCpId(commandSenderIdFor(ou.get()));
    }
    if (auto* updated = unit(normalized.id); updated && updated->alive()) {
        m_destroyedReported.remove(normalized.id);
    }

    emit unitsChanged();
    recomputeReadyForSim();
}

void SimulationEngine::removeUnit(const QString& id) {
    auto idxIt = m_scenarioIndex.find(id);
    if (idxIt != m_scenarioIndex.end()) {
        const size_t idx = idxIt->second;
        m_scenario.units.erase(m_scenario.units.begin() + static_cast<ptrdiff_t>(idx));
        // Rebuild the index since vector indices shift after erase.
        rebuildScenarioIndex();
    }

    m_units.erase(id);
    m_destroyedReported.remove(id);
    for (auto& [oid, unit] : m_units) {
        if (unit) unit->setCpId(commandSenderIdFor(unit.get()));
    }
    emit unitsChanged();
    recomputeReadyForSim();
}

QStringList SimulationEngine::unitIds() const {
    QStringList r;
    for (auto& [id, _] : m_units) r << id;
    return r;
}

void SimulationEngine::setMessageLogEnabled(bool enabled, const QString& path) {
    if (!m_recorder) return;
    if (!m_recorder->setEnabled(enabled, path)) {
        m_lastError = path.isEmpty()
            ? QStringLiteral("消息日志路径不能为空")
            : QStringLiteral("无法打开消息日志: %1").arg(path);
        emit errorOccurred(m_lastError);
    }
}

void SimulationEngine::persistScenario(const QString& path) {
    QString err;
    if (!ScenarioIo::saveToFile(m_scenario, path, &err)) {
        m_lastError = err.isEmpty() ? QStringLiteral("保存场景失败") : err;
        emit errorOccurred(m_lastError);
    }
}

UnitBase* SimulationEngine::unit(const QString& id) const {
    auto it = m_units.find(id);
    if (it == m_units.end()) return nullptr;
    return it->second.get();
}

QJsonArray SimulationEngine::collectPerceptionSnapshot(const QString& forSide) const {
    QJsonArray arr;
    for (const auto& [id, u] : m_units) {
        if (!forSide.isEmpty() && u->sideStr() != forSide) continue;
        arr.append(unitSnapshot(id));
    }
    return arr;
}

QJsonArray SimulationEngine::collectAllUnitsSnapshot() const {
    QJsonArray arr;
    for (const auto& [id, u] : m_units) arr.append(unitSnapshot(id));
    return arr;
}

QJsonArray SimulationEngine::projectilesSnapshot() const {
    QJsonArray result;
    for (const auto& [id, projectile] : m_projectiles) {
        const double x = std::clamp(projectile.position.x, 0.0,
                                    m_scenario.map.widthMeters);
        const double y = std::clamp(projectile.position.y, 0.0,
                                    m_scenario.map.heightMeters);
        result.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("side"), sideName(projectile.side)},
            {QStringLiteral("position"),
             QJsonArray{x, y, projectile.position.alt}},
            {QStringLiteral("headingRad"), projectile.headingRad},
            {QStringLiteral("speed"), projectile.speed},
            {QStringLiteral("age"), projectile.age},
            {QStringLiteral("lifetime"), projectile.lifetime},
            {QStringLiteral("active"), projectile.active()},
            {QStringLiteral("terminalReason"), projectile.terminalReason},
            {QStringLiteral("terminalAge"), projectile.terminalAge},
            {QStringLiteral("resultSettled"), projectile.resultSettled},
            {QStringLiteral("threatRadius"), kProjectileThreatRadiusMeters},
            {QStringLiteral("attackerId"), projectile.attackerId},
            {QStringLiteral("targetId"), projectile.targetId}});
    }
    return result;
}

qsizetype SimulationEngine::activeProjectileCount() const {
    return static_cast<qsizetype>(std::count_if(
        m_projectiles.cbegin(), m_projectiles.cend(),
        [](const auto& entry) { return entry.second.active(); }));
}

QJsonArray SimulationEngine::activeScanContacts() const {
    QJsonArray result;
    for (const ScanContact& contact : m_scanContacts) {
        if (contact.expiresAt <= simTime()) continue;
        result.append(QJsonObject{{QStringLiteral("scannerId"), contact.scannerId},
                                  {QStringLiteral("targetId"), contact.targetId},
                                  {QStringLiteral("side"), sideName(contact.side)},
                                  {QStringLiteral("expiresAt"), contact.expiresAt}});
    }
    return result;
}

bool SimulationEngine::applyRemoteProjectiles(const QJsonArray& projectiles,
                                               QString* error) {
    if (error) error->clear();
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (projectiles.size() > kMaximumProjectiles * 2) {
        return fail(QStringLiteral("权威导弹数量超过上限"));
    }
    std::map<QString, ProjectileState> restored;
    static const QSet<QString> terminalReasons{
        QStringLiteral("hit"), QStringLiteral("miss"), QStringLiteral("expired"),
        QStringLiteral("out_of_bounds"), QStringLiteral("out_of_range"),
        QStringLiteral("target_lost"),
        QStringLiteral("countermeasured")};
    qsizetype activeCount = 0;
    for (const QJsonValue& value : projectiles) {
        if (!value.isObject()) return fail(QStringLiteral("权威导弹对象无效"));
        const QJsonObject object = value.toObject();
        ProjectileState projectile;
        projectile.id = object.value(QStringLiteral("id")).toString();
        const QString side = object.value(QStringLiteral("side")).toString();
        const QJsonArray position = object.value(QStringLiteral("position")).toArray();
        if (projectile.id.isEmpty() || restored.contains(projectile.id)
            || (side != QLatin1String("red") && side != QLatin1String("blue"))
            || position.size() < 2) {
            return fail(QStringLiteral("权威导弹标识或阵营无效"));
        }
        projectile.side = sideFromName(side);
        projectile.position = GeoPos{position.at(0).toDouble(), position.at(1).toDouble(),
                                     position.size() >= 3 ? position.at(2).toDouble() : 0.0};
        projectile.previousPosition = projectile.position;
        projectile.headingRad = object.value(QStringLiteral("headingRad")).toDouble();
        projectile.speed = object.value(QStringLiteral("speed")).toDouble();
        projectile.age = object.value(QStringLiteral("age")).toDouble();
        projectile.lifetime = object.value(QStringLiteral("lifetime")).toDouble();
        projectile.terminalAge = object.value(QStringLiteral("terminalAge")).toDouble(0.0);
        projectile.terminalReason = object.value(QStringLiteral("terminalReason")).toString();
        const bool active = object.value(QStringLiteral("active")).toBool();
        projectile.resultSettled = object.value(QStringLiteral("resultSettled"))
                                       .toBool(!active);
        projectile.attackerId = object.value(QStringLiteral("attackerId")).toString();
        projectile.targetId = object.value(QStringLiteral("targetId")).toString();
        if (active) ++activeCount;
        if (!std::isfinite(projectile.position.x) || !std::isfinite(projectile.position.y)
            || !std::isfinite(projectile.position.alt)
            || projectile.position.x < 0.0 || projectile.position.y < 0.0
            || projectile.position.x > m_scenario.map.widthMeters
            || projectile.position.y > m_scenario.map.heightMeters
            || !std::isfinite(projectile.headingRad) || !finiteNonNegative(projectile.speed)
            || !finiteNonNegative(projectile.age) || !std::isfinite(projectile.lifetime)
            || projectile.lifetime <= 0.0 || !finiteNonNegative(projectile.terminalAge)
            || (active && !projectile.terminalReason.isEmpty())
            || (!active && !terminalReasons.contains(projectile.terminalReason))
            || (active && projectile.resultSettled)
            || (!active && !projectile.resultSettled)) {
            return fail(QStringLiteral("权威导弹数值无效: %1").arg(projectile.id));
        }
        restored.emplace(projectile.id, projectile);
    }
    if (activeCount > kMaximumProjectiles) {
        return fail(QStringLiteral("权威在途导弹数量超过上限"));
    }
    for (auto& [id, unitValue] : m_units) {
        Q_UNUSED(id);
        if (auto* attacker = qobject_cast<AttackUAV*>(unitValue.get())) {
            attacker->clearActiveProjectiles();
        }
    }
    m_projectiles = std::move(restored);
    for (const auto& [id, projectile] : m_projectiles) {
        Q_UNUSED(id);
        if (!projectile.active()) continue;
        if (auto* attacker = qobject_cast<AttackUAV*>(unit(projectile.attackerId))) {
            attacker->restoreActiveProjectile(projectile.id);
        }
    }
    emit projectilesChanged();
    return true;
}

void SimulationEngine::applyRemoteRuntimeState(const QJsonArray& units, double simTime,
                                               bool running, double speedMul,
                                               bool partial) {
    m_timer.stop();
    m_realtimeTick.invalidate();
    m_realtimeDebtSeconds = 0.0;
    const bool runningChangedValue = m_running != running;
    const bool speedChangedValue = m_clock->speedMul() != speedMul;
    m_running = running;
    m_clock->setSimTime(std::max(0.0, simTime));
    m_clock->setSpeedMul(std::clamp(speedMul, 0.0, 8.0));
    if (!partial) m_remoteRuntimeProjection.clear();
    for (const QJsonValue& value : units) {
        if (!value.isObject()) continue;
        const QJsonObject projected = value.toObject();
        const QString id = projected.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) m_remoteRuntimeProjection.insert(id, projected);
    }
    SnapshotCodec::decodeRuntimeUnits(*this, units);
    if (runningChangedValue) emit runningChanged();
    if (speedChangedValue) emit speedMulChanged();
    emit simTimeChanged();
    emit unitsChanged();
}

QJsonArray SimulationEngine::collectCheckpointState() const {
    return SnapshotCodec::encodeCheckpointUnits(*this);
}

QJsonObject SimulationEngine::collectGlobalCheckpointState() const {
    QJsonArray projectiles;
    for (const auto& [id, projectile] : m_projectiles) {
        const WeaponProfile& weapon = projectile.request.weapon;
        projectiles.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("attackerId"), projectile.attackerId},
            {QStringLiteral("targetId"), projectile.targetId},
            {QStringLiteral("side"), sideName(projectile.side)},
            {QStringLiteral("position"),
             QJsonArray{std::clamp(projectile.position.x, 0.0, m_scenario.map.widthMeters),
                        std::clamp(projectile.position.y, 0.0, m_scenario.map.heightMeters),
                        projectile.position.alt}},
            {QStringLiteral("previousPosition"),
             QJsonArray{std::clamp(projectile.previousPosition.x, 0.0,
                                   m_scenario.map.widthMeters),
                        std::clamp(projectile.previousPosition.y, 0.0,
                                   m_scenario.map.heightMeters),
                        projectile.previousPosition.alt}},
            {QStringLiteral("headingRad"), projectile.headingRad},
            {QStringLiteral("speed"), projectile.speed},
            {QStringLiteral("age"), projectile.age},
            {QStringLiteral("lifetime"), projectile.lifetime},
            {QStringLiteral("launchTime"), projectile.launchTime},
            {QStringLiteral("terminalAge"), projectile.terminalAge},
            {QStringLiteral("terminalReason"), projectile.terminalReason},
            {QStringLiteral("resultSettled"), projectile.resultSettled},
            {QStringLiteral("request"),
             QJsonObject{{QStringLiteral("shotSequence"),
                          QString::number(projectile.request.shotSequence)},
                         {QStringLiteral("distance"), projectile.request.distance},
                         {QStringLiteral("attackerEffectiveness"),
                          projectile.request.attackerEffectiveness},
                         {QStringLiteral("weapon"),
                          QJsonObject{{QStringLiteral("hitProbability"), weapon.hitProbability},
                                      {QStringLiteral("minRange"), weapon.minRange},
                                      {QStringLiteral("optimalRange"), weapon.optimalRange},
                                      {QStringLiteral("maxRange"), weapon.maxRange},
                                      {QStringLiteral("damageMin"), weapon.damageMin},
                                      {QStringLiteral("damageMax"), weapon.damageMax},
                                      {QStringLiteral("rangeFalloff"), weapon.rangeFalloff}}}}}});
    }
    QJsonArray collisionCooldowns;
    QStringList collisionKeys = m_collisionCooldowns.keys();
    std::sort(collisionKeys.begin(), collisionKeys.end());
    for (const QString& key : collisionKeys) {
        const double remaining = m_collisionCooldowns.value(key);
        if (!std::isfinite(remaining) || remaining <= 1e-9) continue;
        const int separator = key.indexOf(QLatin1Char('|'));
        if (separator <= 0 || separator >= key.size() - 1) continue;
        collisionCooldowns.append(QJsonObject{
            {QStringLiteral("unitAId"), key.left(separator)},
            {QStringLiteral("unitBId"), key.mid(separator + 1)},
            {QStringLiteral("remaining"), remaining}});
    }
    return {{QStringLiteral("schema"), 6},
            {QStringLiteral("combatSeed"), QString::number(m_battleSeed, 16)},
            {QStringLiteral("projectiles"), projectiles},
            {QStringLiteral("scanContacts"), activeScanContacts()},
            {QStringLiteral("collisionCooldowns"), collisionCooldowns}};
}

bool SimulationEngine::restoreGlobalCheckpointState(const QJsonObject& state,
                                                     QString* error) {
    if (error) error->clear();
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (state.isEmpty() || state.value(QStringLiteral("schema")).toInt(2) <= 2) {
        m_projectiles.clear();
        m_scanContacts.clear();
        m_collisionCooldowns.clear();
        for (auto& [id, value] : m_units) {
            Q_UNUSED(id);
            if (auto* attacker = qobject_cast<AttackUAV*>(value.get())) {
                attacker->clearActiveProjectiles();
            }
        }
        emit projectilesChanged();
        emit scanContactsChanged();
        return true;
    }
    const int schema = state.value(QStringLiteral("schema")).toInt();
    if (schema != 3 && schema != 4 && schema != 5 && schema != 6) {
        return fail(QStringLiteral("不支持的全局检查点版本"));
    }
    bool seedOk = false;
    const quint64 seed = state.value(QStringLiteral("combatSeed")).toString()
                             .toULongLong(&seedOk, 16);
    if (!seedOk || seed == 0) return fail(QStringLiteral("全局检查点随机种子无效"));

    const QJsonArray projectileArray = state.value(QStringLiteral("projectiles")).toArray();
    if (projectileArray.size() > kMaximumProjectiles * 2) {
        return fail(QStringLiteral("全局检查点导弹过多"));
    }
    std::map<QString, ProjectileState> restoredProjectiles;
    qsizetype activeCount = 0;
    static const QSet<QString> terminalReasons{
        QStringLiteral("hit"), QStringLiteral("miss"), QStringLiteral("expired"),
        QStringLiteral("out_of_bounds"), QStringLiteral("out_of_range"),
        QStringLiteral("target_lost"),
        QStringLiteral("countermeasured")};
    for (const QJsonValue& value : projectileArray) {
        const QJsonObject object = value.toObject();
        ProjectileState projectile;
        projectile.id = object.value(QStringLiteral("id")).toString();
        projectile.attackerId = object.value(QStringLiteral("attackerId")).toString();
        projectile.targetId = object.value(QStringLiteral("targetId")).toString();
        const QString side = object.value(QStringLiteral("side")).toString();
        const QJsonArray position = object.value(QStringLiteral("position")).toArray();
        const QJsonArray previous = object.value(QStringLiteral("previousPosition")).toArray();
        UnitBase* attackerUnit = unit(projectile.attackerId);
        UnitBase* targetUnit = unit(projectile.targetId);
        if (projectile.id.isEmpty() || restoredProjectiles.contains(projectile.id)
            || !attackerUnit || !targetUnit || attackerUnit->side() == targetUnit->side()
            || sideName(attackerUnit->side()) != side || position.size() != 3
            || previous.size() != 3) {
            return fail(QStringLiteral("全局检查点导弹引用无效: %1").arg(projectile.id));
        }
        projectile.side = attackerUnit->side();
        projectile.position = GeoPos{position.at(0).toDouble(), position.at(1).toDouble(),
                                     position.at(2).toDouble()};
        projectile.previousPosition = GeoPos{previous.at(0).toDouble(), previous.at(1).toDouble(),
                                             previous.at(2).toDouble()};
        projectile.headingRad = object.value(QStringLiteral("headingRad")).toDouble();
        projectile.speed = object.value(QStringLiteral("speed")).toDouble();
        projectile.age = object.value(QStringLiteral("age")).toDouble();
        projectile.lifetime = object.value(QStringLiteral("lifetime")).toDouble();
        projectile.launchTime = object.value(QStringLiteral("launchTime")).toDouble();
        projectile.terminalAge = object.value(QStringLiteral("terminalAge")).toDouble();
        projectile.terminalReason = object.value(QStringLiteral("terminalReason")).toString();
        projectile.resultSettled = schema >= 4
            ? object.value(QStringLiteral("resultSettled")).toBool(false)
            : !projectile.terminalReason.isEmpty();
        const QJsonObject request = object.value(QStringLiteral("request")).toObject();
        bool sequenceOk = false;
        projectile.request.shotSequence = request.value(QStringLiteral("shotSequence"))
                                              .toString().toULongLong(&sequenceOk);
        projectile.request.attackerId = projectile.attackerId;
        projectile.request.targetId = projectile.targetId;
        projectile.request.distance = request.value(QStringLiteral("distance")).toDouble();
        projectile.request.attackerEffectiveness =
            request.value(QStringLiteral("attackerEffectiveness")).toDouble();
        const QJsonObject weapon = request.value(QStringLiteral("weapon")).toObject();
        projectile.request.weapon = WeaponProfile{
            weapon.value(QStringLiteral("hitProbability")).toDouble(),
            weapon.value(QStringLiteral("minRange")).toDouble(),
            weapon.value(QStringLiteral("optimalRange")).toDouble(),
            weapon.value(QStringLiteral("maxRange")).toDouble(),
            weapon.value(QStringLiteral("damageMin")).toDouble(),
            weapon.value(QStringLiteral("damageMax")).toDouble(),
            weapon.value(QStringLiteral("rangeFalloff")).toDouble()};
        const WeaponProfile& profile = projectile.request.weapon;
        const bool active = projectile.active();
        if (active) ++activeCount;
        const bool validPosition = std::isfinite(projectile.position.x)
            && std::isfinite(projectile.position.y) && std::isfinite(projectile.position.alt)
            && projectile.position.x >= 0.0 && projectile.position.y >= 0.0
            && projectile.position.x <= m_scenario.map.widthMeters
            && projectile.position.y <= m_scenario.map.heightMeters
            && std::isfinite(projectile.previousPosition.x)
            && std::isfinite(projectile.previousPosition.y)
            && std::isfinite(projectile.previousPosition.alt);
        const bool validRequest = sequenceOk && finiteNonNegative(projectile.request.distance)
            && std::isfinite(projectile.request.attackerEffectiveness)
            && projectile.request.attackerEffectiveness >= 0.0
            && projectile.request.attackerEffectiveness <= 1.0
            && std::isfinite(profile.hitProbability) && std::isfinite(profile.minRange)
            && std::isfinite(profile.optimalRange) && std::isfinite(profile.maxRange)
            && std::isfinite(profile.damageMin) && std::isfinite(profile.damageMax)
            && std::isfinite(profile.rangeFalloff) && profile.damageMin >= 0.0
            && profile.damageMax >= profile.damageMin;
        // Schema-4 checkpoints written before the speed rebalance may still
        // contain 420 m/s projectiles. They remain valid and continue at the
        // serialized speed after restore; newly launched projectiles use the
        // current 500 m/s profile.
        const bool validKinematics = schema == 3
            ? std::abs(projectile.speed - 360.0) <= 1e-9
                && std::abs(projectile.lifetime - 18.0) <= 1e-9
            : (std::abs(projectile.speed - kProjectileSpeedMps) <= 1e-9
                || std::abs(projectile.speed - 420.0) <= 1e-9)
                && std::abs(projectile.lifetime - kProjectileLifetimeSec) <= 1e-9;
        if (!validPosition || !std::isfinite(projectile.headingRad)
            || !validKinematics
            || !finiteNonNegative(projectile.age)
            || !finiteNonNegative(projectile.launchTime)
            || !finiteNonNegative(projectile.terminalAge)
            || (!active && !terminalReasons.contains(projectile.terminalReason))
            || (active && projectile.resultSettled)
            || (!active && !projectile.resultSettled)
            || !validRequest) {
            return fail(QStringLiteral("全局检查点导弹数值无效: %1").arg(projectile.id));
        }
        restoredProjectiles.emplace(projectile.id, projectile);
    }
    if (activeCount > kMaximumProjectiles) {
        return fail(QStringLiteral("全局检查点在途导弹过多"));
    }

    std::vector<ScanContact> restoredContacts;
    const QJsonArray contacts = state.value(QStringLiteral("scanContacts")).toArray();
    if (contacts.size() > 4096) return fail(QStringLiteral("全局检查点扫描联系过多"));
    QSet<QString> contactKeys;
    for (const QJsonValue& value : contacts) {
        const QJsonObject object = value.toObject();
        ScanContact contact{object.value(QStringLiteral("scannerId")).toString(),
                            object.value(QStringLiteral("targetId")).toString(),
                            sideFromName(object.value(QStringLiteral("side")).toString()),
                            object.value(QStringLiteral("expiresAt")).toDouble()};
        UnitBase* scanner = unit(contact.scannerId);
        UnitBase* target = unit(contact.targetId);
        const QString key = contact.scannerId + QLatin1Char('\0') + contact.targetId;
        if (!scanner || !target || scanner->kind() != UnitKind::ReconUAV
            || scanner->side() != contact.side || target->side() == contact.side
            || !finiteNonNegative(contact.expiresAt) || contactKeys.contains(key)) {
            return fail(QStringLiteral("全局检查点扫描联系无效"));
        }
        contactKeys.insert(key);
        restoredContacts.push_back(contact);
    }

    m_battleSeed = seed;
    m_projectiles = std::move(restoredProjectiles);
    m_scanContacts = std::move(restoredContacts);
    m_collisionCooldowns.clear();
    if (schema >= 5) {
        const QJsonArray cooldowns = state.value(QStringLiteral("collisionCooldowns")).toArray();
        if (cooldowns.size() > 4096) return fail(QStringLiteral("全局检查点碰撞冷却过多"));
        for (const QJsonValue& value : cooldowns) {
            const QJsonObject object = value.toObject();
            const QString first = object.value(QStringLiteral("unitAId")).toString();
            const QString second = object.value(QStringLiteral("unitBId")).toString();
            const double remaining = object.value(QStringLiteral("remaining")).toDouble(-1.0);
            if (first.isEmpty() || second.isEmpty() || first >= second
                || !unit(first) || !unit(second) || !std::isfinite(remaining)
                || remaining <= 0.0 || remaining > 0.75 + 1e-9) {
                return fail(QStringLiteral("全局检查点碰撞冷却无效"));
            }
            m_collisionCooldowns.insert(first + QLatin1Char('|') + second, remaining);
        }
    }
    for (auto& [id, value] : m_units) {
        Q_UNUSED(id);
        if (auto* attacker = qobject_cast<AttackUAV*>(value.get())) {
            attacker->clearActiveProjectiles();
        }
    }
    for (const auto& [id, projectile] : m_projectiles) {
        Q_UNUSED(id);
        if (!projectile.active()) continue;
        if (auto* attacker = qobject_cast<AttackUAV*>(unit(projectile.attackerId))) {
            attacker->restoreActiveProjectile(projectile.id);
        }
    }
    emit projectilesChanged();
    emit scanContactsChanged();
    return true;
}

bool SimulationEngine::restoreCheckpointState(const QJsonArray& units, double simTime,
                                              bool running, double speedMul,
                                              QString* error) {
    return restoreCheckpointState(units, QJsonObject{}, simTime, running, speedMul, error);
}

bool SimulationEngine::restoreCheckpointState(const QJsonArray& units,
                                              const QJsonObject& globalState,
                                              double simTime, bool running,
                                              double speedMul, QString* error) {
    if (error) error->clear();
    if (!std::isfinite(simTime) || simTime < 0.0
        || !std::isfinite(speedMul) || speedMul < 0.0 || speedMul > 8.0) {
        if (error) *error = QStringLiteral("检查点时间或推演速率无效");
        return false;
    }
    const QJsonArray rollbackState = collectCheckpointState();
    const QJsonObject rollbackGlobalState = collectGlobalCheckpointState();
    const bool wasRunning = m_running;
    m_timer.stop();
    QString restoreError;
    if (!SnapshotCodec::decodeCheckpointUnits(*this, units, &restoreError)
        || !SnapshotCodec::decodeGlobalCheckpoint(*this, globalState, &restoreError)) {
        SnapshotCodec::decodeCheckpointUnits(*this, rollbackState, nullptr);
        SnapshotCodec::decodeGlobalCheckpoint(*this, rollbackGlobalState, nullptr);
        if (wasRunning) {
            m_realtimeDebtSeconds = 0.0;
            m_realtimeTick.start();
            m_timer.start();
        }
        if (error) *error = restoreError;
        return false;
    }
    m_clock->setSimTime(simTime);
    m_clock->setSpeedMul(speedMul);
    applyFrozenSidePolicy();
    m_running = running && m_readyForSim;
    if (m_running) {
        m_realtimeDebtSeconds = 0.0;
        m_realtimeTick.start();
        m_timer.start();
    } else {
        m_realtimeTick.invalidate();
        m_realtimeDebtSeconds = 0.0;
    }
    emit runningChanged();
    emit speedMulChanged();
    emit simTimeChanged();
    emit unitsChanged();
    emit projectilesChanged();
    emit scanContactsChanged();
    return true;
}

QVariantList SimulationEngine::unitsForView() const {
    QVariantList l;
    for (const auto& [id, u] : m_units) {
        QVariantMap m;
        m["id"] = id;
        m["callsign"] = u->callsign();
        m["kind"] = u->kindStr();
        m["side"] = u->sideStr();
        m["hp"] = u->hp();
        m["maxHp"] = u->maxHp();
        m["alive"] = u->alive();
        m["movable"] = u->movable();
        QVariantList p; p << u->pos().x << u->pos().y << u->pos().alt;
        m["position"] = p;
        m["detectRange"] = u->detectRange();
        m["attackRange"] = u->attackRange();
        m["commRange"] = u->commRange();
        m["speed"] = u->speed();
        m["baseSpeed"] = u->baseSpeed();
        m["maxCommandedSpeed"] = u->maxCommandedSpeed();
        m["fuelRemaining"] = u->fuelRemaining();
        m["fuelCapacity"] = u->fuelCapacity();
        m["fuelBurnRate"] = u->fuelBurnRate();
        const double endurance = u->estimatedFuelEndurance();
        m["estimatedEnduranceSec"] = std::isfinite(endurance) ? endurance : -1.0;
        m["abilities"] = u->abilityStateJson().toVariantMap();
        m["serviceRequested"] = u->serviceRequested();
        m["serviceProgress"] = u->serviceProgress();
        m["serviceDuration"] = u->serviceDuration();
        if (auto* attacker = qobject_cast<AttackUAV*>(u.get())) {
            m["ammoRemaining"] = attacker->ammoRemaining();
            m["ammoCapacity"] = attacker->ammoCapacity();
            m["cooldownRemaining"] = attacker->cooldownRemaining();
            m["lastShotOutcome"] = attacker->lastShotOutcome();
            m["fuelRemaining"] = attacker->fuelRemaining();
            m["fuelCapacity"] = attacker->fuelCapacity();
            m["turnaroundProgress"] = attacker->turnaroundProgress();
        }
        if (const auto projected = m_remoteRuntimeProjection.constFind(id);
            projected != m_remoteRuntimeProjection.constEnd()) {
            const QVariantMap projectedMap = projected->toVariantMap();
            for (auto field = projectedMap.constBegin(); field != projectedMap.constEnd(); ++field) {
                m.insert(field.key(), field.value());
            }
        }
        l.append(m);
    }
    return l;
}

void SimulationEngine::appendTimeline(const QString& category, const QString& title,
                                      const QJsonObject& details,
                                      const QString& level) {
    if (m_replaying) return;
    QJsonObject event{{QStringLiteral("sequence"), ++m_timelineSequence},
                      {QStringLiteral("simTime"), simTime()},
                      {QStringLiteral("category"), category},
                      {QStringLiteral("title"), title},
                      {QStringLiteral("level"), level},
                      {QStringLiteral("details"), details}};
    m_timeline.append(event);
    constexpr qsizetype kMaxTimelineEvents = 20000;
    while (m_timeline.size() > kMaxTimelineEvents) m_timeline.removeFirst();
    emit timelineChanged();
}

void SimulationEngine::captureReplayCheckpoint() {
    if (m_replaying || !m_replayRecordingEnabled) return;
    m_replayCheckpoints.push_back(ReplayCheckpoint{
        simTime(), static_cast<qsizetype>(m_replayCommands.size()),
        static_cast<qsizetype>(m_replaySteps.size()), collectCheckpointState(),
        collectGlobalCheckpointState()});
    m_lastReplayCheckpointTime = simTime();
    constexpr size_t kMaxReplayCheckpoints = 720;
    if (m_replayCheckpoints.size() > kMaxReplayCheckpoints) {
        m_replayCheckpoints.erase(m_replayCheckpoints.begin() + 1);
    }
}

double SimulationEngine::replayDuration() const {
    if (!m_replayRecordingEnabled) return 0.0;
    return std::max(m_recordedDuration,
                    m_replayCommands.empty() ? 0.0 : m_replayCommands.back().time);
}

bool SimulationEngine::seekReplay(double targetTime, QString* error) {
    if (error) error->clear();
    if (!m_replayRecordingEnabled) {
        if (error) *error = QStringLiteral("当前引擎未启用本地回放记录");
        return false;
    }
    if (!std::isfinite(targetTime) || targetTime < 0.0
        || targetTime > replayDuration() + 1e-6) {
        if (error) *error = QStringLiteral("回放时间超出可用范围");
        return false;
    }
    if (m_replayInitialScenario.units.empty() || m_replayInitialSeed == 0) {
        if (error) *error = QStringLiteral("当前推演没有可用回放基线");
        return false;
    }

    const auto commands = m_replayCommands;
    const auto steps = m_replaySteps;
    const auto checkpoints = m_replayCheckpoints;
    const QJsonArray timeline = m_timeline;
    const qint64 timelineSequence = m_timelineSequence;
    const Scenario initialScenario = m_replayInitialScenario;
    const quint64 initialSeed = m_replayInitialSeed;
    const double duration = m_recordedDuration;
    const QJsonArray finalSnapshot = m_recordedFinalSnapshot;
    const QJsonArray finalProjectiles = m_recordedFinalProjectiles;
    const double previousSpeed = speedMul();

    const ReplayCheckpoint* selected = nullptr;
    for (const ReplayCheckpoint& checkpoint : checkpoints) {
        if (checkpoint.time <= targetTime + 1e-9
            && (!selected || checkpoint.time > selected->time)) {
            selected = &checkpoint;
        }
    }

    m_replaying = true;
    setRunning(false);
    if (!setScenario(initialScenario)) {
        m_replaying = false;
        if (error) *error = QStringLiteral("无法重建回放初始场景");
        return false;
    }
    restoreCombatSeed(initialSeed);
    qsizetype commandIndex = 0;
    qsizetype stepIndex = 0;
    if (selected && selected->time > 0.0) {
        QString restoreError;
        if (!restoreCheckpointState(selected->state, selected->globalState,
                                    selected->time, false, previousSpeed,
                                    &restoreError)) {
            m_replaying = false;
            if (error) *error = QStringLiteral("回放检查点恢复失败: %1").arg(restoreError);
            return false;
        }
        commandIndex = selected->commandCount;
        stepIndex = selected->stepCount;
    }

    while (simTime() < targetTime - 1e-9) {
        while (commandIndex < static_cast<qsizetype>(commands.size())
               && commands[commandIndex].time <= simTime() + 1e-9) {
            executeCommand(commands[commandIndex].action, commands[commandIndex].args);
            ++commandIndex;
        }
        while (stepIndex < static_cast<qsizetype>(steps.size())
               && steps[stepIndex].startTime < simTime() - 1e-9) {
            ++stepIndex;
        }
        double recordedStep = 0.05;
        if (stepIndex < static_cast<qsizetype>(steps.size())
            && std::abs(steps[stepIndex].startTime - simTime()) <= 1e-6) {
            recordedStep = steps[stepIndex].duration;
            ++stepIndex;
        }
        const double step = std::min(recordedStep, targetTime - simTime());
        if (step <= 1e-9) continue;
        onTickInternal(true, step);
    }
    while (commandIndex < static_cast<qsizetype>(commands.size())
           && commands[commandIndex].time <= targetTime + 1e-9) {
        executeCommand(commands[commandIndex].action, commands[commandIndex].args);
        ++commandIndex;
    }

    m_replaying = false;
    m_replayCommands = commands;
    m_replaySteps = steps;
    m_replayCheckpoints = checkpoints;
    m_replayInitialScenario = initialScenario;
    m_replayInitialSeed = initialSeed;
    m_timeline = timeline;
    m_timelineSequence = timelineSequence;
    m_recordedDuration = duration;
    m_recordedFinalSnapshot = finalSnapshot;
    m_recordedFinalProjectiles = finalProjectiles;
    m_clock->setSpeedMul(previousSpeed);
    emit runningChanged();
    emit speedMulChanged();
    emit simTimeChanged();
    emit unitsChanged();
    emit projectilesChanged();
    emit scanContactsChanged();
    emit timelineChanged();
    return true;
}

QJsonObject SimulationEngine::battleReport() const {
    int shots = 0;
    int hits = 0;
    int kills = 0;
    double damage = 0.0;
    for (const QJsonValue& value : m_timeline) {
        const QJsonObject event = value.toObject();
        if (event.value(QStringLiteral("category")).toString() != QLatin1String("combat")) continue;
        ++shots;
        const QJsonObject details = event.value(QStringLiteral("details")).toObject();
        if (details.value(QStringLiteral("hit")).toBool()) ++hits;
        if (details.value(QStringLiteral("kill")).toBool()) ++kills;
        damage += details.value(QStringLiteral("damage")).toDouble();
    }
    int redLosses = 0;
    int blueLosses = 0;
    for (const QJsonValue& value : m_recordedFinalSnapshot) {
        const QJsonObject unit = value.toObject();
        if (unit.value(QStringLiteral("alive")).toBool()) continue;
        if (unit.value(QStringLiteral("side")).toString() == QLatin1String("red")) ++redLosses;
        else if (unit.value(QStringLiteral("side")).toString() == QLatin1String("blue")) ++blueLosses;
    }
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("duration"), replayDuration()},
            {QStringLiteral("battleSeed"), QString::number(m_replayInitialSeed, 16)},
            {QStringLiteral("summary"),
             QJsonObject{{QStringLiteral("shots"), shots},
                         {QStringLiteral("hits"), hits},
                         {QStringLiteral("kills"), kills},
                         {QStringLiteral("damage"), damage},
                         {QStringLiteral("redLosses"), redLosses},
                         {QStringLiteral("blueLosses"), blueLosses}}},
            {QStringLiteral("scenario"), ScenarioIo::toJson(m_replayInitialScenario)},
            {QStringLiteral("finalUnits"), m_recordedFinalSnapshot},
            {QStringLiteral("finalProjectiles"), m_recordedFinalProjectiles},
            {QStringLiteral("events"), m_timeline}};
}

void SimulationEngine::checkWinLoseCondition() {
    if (m_outcomeReported) return;
    bool redCpAlive = false, blueCpAlive = false;
    for (const auto& [id, u] : m_units) {
        if (u->kind() != UnitKind::CommandPost || !u->alive()) continue;
        if (u->sideStr() == "red") redCpAlive = true;
        if (u->sideStr() == "blue") blueCpAlive = true;
    }
    if (!redCpAlive || !blueCpAlive) {
        m_outcomeReported = true;
        QString winner, loser;
        if (!redCpAlive && !blueCpAlive) {
            winner = "平局——双方指挥所均被摧毁";
            loser = "";
        } else if (!redCpAlive) {
            winner = "蓝方"; loser = "红方";
        } else {
            winner = "红方"; loser = "蓝方";
        }
        setRunning(false);
        appendTimeline(QStringLiteral("outcome"), QStringLiteral("推演结束"),
                       QJsonObject{{QStringLiteral("winner"), winner},
                                   {QStringLiteral("loser"), loser}},
                       QStringLiteral("warn"));
        emit simulationEnded(winner, loser);
    }
}

} // namespace gbr
