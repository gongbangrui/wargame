pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    property var controller: null
    property string side: controller && controller.networked
        ? controller.currentSeatSide : (controller ? controller.focusedSide : "red")
    property string enemySide: side === "red" ? "blue" : "red"
    property var workflow: controller ? controller.vmfWorkflow : ({})
    property bool vmfAvailable: workflow && workflow.stage !== undefined
    property bool compact: width < 360
    property string selectedReconId: ""
    property string selectedTargetId: workflow && workflow.targetId ? workflow.targetId : ""
    property string selectedAttackerId: workflow && workflow.attackerId ? workflow.attackerId : ""
    property string selectedGuideId: workflow && workflow.guideId ? workflow.guideId : ""
    property string waypointX: ""
    property string waypointY: ""
    property string notice: ""

    property color page: AppContext.page
    property color panel: AppContext.panel
    property color panelAlt: AppContext.raised
    property color line: AppContext.line
    property color ink: AppContext.text
    property color dim: AppContext.muted
    property color cyan: AppContext.signal
    property color orange: AppContext.warning
    property color danger: AppContext.danger
    property color success: AppContext.success

    visible: vmfAvailable
    implicitHeight: body.implicitHeight + 18

    function stageLabel(stage) {
        var labels = {
            idle: "等待目标报告",
            targetReported: "目标已报告",
            dispatchPending: "派单等待确认",
            strikeDispatched: "打击已派出",
            groundGuidancePending: "等待地面引导",
            engaging: "攻击进行中",
            targetDestroyed: "目标已摧毁",
            withdrawPending: "撤离等待确认",
            withdrawn: "已下达撤离",
            failed: "工作流失败"
        }
        return labels[stage] || stage || "未启动"
    }

    function stageColor(stage) {
        if (stage === "failed") return root.danger
        if (stage === "targetDestroyed" || stage === "withdrawn") return root.success
        if (stage === "engaging") return root.danger
        if (stage === "idle") return root.dim
        return root.cyan
    }

    function roleAllows(action) {
        if (!root.controller || !root.controller.networked) return true
        if (root.controller.isObserver || root.controller.currentSeatSide !== root.side) return false
        var role = root.controller.currentSeatType
        if (action === "report") return role === "recon"
        if (action === "dispatch" || action === "guidance" || action === "withdraw")
            return role === "commander"
        if (action === "confirm") return role === "ground"
        return false
    }

    function currentSeatUnitId() {
        if (!root.controller || !root.controller.networked) return ""
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; ++i) {
            var seat = seats[i] || ({})
            if (String(seat.seatId || "") === String(root.controller.currentSeatId || ""))
                return String(seat.unitId || "")
        }
        return ""
    }

    function units(kind) {
        if (!root.controller || !root.side) return []
        return root.controller.unitOptions(kind || "", root.side) || []
    }

    function targetOptions() {
        if (!root.controller || !root.side) return []
        var observer = root.controller.networked && root.controller.isObserver
        if (observer) return []
        var source = root.selectedReconId || root.currentSeatUnitId()
        var detected = root.controller.detectedEnemyOptions(source, root.side, root.enemySide) || []
        if (detected.length > 0 || root.controller.networked) return detected
        return root.controller.unitOptions("", root.enemySide) || []
    }

    function findOptionIndex(options, id) {
        for (var i = 0; i < (options || []).length; ++i)
            if (String(options[i].id || "") === String(id || "")) return i
        return -1
    }

    function keepSelection() {
        var recon = root.units("reconuav")
        if (root.selectedReconId === "" || root.findOptionIndex(recon, root.selectedReconId) < 0)
            root.selectedReconId = recon.length > 0 ? recon[0].id : root.currentSeatUnitId()
        var targets = root.targetOptions()
        if (root.selectedTargetId && root.findOptionIndex(targets, root.selectedTargetId) < 0
                && (!root.workflow || root.workflow.stage === "idle")) root.selectedTargetId = ""
        if (!root.selectedTargetId && targets.length > 0) root.selectedTargetId = targets[0].id
        var attackers = root.units("attackuav")
        if (root.selectedAttackerId === "" || root.findOptionIndex(attackers, root.selectedAttackerId) < 0)
            root.selectedAttackerId = root.workflow.attackerId || (attackers.length > 0 ? attackers[0].id : "")
        var guides = root.units("groundscout")
        if (root.selectedGuideId === "" || root.findOptionIndex(guides, root.selectedGuideId) < 0)
            root.selectedGuideId = root.workflow.guideId || (guides.length > 0 ? guides[0].id : "")
        root.setWaypointFromTarget()
    }

    function setWaypointFromTarget() {
        if (!root.controller || !root.selectedTargetId) return
        var target = root.controller.unitAt(root.selectedTargetId) || ({})
        var position = target.position || []
        if (position.length >= 2 && (root.waypointX === "" || root.waypointY === "")) {
            root.waypointX = Number(position[0]).toFixed(0)
            root.waypointY = Number(position[1]).toFixed(0)
        }
    }

    function waypointList() {
        var x = Number(root.waypointX)
        var y = Number(root.waypointY)
        if (!isFinite(x) || !isFinite(y)) return []
        return [{ x: x, y: y }]
    }

    function resultMessage(result, pendingText) {
        var accepted = result && result.accepted === true
        root.notice = result && result.message ? String(result.message)
            : accepted ? pendingText : "操作未提交"
        return accepted
    }

    function lastCommunicationText() {
        if (!root.controller) return "暂无 VMF 通信记录"
        var records = root.controller.messages || []
        for (var i = 0; i < records.length; ++i) {
            var message = records[i] || ({})
            if (message.wireFormat !== "vmf-design-v1") continue
            var requiresAck = message.requiresAck === true
            if (!requiresAck) return "VMF · 已送达 · " + (message.vmfMessage || "消息")
            if (message.acked === true) return "VMF · ACK 已确认 · " + (message.vmfMessage || "消息")
            return "VMF · 等待 ACK · " + (message.vmfMessage || "消息")
        }
        return root.controller.networked && root.controller.communicationState !== "bilateral"
            ? "通信链路：" + (root.controller.communicationState || "未确认")
            : "暂无 VMF 通信记录"
    }

    Connections {
        target: root.controller
        function onUnitsForward() { root.keepSelection() }
        function onOnlineStateChanged() { root.keepSelection() }
        function onVmfWorkflowChanged() { root.keepSelection() }
        function onMessagesForward() { root.keepSelection() }
    }

    Rectangle {
        anchors.fill: parent
        color: root.panel
        border.color: root.line
        radius: 5

        ColumnLayout {
            id: body
            anchors.fill: parent
            anchors.margins: 9
            spacing: 7

            RowLayout {
                Layout.fillWidth: true
                Rectangle { Layout.preferredWidth: 3; Layout.preferredHeight: 18; color: root.stageColor(root.workflow.stage); radius: 2 }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text { text: "VMF 引导打击"; color: root.ink; font.pixelSize: 12; font.bold: true }
                    Text { text: root.side === "blue" ? "蓝方任务链" : "红方任务链"; color: root.dim; font.pixelSize: 9 }
                }
                Text { text: root.stageLabel(root.workflow.stage); color: root.stageColor(root.workflow.stage); font.pixelSize: 10; font.bold: true }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 3
                Repeater {
                    model: ["targetReported", "strikeDispatched", "groundGuidancePending", "engaging", "targetDestroyed", "withdrawn"]
                    delegate: Rectangle {
                        required property string modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: 3
                        color: root.workflow.stage === modelData ? root.stageColor(modelData)
                            : (root.workflow.stage === "idle" ? root.line : root.panelAlt)
                        radius: 2
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: root.workflow.targetId || root.workflow.attackerId || root.workflow.guideId
                text: "目标 " + (root.workflow.targetId || "—")
                    + "  ·  攻击机 " + (root.workflow.attackerId || "—")
                    + "  ·  引导 " + (root.workflow.guideId || "—")
                color: root.dim; font.pixelSize: 9; elide: Text.ElideMiddle
            }
            Text {
                Layout.fillWidth: true
                text: root.lastCommunicationText()
                color: root.dim; font.pixelSize: 9; elide: Text.ElideRight
            }

            ColumnLayout {
                visible: root.workflow.stage === "idle" || root.workflow.stage === "targetReported"
                Layout.fillWidth: true
                spacing: 5
                Text { text: "1 · 侦察目标报告"; color: root.ink; font.pixelSize: 10; font.bold: true }
                RowLayout {
                    Layout.fillWidth: true; spacing: 5
                    ComboBox {
                        id: reconCombo
                        Layout.fillWidth: true
                        model: root.units("reconuav")
                        textRole: "callsign"
                        valueRole: "id"
                        enabled: root.roleAllows("report")
                        currentIndex: root.findOptionIndex(model, root.selectedReconId)
                        onActivated: root.selectedReconId = currentValue || (model[currentIndex] || ({})).id || ""
                        contentItem: Text { text: reconCombo.currentText || "选择侦察机"; color: reconCombo.enabled ? root.ink : root.dim; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter; leftPadding: 6; font.pixelSize: 9 }
                        background: Rectangle { color: root.page; border.color: reconCombo.enabled ? root.line : root.panelAlt; radius: 3 }
                    }
                    ComboBox {
                        id: targetCombo
                        Layout.fillWidth: true
                        model: root.targetOptions()
                        textRole: "callsign"
                        valueRole: "id"
                        enabled: root.roleAllows("report") && count > 0
                        currentIndex: root.findOptionIndex(model, root.selectedTargetId)
                        onActivated: { root.selectedTargetId = currentValue || (model[currentIndex] || ({})).id || ""; root.waypointX = ""; root.waypointY = ""; root.setWaypointFromTarget() }
                        contentItem: Text { text: targetCombo.currentText || "选择可报告目标"; color: targetCombo.enabled ? root.ink : root.dim; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter; leftPadding: 6; font.pixelSize: 9 }
                        background: Rectangle { color: root.page; border.color: targetCombo.enabled ? root.line : root.panelAlt; radius: 3 }
                    }
                    Button {
                        id: reportButton
                        text: "报告"
                        enabled: root.roleAllows("report") && root.selectedReconId && root.selectedTargetId
                        onClicked: root.resultMessage(root.controller.reportGuidedStrikeTarget(
                            root.selectedReconId, root.selectedTargetId, {}), "目标报告已提交，等待回执")
                        contentItem: Text { text: reportButton.text; color: reportButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 9; font.bold: true }
                        background: Rectangle { color: reportButton.enabled ? root.cyan : root.line; radius: 3 }
                    }
                }
            }

            ColumnLayout {
                visible: root.workflow.stage === "targetReported"
                Layout.fillWidth: true
                spacing: 5
                Text { text: "2 · 指挥派单与航路"; color: root.ink; font.pixelSize: 10; font.bold: true }
                RowLayout {
                    Layout.fillWidth: true; spacing: 5
                    ComboBox {
                        id: attackerCombo
                        Layout.fillWidth: true
                        model: root.units("attackuav")
                        textRole: "callsign"
                        valueRole: "id"
                        enabled: root.roleAllows("dispatch")
                        currentIndex: root.findOptionIndex(model, root.selectedAttackerId)
                        onActivated: root.selectedAttackerId = currentValue || (model[currentIndex] || ({})).id || ""
                        contentItem: Text { text: attackerCombo.currentText || "选择攻击机"; color: attackerCombo.enabled ? root.ink : root.dim; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter; leftPadding: 6; font.pixelSize: 9 }
                        background: Rectangle { color: root.page; border.color: attackerCombo.enabled ? root.line : root.panelAlt; radius: 3 }
                    }
                    Button {
                        id: targetPointButton
                        text: "取目标点"
                        enabled: root.selectedTargetId.length > 0
                        onClicked: { root.waypointX = ""; root.waypointY = ""; root.setWaypointFromTarget() }
                        contentItem: Text { text: targetPointButton.text; color: targetPointButton.enabled ? root.cyan : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 9 }
                        background: Rectangle { color: root.page; border.color: targetPointButton.enabled ? root.cyan : root.line; radius: 3 }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 5
                    TextField { id: waypointXField; Layout.fillWidth: true; text: root.waypointX; placeholderText: "航点 X (m)"; enabled: root.roleAllows("dispatch"); onTextEdited: root.waypointX = text; color: root.ink; font.pixelSize: 9; background: Rectangle { color: root.page; border.color: root.line; radius: 3 } }
                    TextField { id: waypointYField; Layout.fillWidth: true; text: root.waypointY; placeholderText: "航点 Y (m)"; enabled: root.roleAllows("dispatch"); onTextEdited: root.waypointY = text; color: root.ink; font.pixelSize: 9; background: Rectangle { color: root.page; border.color: root.line; radius: 3 } }
                    Button {
                        id: dispatchButton
                        text: "派单"
                        enabled: root.roleAllows("dispatch") && root.selectedAttackerId && root.selectedTargetId && root.waypointList().length > 0
                        onClicked: root.resultMessage(root.controller.dispatchGuidedStrike(root.selectedAttackerId, root.selectedTargetId, root.waypointList()), "派单已提交，等待服务器事件")
                        contentItem: Text { text: dispatchButton.text; color: dispatchButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 9; font.bold: true }
                        background: Rectangle { color: dispatchButton.enabled ? root.cyan : root.line; radius: 3 }
                    }
                }
            }

            ColumnLayout {
                visible: root.workflow.stage === "strikeDispatched"
                Layout.fillWidth: true; spacing: 5
                Text { text: "3 · 指挥命令地面引导"; color: root.ink; font.pixelSize: 10; font.bold: true }
                RowLayout {
                    Layout.fillWidth: true; spacing: 5
                    ComboBox {
                        id: guideCombo
                        Layout.fillWidth: true
                        model: root.units("groundscout")
                        textRole: "callsign"
                        valueRole: "id"
                        enabled: root.roleAllows("guidance")
                        currentIndex: root.findOptionIndex(model, root.selectedGuideId)
                        onActivated: root.selectedGuideId = currentValue || (model[currentIndex] || ({})).id || ""
                        contentItem: Text { text: guideCombo.currentText || "选择地面引导单元"; color: guideCombo.enabled ? root.ink : root.dim; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter; leftPadding: 6; font.pixelSize: 9 }
                        background: Rectangle { color: root.page; border.color: guideCombo.enabled ? root.line : root.panelAlt; radius: 3 }
                    }
                    Button {
                        id: guidanceButton
                        text: "下达引导"
                        enabled: root.roleAllows("guidance") && root.selectedGuideId && root.workflow.attackerId && root.workflow.targetId
                        onClicked: root.resultMessage(root.controller.commandGuidedStrikeGroundGuidance(root.selectedGuideId, root.workflow.attackerId, root.workflow.targetId), "引导命令已提交，等待地面确认")
                        contentItem: Text { text: guidanceButton.text; color: guidanceButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 9; font.bold: true }
                        background: Rectangle { color: guidanceButton.enabled ? root.orange : root.line; radius: 3 }
                    }
                }
            }

            ColumnLayout {
                visible: root.workflow.stage === "groundGuidancePending"
                Layout.fillWidth: true; spacing: 5
                Text { text: "4 · 地面确认攻击"; color: root.ink; font.pixelSize: 10; font.bold: true }
                RowLayout {
                    Layout.fillWidth: true; spacing: 5
                    Text { Layout.fillWidth: true; text: "引导单元 " + (root.workflow.guideId || "—") + " → " + (root.workflow.attackerId || "—"); color: root.dim; font.pixelSize: 9; elide: Text.ElideMiddle }
                    Button {
                        id: confirmButton
                        text: "确认攻击"
                        enabled: root.roleAllows("confirm") && root.workflow.guideId && root.workflow.attackerId && root.workflow.targetId && root.waypointList().length > 0
                        onClicked: root.resultMessage(root.controller.confirmGuidedStrikeAttack(root.workflow.guideId, root.workflow.attackerId, root.workflow.targetId, root.waypointList()), "攻击确认已提交，等待交战事件")
                        contentItem: Text { text: confirmButton.text; color: confirmButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 9; font.bold: true }
                        background: Rectangle { color: confirmButton.enabled ? root.danger : root.line; radius: 3 }
                    }
                }
            }

            RowLayout {
                visible: (root.workflow.stage === "engaging" || root.workflow.stage === "targetDestroyed")
                    && root.roleAllows("withdraw")
                Layout.fillWidth: true; spacing: 5
                Text { Layout.fillWidth: true; text: root.workflow.stage === "targetDestroyed" ? "目标摧毁已确认，可组织撤离" : "等待攻击结果，指挥员可确认撤离"; color: root.dim; font.pixelSize: 9; wrapMode: Text.WordWrap }
                Button {
                    id: withdrawButton
                    text: "确认撤离"
                    enabled: root.workflow.attackerId.length > 0
                    onClicked: root.resultMessage(root.controller.withdrawGuidedStrike(root.workflow.attackerId, {}), "撤离命令已提交，等待服务器事件")
                    contentItem: Text { text: withdrawButton.text; color: withdrawButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 9; font.bold: true }
                    background: Rectangle { color: withdrawButton.enabled ? root.orange : root.line; radius: 3 }
                }
            }

            Text {
                visible: root.notice.length > 0
                Layout.fillWidth: true
                text: root.notice
                color: root.notice.indexOf("失败") >= 0 || root.notice.indexOf("拒绝") >= 0 ? root.danger : root.dim
                font.pixelSize: 9; wrapMode: Text.WordWrap
            }
        }
    }

    Component.onCompleted: root.keepSelection()
}
