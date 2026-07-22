pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    property var controller: null
    property var editor: null
    color: "transparent"

    property var snap: ({})
    property bool interactionEnabled: true

    QtObject {
        id: t
        property color text: "#f3f6fb"
        property color textStrong: "#ffffff"
        property color textDim: "#d4dbe6"
        property color muted: "#9099a8"
        property color accent: "#4f9dff"
        property color accentSoft: "#2a4f86"
        property color red: "#ff5566"
        property color blue: "#4d9bff"
        property color danger: "#ff4d6d"
        property color success: "#46d29a"
        property color warning: "#ffb24d"
    }

    Flickable {
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: panelContent.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
        id: panelContent
        width: parent.width - (parent.contentHeight > parent.height ? 8 : 0)
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    text: root.snap.callsign || "\u2014"
                    color: root.snap.alive ? (root.snap.side === "red" ? t.red : (root.snap.side === "blue" ? t.blue : t.textStrong)) : t.muted
                    font.pixelSize: 20
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.maximumWidth: 180
                    renderType: Text.NativeRendering
                }
                Text {
                    text: {
                        var k = root.snap.kind || ""
                        var kinds = { "commandpost": "\u6307\u6325\u6240", "reconuav": "\u4fa6\u5bdf\u65e0\u4eba\u673a", "attackuav": "\u653b\u51fb\u65e0\u4eba\u673a", "groundscout": "\u5730\u9762\u5206\u961f", "jammeruav": "\u7535\u5b50\u5e72\u6270" }
                        var label = kinds[k] || k
                        return label + " \u00b7 " + (root.snap.side === "red" ? "\u7ea2\u65b9" : (root.snap.side === "blue" ? "\u84dd\u65b9" : root.snap.side || "-"))
                    }
                    color: t.muted
                    font.pixelSize: 11
                    renderType: Text.NativeRendering
                }
            }
            GhostButton {
                text: "\u64a4\u79bb"
                visible: root.interactionEnabled && root.snap.alive && root.snap.movable === true
                implicitHeight: 24
                onClicked: root.controller.command("withdraw", { unitId: root.snap.id })
            }
            GhostButton {
                text: "补给"
                visible: root.interactionEnabled && root.snap.alive && root.snap.movable === true
                implicitHeight: 24
                onClicked: root.controller.command("service", { unitId: root.snap.id })
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#2a3142" }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            visible: root.snap.hp !== undefined
            Text {
                text: "\u751f\u547d\u503c"
                color: t.muted
                font.pixelSize: 11
                renderType: Text.NativeRendering
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 10
                radius: 5
                color: "#1a2030"
                Rectangle {
                    anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                    width: {
                        var ratio = Math.max(0, Math.min(1, (root.snap.hp || 0) / Math.max(1, root.snap.maxHp || 100)))
                        return parent.width * ratio
                    }
                    radius: 5
                    color: {
                        var ratio = Math.max(0, Math.min(1, (root.snap.hp || 0) / Math.max(1, root.snap.maxHp || 100)))
                        return ratio > 0.5 ? t.success : (ratio > 0.25 ? t.warning : t.danger)
                    }
                    Behavior on width { NumberAnimation { duration: 250 } }
                }
            }
            Text {
                text: Math.round(root.snap.hp || 0) + " / " + Math.round(root.snap.maxHp || 0)
                color: t.textDim
                font.pixelSize: 11
                font.family: "Consolas"
                renderType: Text.NativeRendering
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.snap.alive && root.snap.subsystems !== undefined
            spacing: 8
            Text { text: "系统"; color: t.muted; font.pixelSize: 11 }
            Repeater {
                model: [
                    { label: "传感", value: root.snap.subsystems ? root.snap.subsystems.sensor : 1 },
                    { label: "通信", value: root.snap.subsystems ? root.snap.subsystems.comms : 1 },
                    { label: "机动", value: root.snap.subsystems ? root.snap.subsystems.mobility : 1 },
                    { label: "武器", value: root.snap.subsystems ? root.snap.subsystems.weapon : 1 }
                ]
                delegate: RowLayout {
                    id: subsystemItem
                    required property var modelData
                    spacing: 3
                    Text { text: subsystemItem.modelData.label; color: t.muted; font.pixelSize: 9 }
                    Rectangle {
                        Layout.preferredWidth: 28; Layout.preferredHeight: 5; radius: 2
                        color: "#252d3a"
                        Rectangle {
                            width: parent.width * Math.max(0, Math.min(1, subsystemItem.modelData.value))
                            height: parent.height; radius: 2
                            color: subsystemItem.modelData.value > 0.6 ? t.success
                                  : subsystemItem.modelData.value > 0.25 ? t.warning : t.danger
                        }
                    }
                }
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            visible: root.interactionEnabled && root.snap.alive && root.snap.movable === true
            Text { text: "\u901f\u5ea6"; color: t.muted; font.pixelSize: 11; renderType: Text.NativeRendering }
            Rectangle {
                Layout.preferredWidth: 104; implicitHeight: 26; radius: 4
                color: "#141b24"; border.color: "#2a3142"
                RowLayout {
                    anchors.fill: parent; spacing: 0
                    TonalButton {
                        text: "\u2212"; base: "#252d3a"; radius: 0
                        implicitWidth: 26; implicitHeight: 26
                        onClicked: {
                            var v = Math.round(root.snap.speed || 0) - 5
                            root.controller.command("setSpeed", { unitId: root.snap.id, speed: Math.max(1, v) })
                        }
                    }
                    TextInput {
                        id: speedInput
                        Layout.fillWidth: true; Layout.alignment: Qt.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                        color: t.textDim; font.pixelSize: 12; font.family: "Consolas"
                        text: Math.round(root.snap.speed || 0)
                        onEditingFinished: {
                            var v = parseInt(text) || 1
                            root.controller.command("setSpeed", { unitId: root.snap.id, speed: Math.max(1, Math.min(400, v)) })
                        }
                    }
                    TonalButton {
                        text: "+"; base: "#252d3a"; radius: 0
                        implicitWidth: 26; implicitHeight: 26
                        onClicked: {
                            var v = Math.round(root.snap.speed || 0) + 5
                            root.controller.command("setSpeed", { unitId: root.snap.id, speed: Math.min(400, v) })
                        }
                    }
                }
            }
            Text { text: "m/s"; color: t.muted; font.pixelSize: 11; renderType: Text.NativeRendering }
            Item { Layout.fillWidth: true }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#2a3142" }

        GridLayout {
            columns: 2
            columnSpacing: 10
            rowSpacing: 3
            Layout.fillWidth: true
            Repeater {
                model: {
                    var rows = [
                        { k: "\u63a2\u6d4b", v: (root.snap.detectRange !== null ? Math.round(root.snap.detectRange) : 0) + " m", c: "#62b4ff" },
                        { k: "\u901a\u4fe1", v: (root.snap.commRange !== null ? Math.round(root.snap.commRange) : 0) + " m", c: "#8593a8" }
                    ]
                    if (root.snap.kind === "attackuav") {
                        rows.push({ k: "\u653b\u51fb", v: (root.snap.attackRange !== null ? Math.round(root.snap.attackRange) : 0) + " m", c: "#ff6b4a" })
                        rows.push({ k: "\u4f24\u5bb3", v: (root.snap.attackPower !== undefined && root.snap.attackPower !== null ? Math.round(root.snap.attackPower) : 0) + " HP", c: "#ff8a40" })
                        rows.push({ k: "\u5f39\u836f", v: (root.snap.ammoRemaining !== undefined ? root.snap.ammoRemaining : 0) + " / " + (root.snap.ammoCapacity !== undefined ? root.snap.ammoCapacity : 0), c: "#f4c95d" })
                        rows.push({ k: "燃油", v: Math.round(root.snap.fuelRemaining || 0) + " / " + Math.round(root.snap.fuelCapacity || 0) + " s", c: "#5fd1c8" })
                        rows.push({ k: "\u51b7\u5374", v: Number(root.snap.cooldownRemaining || 0).toFixed(1) + " s", c: "#c08cff" })
                        var shotLabels = { "hit": "\u547d\u4e2d", "miss": "\u672a\u547d\u4e2d", "out_of_range": "\u8d85\u51fa\u5c04\u7a0b" }
                        rows.push({ k: "\u4e0a\u53d1", v: shotLabels[root.snap.lastShotOutcome] || "-", c: "#46d29a" })
                    }
                    rows.push({ k: "\u4f4d\u7f6e", v: root.snap.position ? Math.round(root.snap.position[0]) + ", " + Math.round(root.snap.position[1]) : "-", c: t.muted })
                    rows.push({ k: "装甲", v: Math.round((root.snap.armor || 0) * 100) + "%", c: "#a9b7c9" })
                    rows.push({ k: "ID", v: root.snap.id || "-", c: t.muted })
                    return rows
                }
                delegate: RowLayout {
                    id: statRow
                    required property var modelData
                    spacing: 6
                    Rectangle { Layout.preferredWidth: 6; Layout.preferredHeight: 6; radius: 3; color: statRow.modelData.c; Layout.alignment: Qt.AlignVCenter }
                    Text { text: statRow.modelData.k; color: t.muted; font.pixelSize: 11; Layout.preferredWidth: 28; renderType: Text.NativeRendering }
                    Text { text: statRow.modelData.v; color: t.textDim; font.pixelSize: 12; font.family: "Consolas"; renderType: Text.NativeRendering }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#2a3142" }

        RowLayout {
            Layout.fillWidth: true
            visible: root.interactionEnabled && root.snap.kind === "attackuav" && root.snap.alive
            spacing: 6
            Text { text: "交战规则"; color: t.muted; font.pixelSize: 11 }
            TonalButton {
                text: "自由交战"; implicitHeight: 24
                base: root.snap.rulesOfEngagement === "free" ? t.success : "#252d3a"
                onClicked: root.controller.command("setRoe", { unitId: root.snap.id, roe: "free" })
            }
            TonalButton {
                text: "武器管制"; implicitHeight: 24
                base: root.snap.rulesOfEngagement === "hold" ? t.warning : "#252d3a"
                onClicked: root.controller.command("setRoe", { unitId: root.snap.id, roe: "hold" })
            }
            GhostButton {
                text: "取消交战"; implicitHeight: 24
                enabled: !!root.snap.targetId
                onClicked: root.controller.command("cancelEngagement", { unitId: root.snap.id })
            }
            Item { Layout.fillWidth: true }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#2a3142"; visible: root.snap.kind === "attackuav" }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            Text { text: "\u72b6\u6001"; color: t.muted; font.pixelSize: 11; renderType: Text.NativeRendering }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 32
                radius: 4
                color: {
                    var s = root.snap.status || ""
                    if (!root.snap.alive) return "#3a1520"
                    if (s.indexOf("攻击") >= 0 || s.indexOf("追击") >= 0) return "#3a2015"
                    if (s.indexOf("摧毁") >= 0) return "#3a2015"
                    if (s.indexOf("引导") >= 0 || s.indexOf("机动") >= 0) return "#1a2a3a"
                    if (s.indexOf("撤离") >= 0) return "#2a1a20"
                    return "#1a2030"
                }
                border.color: {
                    var s = root.snap.status || ""
                    if (!root.snap.alive) return "#ff4d6d"
                    if (s.indexOf("攻击") >= 0 || s.indexOf("追击") >= 0) return "#ff6b4a"
                    if (s.indexOf("摧毁") >= 0) return "#ff4d6d"
                    if (s.indexOf("引导") >= 0 || s.indexOf("机动") >= 0) return "#4f9dff"
                    if (s.indexOf("撤离") >= 0) return "#ff5566"
                    return "#3a455a"
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 6
                    Rectangle {
                        Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                        Layout.alignment: Qt.AlignVCenter
                        color: {
                            var s = root.snap.status || ""
                            if (!root.snap.alive) return t.danger
                            if (s.indexOf("已毁") >= 0 || s.indexOf("摧毁") >= 0) return t.danger
                            if (s.indexOf("攻击") >= 0 || s.indexOf("追击") >= 0) return t.warning
                            if (s.indexOf("引导") >= 0 || s.indexOf("机动") >= 0 || s.indexOf("巡航") >= 0) return t.accent
                            if (s.indexOf("撤离") >= 0) return t.red
                            if (s.indexOf("到达") >= 0 || s.indexOf("待命") >= 0) return t.success
                            return t.muted
                        }
                    }
                    Text {
                        text: root.snap.status || "-"
                        color: {
                            if (!root.snap.alive) return t.danger
                            var s = root.snap.status || ""
                            if (s.indexOf("已毁") >= 0) return t.danger
                            if (s.indexOf("攻击") >= 0 || s.indexOf("追击") >= 0) return t.warning
                            if (s.indexOf("引导") >= 0 || s.indexOf("机动") >= 0 || s.indexOf("巡航") >= 0) return t.accent
                            if (s.indexOf("撤离") >= 0) return t.red
                            return t.textDim
                        }
                        font.pixelSize: 12
                        font.bold: true
                        Layout.fillWidth: true
                        renderType: Text.NativeRendering
                    }
                }
            }
        }
        }
    }
}
