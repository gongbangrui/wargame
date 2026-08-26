#include <gtest/gtest.h>

#include "server/game/StrictVmfTask.h"
#include "server/game/AuthoritativeRoom.h"

using namespace gbr;

namespace {

StrictVmfTaskSet::Result createTask(StrictVmfTaskSet* tasks,
                                    bool waitingForHuman = false) {
    return tasks->createTask(QStringLiteral("red-task-1"), QStringLiteral("red"),
                             QStringLiteral("red_commander"),
                             QStringLiteral("red_recon_1"),
                             QStringLiteral("red_attack_1"),
                             QStringLiteral("red_ground_1"),
                             QStringLiteral("blue_gt_1"),
                             QStringLiteral("corr-1"), waitingForHuman, 1.0);
}

StrictVmfTaskSet::Result apply(StrictVmfTaskSet* tasks, const QString& action,
                               const QString& seatId, quint64 revision,
                               const QSet<QString>& placeholders = {}) {
    return tasks->applyAction(
        QJsonObject{{QStringLiteral("taskId"), QStringLiteral("red-task-1")},
                    {QStringLiteral("action"), action},
                    {QStringLiteral("expectedTaskRevision"),
                     static_cast<qint64>(revision)}},
        seatId, placeholders, static_cast<double>(revision));
}

} // namespace

TEST(StrictVmfTaskSetTest, FullRoleBoundSequenceCompletesAfterReturn) {
    StrictVmfTaskSet tasks;
    ASSERT_TRUE(createTask(&tasks).ok);
    struct Step { const char* action; const char* seat; };
    const Step steps[] = {
        {"reportTarget", "red_recon_1"},
        {"dispatch", "red_commander"},
        {"acceptDispatch", "red_attack_1"},
        {"orderGroundGuidance", "red_commander"},
        {"markRendezvousReady", "red_ground_1"},
        {"identityHello", "red_attack_1"},
        {"identityConfirm", "red_ground_1"},
        {"sendGuidancePackage", "red_ground_1"},
        {"acceptGuidance", "red_attack_1"},
        {"reportAttackReady", "red_attack_1"},
        {"authorizeAttack", "red_ground_1"},
        {"engage", "red_attack_1"},
        {"reportBattleDamage", "red_attack_1"},
        {"confirmDamageAssessment", "red_ground_1"},
        {"confirmTargetDestroyed", "red_recon_1"},
        {"withdraw", "red_commander"},
        {"markReturning", "red_attack_1"},
    };
    quint64 revision = tasks.task(QStringLiteral("red-task-1"))->taskRevision;
    for (const Step& step : steps) {
        const auto result = apply(&tasks, QString::fromLatin1(step.action),
                                  QString::fromLatin1(step.seat), revision);
        ASSERT_TRUE(result.ok) << step.action << ": " << result.code.toStdString();
        revision = result.taskRevision;
    }
    EXPECT_EQ(tasks.task(QStringLiteral("red-task-1"))->stage,
              QStringLiteral("returning"));
    const auto completed = tasks.completeReturn(QStringLiteral("red-task-1"), 50.0);
    ASSERT_TRUE(completed.ok);
    EXPECT_EQ(tasks.task(QStringLiteral("red-task-1"))->health,
              QStringLiteral("completed"));
}

TEST(StrictVmfTaskSetTest, RevisionRoleAndPlaceholderAreAuthoritative) {
    StrictVmfTaskSet tasks;
    ASSERT_TRUE(createTask(&tasks, true).ok);
    const quint64 revision = tasks.task(QStringLiteral("red-task-1"))->taskRevision;
    EXPECT_EQ(apply(&tasks, QStringLiteral("reportTarget"),
                    QStringLiteral("red_attack_1"), revision).code,
              QStringLiteral("VMF_SEQUENCE_INVALID"));
    EXPECT_EQ(apply(&tasks, QStringLiteral("reportTarget"),
                    QStringLiteral("red_recon_1"), revision + 1).code,
              QStringLiteral("TASK_REVISION_MISMATCH"));

    const auto blocked = apply(&tasks, QStringLiteral("reportTarget"),
                               QStringLiteral("red_recon_1"), revision,
                               {QStringLiteral("red_recon_1")});
    ASSERT_TRUE(blocked.ok);
    EXPECT_EQ(blocked.status, QStringLiteral("blocked"));
    EXPECT_EQ(blocked.code, QStringLiteral("WAITING_FOR_HUMAN"));

    const auto takeover = apply(&tasks, QStringLiteral("reportTarget"),
                                QStringLiteral("red_recon_1"),
                                blocked.taskRevision);
    ASSERT_TRUE(takeover.ok);
    EXPECT_EQ(tasks.task(QStringLiteral("red-task-1"))->health,
              QStringLiteral("active"));
}

