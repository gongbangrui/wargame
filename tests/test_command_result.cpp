#include <gtest/gtest.h>

#include "core/CommandResult.h"
#include "core/SimulationEngine.h"
#include "core/SnapshotCodec.h"

#include <QCryptographicHash>
#include <QJsonDocument>

using namespace gbr;

TEST(CommandResultTest, UnknownActionIsRejectedExplicitly) {
    SimulationEngine engine;
    engine.loadDefaultScenario();

    const CommandResult result = engine.command(QStringLiteral("notAnAction"), {});

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.code, QString::fromLatin1(CommandCode::UnknownAction));
    EXPECT_TRUE(result.message.contains(QStringLiteral("未知命令")));
}

TEST(CommandResultTest, InvalidUnitAndArgumentsHaveStableCodes) {
    SimulationEngine engine;
    engine.loadDefaultScenario();

    const auto missing = engine.command(
        QString::fromLatin1(CommandAction::MoveTo),
        {{QStringLiteral("unitId"), QStringLiteral("missing")},
         {QStringLiteral("pos"), QVariantMap{{QStringLiteral("x"), 10.0},
                                              {QStringLiteral("y"), 10.0}}}});
    EXPECT_FALSE(missing.accepted);
    EXPECT_EQ(missing.code, QString::fromLatin1(CommandCode::UnitNotFound));

    const auto outside = engine.command(
        QString::fromLatin1(CommandAction::MoveTo),
        {{QStringLiteral("unitId"), QStringLiteral("red_r1")},
         {QStringLiteral("pos"), QVariantMap{{QStringLiteral("x"), -1.0},
                                              {QStringLiteral("y"), 10.0}}}});
    EXPECT_FALSE(outside.accepted);
    EXPECT_EQ(outside.code, QString::fromLatin1(CommandCode::OutOfBounds));
}

TEST(CommandResultTest, AcceptedCommandContainsAppliedTick) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    engine.stepOnce(0.5);

    const auto result = engine.command(
        QString::fromLatin1(CommandAction::SetSpeed),
        {{QStringLiteral("unitId"), QStringLiteral("red_r1")},
         {QStringLiteral("speed"), 75.0}});

    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(result.code, QString::fromLatin1(CommandCode::Ok));
    EXPECT_EQ(result.appliedAtTick, 10);
}

namespace {

QByteArray deterministicRunHash() {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    EXPECT_TRUE(engine.command(
        QString::fromLatin1(CommandAction::MoveTo),
        {{QStringLiteral("unitId"), QStringLiteral("red_g1")},
         {QStringLiteral("pos"), QVariantMap{{QStringLiteral("x"), 4500.0},
                                              {QStringLiteral("y"), 3500.0}}}}).accepted);
    EXPECT_TRUE(engine.command(
        QString::fromLatin1(CommandAction::MoveTo),
        {{QStringLiteral("unitId"), QStringLiteral("blue_g1")},
         {QStringLiteral("pos"), QVariantMap{{QStringLiteral("x"), 15500.0},
                                              {QStringLiteral("y"), 11500.0}}}}).accepted);
    for (int tick = 0; tick < 200; ++tick) engine.stepOnce(0.05);

    const QByteArray bytes = QJsonDocument(SnapshotCodec::encodeRuntime(engine))
                                 .toJson(QJsonDocument::Compact);
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

} // namespace

TEST(DeterministicReplayTest, OneHundredRunsProduceIdenticalRuntimeHash) {
    const QByteArray expected = deterministicRunHash();
    ASSERT_FALSE(expected.isEmpty());
    for (int run = 1; run < 100; ++run) {
        EXPECT_EQ(deterministicRunHash(), expected) << "run=" << run;
    }
}
