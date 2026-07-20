pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"

Item {
    id: root
    property var controller: null
    property var editor: null

    QtObject {
        id: t
        property color panel: "#0e1322"; property color panelAlt: "#131a2c"
        property color border: "#1e2d4a"; property color borderSoft: "#17213a"
        property color text: "#e8edf5"; property color textStrong: "#ffffff"
        property color textDim: "#bcc8de"; property color muted: "#8896b8"
        property color dimmer: "#5a6a88"
        property color accent: "#4090ff"; property color success: "#36c98a"
        property color warning: "#f0a040"; property color danger: "#f04760"
        property color red: "#f04760"; property color blue: "#4090ff"
        property color rowA: "#0c1120"; property color rowB: "#111728"
    }

    property var units: root.controller.allUnits()
    property bool bothReady: root.controller.redReady && root.controller.blueReady
    Component.onCompleted: {
        canvas.zoom = 0.1
        canvas.centerOn(canvas.mapSize.w / 2, canvas.mapSize.h / 2)
    }
    Connections {
        target: root.controller
        function onUnitsForward() { root.units = root.controller.allUnits() }
        function onTargetDestroyedVisual(unitId, x, y) {
            canvas.refresh()
            canvas.flashDestroyAt(x, y)
        }
    }

    function allRoutes() {
        var rs = []
        var all = root.units
        for (var i = 0; i < all.length; i++) {
            var u = all[i]
            if (!u.schedule || u.schedule.length < 1) continue
            rs.push({ color: u.side === "red" ? t.red : t.blue, pendingColor: t.dimmer, points: u.schedule })
        }
        return rs
    }

    RowLayout {
        anchors.fill: parent; spacing: 0
        Item {
            Layout.fillHeight: true; Layout.fillWidth: true
            MapCanvas { controller: root.controller; editor: root.editor;
                id: canvas
                anchors.fill: parent; anchors.margins: 12
                sideFilter: "all"; focusUnitId: ""; showAllSides: true
                simTime: root.controller.simTime
                showRoutes: routeToggle.checked
                showDetectRange: detectToggle.checked
                showAttackRange: attackToggle.checked
                showRecentPaths: recentPathToggle.checked
                routes: root.allRoutes()
                onUnitClicked: function(uid, btn) {
                    root.controller.setFocusedUnitId(uid)
                    var u = root.controller.unitAt(uid)
                    if (u && u.position) canvas.centerOn(u.position[0], u.position[1])
                }
            }

            Rectangle {
                anchors.right: parent.right; anchors.top: parent.top
                anchors.rightMargin: 24; anchors.topMargin: 24
                color: t.panel; border.color: t.border; radius: 6
                implicitHeight: 34; z: 10
                width: Math.min(canvas.width * 0.45, 370)
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
                    spacing: 8
                    Text { text: "路径"; color: t.textDim; font.pixelSize: 11; renderType: Text.NativeRendering }
                    Switch {
                        id: routeToggle; checked: true
                        Layout.preferredWidth: 36
                        indicator: Rectangle {
                            implicitWidth: 34; implicitHeight: 18; radius: 9
                            color: routeToggle.checked ? t.success : "#3b4252"
                            border.color: routeToggle.checked ? t.success : t.border
                            Rectangle { x: routeToggle.checked ? 18 : 2; y: 2; width: 14; height: 14; radius: 7; color: "#fff"
                                Behavior on x { NumberAnimation { duration: 120 } }
                            }
                        }
                    }
                    Text { text: "侦察"; color: t.textDim; font.pixelSize: 11; renderType: Text.NativeRendering }
                    Switch {
                        id: detectToggle; checked: true
                        Layout.preferredWidth: 36
                        indicator: Rectangle {
                            implicitWidth: 34; implicitHeight: 18; radius: 9
                            color: detectToggle.checked ? t.accent : "#3b4252"
                            border.color: detectToggle.checked ? t.accent : t.border
                            Rectangle { x: detectToggle.checked ? 18 : 2; y: 2; width: 14; height: 14; radius: 7; color: "#fff"
                                Behavior on x { NumberAnimation { duration: 120 } }
                            }
                        }
                    }
                    Text { text: "攻击"; color: t.textDim; font.pixelSize: 11; renderType: Text.NativeRendering }
                    Switch {
                        id: attackToggle; checked: true
                        Layout.preferredWidth: 36
                        indicator: Rectangle {
                            implicitWidth: 34; implicitHeight: 18; radius: 9
                            color: attackToggle.checked ? t.danger : "#3b4252"
                            border.color: attackToggle.checked ? t.danger : t.border
                            Rectangle { x: attackToggle.checked ? 18 : 2; y: 2; width: 14; height: 14; radius: 7; color: "#fff"
                                Behavior on x { NumberAnimation { duration: 120 } }
                            }
                        }
                    }
                    Text { text: "轨迹"; color: t.textDim; font.pixelSize: 11; renderType: Text.NativeRendering }
                    Switch {
                        id: recentPathToggle; checked: true
                        Layout.preferredWidth: 36
                        indicator: Rectangle {
                            implicitWidth: 34; implicitHeight: 18; radius: 9
                            color: recentPathToggle.checked ? t.warning : "#3b4252"
                            border.color: recentPathToggle.checked ? t.warning : t.border
                            Rectangle { x: recentPathToggle.checked ? 18 : 2; y: 2; width: 14; height: 14; radius: 7; color: "#fff"
                                Behavior on x { NumberAnimation { duration: 120 } }
                            }
                        }
                    }
                }
            }
        }
        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: Math.min(480, Math.max(320, root.width * 0.38))
            color: t.panel
            Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 1; color: t.border }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 16; spacing: 12
                ColumnLayout { spacing: 2
                    Text { text: "导演视角"; color: t.textStrong; font.pixelSize: 18; font.bold: true; renderType: Text.NativeRendering }
                    Text { text: "全局态势 · 路径（实线=已完成，虚线=未完成）"; color: t.muted; font.pixelSize: 11; renderType: Text.NativeRendering }
                }

                Rectangle {
                    visible: root.controller.networked
                    Layout.fillWidth: true; Layout.preferredHeight: 104
                    color: t.panelAlt; border.color: root.bothReady ? t.success : t.border; radius: 6
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 10; spacing: 7
                        RowLayout {
                            Layout.fillWidth: true
                            Text { text: "红方"; color: t.red; font.bold: true; font.pixelSize: 12 }
                            Text { text: root.controller.redReady ? "已就绪" : "未就绪"; color: root.controller.redReady ? t.success : t.muted; font.pixelSize: 11 }
                            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 14; color: t.border }
                            Text { text: "蓝方"; color: t.blue; font.bold: true; font.pixelSize: 12 }
                            Text { text: root.controller.blueReady ? "已就绪" : "未就绪"; color: root.controller.blueReady ? t.success : t.muted; font.pixelSize: 11 }
                            Item { Layout.fillWidth: true }
                            Text { text: root.controller.matchPhase === "preparing" ? "准备阶段" : root.controller.matchPhase === "running" ? (root.controller.running ? "推演进行中" : "推演已暂停") : "推演已结束"; color: t.textDim; font.pixelSize: 11 }
                        }
                        RowLayout {
                            Layout.fillWidth: true; spacing: 8
                            TonalButton {
                                visible: root.controller.matchPhase === "preparing"
                                text: "开启推演"; base: t.success
                                enabled: root.bothReady && root.controller.readyForSim
                                onClicked: root.controller.setRunning(true)
                            }
                            TonalButton {
                                visible: root.controller.matchPhase === "running"
                                text: root.controller.running ? "暂停" : "继续"
                                base: root.controller.running ? t.warning : t.success
                                onClicked: root.controller.setRunning(!root.controller.running)
                            }
                            TonalButton {
                                visible: root.controller.matchPhase !== "preparing"
                                text: "结束并重置"; base: t.danger
                                onClicked: root.controller.endMatch()
                            }
                            Item { Layout.fillWidth: true }
                        }
                    }
                }

                SectionTitle { text: "双方态势" }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: t.panelAlt; border.color: t.border; radius: 6
                    ListView {
                        anchors.fill: parent; anchors.margins: 6
                        clip: true; spacing: 4; model: root.units
                        delegate: Rectangle {
                            id: unitStatusRow
                            required property int index
                            required property var modelData
                            width: ListView.view.width; implicitHeight: 56
                            color: unitStatusRow.index % 2 === 0 ? t.rowA : t.rowB
                            border.color: t.borderSoft; radius: 4
                            Row {
                                anchors.fill: parent; anchors.margins: 10; spacing: 8
                                Rectangle { width: 10; height: 10; radius: 5; anchors.verticalCenter: parent.verticalCenter
                                    color: unitStatusRow.modelData.side === "red" ? t.red : t.blue }
                                Column {
                                    anchors.verticalCenter: parent.verticalCenter; spacing: 2
                                    Text {
                                        text: unitStatusRow.modelData.callsign + "  [" + unitStatusRow.modelData.kind + "]"
                                        color: t.text; font.pixelSize: 12; font.bold: true; renderType: Text.NativeRendering
                                    }
                                    Text {
                                        text: "HP " + Math.round(unitStatusRow.modelData.hp) + " / " + Math.round(unitStatusRow.modelData.maxHp) + "  · " + (unitStatusRow.modelData.status || "-")
                                        color: t.textDim; font.pixelSize: 11; renderType: Text.NativeRendering
                                    }
                                }
                                Item { width: 6 }
                                Rectangle {
                                    width: 28; height: 22; radius: 11
                                    color: unitStatusRow.modelData.alive ? t.success : t.danger
                                    anchors.verticalCenter: parent.verticalCenter
                                    Text {
                                        anchors.centerIn: parent
                                        text: unitStatusRow.modelData.alive ? "活" : "毁"
                                        color: "#0e1116"; font.bold: true; font.pixelSize: 11; renderType: Text.NativeRendering
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
                    MessageLog { anchors.fill: parent; anchors.margins: 1; messages: root.controller.messages }
                }
            }
        }
    }
}
