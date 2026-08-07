#include "RoomPersistence.h"

#include "RulesAi.h"

#include "protocol/Protocol.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStringList>
#include <QUuid>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace gbr {

namespace {

constexpr int kCheckpointSchemaVersion = 4;
constexpr int kOldestSupportedCheckpointSchemaVersion = 2;
constexpr qint64 kMaxEventLogBytes = 16 * 1024 * 1024;
constexpr qint64 kMaxCheckpointBytes = 64 * 1024 * 1024;

qint64 eventLogLimit() {
    bool ok = false;
    const qint64 configured = qEnvironmentVariable("WARGAME_EVENT_LOG_MAX_BYTES").toLongLong(&ok);
    return ok && configured > 0 && configured < kMaxEventLogBytes ? configured : kMaxEventLogBytes;
}

bool ensureParentDirectory(const QString& path, QString* error) {
    const QString directory = QFileInfo(path).absolutePath();
    if (QDir().mkpath(directory)) return true;
    if (error) *error = QStringLiteral("无法创建持久化目录: %1").arg(directory);
    return false;
}

bool isWithinRoot(const QString& path, const QString& root) {
    const QString relative = QDir(root).relativeFilePath(path);
    return relative != QLatin1String("..") && !relative.startsWith(QStringLiteral("../"))
        && !QDir::isAbsolutePath(relative);
}

bool durableFlush(QFileDevice& file, const QString& path, QString* error) {
    if (qEnvironmentVariableIntValue("WARGAME_FORCE_FLUSH_FAILURE") != 0) {
        if (error) *error = QStringLiteral("持久化文件同步到磁盘失败（测试注入）: %1").arg(path);
        return false;
    }
    if (!file.flush()) {
        if (error) *error = QStringLiteral("持久化文件写入失败: %1").arg(path);
        return false;
    }
#ifdef Q_OS_UNIX
    if (::fsync(file.handle()) != 0) {
        if (error) *error = QStringLiteral("持久化文件同步到磁盘失败: %1").arg(path);
        return false;
    }
#elif defined(Q_OS_WIN)
    const intptr_t descriptor = static_cast<intptr_t>(file.handle());
    const intptr_t nativeHandle = _get_osfhandle(static_cast<int>(descriptor));
    if (nativeHandle == -1
        || !FlushFileBuffers(reinterpret_cast<HANDLE>(nativeHandle))) {
        if (error) *error = QStringLiteral("持久化文件同步到磁盘失败: %1").arg(path);
        return false;
    }
#endif
    return true;
}

bool durableSyncParentDirectory(const QString& path, QString* error) {
#ifdef Q_OS_UNIX
    const QByteArray directory = QFile::encodeName(QFileInfo(path).absolutePath());
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int descriptor = ::open(directory.constData(), flags);
    if (descriptor < 0) {
        if (error) *error = QStringLiteral("无法打开持久化目录进行同步: %1")
                                .arg(QFileInfo(path).absolutePath());
        return false;
    }
    const bool synced = ::fsync(descriptor) == 0;
    ::close(descriptor);
    if (!synced) {
        if (error) *error = QStringLiteral("持久化目录同步到磁盘失败: %1")
                                .arg(QFileInfo(path).absolutePath());
        return false;
    }
#else
    Q_UNUSED(path);
    Q_UNUSED(error);
#endif
    return true;
}

bool highestEventSequence(const QString& path, quint64* sequence, QString* error) {
    if (sequence) *sequence = 0;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取待轮换事件日志: %1").arg(path);
        return false;
    }
    quint64 highest = 0;
    qint64 lineNumber = 0;
    while (!file.atEnd()) {
        ++lineNumber;
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        const qint64 parsed = document.object().value(QStringLiteral("sequence")).toInteger();
        if (!document.isObject() || parsed <= 0) {
            if (error) {
                *error = QStringLiteral("待轮换事件日志 %1 第 %2 行无效")
                             .arg(path).arg(lineNumber);
            }
            return false;
        }
        highest = std::max(highest, static_cast<quint64>(parsed));
    }
    if (sequence) *sequence = highest;
    return true;
}

bool rotationRename(const QString& source, const QString& destination, bool installing,
                    int generation) {
    if (installing && qEnvironmentVariableIsSet("WARGAME_FORCE_ROTATION_INSTALL_RENAME_FAILURE")
        && qEnvironmentVariableIntValue("WARGAME_FORCE_ROTATION_INSTALL_RENAME_FAILURE")
               == generation) {
        return false;
    }
    return QFile::rename(source, destination);
}

