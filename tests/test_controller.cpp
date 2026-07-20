#include <gtest/gtest.h>

#include "view/SimulationController.h"

#include <QJsonArray>
#include <QStandardPaths>

using namespace gbr;

TEST(SimulationControllerTest, UnitsJsonUsesCanonicalScenarioShape) {
    SimulationController controller;

    const QJsonObject json = controller.unitsJson();

    EXPECT_TRUE(json.value("map").isObject());
    EXPECT_TRUE(json.value("units").isArray());
    EXPECT_TRUE(json.contains("notes"));
}

TEST(SimulationControllerTest, ReplaceUnitsRejectsInvalidInputAtomically) {
    SimulationController controller;
    const QStringList beforeIds = controller.engine()->unitIds();
    QVariantList replacement = controller.unitsJson().value("units").toArray().toVariantList();
    ASSERT_FALSE(replacement.isEmpty());

    QVariantMap invalid = replacement.front().toMap();
    invalid["maxHp"] = 0.0;
    replacement.front() = invalid;

    EXPECT_FALSE(controller.replaceUnits(replacement));
    EXPECT_EQ(controller.engine()->unitIds(), beforeIds);
    EXPECT_TRUE(controller.readyForSim());
}

TEST(SimulationControllerTest, ShortcutSettingIsReadableWhenChangeIsSignaled) {
    QStandardPaths::setTestModeEnabled(true);
    SimulationController controller;
    QString valueObservedByQml;
    QObject::connect(&controller, &SimulationController::shortcutsChanged,
                     &controller, [&controller, &valueObservedByQml]() {
                         valueObservedByQml = controller.loadSetting(
                             QStringLiteral("shortcuts/testImmediateRead")).toString();
                     });

    controller.saveSetting(QStringLiteral("shortcuts/testImmediateRead"),
                           QStringLiteral("Ctrl+Alt+9"));

    EXPECT_EQ(valueObservedByQml, QStringLiteral("Ctrl+Alt+9"));
}
