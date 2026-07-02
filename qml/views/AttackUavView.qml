import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"

Item {
    id: root
    property string side: controller.focusedSide
    property var snap: controller.unitAt(controller.focusedUnitId)

    QtObject {
        id: t
        property color panel: "#1a1f2b"; property color panelAlt: "#222838"
        property color border: "#3a455a"; property color text: "#f3f6fb"
        property color muted: "#b0b8c4"
        property color danger: "#ff4d6d"; property color accent: "#4f9dff"
        property color warning: "#ffb24d"
    }

    RowLayout {
        anchors.fill: parent; spacing: 0
        Item {
            Layout.fillHeight: true; Layout.fillWidth: true
            MapCanvas {
                anchors.fill: parent; anchors.margins: 12
                sideFilter: "all"; focusUnitId: controller.focusedUnitId
                showAllSides: false
                onClickedMap: function(lp) {
                    controller.command("moveTo", { unitId: controller.focusedUnitId, pos: lp })
                }
            }
        }
        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: 420
            color: t.panel
            Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 1; color: t.border }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 16; spacing: 14
                Text { text: "攻击无人机"; color: t.text; font.pixelSize: 18; font.bold: true; renderType: Text.NativeRendering }
                SectionTitle { text: "切换单元" }
                ComboBox {
                    Layout.fillWidth: true
                    model: controller.unitOptions("attackuav", root.side)
                    textRole: "callsign"; valueRole: "id"
                    onActivated: function(idx) { controller.setFocusedUnitId(model[idx].id) }
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 220
                    color: t.panelAlt; border.color: t.border; radius: 6
                    UnitPanel { anchors.fill: parent; anchors.margins: 12 }
                }

                SectionTitle { text: "目标与攻击控制" }
                GridLayout {
                    columns: 2; columnSpacing: 10; rowSpacing: 8
                    Layout.fillWidth: true
                    Text { text: "目标"; color: t.muted; font.pixelSize: 11 }
                    ComboBox {
                        id: targetCombo
                        Layout.fillWidth: true
                        model: controller.unitOptions("", root.side === "red" ? "blue" : "red")
                        textRole: "callsign"; valueRole: "id"
                    }
                }
                Flow {
                    Layout.fillWidth: true; spacing: 8
                    TonalButton {
                        text: "下达攻击命令"; base: t.accent
                        onClicked: {
                            if (targetCombo.currentIndex < 0) return
                            var tid = targetCombo.model[targetCombo.currentIndex].id
                            controller.command("assignTarget", { attackerId: controller.focusedUnitId, targetId: tid })
                            var s = controller.unitAt(tid)
                            controller.command("setFlightPlan", {
                                attackerId: controller.focusedUnitId,
                                waypoints: [{ x: s.position[0], y: s.position[1] }]
                            })
                        }
                    }
                    TonalButton {
                        text: "立即开火"; base: t.danger
                        onClicked: {
                            if (targetCombo.currentIndex < 0) return
                            var tid = targetCombo.model[targetCombo.currentIndex].id
                            controller.command("engageTarget", { attackerId: controller.focusedUnitId, targetId: tid })
                        }
                    }
                    GhostButton {
                        text: "撤离"
                        onClicked: controller.command("withdraw", { unitId: controller.focusedUnitId })
                    }
                }

                SectionTitle { text: "实时消息" }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: t.panelAlt; border.color: t.border; radius: 6
                    MessageLog { anchors.fill: parent; anchors.margins: 1 }
                }
            }
        }
    }
}

