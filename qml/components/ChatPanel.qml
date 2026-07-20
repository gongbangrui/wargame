pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Drawer {
    id: root
    property var controller: null
    property var editor: null
    edge: Qt.RightEdge
    width: Math.min(390, parent ? parent.width * 0.38 : 390)
    height: parent ? parent.height : 720
    modal: false
    interactive: root.controller.networked

    function roleName(role) {
        if (role === "red") return "红方"
        if (role === "blue") return "蓝方"
        if (role === "director") return "导演席"
        if (role === "editor") return "编辑席"
        return role
    }
    function roleColor(role) {
        if (role === "red") return "#ef6370"
        if (role === "blue") return "#55a9e8"
        if (role === "director") return "#e1b65e"
        return "#64c9b8"
    }
    function sendCurrent() {
        var value = input.text.trim()
        if (!value) return
        root.controller.sendChat(value)
        input.text = ""
    }

    background: Rectangle { color: "#0d131c"; border.color: "#283647" }
    contentItem: ColumnLayout {
        spacing: 0
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 58; color: "#131b26"
            Rectangle { anchors.left: parent.left; anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#283647" }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 10
                ColumnLayout { spacing: 1; Layout.fillWidth: true
                    Text { text: "联合通信"; color: "#f2f5f9"; font.bold: true; font.pixelSize: 16 }
                    Text { text: "推演室实时文字频道"; color: "#758398"; font.pixelSize: 10 }
                }
                GhostButton { text: "关闭"; onClicked: root.close() }
            }
        }

        ListView {
            id: messageList; Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; spacing: 0; model: root.controller.chatMessages
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Item {
                id: chatMessage
                required property var modelData
                width: messageList.width; height: messageBody.implicitHeight + 49
                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: "#1d2937" }
                Column {
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.margins: 13; spacing: 5
                    Row {
                        spacing: 7
                        Text { text: chatMessage.modelData.displayName || chatMessage.modelData.username || "用户"; color: root.roleColor(chatMessage.modelData.role); font.bold: true; font.pixelSize: 12 }
                        Text { text: root.roleName(chatMessage.modelData.role); color: "#718096"; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }
                        Text { text: chatMessage.modelData.time ? new Date(chatMessage.modelData.time).toLocaleTimeString(Qt.locale(), "HH:mm:ss") : ""; color: "#5d6a7b"; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }
                    }
                    Text { id: messageBody; width: parent.width; text: chatMessage.modelData.text || ""; color: "#d9e0e8"; font.pixelSize: 13; wrapMode: Text.WrapAnywhere }
                }
            }
            onCountChanged: positionViewAtEnd()
        }

        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 72; color: "#111923"
            Rectangle { anchors.left: parent.left; anchors.top: parent.top; width: parent.width; height: 1; color: "#283647" }
            RowLayout {
                anchors.fill: parent; anchors.margins: 12; spacing: 8
                TextField {
                    id: input; Layout.fillWidth: true; Layout.fillHeight: true; placeholderText: "输入消息"
                    color: "#eef2f6"; maximumLength: 500; selectByMouse: true
                    onAccepted: root.sendCurrent()
                    background: Rectangle { color: "#0a0f16"; border.color: input.activeFocus ? "#2aa897" : "#2a3748"; radius: 5 }
                }
                TonalButton { text: "发送"; base: "#258f82"; enabled: input.text.trim().length > 0; onClicked: root.sendCurrent() }
            }
        }
    }
}