QJsonObject aiStateToJson(const AiCheckpointState& state) {
    QJsonObject object{
        {QStringLiteral("matchGeneration"), QString::number(state.matchGeneration)},
        {QStringLiteral("commandSequence"), QString::number(state.commandSequence)},
        {QStringLiteral("planningGeneration"), QString::number(state.planningGeneration)},
        {QStringLiteral("rngState"), QString::number(state.rngState)},
        {QStringLiteral("aiDifficulty"), state.aiDifficulty},
        {QStringLiteral("providerMode"), state.providerMode},
        {QStringLiteral("providerModel"), state.providerModel},
        {QStringLiteral("selectedProvider"), state.selectedProvider},
        {QStringLiteral("selectedModel"), state.selectedModel},
        {QStringLiteral("resolvedModel"), state.resolvedModel},
        {QStringLiteral("roomConfigVersion"), QString::number(state.roomConfigVersion)},
        {QStringLiteral("ollamaConfigVersion"), QString::number(state.ollamaConfigVersion)},
        {QStringLiteral("fallbackReason"), state.fallbackReason},
        {QStringLiteral("nextDecisionAt"), state.nextDecisionAt},
        {QStringLiteral("nextReplanAt"), state.nextReplanAt},
        {QStringLiteral("consecutiveFailures"), state.consecutiveFailures},
        {QStringLiteral("stickyRules"), state.stickyRules},
        {QStringLiteral("effectiveEngine"), state.effectiveEngine},
        {QStringLiteral("lastFailureClass"), state.lastFailureClass},
        {QStringLiteral("providerRequests"), QString::number(state.providerRequests)},
        {QStringLiteral("providerSuccesses"), QString::number(state.providerSuccesses)},
        {QStringLiteral("providerFailures"), QString::number(state.providerFailures)},
        {QStringLiteral("lastLatencyMs"), state.lastLatencyMs},
        {QStringLiteral("averageLatencyMs"), state.averageLatencyMs},
        {QStringLiteral("strategyPhase"), state.strategyPhase},
        {QStringLiteral("replanReason"), state.replanReason},
        {QStringLiteral("contactMemory"), state.contactMemory},
        {QStringLiteral("nextPrivilegedSampleAt"), state.nextPrivilegedSampleAt},
        {QStringLiteral("privilegedSampleSequence"),
         QString::number(state.privilegedSampleSequence)}};
    if (state.currentPlan.has_value()) {
        object[QStringLiteral("currentPlan")] = state.currentPlan->toJson();
    }
    return object;
}

bool unsignedField(const QJsonObject& object, const QString& name, quint64* value) {
    const QJsonValue field = object.value(name);
    if (!field.isString()) return false;
    bool ok = false;
    const quint64 parsed = field.toString().toULongLong(&ok);
    if (!ok || QString::number(parsed) != field.toString()) return false;
    if (value) *value = parsed;
    return true;
}

