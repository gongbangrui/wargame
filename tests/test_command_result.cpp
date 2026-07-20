#include <gtest/gtest.h>

#include "core/CommandResult.h"
#include "core/SimulationEngine.h"
#include "core/UnitBase.h"

using namespace gbr;

class CommandResultTest : public ::testing::Test {
protected:
    void SetUp() override { engine.loadDefaultScenario(); }
    SimulationEngine engine;
};

TEST_F(CommandResultTest, UnknownActionIsRejectedWithStableCode) {
    const CommandResult result = engine.executeCommand(QStringLiteral("unknown"), {});
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.code, QString::fromLatin1(CommandCode::UnknownAction));
}

TEST_F(CommandResultTest, FriendlyAttackIsRejected) {
    const CommandResult result = engine.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("red_r1")}});
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.code, QString::fromLatin1(CommandCode::InvalidTarget));
}

TEST_F(CommandResultTest, DeadUnitIsRejected) {
    ASSERT_NE(engine.unit(QStringLiteral("red_r1")), nullptr);
    engine.unit(QStringLiteral("red_r1"))->setHp(0.0);
    const CommandResult result = engine.executeCommand(
        QStringLiteral("moveTo"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_r1")},
                    {QStringLiteral("pos"),
                     QVariantMap{{QStringLiteral("x"), 5000.0},
                                 {QStringLiteral("y"), 5000.0}}}});
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.code, QString::fromLatin1(CommandCode::UnitDestroyed));
}

TEST_F(CommandResultTest, AcceptedCommandReturnsStructuredQmlResult) {
    const QVariantMap result = engine.command(
        QStringLiteral("setSpeed"),
        QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_a1")},
                    {QStringLiteral("speed"), 125.0}});
    EXPECT_TRUE(result.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(result.value(QStringLiteral("code")).toString(), QStringLiteral("OK"));
    EXPECT_DOUBLE_EQ(engine.unit(QStringLiteral("red_a1"))->speed(), 125.0);
}
