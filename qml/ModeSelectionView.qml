import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "components"

Item {
    id: root

    signal localRequested()
    signal remoteRequested()

    function beginRemote() {
        remoteRequested()
    }

    Rectangle {
        anchors.fill: parent
        color: "#080b14"
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 760)
        spacing: 18

        Text {
            text: "兵棋推演"
            color: "#ffffff"
            font.pixelSize: 28
            font.bold: true
            Layout.alignment: Qt.AlignHCenter
            renderType: Text.NativeRendering
        }
        Text {
            text: "选择本次推演的运行方式"
            color: "#8896b8"
            font.pixelSize: 14
            Layout.alignment: Qt.AlignHCenter
            renderType: Text.NativeRendering
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 12
            spacing: 14

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 230
                color: "#0e1322"
                border.color: "#1e2d4a"
                border.width: 1
                radius: 6

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 22
                    spacing: 10

                    Text {
                        text: "本地推演"
                        color: "#ffffff"
                        font.pixelSize: 18
                        font.bold: true
                        renderType: Text.NativeRendering
                    }
                    Text {
                        text: "在本机运行完整推演，可编辑场景并控制所有本地功能。"
                        color: "#bcc8de"
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        renderType: Text.NativeRendering
                    }
                    Item { Layout.fillHeight: true }
                    TonalButton {
                        text: "进入本地模式"
                        base: "#2a4f86"
                        Layout.fillWidth: true
                        onClicked: root.localRequested()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 230
                color: "#0e1322"
                border.color: "#1e2d4a"
                border.width: 1
                radius: 6

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 22
                    spacing: 10

                    Text {
                        text: "联网推演"
                        color: "#ffffff"
                        font.pixelSize: 18
                        font.bold: true
                        renderType: Text.NativeRendering
                    }
                    Text {
                        text: "连接权威服务器。角色、阵营和可见状态由服务器 token 决定。"
                        color: "#bcc8de"
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        renderType: Text.NativeRendering
                    }
                    Text {
                        visible: controller.hasConnectionDefaults
                        text: controller.connectionDefaultUrl
                        color: "#4090ff"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        renderType: Text.NativeRendering
                    }
                    Item { Layout.fillHeight: true }
                    TonalButton {
                        text: "连接服务器"
                        base: "#246b57"
                        Layout.fillWidth: true
                        onClicked: root.remoteRequested()
                    }
                }
            }
        }
    }
}
