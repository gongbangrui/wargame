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

TEST(StrictVmfRoomTest, PlaceholdersAreReadyAndHumanTakeoverKeepsUnitIdentity) {
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
    ASSERT_TRUE(room.installPlaceholdersForMissing().ok);
    const AuthoritativeRoom::Seat placeholder = room.seat(QStringLiteral("red_attack_1"));
    ASSERT_EQ(placeholder.controllerType, QStringLiteral("placeholder"));
    EXPECT_TRUE(placeholder.deployed);
    EXPECT_TRUE(placeholder.ready);

    ASSERT_TRUE(room.claimSeat(1, QStringLiteral("red commander"),
                               QStringLiteral("red_commander"),
                               QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(room.claimSeat(2, QStringLiteral("blue commander"),
                               QStringLiteral("blue_commander"),
                               QStringLiteral("commandpost")).ok);
    ASSERT_TRUE(room.deployInitial(QStringLiteral("red_commander"), GeoPos{100, 100, 0}).ok);
    ASSERT_TRUE(room.deployInitial(QStringLiteral("blue_commander"), GeoPos{900, 900, 0}).ok);
    ASSERT_TRUE(room.setReady(1, true).ok);
    ASSERT_TRUE(room.setReady(2, true).ok);
    EXPECT_TRUE(room.readiness().value(QStringLiteral("ready")).toBool());

    ASSERT_TRUE(room.claimSeat(3, QStringLiteral("attack human"),
                               QStringLiteral("red_attack_1"),
                               QStringLiteral("attackuav")).ok);
    const AuthoritativeRoom::Seat taken = room.seat(QStringLiteral("red_attack_1"));
    EXPECT_EQ(taken.controllerType, QStringLiteral("human"));
    EXPECT_EQ(taken.unitId, placeholder.unitId);
    EXPECT_TRUE(taken.deployed);
    EXPECT_FALSE(taken.ready);
    EXPECT_FALSE(room.seat(QStringLiteral("red_commander")).ready);
    ASSERT_TRUE(room.setReady(3, true).ok);
    ASSERT_TRUE(room.setReady(1, true).ok);
    EXPECT_TRUE(room.readiness().value(QStringLiteral("ready")).toBool());

    ASSERT_TRUE(room.removePlaceholders().ok);
    EXPECT_TRUE(room.hasSeat(QStringLiteral("red_attack_1")));
    EXPECT_FALSE(room.hasSeat(QStringLiteral("red_recon_1")));
}