TEST(StrictVmfTaskSetTest, SurvivingTargetReturnsDamageAssessmentToEngagement) {
    StrictVmfTaskSet tasks;
    ASSERT_TRUE(createTask(&tasks).ok);
    StrictVmfTaskSet::Task* task = tasks.task(QStringLiteral("red-task-1"));
    ASSERT_NE(task, nullptr);
    task->stage = QStringLiteral("damageAssessmentPending");
    const auto result = tasks.applyAction(
        QJsonObject{{QStringLiteral("taskId"), QStringLiteral("red-task-1")},
                    {QStringLiteral("action"), QStringLiteral("confirmDamageAssessment")},
                    {QStringLiteral("expectedTaskRevision"),
                     static_cast<qint64>(task->taskRevision)},
                    {QStringLiteral("targetDestroyed"), false}},
        QStringLiteral("red_ground_1"), {}, 10.0);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(tasks.task(QStringLiteral("red-task-1"))->stage,
              QStringLiteral("engaging"));
}

TEST(StrictVmfTaskSetTest, ResourcesAreExclusiveAndCheckpointIsValidated) {
    StrictVmfTaskSet tasks;
    ASSERT_TRUE(createTask(&tasks).ok);
    const auto conflict = tasks.createTask(
        QStringLiteral("red-task-2"), QStringLiteral("red"),
        QStringLiteral("red_commander"), QStringLiteral("red_recon_1"),
        QStringLiteral("red_attack_2"), QStringLiteral("red_ground_2"),
        QStringLiteral("blue_gt_2"), QStringLiteral("corr-2"), false, 2.0);
    EXPECT_FALSE(conflict.ok);
    EXPECT_EQ(conflict.code, QStringLiteral("RESOURCE_BUSY"));

    StrictVmfTaskSet blockedTasks;
    ASSERT_TRUE(createTask(&blockedTasks, true).ok);
    const auto blockedConflict = blockedTasks.createTask(
        QStringLiteral("red-task-2"), QStringLiteral("red"),
        QStringLiteral("red_commander"), QStringLiteral("red_recon_1"),
        QStringLiteral("red_attack_2"), QStringLiteral("red_ground_2"),
        QStringLiteral("blue_gt_2"), QStringLiteral("corr-2"), false, 2.0);
    EXPECT_FALSE(blockedConflict.ok);
    EXPECT_EQ(blockedConflict.code, QStringLiteral("RESOURCE_BUSY"));

    StrictVmfTaskSet restored;
    QString error;
    ASSERT_TRUE(restored.restore(tasks.toJson(), &error)) << error.toStdString();
    ASSERT_NE(restored.task(QStringLiteral("red-task-1")), nullptr);
    EXPECT_EQ(restored.task(QStringLiteral("red-task-1"))->targetId,
              QStringLiteral("blue_gt_1"));

    QJsonObject malformed = tasks.toJson();
    QJsonArray values = malformed.value(QStringLiteral("tasks")).toArray();
    QJsonObject task = values.first().toObject();
    task[QStringLiteral("stage")] = QStringLiteral("skip-to-complete");
    values[0] = task;
    malformed[QStringLiteral("tasks")] = values;
    EXPECT_FALSE(restored.restore(malformed, &error));
}