bool aiStateFromJson(const QJsonObject& object, AiCheckpointState* state, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    AiCheckpointState parsed;
    if (!unsignedField(object, QStringLiteral("matchGeneration"), &parsed.matchGeneration)
        || parsed.matchGeneration == 0
        || !unsignedField(object, QStringLiteral("commandSequence"), &parsed.commandSequence)
        || !unsignedField(object, QStringLiteral("planningGeneration"),
                          &parsed.planningGeneration)
        || !unsignedField(object, QStringLiteral("rngState"), &parsed.rngState)
        || parsed.rngState == 0
        || !unsignedField(object, QStringLiteral("providerRequests"),
                          &parsed.providerRequests)
        || !unsignedField(object, QStringLiteral("providerSuccesses"),
                          &parsed.providerSuccesses)
        || !unsignedField(object, QStringLiteral("providerFailures"),
                          &parsed.providerFailures)) {
        return fail(QStringLiteral("AI 检查点无符号计数器无效"));
    }
    const QJsonValue nextDecisionAt = object.value(QStringLiteral("nextDecisionAt"));
    const QJsonValue nextReplanAt = object.value(QStringLiteral("nextReplanAt"));
    const QJsonValue consecutiveFailures = object.value(QStringLiteral("consecutiveFailures"));
    const QJsonValue stickyRules = object.value(QStringLiteral("stickyRules"));
    const QJsonValue effectiveEngine = object.value(QStringLiteral("effectiveEngine"));
    const QJsonValue lastFailureClass = object.value(QStringLiteral("lastFailureClass"));
    const QJsonValue lastLatencyMs = object.value(QStringLiteral("lastLatencyMs"));
    const QJsonValue averageLatencyMs = object.value(QStringLiteral("averageLatencyMs"));
    parsed.nextDecisionAt = nextDecisionAt.toDouble(-1.0);
    parsed.nextReplanAt = nextReplanAt.toDouble(-1.0);
    parsed.consecutiveFailures = consecutiveFailures.toInt(-1);
    parsed.stickyRules = stickyRules.toBool();
    parsed.effectiveEngine = effectiveEngine.toString();
    parsed.lastFailureClass = lastFailureClass.toString();
    parsed.lastLatencyMs = lastLatencyMs.toInteger(-1);
    parsed.averageLatencyMs = averageLatencyMs.toInteger(-1);
    parsed.aiDifficulty = object.value(QStringLiteral("aiDifficulty"))
                              .toString(QStringLiteral("normal"));
    parsed.providerMode = object.value(QStringLiteral("providerMode"))
                              .toString(QStringLiteral("auto"));
    parsed.providerModel = object.value(QStringLiteral("providerModel"))
                               .toString(QStringLiteral("qwen3:4b"));
    parsed.selectedProvider = object.value(QStringLiteral("selectedProvider"))
                                 .toString(parsed.providerMode == QLatin1String("rules")
                                               ? QStringLiteral("rules")
                                               : QStringLiteral("ollama"));
    parsed.selectedModel = object.value(QStringLiteral("selectedModel")).toString();
    parsed.resolvedModel = object.value(QStringLiteral("resolvedModel")).toString();
    parsed.fallbackReason = object.value(QStringLiteral("fallbackReason")).toString();
    if (object.contains(QStringLiteral("roomConfigVersion"))
        && !unsignedField(object, QStringLiteral("roomConfigVersion"),
                          &parsed.roomConfigVersion)) {
        return fail(QStringLiteral("AI 房间配置版本无效"));
    }
    if (object.contains(QStringLiteral("ollamaConfigVersion"))
        && !unsignedField(object, QStringLiteral("ollamaConfigVersion"),
                          &parsed.ollamaConfigVersion)) {
        return fail(QStringLiteral("AI Ollama 配置版本无效"));
    }
    parsed.strategyPhase = object.value(QStringLiteral("strategyPhase"))
                               .toString(QStringLiteral("recon"));
    parsed.replanReason = object.value(QStringLiteral("replanReason")).toString();
    parsed.nextPrivilegedSampleAt = object.value(QStringLiteral("nextPrivilegedSampleAt"))
                                        .toDouble(0.0);
    const QJsonValue privilegedSampleSequence = object.value(
        QStringLiteral("privilegedSampleSequence"));
    if (object.contains(QStringLiteral("privilegedSampleSequence"))) {
        if (!privilegedSampleSequence.isString()) {
            return fail(QStringLiteral("AI 战略采样序号无效"));
        }
        bool sequenceOk = false;
        parsed.privilegedSampleSequence = privilegedSampleSequence.toString().toULongLong(&sequenceOk);
        if (!sequenceOk || QString::number(parsed.privilegedSampleSequence)
                               != privilegedSampleSequence.toString()) {
            return fail(QStringLiteral("AI 战略采样序号无效"));
        }
    }
    if (!nextDecisionAt.isDouble() || !nextReplanAt.isDouble()
        || !std::isfinite(parsed.nextDecisionAt) || parsed.nextDecisionAt < 0.0
        || !std::isfinite(parsed.nextReplanAt) || parsed.nextReplanAt < 0.0
        || !consecutiveFailures.isDouble() || parsed.consecutiveFailures < 0
        || !stickyRules.isBool() || !effectiveEngine.isString()
        || (parsed.effectiveEngine != QLatin1String("rules")
            && parsed.effectiveEngine != QLatin1String("ollama"))
        || !lastFailureClass.isString() || parsed.lastFailureClass.size() > 128
        || (parsed.aiDifficulty != QLatin1String("easy")
            && parsed.aiDifficulty != QLatin1String("normal")
            && parsed.aiDifficulty != QLatin1String("hard"))
        || (parsed.providerMode != QLatin1String("rules")
            && parsed.providerMode != QLatin1String("auto")
            && parsed.providerMode != QLatin1String("ollama"))
        || parsed.providerModel.isEmpty() || parsed.providerModel.size() > 128
        || (parsed.selectedProvider != QLatin1String("rules")
            && parsed.selectedProvider != QLatin1String("ollama"))
        || parsed.selectedModel.size() > 128
        || parsed.resolvedModel.size() > 128
        || parsed.fallbackReason.size() > 256
        || parsed.strategyPhase.isEmpty() || parsed.strategyPhase.size() > 64
        || parsed.replanReason.size() > 128
        || !lastLatencyMs.isDouble() || parsed.lastLatencyMs < 0
        || !averageLatencyMs.isDouble() || parsed.averageLatencyMs < 0
        || !std::isfinite(parsed.nextPrivilegedSampleAt)
        || parsed.nextPrivilegedSampleAt < 0.0
        || parsed.providerSuccesses > parsed.providerRequests
        || parsed.providerFailures > parsed.providerRequests - parsed.providerSuccesses) {
        return fail(QStringLiteral("AI 检查点状态无效"));
    }
    const QJsonValue contactMemory = object.value(QStringLiteral("contactMemory"));
    if (!contactMemory.isUndefined()) {
        if (!contactMemory.isArray() || contactMemory.toArray().size() > 256) {
            return fail(QStringLiteral("AI 接触记忆结构无效"));
        }
        for (const QJsonValue& value : contactMemory.toArray()) {
            if (!value.isObject()) return fail(QStringLiteral("AI 接触记忆项无效"));
            AiObservedTarget parsedContact;
            QString contactError;
            if (!AiObservedTarget::fromJson(value.toObject(), &parsedContact, &contactError)) {
                return fail(QStringLiteral("AI 接触记忆项无效: %1").arg(contactError));
            }
        }
        parsed.contactMemory = contactMemory.toArray();
    }
    const QJsonValue currentPlan = object.value(QStringLiteral("currentPlan"));
    if (!currentPlan.isUndefined()) {
        if (!currentPlan.isObject()) return fail(QStringLiteral("AI 检查点计划无效"));
        AiPlanV1 plan;
        QString planError;
        if (!AiPlanV1::fromJson(currentPlan.toObject(), &plan, &planError)) {
            return fail(QStringLiteral("AI 检查点计划无效: %1").arg(planError));
        }
        if (plan.matchGeneration != parsed.matchGeneration) {
            return fail(QStringLiteral("AI 检查点计划与对局代次不匹配"));
        }
        parsed.currentPlan = plan;
    }
    if (state) *state = parsed;
    return true;
}

