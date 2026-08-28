#include "VmfDemoWorkflow.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>

namespace gbr {

namespace {

bool finiteNumber(const QJsonValue& value) {
    return value.isDouble() && std::isfinite(value.toDouble());
}

bool validPosition(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject position = value.toObject();
    const double lat = position.value(QStringLiteral("lat")).toDouble(1000.0);
    const double lon = position.value(QStringLiteral("lon")).toDouble(1000.0);
    const double heading = position.value(QStringLiteral("heading")).toDouble(0.0);
    return finiteNumber(position.value(QStringLiteral("lat")))
        && finiteNumber(position.value(QStringLiteral("lon")))
        && (!position.contains(QStringLiteral("heading"))
            || finiteNumber(position.value(QStringLiteral("heading"))))
        && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0
        && heading >= 0.0 && heading <= 360.0;
}

bool validTargetPatch(const QJsonObject& patch) {
    static const QSet<QString> allowed{QStringLiteral("position"),
                                       QStringLiteral("visibility"),
                                       QStringLiteral("health"),
                                       QStringLiteral("status")};
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
        if (!allowed.contains(it.key())) return false;
    }
    if (patch.contains(QStringLiteral("position"))
        && !validPosition(patch.value(QStringLiteral("position")))) return false;
    if (patch.contains(QStringLiteral("visibility"))) {
        const QString visibility = patch.value(QStringLiteral("visibility")).toString();
        if (visibility != QLatin1String("visible")
            && visibility != QLatin1String("hidden")) return false;
    }
    if (patch.contains(QStringLiteral("health"))) {
        if (!finiteNumber(patch.value(QStringLiteral("health")))) return false;
        const double health = patch.value(QStringLiteral("health")).toDouble();
        if (health < 0.0 || health > 100.0) return false;
    }
    if (patch.contains(QStringLiteral("status"))) {
        const QString status = patch.value(QStringLiteral("status")).toString();
        static const QSet<QString> statuses{QStringLiteral("active"),
                                            QStringLiteral("damaged"),
                                            QStringLiteral("destroyed")};
        if (!statuses.contains(status)) return false;
    }
    return !patch.isEmpty();
}

QJsonObject mergeObject(QJsonObject base, const QJsonObject& patch) {
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
        base.insert(it.key(), it.value());
    }
    return base;
}

} // namespace

VmfDemoWorkflow::VmfDemoWorkflow() {
    reset();
}

QList<VmfDemoWorkflow::ActionSpec> VmfDemoWorkflow::actionSpecs() {
    return {
        {QStringLiteral("reportTarget"), QStringLiteral("recon"), 0,
         QStringLiteral("composeTargetReport"), QStringLiteral("生成并发送目标报告")},
        {QStringLiteral("planRoute"), QStringLiteral("commander"), 1,
         QStringLiteral("planRoute"), QStringLiteral("规划攻击航路")},
        {QStringLiteral("acceptRoute"), QStringLiteral("attack"), 1,
         QStringLiteral("acceptRoute"), QStringLiteral("攻击席位确认航路")},
        {QStringLiteral("issueGuidance"), QStringLiteral("commander"), 2,
         QStringLiteral("issueGuidance"), QStringLiteral("下达引导命令")},
        {QStringLiteral("acknowledgeGuidance"), QStringLiteral("ground"), 2,
         QStringLiteral("acknowledgeGuidance"), QStringLiteral("地面席位确认引导命令")},
        {QStringLiteral("identityHello"), QStringLiteral("attack"), 3,
         QStringLiteral("identityHello"), QStringLiteral("攻击席位发送身份报告")},
        {QStringLiteral("identityConfirm"), QStringLiteral("ground"), 3,
         QStringLiteral("identityConfirm"), QStringLiteral("地面席位确认身份")},
        {QStringLiteral("sendGuidancePackage"), QStringLiteral("ground"), 3,
         QStringLiteral("sendGuidancePackage"), QStringLiteral("发送目标和引导航路")},
        {QStringLiteral("acceptGuidance"), QStringLiteral("attack"), 3,
         QStringLiteral("acceptGuidance"), QStringLiteral("攻击席位确认引导包")},
        {QStringLiteral("reportAttackReady"), QStringLiteral("attack"), 3,
         QStringLiteral("reportAttackReady"), QStringLiteral("报告进入攻击航线")},
        {QStringLiteral("authorizeAttack"), QStringLiteral("ground"), 3,
         QStringLiteral("authorizeAttack"), QStringLiteral("地面席位授权攻击")},
        {QStringLiteral("simulateAttack"), QStringLiteral("attack"), 3,
         QStringLiteral("simulateAttack"), QStringLiteral("执行模拟攻击")},
        {QStringLiteral("reportBattleDamage"), QStringLiteral("attack"), 3,
         QStringLiteral("reportBattleDamage"), QStringLiteral("报告模拟战果")},
        {QStringLiteral("confirmDamageAssessment"), QStringLiteral("ground"), 3,
         QStringLiteral("confirmDamageAssessment"), QStringLiteral("确认毁伤评估")},
        {QStringLiteral("confirmTargetDestroyed"), QStringLiteral("recon"), 4,
         QStringLiteral("confirmTargetDestroyed"), QStringLiteral("确认目标效果")},
        {QStringLiteral("withdraw"), QStringLiteral("commander"), 5,
         QStringLiteral("withdraw"), QStringLiteral("下达返航命令")},
        {QStringLiteral("confirmReturned"), QStringLiteral("attack"), 5,
         QStringLiteral("confirmReturned"), QStringLiteral("确认返航完成")},
    };
}