TEST(StrictVmfTaskSetTest, AuthoritativeRouteRoundTripsWithTaskCheckpoint) {
    StrictVmfTaskSet tasks;
    const QJsonArray route{
        QJsonObject{{QStringLiteral("x"), 1200.0}, {QStringLiteral("y"), 2400.0}},
        QJsonObject{{QStringLiteral("x"), 3600.0}, {QStringLiteral("y"), 4800.0}}};
    ASSERT_TRUE(tasks.createTask(
        QStringLiteral("red-task-route"), QStringLiteral("red"),
        QStringLiteral("red_commander"), QStringLiteral("red_recon_1"),
        QStringLiteral("red_attack_1"), QStringLiteral("red_ground_1"),
        QStringLiteral("blue_gt_1"), QStringLiteral("corr-route"), false, 1.0,
        route).ok);

    StrictVmfTaskSet restored;
    QString error;
    ASSERT_TRUE(restored.restore(tasks.toJson(), &error)) << error.toStdString();
    ASSERT_NE(restored.task(QStringLiteral("red-task-route")), nullptr);
    EXPECT_EQ(restored.task(QStringLiteral("red-task-route"))->route, route);

    QJsonObject legacy = tasks.toJson();
    QJsonArray legacyTasks = legacy.value(QStringLiteral("tasks")).toArray();
    QJsonObject legacyTask = legacyTasks.first().toObject();
    QJsonArray legacyHistory;
    for (int index = 0; index < 200; ++index) {
        legacyHistory.append(QJsonObject{{QStringLiteral("action"),
                                          QStringLiteral("legacy-%1").arg(index)}});
    }
    legacyTask[QStringLiteral("eventHistory")] = legacyHistory;
    legacyTasks[0] = legacyTask;
    legacy[QStringLiteral("tasks")] = legacyTasks;
    ASSERT_TRUE(restored.restore(legacy, &error)) << error.toStdString();
    EXPECT_EQ(restored.task(QStringLiteral("red-task-route"))->eventHistory.size(),
              StrictVmfTaskSet::EventHistoryLimit);
    EXPECT_EQ(restored.task(QStringLiteral("red-task-route"))->eventHistory.first()
                  .toObject().value(QStringLiteral("action")).toString(),
              QStringLiteral("legacy-136"));
}

TEST(StrictVmfTaskSetTest, CompletedHistoryIsBoundedAndRemainsRestorable) {
    StrictVmfTaskSet tasks;
    for (int index = 0; index <= StrictVmfTaskSet::HistoryLimit; ++index) {
        const QString suffix = QString::number(index);
        const QString taskId = QStringLiteral("red-task-%1").arg(suffix);
        ASSERT_TRUE(tasks.createTask(
            taskId, QStringLiteral("red"), QStringLiteral("red_commander"),
            QStringLiteral("red_recon_%1").arg(suffix),
            QStringLiteral("red_attack_%1").arg(suffix),
            QStringLiteral("red_ground_%1").arg(suffix),
            QStringLiteral("blue_gt_%1").arg(suffix),
            QStringLiteral("corr-%1").arg(suffix), false,
            static_cast<double>(index)).ok);
        StrictVmfTaskSet::Task* task = tasks.task(taskId);
        ASSERT_NE(task, nullptr);
        task->stage = QStringLiteral("returning");
        ASSERT_TRUE(tasks.completeReturn(taskId, static_cast<double>(index) + 0.5).ok);
    }

    EXPECT_EQ(tasks.tasks().size(), StrictVmfTaskSet::HistoryLimit);
    EXPECT_EQ(tasks.task(QStringLiteral("red-task-0")), nullptr);
    EXPECT_NE(tasks.task(QStringLiteral("red-task-100")), nullptr);

    StrictVmfTaskSet restored;
    QString error;
    EXPECT_TRUE(restored.restore(tasks.toJson(), &error)) << error.toStdString();
}

