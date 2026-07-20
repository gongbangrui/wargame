pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root
    property var controller: null
    property var editor: null
    color: "transparent"

    property var snap: ({})

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

    ColumnLayout {
        anchors.fill: parent
        spacing: 6
        clip: true

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
                visible: root.snap.alive && root.snap.movable === true
                implicitHeight: 24
                onClicked: root.controller.command("withdraw", { unitId: root.snap.id })
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
            spacing: 10
            visible: root.snap.alive && root.snap.movable === true
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
                    }
                    rows.push({ k: "\u4f4d\u7f6e", v: root.snap.position ? Math.round(root.snap.position[0]) + ", " + Math.round(root.snap.position[1]) : "-", c: t.muted })
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
