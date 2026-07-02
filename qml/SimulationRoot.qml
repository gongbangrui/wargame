import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "components"
import "views"

Item {
    id: root
    anchors.fill: parent

    QtObject {
        id: theme
        property color bg:          "#0e1116"
        property color panel:       "#1a1f2b"
        property color panelAlt:    "#222838"
        property color border:      "#3a455a"
        property color borderSoft:  "#2a3142"
        property color text:        "#f3f6fb"
        property color textStrong:  "#ffffff"
        property color textDim:     "#d4dbe6"
        property color textMuted:   "#b0b8c4"
        property color accent:      "#4f9dff"
        property color accentSoft:  "#2a4f86"
        property color red:         "#ff5566"
        property color blue:        "#4d9bff"
        property color success:     "#46d29a"
        property color warning:     "#ffb24d"
        property color danger:      "#ff4d6d"
    }

    Rectangle {
        id: topBar
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        height: 52
        color: theme.panel
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: theme.border }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16; anchors.rightMargin: 16; anchors.topMargin: 8; anchors.bottomMargin: 8
            spacing: 14

            Text {
                text: "兵棋推演"
                color: theme.textStrong; font.pixelSize: 16; font.bold: true
                Layout.rightMargin: 8; renderType: Text.NativeRendering
            }
            Rectangle { width: 1; height: 24; color: theme.border; Layout.alignment: Qt.AlignVCenter }

            Text { text: "视角"; color: theme.textDim; font.pixelSize: 12 }
            ComboBox {
                id: viewCombo
                Layout.preferredWidth: 160
                model: controller.viewModeOptions()
                currentIndex: Math.max(0, model.indexOf(controller.viewMode))
                onActivated: function(idx) { controller.viewMode = model[idx] }
            }

            Rectangle { width: 1; height: 24; color: theme.border; Layout.alignment: Qt.AlignVCenter }

            Text { text: "时间"; color: theme.textDim; font.pixelSize: 12 }
            Text {
                text: controller.simTime.toFixed(1) + " s"
                color: theme.textStrong
                font.family: "Consolas"; font.pixelSize: 14
                Layout.preferredWidth: 80
                renderType: Text.NativeRendering
                Connections {
                    target: controller
                    function onSimTimeForward() { parent.text = controller.simTime.toFixed(1) + " s" }
                }
            }

            Rectangle { width: 1; height: 24; color: theme.border; Layout.alignment: Qt.AlignVCenter }

            Row {
                spacing: 8
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    id: runSwitch
                    width: 44; height: 24; radius: 12
                    color: controller.running ? theme.success : "#3b4252"
                    border.color: controller.running ? theme.success : theme.border
                    Rectangle {
                        width: 20; height: 20; radius: 10
                        color: "#fff"
                        x: controller.running ? 22 : 2
                        anchors.verticalCenter: parent.verticalCenter
                        Behavior on x { NumberAnimation { duration: 120 } }
                    }
                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: controller.setRunning(!controller.running)
                    }
                }
                Text {
                    text: controller.running ? "运行中" : "已暂停"
                    color: controller.running ? theme.success : theme.textDim
                    font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter
                    renderType: Text.NativeRendering
                }
            }

            Text { text: "速率"; color: theme.textDim; font.pixelSize: 12 }
            ComboBox {
                id: speedCombo
                Layout.preferredWidth: 90
                model: ["暂停", "1x", "2x", "4x", "8x"]
                currentIndex: 1
                onActivated: function(idx) { controller.setSpeed([0,1,2,4,8][idx]) }
            }
            GhostButton {
                text: "单步"; onClicked: controller.stepOnce()
            }

            Item { Layout.fillWidth: true }

            GhostButton {
                text: "加载默认"; onClicked: controller.loadDefault()
            }
        }
    }

    Loader {
        id: pageLoader
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: topBar.bottom; anchors.bottom: parent.bottom
        sourceComponent: {
            switch (controller.viewMode) {
            case "editor": return editorPage
            case "commandpost-red":
            case "commandpost-blue": return commandPostPage
            case "director": return directorPage
            }
            return commandPostPage
        }
    }

    Component { id: editorPage; ScenarioEditorView {} }
    Component { id: commandPostPage; CommandPostView {} }
    Component { id: directorPage; DirectorView {} }
}
