#include "StrictVmfTask.h"

#include <QJsonDocument>

#include <algorithm>
#include <cmath>

namespace gbr {

namespace {

bool validId(const QString& value) {
    return !value.trimmed().isEmpty() && value.size() <= 64;
}

bool isOpenTask(const StrictVmfTaskSet::Task& task) {
    return task.health == QLatin1String("active")
        || task.health == QLatin1String("blocked");
}

bool validRoute(const QJsonArray& route) {
    if (route.size() > 32) return false;
    for (const QJsonValue& value : route) {
        if (!value.isObject()) return false;
        const QJsonObject point = value.toObject();
        if (!point.value(QStringLiteral("x")).isDouble()
            || !point.value(QStringLiteral("y")).isDouble()
            || !std::isfinite(point.value(QStringLiteral("x")).toDouble())
            || !std::isfinite(point.value(QStringLiteral("y")).toDouble())) {
            return false;
        }
    }
    return true;
}

constexpr qsizetype kLegacyEventHistoryLimit = 4096;

QJsonObject taskToJson(const StrictVmfTaskSet::Task& task) {
    return {{QStringLiteral("taskId"), task.taskId},
            {QStringLiteral("side"), task.side},
            {QStringLiteral("taskRevision"), static_cast<qint64>(task.taskRevision)},
            {QStringLiteral("stage"), task.stage},
            {QStringLiteral("health"), task.health},
            {QStringLiteral("blockCode"), task.blockCode},
            {QStringLiteral("commanderSeatId"), task.commanderSeatId},
            {QStringLiteral("reconSeatId"), task.reconSeatId},
            {QStringLiteral("attackSeatId"), task.attackSeatId},
            {QStringLiteral("groundSeatId"), task.groundSeatId},
            {QStringLiteral("targetId"), task.targetId},
            {QStringLiteral("correlationId"), task.correlationId},
            {QStringLiteral("route"), task.route},
            {QStringLiteral("pendingMessageIds"), QJsonArray::fromStringList(task.pendingMessageIds)},
            {QStringLiteral("eventHistory"), task.eventHistory},
            {QStringLiteral("createdAt"), task.createdAt},
            {QStringLiteral("updatedAt"), task.updatedAt}};
}

bool taskFromJson(const QJsonObject& object, StrictVmfTaskSet::Task* task, QString* error) {
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    StrictVmfTaskSet::Task parsed;
    parsed.taskId = object.value(QStringLiteral("taskId")).toString();
    parsed.side = object.value(QStringLiteral("side")).toString();
    const qint64 taskRevision = object.value(QStringLiteral("taskRevision")).toInteger();
    parsed.taskRevision = taskRevision > 0 ? static_cast<quint64>(taskRevision) : 0;
    parsed.stage = object.value(QStringLiteral("stage")).toString();
    parsed.health = object.value(QStringLiteral("health")).toString();
    parsed.blockCode = object.value(QStringLiteral("blockCode")).toString();
    parsed.commanderSeatId = object.value(QStringLiteral("commanderSeatId")).toString();
    parsed.reconSeatId = object.value(QStringLiteral("reconSeatId")).toString();
    parsed.attackSeatId = object.value(QStringLiteral("attackSeatId")).toString();
    parsed.groundSeatId = object.value(QStringLiteral("groundSeatId")).toString();
    parsed.targetId = object.value(QStringLiteral("targetId")).toString();
    parsed.correlationId = object.value(QStringLiteral("correlationId")).toString();
    parsed.route = object.value(QStringLiteral("route")).toArray();
    parsed.eventHistory = object.value(QStringLiteral("eventHistory")).toArray();
    parsed.createdAt = object.value(QStringLiteral("createdAt")).toDouble(-1.0);
    parsed.updatedAt = object.value(QStringLiteral("updatedAt")).toDouble(-1.0);
    static const QSet<QString> stages{
        QStringLiteral("awaitingTargetReport"), QStringLiteral("targetReported"),
        QStringLiteral("dispatchPending"), QStringLiteral("enRoute"),
        QStringLiteral("groundGuidancePending"), QStringLiteral("rendezvousReady"),
        QStringLiteral("identityHandshakePending"), QStringLiteral("guidancePackagePending"),
        QStringLiteral("routeAcceptancePending"), QStringLiteral("attackLanePending"),
        QStringLiteral("attackAuthorizationPending"), QStringLiteral("engaging"),
        QStringLiteral("damageReportPending"), QStringLiteral("damageAssessmentPending"),
        QStringLiteral("reconConfirmationPending"), QStringLiteral("targetDestroyed"),
        QStringLiteral("withdrawPending"), QStringLiteral("returning"),
        QStringLiteral("completed")};
    if (!validId(parsed.taskId) || !StrictVmfTaskSet::validSide(parsed.side)
        || parsed.taskRevision == 0 || !validId(parsed.commanderSeatId)
        || !validId(parsed.reconSeatId) || !validId(parsed.attackSeatId)
        || !validId(parsed.groundSeatId) || !validId(parsed.targetId)
        || !validId(parsed.correlationId) || !stages.contains(parsed.stage)
        || (parsed.health != QLatin1String("active")
            && parsed.health != QLatin1String("blocked")
            && parsed.health != QLatin1String("completed")
            && parsed.health != QLatin1String("cancelled"))
        || !object.value(QStringLiteral("route")).isArray()
        || !validRoute(parsed.route)
        || parsed.eventHistory.size() > kLegacyEventHistoryLimit
        || !std::isfinite(parsed.createdAt) || parsed.createdAt < 0.0
        || !std::isfinite(parsed.updatedAt) || parsed.updatedAt < 0.0) {
        return fail(QStringLiteral("严格 VMF 任务检查点无效"));
    }
    while (parsed.eventHistory.size() > StrictVmfTaskSet::EventHistoryLimit) {
        parsed.eventHistory.removeFirst();
    }
    const QJsonValue pending = object.value(QStringLiteral("pendingMessageIds"));
    if (!pending.isArray()) return fail(QStringLiteral("严格 VMF pending 消息无效"));
    for (const QJsonValue& value : pending.toArray()) {
        if (!validId(value.toString())) return fail(QStringLiteral("严格 VMF pending 消息 ID 无效"));
        parsed.pendingMessageIds.append(value.toString());
    }
    if (task) *task = parsed;
    return true;
}

} // namespace

bool StrictVmfTaskSet::validSide(const QString& side) {
    return side == QLatin1String("red") || side == QLatin1String("blue");
}

bool StrictVmfTaskSet::validStageAction(const QString& stage, const QString& action) {
    static const QHash<QString, QStringList> allowed{
        {QStringLiteral("awaitingTargetReport"), {QStringLiteral("reportTarget")} },
        {QStringLiteral("targetReported"), {QStringLiteral("dispatch")} },
        {QStringLiteral("dispatchPending"), {QStringLiteral("acceptDispatch")} },
        {QStringLiteral("enRoute"), {QStringLiteral("orderGroundGuidance")} },
        {QStringLiteral("groundGuidancePending"), {QStringLiteral("markRendezvousReady")} },
        {QStringLiteral("rendezvousReady"), {QStringLiteral("identityHello")} },
        {QStringLiteral("identityHandshakePending"), {QStringLiteral("identityConfirm")} },
        {QStringLiteral("guidancePackagePending"), {QStringLiteral("sendGuidancePackage")} },
        {QStringLiteral("routeAcceptancePending"), {QStringLiteral("acceptGuidance")} },
        {QStringLiteral("attackLanePending"), {QStringLiteral("reportAttackReady")} },
        {QStringLiteral("attackAuthorizationPending"), {QStringLiteral("authorizeAttack")} },
        {QStringLiteral("engaging"), {QStringLiteral("engage")} },
        {QStringLiteral("damageReportPending"), {QStringLiteral("reportBattleDamage")} },
        {QStringLiteral("damageAssessmentPending"), {QStringLiteral("confirmDamageAssessment")} },
        {QStringLiteral("reconConfirmationPending"), {QStringLiteral("confirmTargetDestroyed")} },
        {QStringLiteral("targetDestroyed"), {QStringLiteral("withdraw")} },
        {QStringLiteral("withdrawPending"), {QStringLiteral("markReturning")} }
    };
    return allowed.value(stage).contains(action);
}

QString StrictVmfTaskSet::nextStage(const QString& stage, const QString& action) {
    if (action == QLatin1String("reportTarget")) return QStringLiteral("targetReported");
    if (action == QLatin1String("dispatch")) return QStringLiteral("dispatchPending");
    if (action == QLatin1String("acceptDispatch")) return QStringLiteral("enRoute");
    if (action == QLatin1String("orderGroundGuidance")) return QStringLiteral("groundGuidancePending");
    if (action == QLatin1String("markRendezvousReady")) return QStringLiteral("rendezvousReady");
    if (action == QLatin1String("identityHello")) return QStringLiteral("identityHandshakePending");
    if (action == QLatin1String("identityConfirm")) return QStringLiteral("guidancePackagePending");
    if (action == QLatin1String("sendGuidancePackage")) return QStringLiteral("routeAcceptancePending");
    if (action == QLatin1String("acceptGuidance")) return QStringLiteral("attackLanePending");
    if (action == QLatin1String("reportAttackReady")) return QStringLiteral("attackAuthorizationPending");
    if (action == QLatin1String("authorizeAttack")) return QStringLiteral("engaging");
    if (action == QLatin1String("engage")) return QStringLiteral("damageReportPending");
    if (action == QLatin1String("reportBattleDamage")) return QStringLiteral("damageAssessmentPending");
    if (action == QLatin1String("confirmDamageAssessment")) return QStringLiteral("reconConfirmationPending");
    if (action == QLatin1String("confirmTargetDestroyed")) return QStringLiteral("targetDestroyed");
    if (action == QLatin1String("withdraw")) return QStringLiteral("withdrawPending");
    if (action == QLatin1String("markReturning")) return QStringLiteral("returning");
    Q_UNUSED(stage);
    return {};
}

bool StrictVmfTaskSet::roleMatches(const Task& task, const QString& action,
                                   const QString& actorSeatId) {
    if (action == QLatin1String("reportTarget")
        || action == QLatin1String("confirmTargetDestroyed")) return actorSeatId == task.reconSeatId;
    if (action == QLatin1String("dispatch") || action == QLatin1String("orderGroundGuidance")
        || action == QLatin1String("withdraw")) return actorSeatId == task.commanderSeatId;
    if (action == QLatin1String("acceptDispatch") || action == QLatin1String("identityHello")
        || action == QLatin1String("acceptGuidance") || action == QLatin1String("reportAttackReady")
        || action == QLatin1String("engage") || action == QLatin1String("reportBattleDamage")
        || action == QLatin1String("markReturning")) {
        return actorSeatId == task.attackSeatId;
    }
    if (action == QLatin1String("markRendezvousReady")
        || action == QLatin1String("identityConfirm")
        || action == QLatin1String("sendGuidancePackage")
        || action == QLatin1String("authorizeAttack") || action == QLatin1String("confirmDamageAssessment")) {
        return actorSeatId == task.groundSeatId;
    }
    return false;
}

void StrictVmfTaskSet::appendHistory(Task& task, const QString& action,
                                     const QString& actorSeatId, double now,
                                     const QString& code) {
    task.eventHistory.append(QJsonObject{{QStringLiteral("action"), action},
                                         {QStringLiteral("actorSeatId"), actorSeatId},
                                         {QStringLiteral("stage"), task.stage},
                                         {QStringLiteral("health"), task.health},
                                         {QStringLiteral("code"), code},
                                         {QStringLiteral("time"), now}});
    while (task.eventHistory.size() > EventHistoryLimit) {
        task.eventHistory.removeFirst();
    }
}

StrictVmfTaskSet::Result StrictVmfTaskSet::failure(const QString& code,
                                                   const QString& message,
                                                   quint64 revision) const {
    return {false, QStringLiteral("rejected"), code, message, revision, {}};
}

StrictVmfTaskSet::Result StrictVmfTaskSet::success(Task& task, const QString& status) {
    ++task.taskRevision;
    return {true, status, QStringLiteral("OK"), {}, task.taskRevision,
            task.pendingMessageIds};
}

StrictVmfTaskSet::Result StrictVmfTaskSet::createTask(
    const QString& taskId, const QString& side, const QString& commanderSeatId,
    const QString& reconSeatId, const QString& attackSeatId, const QString& groundSeatId,
    const QString& targetId, const QString& correlationId, bool waitingForHuman, double now,
    const QJsonArray& route) {
    if (!validId(taskId) || !validSide(side) || !validId(commanderSeatId)
        || !validId(reconSeatId) || !validId(attackSeatId) || !validId(groundSeatId)
        || !validId(targetId) || !validId(correlationId) || !validRoute(route)) {
        return failure(QStringLiteral("INVALID_TASK"), QStringLiteral("严格 VMF 任务绑定无效"));
    }
    if (m_tasks.contains(taskId)) return failure(QStringLiteral("DUPLICATE_TASK"), QStringLiteral("任务已存在"));
    int active = 0;
    for (const Task& task : m_tasks) {
        if (task.side == side && isOpenTask(task)) ++active;
        if (isOpenTask(task)
            && (task.reconSeatId == reconSeatId || task.attackSeatId == attackSeatId
                || task.groundSeatId == groundSeatId || task.targetId == targetId)) {
            return failure(QStringLiteral("RESOURCE_BUSY"), QStringLiteral("任务资源已被占用"));
        }
    }
    if (active >= ActiveLimitPerSide) return failure(QStringLiteral("TASK_LIMIT"), QStringLiteral("本方活动任务已达到上限"));
    Task task;
    task.taskId = taskId; task.side = side; task.commanderSeatId = commanderSeatId;
    task.reconSeatId = reconSeatId; task.attackSeatId = attackSeatId;
    task.groundSeatId = groundSeatId; task.targetId = targetId;
    task.correlationId = correlationId; task.route = route;
    task.createdAt = now; task.updatedAt = now;
    if (waitingForHuman) { task.health = QStringLiteral("blocked"); task.blockCode = QStringLiteral("WAITING_FOR_HUMAN"); }
    m_tasks.insert(taskId, task); m_historyOrder.append(taskId);
    while (m_tasks.size() > HistoryLimit) {
        qsizetype removableIndex = -1;
        for (qsizetype index = 0; index < m_historyOrder.size(); ++index) {
            const auto stored = m_tasks.constFind(m_historyOrder.at(index));
            if (stored == m_tasks.cend() || !isOpenTask(stored.value())) {
                removableIndex = index;
                break;
            }
        }
        if (removableIndex < 0) break;
        m_tasks.remove(m_historyOrder.takeAt(removableIndex));
    }
    Task& inserted = m_tasks[taskId];
    appendHistory(inserted, QStringLiteral("createTask"), commanderSeatId, now,
                  inserted.blockCode);
    return success(inserted, inserted.health == QLatin1String("blocked")
                       ? QStringLiteral("blocked") : QStringLiteral("accepted"));
}

StrictVmfTaskSet::Result StrictVmfTaskSet::applyAction(
    const QJsonObject& command, const QString& actorSeatId,
    const QSet<QString>& placeholderSeats, double now) {
    const QString taskId = command.value(QStringLiteral("taskId")).toString();
    Task* task = this->task(taskId);
    if (!task) return failure(QStringLiteral("TASK_NOT_FOUND"), QStringLiteral("任务不存在"));
    const quint64 expected = command.value(QStringLiteral("expectedTaskRevision")).toInteger();
    if (expected != task->taskRevision) return failure(QStringLiteral("TASK_REVISION_MISMATCH"), QStringLiteral("任务版本已变化"), task->taskRevision);
    const QString action = command.value(QStringLiteral("action")).toString();
    if (task->health == QLatin1String("completed") || task->health == QLatin1String("cancelled")) return failure(QStringLiteral("TASK_CLOSED"), QStringLiteral("任务已结束"), task->taskRevision);
    if (!validStageAction(task->stage, action) || !roleMatches(*task, action, actorSeatId)) return failure(QStringLiteral("VMF_SEQUENCE_INVALID"), QStringLiteral("任务阶段或战位不允许该动作"), task->taskRevision);
    if (placeholderSeats.contains(actorSeatId)) {
        task->health = QStringLiteral("blocked"); task->blockCode = QStringLiteral("WAITING_FOR_HUMAN"); task->updatedAt = now;
        appendHistory(*task, action, actorSeatId, now, task->blockCode);
        ++task->taskRevision;
        return {true, QStringLiteral("blocked"), task->blockCode, QStringLiteral("该席位仍由服务器占位"), task->taskRevision, {}};
    }
    task->health = QStringLiteral("active"); task->blockCode.clear();
    task->stage = action == QLatin1String("confirmDamageAssessment")
            && command.contains(QStringLiteral("targetDestroyed"))
            && !command.value(QStringLiteral("targetDestroyed")).toBool()
        ? QStringLiteral("engaging") : nextStage(task->stage, action);
    task->updatedAt = now;
    appendHistory(*task, action, actorSeatId, now);
    return success(*task);
}

StrictVmfTaskSet::Result StrictVmfTaskSet::setBlocked(const QString& taskId,
                                                      const QString& code, double now) {
    Task* task = this->task(taskId);
    if (!task) return failure(QStringLiteral("TASK_NOT_FOUND"), QStringLiteral("任务不存在"));
    task->health = QStringLiteral("blocked"); task->blockCode = code; task->updatedAt = now;
    appendHistory(*task, QStringLiteral("blocked"), {}, now, code); ++task->taskRevision;
    return {true, QStringLiteral("blocked"), code, {}, task->taskRevision, task->pendingMessageIds};
}

StrictVmfTaskSet::Result StrictVmfTaskSet::clearBlocked(const QString& taskId, double now) {
    Task* task = this->task(taskId);
    if (!task) return failure(QStringLiteral("TASK_NOT_FOUND"), QStringLiteral("任务不存在"));
    task->health = QStringLiteral("active"); task->blockCode.clear(); task->updatedAt = now;
    appendHistory(*task, QStringLiteral("unblocked"), {}, now); ++task->taskRevision;
    return {true, QStringLiteral("accepted"), QStringLiteral("OK"), {}, task->taskRevision, task->pendingMessageIds};
}

StrictVmfTaskSet::Result StrictVmfTaskSet::completeReturn(const QString& taskId, double now) {
    Task* task = this->task(taskId);
    if (!task || task->stage != QLatin1String("returning")) return failure(QStringLiteral("VMF_SEQUENCE_INVALID"), QStringLiteral("任务尚未到达返航点"));
    task->stage = QStringLiteral("completed"); task->health = QStringLiteral("completed"); task->updatedAt = now;
    appendHistory(*task, QStringLiteral("completeReturn"), {}, now); ++task->taskRevision;
    return {true, QStringLiteral("accepted"), QStringLiteral("OK"), {}, task->taskRevision, {}};
}

const StrictVmfTaskSet::Task* StrictVmfTaskSet::task(const QString& taskId) const {
    const auto it = m_tasks.constFind(taskId);
    return it == m_tasks.cend() ? nullptr : &it.value();
}
StrictVmfTaskSet::Task* StrictVmfTaskSet::task(const QString& taskId) { return m_tasks.contains(taskId) ? &m_tasks[taskId] : nullptr; }

QList<StrictVmfTaskSet::Task> StrictVmfTaskSet::tasks(const QString& side) const {
    QList<Task> result;
    for (const Task& task : m_tasks) if (side.isEmpty() || task.side == side) result.append(task);
    std::sort(result.begin(), result.end(), [](const Task& a, const Task& b) {
        if (a.createdAt != b.createdAt) return a.createdAt < b.createdAt;
        return a.taskId < b.taskId;
    });
    return result;
}

QJsonObject StrictVmfTaskSet::toJson() const {
    QJsonArray values;
    for (const Task& task : tasks()) values.append(taskToJson(task));
    return {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("tasks"), values}};
}

bool StrictVmfTaskSet::restore(const QJsonObject& object, QString* error) {
    if (object.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        if (error) *error = QStringLiteral("严格 VMF 任务 schema 无效");
        return false;
    }
    const QJsonValue values = object.value(QStringLiteral("tasks"));
    if (!values.isArray() || values.toArray().size() > HistoryLimit) {
        if (error) *error = QStringLiteral("严格 VMF 任务列表无效"); return false;
    }
    QHash<QString, Task> restored;
    for (const QJsonValue& value : values.toArray()) {
        Task task; if (!value.isObject() || !taskFromJson(value.toObject(), &task, error)) return false;
        if (restored.contains(task.taskId)) { if (error) *error = QStringLiteral("严格 VMF 任务 ID 重复"); return false; }
        restored.insert(task.taskId, task);
    }
    m_tasks = restored;
    const QList<Task> ordered = tasks();
    m_historyOrder.clear();
    for (const Task& task : ordered) m_historyOrder.append(task.taskId);
    return true;
}

} // namespace gbr