QJsonObject checkpointToJson(const RoomCheckpoint& checkpoint) {
    QJsonObject object{
        {QStringLiteral("checkpointSchemaVersion"), kCheckpointSchemaVersion},
        {QStringLiteral("protocolVersion"), Protocol::Version},
        {QStringLiteral("savedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("scenario"), ScenarioIo::toJson(checkpoint.scenario)},
        {QStringLiteral("runInitialScenario"), ScenarioIo::toJson(checkpoint.runInitialScenario)},
        {QStringLiteral("runtimeUnits"), checkpoint.runtimeUnits},
        {QStringLiteral("engineState"), checkpoint.engineState},
        {QStringLiteral("commandHistory"), checkpoint.commandHistory},
        {QStringLiteral("authoritativeRoom"), checkpoint.authoritativeRoom},
        {QStringLiteral("roomState"),
         QJsonObject{{QStringLiteral("phase"), checkpoint.phase},
                     {QStringLiteral("redReady"), checkpoint.redReady},
                     {QStringLiteral("blueReady"), checkpoint.blueReady},
                     {QStringLiteral("running"), checkpoint.running},
                     {QStringLiteral("simTime"), checkpoint.simTime},
                     {QStringLiteral("speed"), checkpoint.speed},
                     {QStringLiteral("scenarioRevision"),
                      static_cast<qint64>(checkpoint.scenarioRevision)},
                     {QStringLiteral("stateRevision"),
                      static_cast<qint64>(checkpoint.stateRevision)},
                     {QStringLiteral("eventSequence"),
                      static_cast<qint64>(checkpoint.eventSequence)},
                     {QStringLiteral("mapMarks"), checkpoint.mapMarks}}}};
    if (checkpoint.aiState.has_value()) {
        object[QStringLiteral("aiState")] = aiStateToJson(*checkpoint.aiState);
    }
    return object;
}

bool checkpointFromJson(const QJsonObject& object, RoomCheckpoint* checkpoint,
                        QString* error) {
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const int checkpointSchemaVersion = object.value(
        QStringLiteral("checkpointSchemaVersion")).toInt();
    if (checkpointSchemaVersion < kOldestSupportedCheckpointSchemaVersion
        || checkpointSchemaVersion > kCheckpointSchemaVersion) {
        return fail(QStringLiteral("检查点结构版本不兼容"));
    }
    const int storedProtocolVersion = object.value(QStringLiteral("protocolVersion")).toInt();
    const bool compatibleProtocol = checkpointSchemaVersion >= 4
        ? storedProtocolVersion == 4
        : storedProtocolVersion == 3 || storedProtocolVersion == 4;
    if (!compatibleProtocol) {
        return fail(QStringLiteral("检查点协议版本不兼容"));
    }
    if (!object.value(QStringLiteral("scenario")).isObject()
        || !object.value(QStringLiteral("runInitialScenario")).isObject()
        || !object.value(QStringLiteral("runtimeUnits")).isArray()
        || (checkpointSchemaVersion >= 3
            && !object.value(QStringLiteral("engineState")).isObject())
        || !object.value(QStringLiteral("commandHistory")).isArray()
        || !object.value(QStringLiteral("authoritativeRoom")).isObject()
        || !object.value(QStringLiteral("roomState")).isObject()) {
        return fail(QStringLiteral("检查点必需字段类型无效"));
    }
    const QJsonObject room = object.value(QStringLiteral("roomState")).toObject();
    const QString phase = room.value(QStringLiteral("phase")).toString();
    if (phase != QLatin1String("preparing") && phase != QLatin1String("running")
        && phase != QLatin1String("paused") && phase != QLatin1String("finished")) {
        return fail(QStringLiteral("检查点推演阶段无效"));
    }
    for (const QString& field : {QStringLiteral("redReady"), QStringLiteral("blueReady"),
                                 QStringLiteral("running")}) {
        if (!room.value(field).isBool()) {
            return fail(QStringLiteral("检查点布尔状态无效: %1").arg(field));
        }
    }
    const double simTime = room.value(QStringLiteral("simTime")).toDouble(-1.0);
    const double speed = room.value(QStringLiteral("speed")).toDouble(-1.0);
    const bool running = room.value(QStringLiteral("running")).toBool();
    const qint64 scenarioRevision = room.value(QStringLiteral("scenarioRevision")).toInteger();
    const qint64 stateRevision = room.value(QStringLiteral("stateRevision")).toInteger();
    const qint64 eventSequence = room.value(QStringLiteral("eventSequence")).toInteger();
    if (!std::isfinite(simTime) || simTime < 0.0 || !std::isfinite(speed)
        || speed < 0.0 || speed > 8.0 || scenarioRevision <= 0
        || stateRevision <= 0 || eventSequence < 0) {
        return fail(QStringLiteral("检查点房间状态无效"));
    }
    if ((phase == QLatin1String("running")) != running) {
        return fail(QStringLiteral("检查点阶段与运行状态冲突"));
    }
    const Scenario scenario = ScenarioIo::fromJson(object.value(QStringLiteral("scenario")).toObject());
    const QJsonArray runtimeUnits = object.value(QStringLiteral("runtimeUnits")).toArray();
    if (scenario.units.empty()
        && (phase != QLatin1String("preparing") || room.value(QStringLiteral("running")).toBool()
            || !runtimeUnits.isEmpty())) {
        return fail(QStringLiteral("空场景只能用于未运行的准备阶段"));
    }

    const Scenario runInitialScenario = ScenarioIo::fromJson(
        object.value(QStringLiteral("runInitialScenario")).toObject());
    if (phase != QLatin1String("preparing") && runInitialScenario.units.empty()) {
        return fail(QStringLiteral("检查点缺少开局场景"));
    }

    checkpoint->sourceSchemaVersion = checkpointSchemaVersion;
    checkpoint->scenario = scenario;
    checkpoint->runInitialScenario = runInitialScenario;
    checkpoint->runtimeUnits = runtimeUnits;
    checkpoint->engineState = checkpointSchemaVersion >= 3
        ? object.value(QStringLiteral("engineState")).toObject() : QJsonObject{};
    checkpoint->commandHistory = object.value(QStringLiteral("commandHistory")).toArray();
    checkpoint->authoritativeRoom = object.value(QStringLiteral("authoritativeRoom")).toObject();
    checkpoint->phase = phase;
    checkpoint->redReady = room.value(QStringLiteral("redReady")).toBool();
    checkpoint->blueReady = room.value(QStringLiteral("blueReady")).toBool();
    checkpoint->running = running;
    checkpoint->simTime = simTime;
    checkpoint->speed = speed;
    checkpoint->scenarioRevision = static_cast<quint64>(scenarioRevision);
    checkpoint->stateRevision = static_cast<quint64>(stateRevision);
    checkpoint->eventSequence = static_cast<quint64>(eventSequence);
    const QJsonValue mapMarks = room.value(QStringLiteral("mapMarks"));
    if (!mapMarks.isUndefined() && !mapMarks.isArray()) {
        return fail(QStringLiteral("检查点地图标记结构无效"));
    }
    checkpoint->mapMarks = mapMarks.toArray();
    if (checkpoint->mapMarks.size() > 200) {
        return fail(QStringLiteral("检查点地图标记过多"));
    }
    const QJsonValue aiState = object.value(QStringLiteral("aiState"));
    checkpoint->aiState.reset();
    if (!aiState.isUndefined()) {
        if (!aiState.isObject()) return fail(QStringLiteral("AI 检查点结构无效"));
        AiCheckpointState parsed;
        if (!aiStateFromJson(aiState.toObject(), &parsed, error)) return false;
        checkpoint->aiState = parsed;
    }
    return true;
}

} // namespace

QString RoomPersistence::resolvePathWithinRoot(const QString& path, const QString& dataDir,
                                               QString* error) {
    if (error) error->clear();
    const QString rootInput = dataDir.trimmed();
    if (rootInput.isEmpty()) {
        if (error) *error = QStringLiteral("DATA_DIR 不能为空");
        return {};
    }
    const QString rootAbsolute = QFileInfo(rootInput).absoluteFilePath();
    if (!QDir().mkpath(rootAbsolute)) {
        if (error) *error = QStringLiteral("无法创建 DATA_DIR: %1").arg(rootAbsolute);
        return {};
    }
    const QString root = QFileInfo(rootAbsolute).canonicalFilePath();
    if (root.isEmpty()) {
        if (error) *error = QStringLiteral("DATA_DIR 不是可解析目录: %1").arg(rootAbsolute);
        return {};
    }
    const QString input = path.trimmed();
    if (input.isEmpty()) {
        if (error) *error = QStringLiteral("持久化路径为空");
        return {};
    }
    const QString candidate = QDir::isAbsolutePath(input)
        ? QDir::cleanPath(input) : QDir(root).filePath(QDir::cleanPath(input));
    if (!isWithinRoot(candidate, root)) {
        if (error) *error = QStringLiteral("持久化路径超出 DATA_DIR: %1").arg(input);
        return {};
    }
    QFileInfo candidateInfo(candidate);
    if (candidateInfo.isSymLink()) {
        const QString linkTarget = candidateInfo.symLinkTarget();
        const QString canonicalTarget = QFileInfo(linkTarget).canonicalFilePath();
        if (canonicalTarget.isEmpty() || !isWithinRoot(canonicalTarget, root)) {
            if (error) *error = QStringLiteral("持久化目标符号链接越界: %1").arg(input);
            return {};
        }
    }
    QString ancestor = candidateInfo.exists() ? candidate : candidateInfo.absolutePath();
    while (!ancestor.isEmpty() && !QFileInfo::exists(ancestor)) {
        const QString parent = QFileInfo(ancestor).absolutePath();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    const QString canonicalAncestor = QFileInfo(ancestor).canonicalFilePath();
    if (canonicalAncestor.isEmpty() || !isWithinRoot(canonicalAncestor, root)) {
        if (error) *error = QStringLiteral("持久化路径包含越界符号链接: %1").arg(input);
        return {};
    }
    if (candidateInfo.exists() && !candidateInfo.isFile()) {
        if (error) *error = QStringLiteral("持久化目标不是普通文件: %1").arg(input);
        return {};
    }
    return candidate;
}

RoomPersistence::RoomPersistence(QString checkpointPath, QString eventLogPath,
                                 QString dataDir)
    : m_checkpointPath(std::move(checkpointPath)),
      m_eventLogPath(std::move(eventLogPath)) {
    QString checkpointError;
    const QString resolvedCheckpoint = resolvePathWithinRoot(
        m_checkpointPath, dataDir.isEmpty() ? QFileInfo(m_checkpointPath).absolutePath() : dataDir,
        &checkpointError);
    QString eventError;
    const QString resolvedEvent = resolvePathWithinRoot(
        m_eventLogPath, dataDir.isEmpty() ? QFileInfo(m_eventLogPath).absolutePath() : dataDir,
        &eventError);
    if (resolvedCheckpoint.isEmpty() || resolvedEvent.isEmpty()) {
        m_configurationError = checkpointError.isEmpty() ? eventError : checkpointError;
        return;
    }
    m_checkpointPath = resolvedCheckpoint;
    m_eventLogPath = resolvedEvent;
}

bool RoomPersistence::saveCheckpoint(const RoomCheckpoint& checkpoint, QString* error) const {
    if (error) error->clear();
    if (!m_configurationError.isEmpty()) {
        if (error) *error = m_configurationError;
        return false;
    }
    if (!ensureParentDirectory(m_checkpointPath, error)) return false;
    const QByteArray data = QJsonDocument(checkpointToJson(checkpoint))
                                .toJson(QJsonDocument::Indented);
    if (data.size() > kMaxCheckpointBytes) {
        if (error) *error = QStringLiteral("检查点数据超过 64 MiB 限制");
        return false;
    }
    QSaveFile file(m_checkpointPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法写入检查点: %1").arg(m_checkpointPath);
        return false;
    }
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        if (error) *error = QStringLiteral("检查点原子写入失败: %1").arg(m_checkpointPath);
        return false;
    }
    if (!durableFlush(file, m_checkpointPath, error)) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error) *error = QStringLiteral("检查点原子写入失败: %1").arg(m_checkpointPath);
        return false;
    }
    return durableSyncParentDirectory(m_checkpointPath, error);
}

