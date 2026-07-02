import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"

Item {
    id: root

    QtObject {
        id: t
        property color panel: "#1a1f2b"; property color panelAlt: "#222838"
        property color border: "#3a455a"; property color text: "#f3f6fb"
        property color textDim: "#d4dbe6"; property color muted: "#b0b8c4"
        property color accent: "#4f9dff"; property color success: "#46d29a"
        property color danger: "#ff4d6d"; property color red: "#ff5566"; property color blue: "#4d9bff"
    }

    property var units: controller.allUnits()
    Connections {
        target: controller.engine
        function onUnitsChanged() { root.units = controller.allUnits() }
    }

    function allRoutes() {
        var rs = []
        var all = controller.allUnits()
        for (var i = 0; i < all.length; i++) {
            var u = all[i]
            if (!u.schedule || u.schedule.length < 1) continue
            rs.push({ color: u.side === "red" ? "#ff5566" : "#4d9bff", pendingColor: "#8a93a6", points: u.schedule })
        }
        return rs
    }

    RowLayout {
        anchors.fill: parent; spacing: 0
        Item {
            Layout.fillHeight: true; Layout.fillWidth: true
            MapCanvas {
                id: canvas
                anchors.fill: parent; anchors.margins: 12
                sideFilter: "all"
                focusUnitId: ""
                showAllSides: true
                simTime: controller.simTime
                showRoutes: routeToggle.checked
                showDetectRange: detectToggle.checked
                showAttackRange: attackToggle.checked
                
                routes: allRoutes()
                onUnitClicked: function(uid, btn) { controller.setFocusedUnitId(uid) }
            }

            Rectangle {
                anchors.right: parent.right; anchors.top: parent.top
                anchors.rightMargin: 24; anchors.topMargin: 24
                color: "#1a1f2b"; border.color: "#3a455a"; radius: 6
                implicitHeight: 32; implicitWidth: 370; z: 10
                RowLayout {
                    anchors.centerIn: parent; spacing: 6
                    // 红方开关
                    Switch { id: redToggle; checked: true }
                    Text { text: "红方"; color: redToggle.checked ? t.red : t.muted; font.pixelSize: 10; renderType: Text.NativeRendering }
                    // 蓝方开关
                    Switch { id: blueToggle; checked: true }
                    Text { text: "蓝方"; color: blueToggle.checked ? t.blue : t.muted; font.pixelSize: 10; renderType: Text.NativeRendering }
                    // 路径
                    Switch { id: routeToggle; checked: true }
                    Text { text: "路径"; color: routeToggle.checked ? t.success : t.muted; font.pixelSize: 10; renderType: Text.NativeRendering }
                    // 侦察范围
                    Switch { id: detectToggle; checked: true }
                    Text { text: "侦察"; color: detectToggle.checked ? t.success : t.muted; font.pixelSize: 10; renderType: Text.NativeRendering }
                    // 攻击范围
                    Switch { id: attackToggle; checked: true }
                    Text { text: "攻击"; color: attackToggle.checked ? t.success : t.muted; font.pixelSize: 10; renderType: Text.NativeRendering }
                }
            }
        }
        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: 480
            color: t.panel
            Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 1; color: t.border }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 16; spacing: 14
                Text { text: "导演视角"; color: t.text; font.pixelSize: 18; font.bold: true; renderType: Text.NativeRendering }
                Text { text: "可见双方全部单元、范围与消息流 · 路径(实线=已完成,虚线=未完成)"; color: t.muted; font.pixelSize: 10; renderType: Text.NativeRendering }

                SectionTitle { text: "双方态势" }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: t.panelAlt; border.color: t.border; radius: 6
                    ListView {
                        anchors.fill: parent; anchors.margins: 6
                        clip: true; spacing: 4; model: root.units
                        delegate: Rectangle {
                            width: ListView.view.width; implicitHeight: 56
                            color: index % 2 === 0 ? "#171c27" : "#1d2330"
                            border.color: "#2a3142"; radius: 4
                            Row {
                                anchors.fill: parent; anchors.margins: 10; spacing: 6
                                Rectangle { width: 10; height: 10; radius: 5; anchors.verticalCenter: parent.verticalCenter
                                    color: modelData.side === "red" ? t.red : t.blue }
                                Column {
                                    anchors.verticalCenter: parent.verticalCenter; spacing: 2
                                    Text {
                                        text: modelData.callsign + "  [" + modelData.kind + "]"
                                        color: t.text; font.pixelSize: 12; font.bold: true; renderType: Text.NativeRendering
                                    }
                                    Text {
                                        text: "HP " + Math.round(modelData.hp) + " / " + Math.round(modelData.maxHp) + "  · " + (modelData.status || "-")
                                        color: t.textDim; font.pixelSize: 10; renderType: Text.NativeRendering
                                    }
                                }
                                Item { width: 6 }
                                Rectangle {
                                    width: 28; height: 22; radius: 11
                                    color: modelData.alive ? t.success : t.danger
                                    anchors.verticalCenter: parent.verticalCenter
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.alive ? "活" : "毁"
                                        color: "#0e1116"; font.bold: true; font.pixelSize: 10; renderType: Text.NativeRendering
                                    }
                                }
                            }
                        }
                    }
                }

                SectionTitle { text: "全局消息流" }
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 220
                    color: t.panelAlt; border.color: t.border; radius: 6
                    MessageLog { anchors.fill: parent; anchors.margins: 1; messages: controller.messages }
                }
            }
        }
    }
}
