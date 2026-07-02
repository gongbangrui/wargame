import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    color: "transparent"

    property var snap: ({})
    property string sideFilter: "all"

    QtObject {
        id: t
        property color text: "#f3f6fb"
        property color textDim: "#d4dbe6"
        property color muted: "#b0b8c4"
        property color accent: "#4f9dff"
        property color red: "#ff5566"
        property color blue: "#4d9bff"
    }

    Connections {
        target: controller
        function onUnitsForward() { root.snap = controller.unitAt(root.snap.id || "") }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Text {
            text: root.snap.callsign || "—"
            color: root.snap.side === "red" ? t.red : (root.snap.side === "blue" ? t.blue : t.text)
            font.pixelSize: 18
            font.bold: true
            renderType: Text.NativeRendering
        }
        Text {
            text: (root.snap.kind || "-") + " · " + (root.snap.side || "-")
            color: t.muted
            font.pixelSize: 11
            renderType: Text.NativeRendering
        }

        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 4
            Layout.fillWidth: true

            Repeater {
                model: [
                    { k: "HP",   v: (root.snap.hp!=null?Math.round(root.snap.hp):"-") + " / " + (root.snap.maxHp!=null?Math.round(root.snap.maxHp):"-") },
                    { k: "探测", v: (root.snap.detectRange!=null?Math.round(root.snap.detectRange):0) + " m" },
                    { k: "攻击", v: (root.snap.attackRange!=null?Math.round(root.snap.attackRange):0) + " m" },
                    { k: "通信", v: (root.snap.commRange!=null?Math.round(root.snap.commRange):0) + " m" },
                    { k: "速度", v: (root.snap.speed!=null?Math.round(root.snap.speed):0) + " m/s" },
                    { k: "位置", v: root.snap.position ? Math.round(root.snap.position[0]) + ", " + Math.round(root.snap.position[1]) : "-" }
                ]
                delegate: RowLayout {
                    spacing: 8
                    Text { text: modelData.k; color: t.muted; font.pixelSize: 11; Layout.preferredWidth: 36 }
                    Text { text: modelData.v; color: t.text; font.pixelSize: 12; font.family: "Consolas" }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: "#2a3142" }

        Text { text: "状态"; color: t.muted; font.pixelSize: 11 }
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 28; radius: 4
            color: "#222838"; border.color: "#2a3142"
            Text {
                anchors.fill: parent; anchors.margins: 8
                text: root.snap.status || "-"
                color: t.text
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                renderType: Text.NativeRendering
            }
        }

        Text { text: "侦察到的目标"; color: t.muted; font.pixelSize: 11 }
        ListView {
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            model: root.snap.detections || []
            delegate: Rectangle {
                width: ListView.view.width
                implicitHeight: 30
                color: index % 2 === 0 ? "#171c27" : "#1d2330"
                border.color: "#2a3142"
                Row {
                    anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                    spacing: 8
                    Rectangle { width: 8; height: 8; radius: 4; anchors.verticalCenter: parent.verticalCenter
                        color: modelData.side === "red" ? t.red : t.blue }
                    Text { text: modelData.callsign + " (" + modelData.kind + ")"; color: t.text; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                    Text { text: Math.round(modelData.distance) + " m"; color: t.muted; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter; x: parent.width - 60 }
                }
            }
        }
    }
}
