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

TEST(SimulationControllerTest, UpsertGeneratedUnitReturnsStableId) {
    SimulationController controller;
    QVariantMap unit{{QStringLiteral("id"), QString()},
                     {QStringLiteral("callsign"), QStringLiteral("定位测试单元")},
                     {QStringLiteral("kind"), QStringLiteral("groundscout")},
                     {QStringLiteral("side"), QStringLiteral("red")},
                     {QStringLiteral("x"), 4321.0}, {QStringLiteral("y"), 6789.0},
                     {QStringLiteral("alt"), 0.0}, {QStringLiteral("detectRange"), 3000.0},
                     {QStringLiteral("attackRange"), 0.0}, {QStringLiteral("commRange"), 10000.0},
                     {QStringLiteral("speed"), 6.0}, {QStringLiteral("maxHp"), 80.0},
                     {QStringLiteral("attackPower"), 0.0}};

    const QString id = controller.upsertUnit(unit);

    EXPECT_FALSE(id.isEmpty());
    const QJsonObject scenario = controller.unitsJson();
    bool found = false;
    for (const QJsonValue& value : scenario.value(QStringLiteral("units")).toArray()) {
        if (value.toObject().value(QStringLiteral("id")).toString() == id) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(SimulationControllerTest, CommandPostViewKeepsValidSelectedUnit) {
    SimulationController controller;
    controller.setViewMode(QStringLiteral("commandpost-red"));
    const QVariantList reconUnits = controller.unitOptions(QStringLiteral("reconuav"),
                                                            QStringLiteral("red"));
    ASSERT_FALSE(reconUnits.isEmpty());
    const QString reconId = reconUnits.front().toMap().value(QStringLiteral("id")).toString();

    controller.setFocusedUnitId(reconId);
    controller.loadDefault(); // Scenario refresh must not reset focus to the CP.

    EXPECT_EQ(controller.focusedUnitId(), reconId);
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
