import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    color: "#0e1322"
    border.color: "#1e2d4a"
    border.width: 1

    function roleLabel(message) {
        if (message.role === "director") return "导演"
        if (message.side === "red") return "红方"
        if (message.side === "blue") return "蓝方"
        return message.userId || "用户"
    }

    function send() {
        var text = input.text.trim()
        if (!text.length) return
        var result = controller.sendChat(text)
        if (result.accepted) input.clear()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        Text {
            text: "推演对话"
            color: "#ffffff"
            font.pixelSize: 16
            font.bold: true
            Layout.fillWidth: true
            renderType: Text.NativeRendering
        }
        Text {
            text: "红方、蓝方与导演实时可见"
            color: "#8896b8"
            font.pixelSize: 11
            Layout.fillWidth: true
            renderType: Text.NativeRendering
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#080b14"
            border.color: "#17213a"
            border.width: 1
            radius: 4
            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 6
                clip: true
                spacing: 6
                model: controller.chatMessages
                onCountChanged: Qt.callLater(function() { list.positionViewAtEnd() })
                delegate: Column {
                    width: list.width
                    spacing: 2
                    Text {
                        text: root.roleLabel(modelData) + "  " + ((modelData.time || "").slice(11, 19))
                        color: modelData.side === "red" ? "#f04760"
                               : modelData.side === "blue" ? "#4090ff" : "#36c98a"
                        font.pixelSize: 11
                        font.bold: true
                        renderType: Text.NativeRendering
                    }
                    Text {
                        width: parent.width
                        text: modelData.text || ""
                        color: "#e8edf5"
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                        renderType: Text.NativeRendering
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            TextField {
                id: input
                Layout.fillWidth: true
                maximumLength: 500
                placeholderText: "输入消息"
                color: "#e8edf5"
                selectByMouse: true
                onAccepted: root.send()
                background: Rectangle {
                    color: "#080b14"
                    border.color: input.activeFocus ? "#4090ff" : "#1e2d4a"
                    border.width: 1
                    radius: 4
                }
            }
            Button {
                text: "发送"
                enabled: input.text.trim().length > 0
                onClicked: root.send()
            }
        }
    }
}
