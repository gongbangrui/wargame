pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Drawer {
    id: root
    property var controller: null
    property var editor: null
    edge: Qt.RightEdge
    width: Math.min(390, parent ? Math.max(Math.min(320, parent.width), parent.width * 0.38) : 390)
    height: parent ? parent.height : 720
    modal: false
    interactive: root.controller.networked
    property var selectedRecipientSeatIds: []
    property color page: AppContext.page
    property color panel: AppContext.panel
    property color raised: AppContext.raised
    property color borderDefault: AppContext.line
    property color textPrimary: AppContext.text
    property color textMuted: AppContext.muted
    property color signal: AppContext.signal
    readonly property bool isCommander: root.controller.currentSeatType === "commander"

    function roleName(seatId) {
        if (!seatId) return "房间大厅"
        var parts = seatId.split("_")
        var kind = { commander: "指挥官", attack: "攻击机", recon: "侦察机", ground: "地面单位", jammer: "干扰机" }
        var slot = parts.length > 2 && parts[1] !== "commander" ? " #" + parts[2] : ""
        return (parts[0] === "red" ? "红方" : "蓝方") + " · " + (kind[parts[1]] || parts[1] || seatId) + slot
    }
    function roleColor(seatId) {
        if (seatId && seatId.indexOf("red_") === 0) return "#ef6370"
        if (seatId && seatId.indexOf("blue_") === 0) return "#55a9e8"
        return root.signal
    }
    function sendCurrent() {
        var value = input.text.trim()
        if (!value || !root.canSendCurrent()) return
        root.controller.sendChat(value, root.selectedRecipientSeatIds)
        input.text = ""
    }
    function commanderSeat() {
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) {
            if (seats[i].side === root.controller.currentSeatSide
                    && seats[i].seatType === "commander") return seats[i]
        }
        return null
    }
    function syncRoleRecipients() {
        if (root.isCommander) return
        var commander = root.commanderSeat()
        root.selectedRecipientSeatIds = commander && commander.occupied
            ? [commander.seatId] : []
    }
    function canSendCurrent() {
        if (root.selectedRecipientSeatIds.length === 0) return false
        if (root.isCommander) return true
        if (root.controller.matchPhase === "preparing") return true
        var state = root.controller.communicationState
        return root.controller.matchPhase === "running"
            && (state === "bilateral" || state === "twoWay")
    }
    function composerStatus() {
        if (root.isCommander) return "选择本方收件战位"
        if (!root.commanderSeat() || !root.commanderSeat().occupied) return "本方指挥官未就位"
        if (root.controller.matchPhase === "preparing") return "发送给本方指挥官 · 准备阶段可用"
        if (root.canSendCurrent()) return "发送给本方指挥官 · 双向通信"
        return "当前与指挥官无双向通信，暂不能发送"
    }
    function recipientOptions() {
        var result = []
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) {
            var seat = seats[i]
            if (seat.occupied && seat.side === root.controller.currentSeatSide
                    && seat.seatId !== root.controller.currentSeatId
                    && (root.isCommander || seat.seatType === "commander"))
                result.push(seat)
        }
        return result
    }

    Component.onCompleted: root.syncRoleRecipients()
    Connections {
        target: root.controller
        function onOnlineSeatsChanged() { root.syncRoleRecipients() }
        function onOnlineStateChanged() { root.syncRoleRecipients() }
    }
    function recipientSelected(seatId) { return root.selectedRecipientSeatIds.indexOf(seatId) >= 0 }
    function toggleRecipient(seatId, checked) {
        var next = root.selectedRecipientSeatIds.slice()
        var index = next.indexOf(seatId)
        if (checked && index < 0) next.push(seatId)
        if (!checked && index >= 0) next.splice(index, 1)
        root.selectedRecipientSeatIds = next
    }

    background: Rectangle { color: root.panel; border.color: root.borderDefault }
    contentItem: ColumnLayout {
        spacing: 0
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 58; color: root.raised
            Rectangle { anchors.left: parent.left; anchors.bottom: parent.bottom; width: parent.width; height: 1; color: root.borderDefault }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 10
                ColumnLayout { spacing: 1; Layout.fillWidth: true
                    Text { text: root.isCommander ? "实时通信收件箱" : "指挥官消息收件箱"; color: root.textPrimary; font.bold: true; font.pixelSize: 16 }
                    Text { text: root.controller.chatMessages.length + " 条消息"; color: root.textMuted; font.pixelSize: 10 }
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
                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: root.borderDefault }
                Column {
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.margins: 13; spacing: 5
                    RowLayout {
                        width: parent.width; spacing: 7
                        Text { Layout.fillWidth: true; text: chatMessage.modelData.displayName || chatMessage.modelData.username || "用户"; color: root.roleColor(chatMessage.modelData.seatId); font.bold: true; font.pixelSize: 12; elide: Text.ElideRight }
                        Text { text: root.roleName(chatMessage.modelData.seatId); color: root.textMuted; font.pixelSize: 10 }
                        Text { text: chatMessage.modelData.time ? new Date(chatMessage.modelData.time).toLocaleTimeString(Qt.locale(), "HH:mm:ss") : ""; color: root.textMuted; font.pixelSize: 10 }
                    }
                    Text { id: messageBody; width: parent.width; text: chatMessage.modelData.text || ""; color: root.textPrimary; font.pixelSize: 13; wrapMode: Text.WordWrap }
                }
            }
            onCountChanged: positionViewAtEnd()
        }

        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 128; color: root.raised
            Rectangle { anchors.left: parent.left; anchors.top: parent.top; width: parent.width; height: 1; color: root.borderDefault }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 10; spacing: 6
                RowLayout { Layout.fillWidth: true; spacing: 8
                    Text { text: root.isCommander ? "接收战位" : "收件人"; color: root.textMuted; font.pixelSize: 10 }
                    ListView {
                        id: recipientList; Layout.fillWidth: true; Layout.preferredHeight: 30
                        orientation: ListView.Horizontal; spacing: 6; clip: true
                        model: root.recipientOptions()
                        delegate: CheckBox { id: recipientCheckBox
                            required property var modelData
                            text: root.roleName(recipientCheckBox.modelData.seatId)
                            checked: root.recipientSelected(recipientCheckBox.modelData.seatId)
                            enabled: root.isCommander
                            Accessible.name: root.isCommander ? "选择收件战位" + text : "收件人已固定为" + text
                            onToggled: root.toggleRecipient(recipientCheckBox.modelData.seatId, checked)
                            contentItem: Text { text: recipientCheckBox.text; color: recipientCheckBox.checked ? root.signal : root.textMuted; font.pixelSize: 10; leftPadding: 22; verticalAlignment: Text.AlignVCenter }
                            indicator: Rectangle { x: 2; anchors.verticalCenter: parent.verticalCenter; width: 15; height: 15; radius: 3; color: recipientCheckBox.checked ? root.signal : root.page; border.color: recipientCheckBox.checked ? root.signal : root.borderDefault; Text { anchors.centerIn: parent; text: recipientCheckBox.checked ? "✓" : ""; color: root.page; font.pixelSize: 10 } }
                        }
                    }
                }
                RowLayout { Layout.fillWidth: true; Layout.fillHeight: true; spacing: 8
                TextField {
                    id: input; Layout.fillWidth: true; Layout.fillHeight: true
                    placeholderText: root.canSendCurrent() ? "输入消息" : root.composerStatus()
                    color: root.textPrimary; maximumLength: 500; selectByMouse: true
                    enabled: root.canSendCurrent()
                    Accessible.name: root.isCommander ? "指挥官通信消息" : "发送给本方指挥官的消息"
                    Accessible.description: root.composerStatus()
                    onAccepted: root.sendCurrent()
                    background: Rectangle { color: root.page; border.color: input.activeFocus ? root.signal : root.borderDefault; radius: 5 }
                }
                TonalButton { text: "发送"; base: root.signal; enabled: input.text.trim().length > 0 && root.canSendCurrent(); Accessible.name: "发送通信消息"; onClicked: root.sendCurrent() }
                }
                Text { Layout.fillWidth: true; text: root.composerStatus(); color: root.canSendCurrent() ? root.signal : AppContext.warning; font.pixelSize: 9; elide: Text.ElideRight }
            }
        }
    }
}