QStringList VmfDemoWorkflow::phaseIds() {
    return {QStringLiteral("target-report"), QStringLiteral("route-planning"),
            QStringLiteral("guidance-command"), QStringLiteral("ground-guidance"),
            QStringLiteral("destruction-confirmation"), QStringLiteral("return")};
}

QStringList VmfDemoWorkflow::phaseTitles() {
    return {QStringLiteral("目标报告"), QStringLiteral("航路规划"),
            QStringLiteral("引导命令"), QStringLiteral("地面引导"),
            QStringLiteral("摧毁确认"), QStringLiteral("返航")};
}

bool VmfDemoWorkflow::validIdentifier(const QString& value) {
    if (value.isEmpty() || value.size() > 64) return false;
    return std::all_of(value.cbegin(), value.cend(), [](QChar character) {
        return character.isLetterOrNumber() || character == QLatin1Char('_')
            || character == QLatin1Char('-') || character == QLatin1Char('.');
    });
}

bool VmfDemoWorkflow::validateTargetScript(const QJsonObject& script, QString* error) {
    if (error) error->clear();
    if (script.isEmpty()) return true;
    if (script.value(QStringLiteral("version")).toInt() != 1
        || !script.value(QStringLiteral("targets")).isArray()
        || !script.value(QStringLiteral("timeline")).isArray()) {
        if (error) *error = QStringLiteral("固定靶脚本必须包含 version=1、targets 和 timeline");
        return false;
    }
    QSet<QString> targetIds;
    for (const QJsonValue& value : script.value(QStringLiteral("targets")).toArray()) {
        const QJsonObject target = value.toObject();
        const QString id = target.value(QStringLiteral("id")).toString();
        const QJsonObject initial = target.value(QStringLiteral("initial")).toObject();
        if (!value.isObject() || !validIdentifier(id) || targetIds.contains(id)
            || !validTargetPatch(initial)) {
            if (error) *error = QStringLiteral("固定靶初始状态无效或 ID 重复");
            return false;
        }
        targetIds.insert(id);
    }
    int previousPhase = -1;
    const QStringList phases = phaseIds();
    for (const QJsonValue& value : script.value(QStringLiteral("timeline")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QJsonObject trigger = entry.value(QStringLiteral("trigger")).toObject();
        const QString phase = trigger.value(QStringLiteral("phase")).toString();
        const int phaseIndex = phases.indexOf(phase);
        const QString targetId = entry.value(QStringLiteral("targetId")).toString();
        if (!value.isObject() || phaseIndex < 0 || phaseIndex < previousPhase
            || !targetIds.contains(targetId)
            || !validTargetPatch(entry.value(QStringLiteral("patch")).toObject())) {
            if (error) *error = QStringLiteral("固定靶时间线触发点、目标或 patch 无效");
            return false;
        }
        previousPhase = phaseIndex;
    }
    if (QJsonDocument(script).toJson(QJsonDocument::Compact).size() > 256 * 1024) {
        if (error) *error = QStringLiteral("固定靶脚本超过 256 KiB");
        return false;
    }
    return true;
}

QJsonObject VmfDemoWorkflow::normalizedScript(const QJsonObject& script) {
    if (!script.isEmpty()) return script;
    return QJsonObject{{QStringLiteral("version"), 1},
                       {QStringLiteral("targets"), QJsonArray{}},
                       {QStringLiteral("timeline"), QJsonArray{}}};
}

void VmfDemoWorkflow::reset(double now) {
    Q_UNUSED(now);
    m_actionIndex = 0;
    m_status = QStringLiteral("active");
    m_cancelUntilMs = 0;
    m_cancelledGeneration = 0;
    m_traces = {};
    m_actionHistory = {};
    m_seenActionIds.clear();
    clearTaskState();
    if (m_targetScript.isEmpty()) m_targetScript = normalizedScript({});
    rebuildTargetStateForCurrentPhase();
}

void VmfDemoWorkflow::clearTaskState() {
    m_task = QJsonObject{
        {QStringLiteral("taskId"), QStringLiteral("demo-%1").arg(m_generation)},
        {QStringLiteral("generation"), static_cast<qint64>(m_generation)},
        {QStringLiteral("target"), QJsonObject{}},
        {QStringLiteral("route"), QJsonObject{}},
        {QStringLiteral("guidance"), QJsonObject{}},
        {QStringLiteral("effect"), QJsonObject{}}};
    m_reports = {};
}

void VmfDemoWorkflow::appendReport(const QJsonObject& report) {
    if (report.isEmpty()) return;
    m_reports.append(report);
    while (m_reports.size() > 128) m_reports.removeFirst();
}

void VmfDemoWorkflow::archiveGeneration(const QString& status) {
    m_archivedGenerations.append(QJsonObject{
        {QStringLiteral("generation"), static_cast<qint64>(m_generation)},
        {QStringLiteral("taskId"), m_task.value(QStringLiteral("taskId"))},
        {QStringLiteral("reportCount"), m_reports.size()},
        {QStringLiteral("reports"), m_reports},
        {QStringLiteral("status"), status}});
    while (m_archivedGenerations.size() > 20) m_archivedGenerations.removeFirst();
}

void VmfDemoWorkflow::rebuildTargetStateForCurrentPhase() {
    m_scriptCursor = 0;
    m_targetState = {};
    for (const QJsonValue& value : m_targetScript.value(QStringLiteral("targets")).toArray()) {
        const QJsonObject target = value.toObject();
        m_targetState.insert(target.value(QStringLiteral("id")).toString(),
                             target.value(QStringLiteral("initial")));
    }
    applyScriptForCurrentPhase();
}

const VmfDemoWorkflow::ActionSpec* VmfDemoWorkflow::currentAction() const {
    static const QList<ActionSpec> specs = actionSpecs();
    return m_actionIndex >= 0 && m_actionIndex < specs.size() ? &specs.at(m_actionIndex)
                                                              : nullptr;
}

QString VmfDemoWorkflow::currentSeatType() const {
    const ActionSpec* spec = currentAction();
    return spec ? spec->seatType : QString{};
}

void VmfDemoWorkflow::applyScriptForCurrentPhase() {
    const ActionSpec* current = currentAction();
    const int phase = current ? current->phase : phaseIds().size();
    const QJsonArray timeline = m_targetScript.value(QStringLiteral("timeline")).toArray();
    while (m_scriptCursor < timeline.size()) {
        const QJsonObject entry = timeline.at(m_scriptCursor).toObject();
        const int triggerPhase = phaseIds().indexOf(
            entry.value(QStringLiteral("trigger")).toObject()
                .value(QStringLiteral("phase")).toString());
        if (triggerPhase > phase) break;
        const QString targetId = entry.value(QStringLiteral("targetId")).toString();
        m_targetState.insert(targetId,
            mergeObject(m_targetState.value(targetId).toObject(),
                        entry.value(QStringLiteral("patch")).toObject()));
        ++m_scriptCursor;
    }
}

VmfDemoWorkflow::Result VmfDemoWorkflow::failure(const QString& code,
                                                  const QString& message) const {
    return {false, QStringLiteral("rejected"), code, message, m_revision,
            stateProjection(false)};
}

VmfDemoWorkflow::Result VmfDemoWorkflow::success(const QString& status) const {
    return {true, status, QStringLiteral("OK"), {}, m_revision, stateProjection(false)};
}

std::optional<VmfDemoWorkflow::Result> VmfDemoWorkflow::duplicateActionResult(
    const QJsonObject& command, const QString& actorSeatType) const {
    const QString actionId = command.value(QStringLiteral("actionId")).toString();
    if (!m_seenActionIds.contains(actionId)) return std::nullopt;
    for (qsizetype index = m_actionHistory.size(); index > 0; --index) {
        const QJsonObject entry = m_actionHistory.at(index - 1).toObject();
        if (entry.value(QStringLiteral("actionId")).toString() != actionId) continue;
        if (entry.value(QStringLiteral("action")).toString()
                != command.value(QStringLiteral("action")).toString()
            || entry.value(QStringLiteral("actorSeatType")).toString() != actorSeatType) {
            return failure(QStringLiteral("ACTION_ID_CONFLICT"),
                           QStringLiteral("演示动作 ID 已用于其他动作或战位"));
        }
        return success(QStringLiteral("duplicate"));
    }
    return failure(QStringLiteral("ACTION_ID_CONFLICT"),
                   QStringLiteral("演示动作 ID 的幂等记录不完整"));
}

VmfDemoWorkflow::Result VmfDemoWorkflow::applyAction(
    const QJsonObject& command, const QString& actorSeatType,
    const QJsonObject& trace, double now) {
    const QString actionId = command.value(QStringLiteral("actionId")).toString();
    const quint64 expected = static_cast<quint64>(
        command.value(QStringLiteral("expectedRevision")).toInteger());
    if (!validIdentifier(actionId)) {
        return failure(QStringLiteral("INVALID_ACTION_ID"), QStringLiteral("演示动作 ID 无效"));
    }
    if (const auto duplicate = duplicateActionResult(command, actorSeatType)) {
        return *duplicate;
    }
    if (expected != m_revision) {
        return failure(QStringLiteral("DEMO_REVISION_MISMATCH"),
                       QStringLiteral("演示流程版本已变化，请刷新后重试"));
    }
    if (m_status == QLatin1String("paused")) {
        return failure(QStringLiteral("DEMO_PAUSED"), QStringLiteral("演示流程已暂停"));
    }
    if (m_status == QLatin1String("cancelled")) {
        return failure(QStringLiteral("DEMO_CANCELLED"), QStringLiteral("当前演示任务已取消"));
    }
    if (m_status == QLatin1String("completed") || !currentAction()) {
        return failure(QStringLiteral("DEMO_COMPLETED"), QStringLiteral("演示流程已经完成"));
    }
    const ActionSpec spec = *currentAction();
    if (command.value(QStringLiteral("action")).toString() != spec.action) {
        return failure(QStringLiteral("DEMO_SEQUENCE_INVALID"),
                       QStringLiteral("当前子步骤不允许该动作"));
    }
    if (actorSeatType != spec.seatType && actorSeatType != QLatin1String("automation")) {
        return failure(QStringLiteral("DEMO_ROLE_FORBIDDEN"),
                       QStringLiteral("当前战位不能执行该动作"));
    }
    if (trace.isEmpty()) {
        return failure(QStringLiteral("DEMO_TRACE_REQUIRED"),
                       QStringLiteral("演示动作缺少经过校验的 VMF trace"));
    }

    const QJsonObject payload = command.value(QStringLiteral("payload")).toObject();
    const QString taskId = m_task.value(QStringLiteral("taskId")).toString(
        QStringLiteral("demo-%1").arg(m_generation));
    if (spec.action == QLatin1String("reportTarget")) {
        QJsonObject target = payload.value(QStringLiteral("target")).toObject();
        // The server always supplies a canonical target.  Keep a harmless
        // placeholder for direct workflow users and legacy tests; it is
        // never accepted as a network command without server validation.
        if (target.isEmpty()) {
            target = QJsonObject{{QStringLiteral("targetId"), QStringLiteral("demo-target")},
                                 {QStringLiteral("targetKind"), QStringLiteral("entity")},
                                 {QStringLiteral("targetType"), QStringLiteral("unknown")},
                                 {QStringLiteral("targetTypeCode"), 7},
                                 {QStringLiteral("targetCount"), 1},
                                 {QStringLiteral("friendFoe"), QStringLiteral("enemy")},
                                 {QStringLiteral("status"), QStringLiteral("tracked")}};
        }
        m_task.insert(QStringLiteral("target"), target);
        m_task.insert(QStringLiteral("taskId"), taskId);
        m_task.insert(QStringLiteral("generation"), static_cast<qint64>(m_generation));
    } else if (spec.action == QLatin1String("planRoute")) {
        const QJsonObject route = payload.value(QStringLiteral("route")).toObject();
        QJsonObject normalizedRoute = route;
        if (!normalizedRoute.value(QStringLiteral("points")).isArray()
            || normalizedRoute.value(QStringLiteral("points")).toArray().size() < 2) {
            normalizedRoute = QJsonObject{{QStringLiteral("points"), QJsonArray{
                QJsonObject{{QStringLiteral("x"), 0.0}, {QStringLiteral("y"), 0.0}},
                QJsonObject{{QStringLiteral("x"), 1.0}, {QStringLiteral("y"), 1.0}}}}};
        }
        m_task.insert(QStringLiteral("route"), normalizedRoute);
    } else if (spec.action == QLatin1String("sendGuidancePackage")) {
        QJsonObject guidance = m_task.value(QStringLiteral("guidance")).toObject();
        guidance.insert(QStringLiteral("packageId"),
                        payload.value(QStringLiteral("packageId")).toString(
                            QStringLiteral("package-%1").arg(m_generation)));
        guidance.insert(QStringLiteral("sent"), true);
        m_task.insert(QStringLiteral("guidance"), guidance);
    } else if (spec.action == QLatin1String("simulateAttack")) {
        const QJsonObject target = m_task.value(QStringLiteral("target")).toObject();
        const bool positionTarget = target.value(QStringLiteral("targetKind")).toString()
            == QLatin1String("position");
        m_task.insert(QStringLiteral("effect"), QJsonObject{
            {QStringLiteral("mode"), QStringLiteral("simulation")},
            {QStringLiteral("outcome"), positionTarget ? QStringLiteral("effect-applied")
                                                          : QStringLiteral("destroyed")},
            {QStringLiteral("damagePercent"), 100.0},
            {QStringLiteral("destroyed"), !positionTarget},
            {QStringLiteral("engineStateUntouched"), true},
            {QStringLiteral("createdAt"), now}});
    } else if (spec.action == QLatin1String("reportBattleDamage")) {
        QJsonObject effect = m_task.value(QStringLiteral("effect")).toObject();
        effect.insert(QStringLiteral("reported"), true);
        m_task.insert(QStringLiteral("effect"), effect);
    } else if (spec.action == QLatin1String("confirmDamageAssessment")) {
        QJsonObject effect = m_task.value(QStringLiteral("effect")).toObject();
        effect.insert(QStringLiteral("assessed"), true);
        m_task.insert(QStringLiteral("effect"), effect);
    } else if (spec.action == QLatin1String("confirmTargetDestroyed")) {
        QJsonObject effect = m_task.value(QStringLiteral("effect")).toObject();
        effect.insert(QStringLiteral("confirmed"), true);
        m_task.insert(QStringLiteral("effect"), effect);
    } else if (spec.action == QLatin1String("withdraw")) {
        QJsonObject route = m_task.value(QStringLiteral("route")).toObject();
        route.insert(QStringLiteral("return"), true);
        m_task.insert(QStringLiteral("route"), route);
    }

    QJsonObject report{{QStringLiteral("reportId"),
                        QStringLiteral("report-%1-%2").arg(m_generation).arg(m_revision)},
                       {QStringLiteral("generation"), static_cast<qint64>(m_generation)},
                       {QStringLiteral("taskId"), taskId},
                       {QStringLiteral("action"), spec.action},
                       {QStringLiteral("seatType"), actorSeatType},
                       {QStringLiteral("createdAt"), now},
                       {QStringLiteral("details"), payload}};
    if (trace.contains(QStringLiteral("messageId"))) {
        report.insert(QStringLiteral("messageId"), trace.value(QStringLiteral("messageId")));
    }
    if (trace.contains(QStringLiteral("traceId"))) {
        report.insert(QStringLiteral("traceId"), trace.value(QStringLiteral("traceId")));
    }
    appendReport(report);

    QJsonObject storedTrace = trace;
    storedTrace.insert(QStringLiteral("actionId"), actionId);
    storedTrace.insert(QStringLiteral("action"), spec.action);
    storedTrace.insert(QStringLiteral("phase"), phaseIds().value(spec.phase));
    storedTrace.insert(QStringLiteral("phaseTitle"), phaseTitles().value(spec.phase));
    storedTrace.insert(QStringLiteral("substep"), spec.substep);
    storedTrace.insert(QStringLiteral("actorSeatType"), actorSeatType);
    storedTrace.insert(QStringLiteral("createdAt"), now);
    storedTrace.insert(QStringLiteral("ack"), QJsonObject{
        {QStringLiteral("required"), trace.value(QStringLiteral("requiresAck")).toBool()},
        {QStringLiteral("status"), QStringLiteral("accepted")},
        {QStringLiteral("automatic"), actorSeatType == QLatin1String("automation")}});
    m_traces.append(storedTrace);
    while (m_traces.size() > TraceLimit) m_traces.removeFirst();

    m_seenActionIds.insert(actionId);
    m_actionHistory.append(QJsonObject{{QStringLiteral("actionId"), actionId},
                                       {QStringLiteral("action"), spec.action},
                                       {QStringLiteral("actorSeatType"), actorSeatType},
                                       {QStringLiteral("generation"),
                                        static_cast<qint64>(m_generation)},
                                       {QStringLiteral("time"), now}});
    while (m_actionHistory.size() > ActionHistoryLimit) {
        const QString expiredId = m_actionHistory.first().toObject()
                                      .value(QStringLiteral("actionId")).toString();
        m_actionHistory.removeFirst();
        m_seenActionIds.remove(expiredId);
    }
    ++m_actionIndex;
    ++m_revision;
    m_status = currentAction() ? QStringLiteral("active") : QStringLiteral("completed");
    applyScriptForCurrentPhase();
    return success();
}

VmfDemoWorkflow::Result VmfDemoWorkflow::applyControl(
    const QString& action, const QJsonObject& payload, double now) {
    if (action == QLatin1String("cancel")) {
        if (m_status == QLatin1String("completed")) {
            return failure(QStringLiteral("DEMO_COMPLETED"), QStringLiteral("已完成任务请开始新任务"));
        }
        m_status = QStringLiteral("cancelled");
        m_cancelledGeneration = m_generation;
        m_cancelUntilMs = payload.value(QStringLiteral("cancelUntilMs")).toInteger();
        if (m_cancelUntilMs <= 0) m_cancelUntilMs = QDateTime::currentMSecsSinceEpoch() + 3000;
        ++m_revision;
        return success(QStringLiteral("cancelled"));
    }
    if (action == QLatin1String("start")) {
        if (m_status != QLatin1String("completed")) {
            return failure(QStringLiteral("DEMO_NOT_COMPLETED"),
                           QStringLiteral("当前任务尚未完成，不能开始新任务"));
        }
        archiveGeneration(QStringLiteral("completed"));
        ++m_generation;
        ++m_revision;
        reset(now);
        return success(QStringLiteral("started"));
    }
    if (action == QLatin1String("reset")) {
        ++m_generation;
        ++m_revision;
        reset(now);
        return success();
    }
    if (action == QLatin1String("pause")) {
        if (m_status != QLatin1String("completed")) m_status = QStringLiteral("paused");
        ++m_revision;
        return success(QStringLiteral("paused"));
    }
    if (action == QLatin1String("resume")) {
        if (m_status == QLatin1String("paused")) m_status = QStringLiteral("active");
        ++m_revision;
        return success();
    }
    if (action == QLatin1String("jump")) {
        const int phase = phaseIds().indexOf(payload.value(QStringLiteral("phase")).toString());
        if (phase < 0) {
            return failure(QStringLiteral("INVALID_DEMO_PHASE"),
                           QStringLiteral("演示跳转步骤无效"));
        }
        const QList<ActionSpec> specs = actionSpecs();
        const auto it = std::find_if(specs.cbegin(), specs.cend(), [phase](const ActionSpec& spec) {
            return spec.phase == phase;
        });
        m_actionIndex = static_cast<int>(std::distance(specs.cbegin(), it));
        m_status = QStringLiteral("active");
        ++m_generation;
        ++m_revision;
        rebuildTargetStateForCurrentPhase();
        return success();
    }
    if (action == QLatin1String("setTargetScript")) {
        const QJsonObject script = payload.value(QStringLiteral("script")).toObject();
        QString scriptError;
        if (!validateTargetScript(script, &scriptError)) {
            return failure(QStringLiteral("INVALID_TARGET_SCRIPT"), scriptError);
        }
        m_targetScript = normalizedScript(script);
        ++m_generation;
        ++m_revision;
        reset(now);
        return success();
    }
    return failure(QStringLiteral("UNKNOWN_DEMO_CONTROL"),
                   QStringLiteral("未知演示控制操作"));
}

bool VmfDemoWorkflow::advanceCancellation(qint64 nowEpochMs) {
    if (m_status != QLatin1String("cancelled") || m_cancelUntilMs <= 0) return false;
    if (nowEpochMs < m_cancelUntilMs) return false;
    archiveGeneration(QStringLiteral("cancelled"));
    ++m_generation;
    ++m_revision;
    reset();
    return true;
}

QJsonObject VmfDemoWorkflow::stateProjection(bool includeTechnicalTrace) const {
    const ActionSpec* current = currentAction();
    const ActionSpec* action = m_status == QLatin1String("cancelled") ? nullptr : current;
    const int phase = current ? current->phase : phaseIds().size() - 1;
    QJsonArray phases;
    for (int index = 0; index < phaseIds().size(); ++index) {
        QString status = index < phase ? QStringLiteral("completed")
            : index == phase && m_status != QLatin1String("completed") ? m_status
                                                                       : QStringLiteral("pending");
        if (m_status == QLatin1String("completed")) status = QStringLiteral("completed");
        if (m_status == QLatin1String("cancelled") && index == phase) {
            status = QStringLiteral("cancelled");
        }
        phases.append(QJsonObject{{QStringLiteral("id"), phaseIds().at(index)},
                                  {QStringLiteral("title"), phaseTitles().at(index)},
                                  {QStringLiteral("status"), status}});
    }
    QJsonObject result{{QStringLiteral("schemaVersion"), SchemaVersion},
                       {QStringLiteral("profile"), QString::fromLatin1(ProfileId)},
                       {QStringLiteral("generation"), static_cast<qint64>(m_generation)},
                       {QStringLiteral("revision"), static_cast<qint64>(m_revision)},
                       {QStringLiteral("phase"), phaseIds().value(phase)},
                       {QStringLiteral("phaseTitle"), phaseTitles().value(phase)},
                       {QStringLiteral("substep"), action ? action->substep
                           : m_status == QLatin1String("cancelled")
                               ? QStringLiteral("cancelled") : QStringLiteral("completed")},
                       {QStringLiteral("status"), m_status},
                       {QStringLiteral("activeSeat"), action ? action->seatType : QString{}},
                       {QStringLiteral("expectedAction"), action ? action->action : QString{}},
                       {QStringLiteral("actionTitle"), action ? action->title
                           : m_status == QLatin1String("cancelled")
                               ? QStringLiteral("任务已取消") : QStringLiteral("演示完成")},
                       {QStringLiteral("phases"), phases},
                       {QStringLiteral("scriptCursor"), m_scriptCursor},
                       {QStringLiteral("targetState"), m_targetState},
                       {QStringLiteral("task"), m_task},
                       {QStringLiteral("reports"), m_reports},
                       {QStringLiteral("archive"), m_archivedGenerations},
                       {QStringLiteral("cancelUntilMs"), m_cancelUntilMs},
                       {QStringLiteral("cancelledGeneration"),
                        static_cast<qint64>(m_cancelledGeneration)},
                       {QStringLiteral("traceCount"), m_traces.size()}};
    const QByteArray scriptJson = QJsonDocument(m_targetScript).toJson(QJsonDocument::Compact);
    result.insert(QStringLiteral("targetScriptHash"), QString::fromLatin1(
        QCryptographicHash::hash(scriptJson, QCryptographicHash::Sha256).toHex()));
    if (includeTechnicalTrace) result.insert(QStringLiteral("traces"), m_traces);
    else if (!m_traces.isEmpty()) {
        QJsonObject summary = m_traces.last().toObject();
        summary.remove(QStringLiteral("canonicalXml"));
        summary.remove(QStringLiteral("decodedXml"));
        summary.remove(QStringLiteral("wireBytes"));
        summary.remove(QStringLiteral("binaryPreview"));
        summary.remove(QStringLiteral("hexPreview"));
        summary.remove(QStringLiteral("fields"));
        result.insert(QStringLiteral("latestTrace"), summary);
    }
    return result;
}

QJsonObject VmfDemoWorkflow::toJson() const {
    QJsonArray seen;
    QStringList actionIds = m_seenActionIds.values();
    actionIds.sort();
    for (const QString& value : actionIds) seen.append(value);
    return QJsonObject{{QStringLiteral("schemaVersion"), SchemaVersion},
                       {QStringLiteral("profile"), QString::fromLatin1(ProfileId)},
                       {QStringLiteral("generation"), static_cast<qint64>(m_generation)},
                       {QStringLiteral("revision"), static_cast<qint64>(m_revision)},
                       {QStringLiteral("actionIndex"), m_actionIndex},
                       {QStringLiteral("status"), m_status},
                       {QStringLiteral("targetScript"), m_targetScript},
                       {QStringLiteral("scriptCursor"), m_scriptCursor},
                       {QStringLiteral("targetState"), m_targetState},
                       {QStringLiteral("task"), m_task},
                       {QStringLiteral("reports"), m_reports},
                       {QStringLiteral("archive"), m_archivedGenerations},
                       {QStringLiteral("cancelUntilMs"), m_cancelUntilMs},
                       {QStringLiteral("cancelledGeneration"),
                        static_cast<qint64>(m_cancelledGeneration)},
                       {QStringLiteral("traces"), m_traces},
                       {QStringLiteral("actionHistory"), m_actionHistory},
                       {QStringLiteral("seenActionIds"), seen}};
}

bool VmfDemoWorkflow::restore(const QJsonObject& object, QString* error) {
    if (error) error->clear();
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const int storedSchema = object.value(QStringLiteral("schemaVersion")).toInt();
    if (storedSchema == 1) {
        const QJsonObject legacyScript = object.value(QStringLiteral("targetScript")).toObject();
        QString scriptError;
        if (!validateTargetScript(legacyScript, &scriptError)) return fail(scriptError);
        m_targetScript = normalizedScript(legacyScript);
        const qint64 legacyGeneration = object.value(QStringLiteral("generation")).toInteger();
        const qint64 legacyRevision = object.value(QStringLiteral("revision")).toInteger();
        if (legacyGeneration <= 0 || legacyRevision <= 0) {
            return fail(QStringLiteral("旧演示流程代次或版本无效"));
        }
        m_archivedGenerations = QJsonArray{QJsonObject{
            {QStringLiteral("generation"), legacyGeneration},
            {QStringLiteral("status"), QStringLiteral("migrated")},
            {QStringLiteral("migration"), QStringLiteral("schema1-to-schema2")}}};
        m_generation = static_cast<quint64>(legacyGeneration + 1);
        m_revision = static_cast<quint64>(legacyRevision + 1);
        reset();
        return true;
    }
    const qint64 generation = object.value(QStringLiteral("generation")).toInteger();
    const qint64 revision = object.value(QStringLiteral("revision")).toInteger();
    const int actionIndex = object.value(QStringLiteral("actionIndex")).toInt(-1);
    const QString status = object.value(QStringLiteral("status")).toString();
    const int actionCount = actionSpecs().size();
    if (storedSchema != SchemaVersion
        || object.value(QStringLiteral("profile")).toString() != QLatin1String(ProfileId)
        || generation <= 0 || revision <= 0 || actionIndex < 0
        || actionIndex > actionCount
        || (status != QLatin1String("active") && status != QLatin1String("paused")
            && status != QLatin1String("completed") && status != QLatin1String("cancelled"))
        || (status == QLatin1String("completed") && actionIndex != actionCount)) {
        return fail(QStringLiteral("演示流程检查点头部无效"));
    }
    const QJsonObject script = object.value(QStringLiteral("targetScript")).toObject();
    QString scriptError;
    if (!validateTargetScript(script, &scriptError)) return fail(scriptError);
    const QJsonArray traces = object.value(QStringLiteral("traces")).toArray();
    const QJsonArray history = object.value(QStringLiteral("actionHistory")).toArray();
    const QJsonArray seen = object.value(QStringLiteral("seenActionIds")).toArray();
    const int scriptCursor = object.value(QStringLiteral("scriptCursor")).toInt(-1);
    if (traces.size() > TraceLimit || history.size() > ActionHistoryLimit
        || seen.size() > ActionHistoryLimit || scriptCursor < 0
        || scriptCursor > script.value(QStringLiteral("timeline")).toArray().size()
        || !object.value(QStringLiteral("targetState")).isObject()) {
        return fail(QStringLiteral("演示流程检查点内容无效"));
    }
    if (object.contains(QStringLiteral("task")) && !object.value(QStringLiteral("task")).isObject()) {
        return fail(QStringLiteral("演示任务状态结构无效"));
    }
    if (object.contains(QStringLiteral("reports")) && !object.value(QStringLiteral("reports")).isArray()) {
        return fail(QStringLiteral("演示报告历史结构无效"));
    }
    if (object.contains(QStringLiteral("archive")) && !object.value(QStringLiteral("archive")).isArray()) {
        return fail(QStringLiteral("演示报告归档结构无效"));
    }
    for (const QJsonValue& value : object.value(QStringLiteral("archive")).toArray()) {
        if (!value.isObject()) return fail(QStringLiteral("演示报告归档项无效"));
        const QJsonObject archive = value.toObject();
        if (archive.contains(QStringLiteral("reports"))
            && (!archive.value(QStringLiteral("reports")).isArray()
                || archive.value(QStringLiteral("reports")).toArray().size() > 128)) {
            return fail(QStringLiteral("演示报告归档明细无效"));
        }
    }
    QSet<QString> ids;
    for (const QJsonValue& value : seen) {
        if (!value.isString() || !validIdentifier(value.toString())
            || ids.contains(value.toString())) return fail(QStringLiteral("演示动作幂等记录无效"));
        ids.insert(value.toString());
    }
    QSet<QString> historyIds;
    const QList<ActionSpec> specs = actionSpecs();
    for (const QJsonValue& value : history) {
        if (!value.isObject()) return fail(QStringLiteral("演示动作历史结构无效"));
        const QJsonObject entry = value.toObject();
        const QString actionId = entry.value(QStringLiteral("actionId")).toString();
        const QString action = entry.value(QStringLiteral("action")).toString();
        const QString actor = entry.value(QStringLiteral("actorSeatType")).toString();
        const bool knownAction = std::any_of(specs.cbegin(), specs.cend(),
            [&action](const ActionSpec& spec) { return spec.action == action; });
        const bool knownActor = actor == QLatin1String("automation")
            || std::any_of(specs.cbegin(), specs.cend(),
                [&actor](const ActionSpec& spec) { return spec.seatType == actor; });
        if (!validIdentifier(actionId) || historyIds.contains(actionId) || !knownAction
            || !knownActor || !finiteNumber(entry.value(QStringLiteral("time")))) {
            return fail(QStringLiteral("演示动作历史内容无效"));
        }
        historyIds.insert(actionId);
    }
    if (historyIds != ids) return fail(QStringLiteral("演示动作幂等记录不一致"));

    const QJsonObject targetState = object.value(QStringLiteral("targetState")).toObject();
    VmfDemoWorkflow expected;
    expected.m_targetScript = script;
    expected.m_actionIndex = actionIndex;
    expected.rebuildTargetStateForCurrentPhase();
    if (scriptCursor != expected.m_scriptCursor || targetState != expected.m_targetState) {
        return fail(QStringLiteral("演示固定靶状态与当前步骤不一致"));
    }
    for (const QJsonValue& value : traces) {
        if (!value.isObject()) return fail(QStringLiteral("演示 trace 结构无效"));
        const QJsonObject trace = value.toObject();
        if (!validIdentifier(trace.value(QStringLiteral("actionId")).toString())
            || trace.value(QStringLiteral("action")).toString().isEmpty()
            || phaseIds().indexOf(trace.value(QStringLiteral("phase")).toString()) < 0
            || !finiteNumber(trace.value(QStringLiteral("createdAt")))) {
            return fail(QStringLiteral("演示 trace 内容无效"));
        }
    }
    m_generation = static_cast<quint64>(generation);
    m_revision = static_cast<quint64>(revision);
    m_actionIndex = actionIndex;
    m_status = status;
    m_targetScript = script;
    m_scriptCursor = scriptCursor;
    m_targetState = targetState;
    m_task = object.value(QStringLiteral("task")).toObject();
    if (m_task.isEmpty()) clearTaskState();
    m_reports = object.value(QStringLiteral("reports")).toArray();
    m_archivedGenerations = object.value(QStringLiteral("archive")).toArray();
    m_cancelUntilMs = object.value(QStringLiteral("cancelUntilMs")).toInteger();
    m_cancelledGeneration = static_cast<quint64>(
        object.value(QStringLiteral("cancelledGeneration")).toInteger());
    m_traces = traces;
    m_actionHistory = history;
    m_seenActionIds = ids;
    return true;
}

} // namespace gbr