bool RoomPersistence::loadCheckpointFile(const QString& path, RoomCheckpoint* checkpoint,
                                         QString* error) const {
    QString pathError;
    if (resolvePathWithinRoot(path, QFileInfo(m_checkpointPath).absolutePath(), &pathError).isEmpty()) {
        if (error) *error = pathError;
        return false;
    }
    QFile file(path);
    if (file.exists() && file.size() > kMaxCheckpointBytes) {
        if (error) *error = QStringLiteral("检查点文件超过 64 MiB 限制");
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取检查点: %1").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        if (error) *error = QStringLiteral("检查点 JSON 无效（偏移 %1）: %2")
                                .arg(parseError.offset).arg(parseError.errorString());
        return false;
    }
    return checkpointFromJson(document.object(), checkpoint, error);
}

bool RoomPersistence::loadCheckpoint(RoomCheckpoint* checkpoint, QString* error) const {
    if (error) error->clear();
    if (!checkpoint) {
        if (error) *error = QStringLiteral("检查点输出参数为空");
        return false;
    }
    return loadCheckpointFile(m_checkpointPath, checkpoint, error);
}

bool RoomPersistence::appendEvent(quint64 sequence, const QString& kind,
                                  const QJsonObject& payload, QString* error,
                                  bool* checkpointRequired) const {
    if (error) error->clear();
    if (checkpointRequired) *checkpointRequired = false;
    if (!m_configurationError.isEmpty()) {
        if (error) *error = m_configurationError;
        return false;
    }
    if (sequence == 0 || kind.isEmpty()) {
        if (error) *error = QStringLiteral("持久化事件序号或类型无效");
        return false;
    }
    if (!ensureParentDirectory(m_eventLogPath, error)) return false;
    const QStringList generations{m_eventLogPath, m_eventLogPath + QStringLiteral(".1"),
                                  m_eventLogPath + QStringLiteral(".2"),
                                  m_eventLogPath + QStringLiteral(".3")};
    for (const QString& generation : generations) {
        QString pathError;
        if (resolvePathWithinRoot(generation, QFileInfo(m_eventLogPath).absolutePath(),
                                  &pathError).isEmpty()) {
            if (error) *error = pathError;
            return false;
        }
    }
    const QJsonObject event{{QStringLiteral("eventSchemaVersion"), 1},
                            {QStringLiteral("sequence"), static_cast<qint64>(sequence)},
                            {QStringLiteral("kind"), kind},
                            {QStringLiteral("recordedAt"),
                             QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                            {QStringLiteral("payload"), payload}};
    const QByteArray line = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    if (line.size() > eventLogLimit()) {
        if (error) *error = QStringLiteral("事件日志记录超过大小限制: %1").arg(m_eventLogPath);
        return false;
    }
    QFileInfo info(m_eventLogPath);
    const bool needsRotation = info.exists() && info.size() >= eventLogLimit();
    if (needsRotation) {
        QFile probe(m_eventLogPath);
        if (!probe.open(QIODevice::WriteOnly | QIODevice::Append)) {
            if (error) *error = QStringLiteral("无法写入事件日志: %1").arg(m_eventLogPath);
            return false;
        }
        probe.close();
        const QString oldestGeneration = generations.constLast();
        if (QFileInfo::exists(oldestGeneration)) {
            if (!QFileInfo(oldestGeneration).isFile()) {
                if (error) {
                    *error = QStringLiteral("事件日志轮换目标不是普通文件: %1")
                                 .arg(oldestGeneration);
                }
                return false;
            }
            quint64 oldestLastSequence = 0;
            if (!highestEventSequence(oldestGeneration, &oldestLastSequence, error)) return false;
            RoomCheckpoint checkpoint;
            QString checkpointError;
            if (!loadCheckpoint(&checkpoint, &checkpointError)
                || checkpoint.eventSequence < oldestLastSequence) {
                if (checkpointRequired) *checkpointRequired = true;
                if (error) {
                    *error = QStringLiteral("事件日志轮换前需要更新检查点（最旧日志截至 %1，检查点为 %2）")
                                 .arg(oldestLastSequence)
                                 .arg(checkpoint.eventSequence);
                    if (!checkpointError.isEmpty()) error->append(QStringLiteral(": ") + checkpointError);
                }
                return false;
            }
        }
        const QString token = QStringLiteral(".rotate-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QVector<bool> staged(generations.size(), false);
        QVector<bool> installed(generations.size(), false);
        const auto temporary = [&generations, &token](int index) {
            return generations.at(index) + token;
        };
        const auto restoreRotation = [&]() {
            bool restored = true;
            for (int index = generations.size() - 2; index >= 0; --index) {
                if (!installed.at(index)) continue;
                const QString destination = generations.at(index + 1);
                const QString stagedPath = temporary(index);
                if (QFileInfo::exists(stagedPath)) continue;
                if (!QFileInfo(destination).isFile()
                    || !QFile::rename(destination, stagedPath)) {
                    restored = false;
                }
            }
            for (int index = 0; index < generations.size(); ++index) {
                if (!staged.at(index)) continue;
                const QString stagedPath = temporary(index);
                const QString destination = generations.at(index);
                if (!QFileInfo::exists(stagedPath) || QFileInfo::exists(destination)
                    || !QFile::rename(stagedPath, destination)) {
                    restored = false;
                }
            }
            for (int index = 0; index < generations.size(); ++index) {
                if (staged.at(index)
                    && (!QFileInfo(generations.at(index)).isFile()
                        || QFileInfo::exists(temporary(index)))) {
                    restored = false;
                }
            }
            return restored;
        };
        const auto failRotation = [&](const QString& failedPath) {
            const bool restored = restoreRotation();
            if (error) {
                if (restored) {
                    *error = QStringLiteral("无法轮换事件日志，已恢复原有日志: %1")
                                 .arg(failedPath);
                } else {
                    *error = QStringLiteral("无法轮换事件日志，事务备份保留于: %1")
                                 .arg(m_eventLogPath + token);
                }
            }
            return false;
        };
        for (int index = 0; index < generations.size(); ++index) {
            const QString source = generations.at(index);
            const QString stagedPath = temporary(index);
            if (!QFileInfo::exists(source)) continue;
            if (!rotationRename(source, stagedPath, false, index)) return failRotation(source);
            staged[index] = true;
        }
        const auto install = [&](int sourceIndex, int destinationIndex) {
            if (!staged.at(sourceIndex)) return true;
            if (!rotationRename(temporary(sourceIndex), generations.at(destinationIndex), true,
                                sourceIndex)) {
                return false;
            }
            installed[sourceIndex] = true;
            return true;
        };
        if (!install(2, 3) || !install(1, 2) || !install(0, 1)) {
            return failRotation(m_eventLogPath);
        }
        if (staged.at(3) && !QFile::remove(temporary(3))) {
            return failRotation(temporary(3));
        }
    }
    QFile file(m_eventLogPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error) *error = QStringLiteral("无法写入事件日志: %1").arg(m_eventLogPath);
        return false;
    }
    const qint64 originalSize = file.size();
    if (file.write(line) != line.size()) {
        file.resize(originalSize);
        if (error) *error = QStringLiteral("事件日志写入失败: %1").arg(m_eventLogPath);
        return false;
    }
    if (!durableFlush(file, m_eventLogPath, error)) {
        file.resize(originalSize);
        return false;
    }
    return true;
}

QJsonArray RoomPersistence::eventsAfter(quint64 sequence, QString* error) const {
    if (error) error->clear();
    QJsonArray events;
    quint64 lastSequence = sequence;
    const QStringList paths{m_eventLogPath + QStringLiteral(".3"),
                            m_eventLogPath + QStringLiteral(".2"),
                            m_eventLogPath + QStringLiteral(".1"), m_eventLogPath};
    for (const QString& path : paths) {
        QString pathError;
        if (resolvePathWithinRoot(path, QFileInfo(m_eventLogPath).absolutePath(), &pathError).isEmpty()) {
            if (error) *error = pathError;
            return {};
        }
        QFile file(path);
        if (!file.exists()) continue;
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("无法读取事件日志: %1").arg(path);
            return {};
        }
        qint64 lineNumber = 0;
        while (!file.atEnd()) {
            ++lineNumber;
            const QByteArray line = file.readLine().trimmed();
            if (line.isEmpty()) continue;
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
            const QJsonObject event = document.object();
            const qint64 eventSequence = event.value(QStringLiteral("sequence")).toInteger();
            if (!document.isObject()
                || event.value(QStringLiteral("eventSchemaVersion")).toInt() != 1
                || eventSequence <= 0 || event.value(QStringLiteral("kind")).toString().isEmpty()
                || !event.value(QStringLiteral("payload")).isObject()) {
                if (error) {
                    *error = QStringLiteral("事件日志 %1 第 %2 行无效")
                                 .arg(path).arg(lineNumber);
                }
                return {};
            }
            if (static_cast<quint64>(eventSequence) <= sequence) continue;
            if (static_cast<quint64>(eventSequence) != lastSequence + 1) {
                if (error) *error = QStringLiteral("事件日志序号不连续");
                return {};
            }
            events.append(event);
            lastSequence = static_cast<quint64>(eventSequence);
        }
    }
    return events;
}

} // namespace gbr