TEST(StrictVmfRoomTest, SingleSideRosterRequiresHumanCommanderAndAutomatesDisconnects) {
    AuthoritativeRoom room;
    QHash<QString, int> limits;
    for (const QString& side : {QStringLiteral("red"), QStringLiteral("blue")}) {
        limits.insert(side + QStringLiteral("_commander"), 1);
        for (const QString& type : {QStringLiteral("attack"), QStringLiteral("recon"),
                                    QStringLiteral("ground"), QStringLiteral("jammer")}) {
            limits.insert(QStringLiteral("%1_%2").arg(side, type), 1);
        }
    }
    QString error;
    ASSERT_TRUE(room.setSeatLimits(limits, &error)) << error.toStdString();
    ASSERT_TRUE(room.setTemplateCatalog(AuthoritativeRoom::defaultTemplateCatalog(), &error));
    ASSERT_TRUE(room.setScenarioUnits(ScenarioIo::defaultScenario().units, &error))
        << error.toStdString();
    ASSERT_TRUE(room.setVmfSingleSide(true).ok);

    EXPECT_FALSE(room.readiness().value(QStringLiteral("ready")).toBool());
    EXPECT_FALSE(room.hasSeat(QStringLiteral("red_commander")));
    const AuthoritativeRoom::Seat fixedTarget = room.seat(QStringLiteral("blue_commander"));
    ASSERT_EQ(fixedTarget.controlMode, QStringLiteral("fixed-target"));
    EXPECT_EQ(fixedTarget.sourceUnitId, QStringLiteral("blue_cp"));
    EXPECT_EQ(room.claimSeat(9, QStringLiteral("blue human"),
                             QStringLiteral("blue_commander"),
                             QStringLiteral("commandpost")).code,
              QStringLiteral("SIDE_RESERVED_FOR_FIXED_TARGET"));

    const AuthoritativeRoom::Seat placeholder = room.seat(QStringLiteral("red_attack_1"));
    ASSERT_EQ(placeholder.controllerType, QStringLiteral("placeholder"));
    EXPECT_EQ(placeholder.controlMode, QStringLiteral("vmf-auto"));
    EXPECT_TRUE(placeholder.deployed);
    EXPECT_TRUE(placeholder.ready);

    ASSERT_TRUE(room.claimSeat(1, QStringLiteral("red commander"),
                               QStringLiteral("red_commander"),
                               QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("attack human"),
                               QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    const AuthoritativeRoom::Seat taken = room.seat(QStringLiteral("red_attack_1"));
    EXPECT_EQ(taken.controllerType, QStringLiteral("human"));
    EXPECT_EQ(taken.unitId, placeholder.unitId);
    EXPECT_TRUE(taken.deployed);
    EXPECT_FALSE(taken.ready);
    ASSERT_TRUE(room.setReady(3, true).ok);
    ASSERT_TRUE(room.setReady(1, true).ok);
    EXPECT_TRUE(room.readiness().value(QStringLiteral("ready")).toBool());
    ASSERT_TRUE(room.start().ok);

    ASSERT_TRUE(room.disconnect(3).ok);
    const AuthoritativeRoom::Seat automated = room.seat(QStringLiteral("red_attack_1"));
    EXPECT_EQ(automated.controllerType, QStringLiteral("placeholder"));
    EXPECT_EQ(automated.controlMode, QStringLiteral("vmf-auto"));
    EXPECT_EQ(automated.unitId, placeholder.unitId);
    EXPECT_TRUE(automated.connected);
    EXPECT_TRUE(automated.ready);

    EXPECT_EQ(room.claimSeat(4, QStringLiteral("replacement"),
                             QStringLiteral("red_attack_1"),
                             QStringLiteral("attackuav")).code,
              QString());
    EXPECT_EQ(room.seat(QStringLiteral("red_attack_1")).unitId, placeholder.unitId);
}

TEST(StrictVmfRoomTest, ScenarioEditsPruneAutomationButProtectHumanBindings) {
    AuthoritativeRoom room;
    QHash<QString, int> limits;
    for (const QString& side : {QStringLiteral("red"), QStringLiteral("blue")}) {
        limits.insert(side + QStringLiteral("_commander"), 1);
        for (const QString& type : {QStringLiteral("attack"), QStringLiteral("recon"),
                                    QStringLiteral("ground"), QStringLiteral("jammer")}) {
            limits.insert(QStringLiteral("%1_%2").arg(side, type), 1);
        }
    }
    QString error;
    ASSERT_TRUE(room.setSeatLimits(limits, &error)) << error.toStdString();
    ASSERT_TRUE(room.setTemplateCatalog(AuthoritativeRoom::defaultTemplateCatalog(), &error));
    const Scenario initial = ScenarioIo::defaultScenario();
    ASSERT_TRUE(room.setScenarioUnits(initial.units, &error)) << error.toStdString();
    ASSERT_TRUE(room.setVmfSingleSide(true).ok);

    const QString automaticSource = room.seat(QStringLiteral("red_attack_1")).sourceUnitId;
    ASSERT_FALSE(automaticSource.isEmpty());
    Scenario withoutAutomaticSource = initial;
    std::erase_if(withoutAutomaticSource.units, [&automaticSource](const ScenarioUnit& unit) {
        return unit.id == automaticSource;
    });
    ASSERT_TRUE(room.setScenarioUnits(withoutAutomaticSource.units, &error)) << error.toStdString();
    EXPECT_FALSE(room.hasSeat(QStringLiteral("red_attack_1")));
    ASSERT_TRUE(room.setVmfSingleSide(true).ok);
    EXPECT_EQ(room.seat(QStringLiteral("red_attack_1")).controllerType,
              QStringLiteral("placeholder"));

    ASSERT_TRUE(room.claimSeat(1, QStringLiteral("red commander"),
                               QStringLiteral("red_commander"),
                               QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(room.claimSeat(2, QStringLiteral("red recon"),
                               QStringLiteral("red_recon_1"),
                               QStringLiteral("reconuav")).ok);
    const QString humanSource = room.seat(QStringLiteral("red_recon_1")).sourceUnitId;
    ASSERT_FALSE(humanSource.isEmpty());
    Scenario withoutHumanSource = withoutAutomaticSource;
    std::erase_if(withoutHumanSource.units, [&humanSource](const ScenarioUnit& unit) {
        return unit.id == humanSource;
    });
    EXPECT_FALSE(room.setScenarioUnits(withoutHumanSource.units, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("occupied seat source unit")));
    EXPECT_EQ(room.seat(QStringLiteral("red_recon_1")).controllerType,
              QStringLiteral("human"));
}

TEST(StrictVmfRoomTest, ResetRestoresAutomationAndFixedTargets) {
    AuthoritativeRoom room;
    QString error;
    ASSERT_TRUE(room.setTemplateCatalog(AuthoritativeRoom::defaultTemplateCatalog(), &error));
    ASSERT_TRUE(room.setScenarioUnits(ScenarioIo::defaultScenario().units, &error));
    ASSERT_TRUE(room.setVmfSingleSide(true).ok);

    ASSERT_TRUE(room.applyOperation(QStringLiteral("vmf-reset"), QStringLiteral("reset"),
                                    room.revision()).ok);
    EXPECT_EQ(room.seat(QStringLiteral("blue_commander")).controlMode,
              QStringLiteral("fixed-target"));
    EXPECT_EQ(room.seat(QStringLiteral("red_recon_1")).controlMode,
              QStringLiteral("vmf-auto"));
    EXPECT_TRUE(room.seat(QStringLiteral("red_recon_1")).deployed);
}

TEST(StrictVmfRoomTest, PreparingDeparturesRestoreAutomaticSeats) {
    AuthoritativeRoom room;
    QString error;
    ASSERT_TRUE(room.setTemplateCatalog(AuthoritativeRoom::defaultTemplateCatalog(), &error));
    ASSERT_TRUE(room.setScenarioUnits(ScenarioIo::defaultScenario().units, &error));
    ASSERT_TRUE(room.setVmfSingleSide(true).ok);
    ASSERT_TRUE(room.claimSeat(1, QStringLiteral("commander"),
                               QStringLiteral("red_commander"),
                               QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(room.claimSeat(2, QStringLiteral("attack"),
                               QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);

    ASSERT_TRUE(room.leave(2).ok);
    EXPECT_EQ(room.seat(QStringLiteral("red_attack_1")).controllerType,
              QStringLiteral("placeholder"));
    EXPECT_EQ(room.seat(QStringLiteral("red_attack_1")).controlMode,
              QStringLiteral("vmf-auto"));

    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("recon"),
                               QStringLiteral("red_recon_1"),
                               QStringLiteral("reconuav")).ok);
    ASSERT_TRUE(room.leave(1, 3).ok);
    EXPECT_EQ(room.seat(QStringLiteral("red_commander")).userId, 3);
    EXPECT_EQ(room.seat(QStringLiteral("red_recon_1")).controllerType,
              QStringLiteral("placeholder"));
}
