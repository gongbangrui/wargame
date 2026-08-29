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
    property bool strictProfile: controller && controller.networked
        && controller.protocolProfile === "vmf-guided-strike-v1"
    property var strictTasks: controller && controller.vmfTasks
        && controller.vmfTasks.tasks ? controller.vmfTasks.tasks : []
    property string selectedTaskId: ""
    property string strictTargetId: ""
    property var strictTrace: controller ? controller.vmfTrace : ({})
    property bool vmfAvailable: workflow && workflow.stage !== undefined
    property bool compact: width < 360
    property string selectedReconId: ""
    property string selectedTargetId: workflow && workflow.targetId ? workflow.targetId : ""
    property string selectedAttackerId: workflow && workflow.attackerId ? workflow.attackerId : ""
    property string selectedGuideId: workflow && workflow.guideId ? workflow.guideId : ""
    property string waypointX: ""
    property string waypointY: ""
    property string notice: ""
    property bool strictSubmitting: false
    property bool strictDiagnosticsExpanded: false

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

    visible: strictProfile || vmfAvailable
    implicitHeight: strictProfile ? strictBody.implicitHeight + 18 : body.implicitHeight + 18

    function strictTask() {
        for (var i = 0; i < root.strictTasks.length; ++i)
            if (String(root.strictTasks[i].taskId || "") === root.selectedTaskId)
                return root.strictTasks[i]
        return root.strictTasks.length > 0 ? root.strictTasks[0] : ({})
    }

    function strictSeat(type) {
        var seats = root.controller ? root.controller.onlineSeats || [] : []
        for (var i = 0; i < seats.length; ++i)
            if (seats[i].side === root.side && seats[i].seatType === type) return seats[i]
        return ({})
    }

    function strictStageAction(stage) {
        var actionByStage = {
            awaitingTargetReport: "reportTarget", targetReported: "dispatch",
            dispatchPending: "acceptDispatch", enRoute: "orderGroundGuidance",
            groundGuidancePending: "markRendezvousReady", rendezvousReady: "identityHello",
            identityHandshakePending: "identityConfirm", guidancePackagePending: "sendGuidancePackage",
            routeAcceptancePending: "acceptGuidance", attackLanePending: "reportAttackReady",
            attackAuthorizationPending: "authorizeAttack", engaging: "engage",
            damageReportPending: "reportBattleDamage", damageAssessmentPending: "confirmDamageAssessment",
            reconConfirmationPending: "confirmTargetDestroyed", targetDestroyed: "withdraw",
            withdrawPending: "markReturning"
        }
        return actionByStage[String(stage || "")] || ""
    }

    function strictActionRole(action) {
        var roles = {
            reportTarget: "recon", dispatch: "commander", acceptDispatch: "attack",
            orderGroundGuidance: "commander", markRendezvousReady: "ground",
            identityHello: "attack", identityConfirm: "ground", sendGuidancePackage: "ground",
            acceptGuidance: "attack", reportAttackReady: "attack", authorizeAttack: "ground",
            engage: "attack", reportBattleDamage: "attack", confirmDamageAssessment: "ground",
            confirmTargetDestroyed: "recon", withdraw: "commander", markReturning: "attack"
        }
        return roles[action] || ""
    }

    function strictActorSeat(task) {
        var action = root.strictStageAction(task.stage)
        var role = root.strictActionRole(action)
        if (!role) return ({})
        var seatId = String(task[role + "SeatId"] || "")
        var seats = root.controller ? root.controller.onlineSeats || [] : []
        for (var i = 0; i < seats.length; ++i)
            if (String(seats[i].seatId || "") === seatId) return seats[i]
        return root.strictSeat(role)
    }

    function strictActionFor(task) {
        if (!root.controller || root.controller.isObserver) return ""
        var action = root.strictStageAction(task.stage)
        var actor = root.strictActorSeat(task)
        return action && actor.controlMode === "human"
                && String(actor.seatId || "") === String(root.controller.currentSeatId || "")
            ? action : ""
    }

    function strictActorStatus(task) {
        var actor = root.strictActorSeat(task)
        if (!actor.seatId) return "等待任务绑定"
        if (actor.controlMode === "vmf-auto") return "服务器正在自动推进"
        if (actor.controlMode === "fixed-target") return "固定目标无需操作"
        if (String(actor.seatId) === String(root.controller.currentSeatId || ""))
            return "轮到本战位操作"
        var roleLabels = { commander: "指挥席", recon: "侦察席", attack: "攻击席", ground: "地面引导席" }
        return "等待 " + (roleLabels[String(actor.seatType || "")] || "其他战位")
    }

    function strictActionLabel(action) {
        var labels = {
            reportTarget: "提交目标报告", dispatch: "下达派单", acceptDispatch: "接受派单",
            orderGroundGuidance: "命令地面引导", markRendezvousReady: "确认会合就绪",
            identityHello: "发起身份握手", identityConfirm: "确认身份",
            sendGuidancePackage: "发送引导包", acceptGuidance: "接受航路",
            reportAttackReady: "报告攻击就绪", authorizeAttack: "授权攻击",
            engage: "实施攻击", reportBattleDamage: "报告毁伤",
            confirmDamageAssessment: "确认毁伤评估", confirmTargetDestroyed: "确认目标摧毁",
            withdraw: "下达撤离", markReturning: "报告返航"
        }
        return labels[action] || "等待绑定战位操作"
    }

    function strictActionHint(action) {
        var hints = {
            reportTarget: "核实已发现目标",
            dispatch: "确认系统生成的目标航路",
            acceptDispatch: "接收任务并开始航渡",
            orderGroundGuidance: "通知地面战位准备引导",
            markRendezvousReady: "确认攻击机进入会合区",
            identityHello: "向地面战位发起识别",
            identityConfirm: "确认攻击机身份",
            sendGuidancePackage: "发送目标点与航路",
            acceptGuidance: "接收地面引导航路",
            reportAttackReady: "报告已进入攻击航线",
            authorizeAttack: "允许攻击机实施打击",
            engage: "对任务目标实施攻击",
            reportBattleDamage: "回报目标毁伤状态",
            confirmDamageAssessment: "确认毁伤评估",
            confirmTargetDestroyed: "复核目标摧毁状态",
            withdraw: "命令攻击机撤离",
            markReturning: "确认开始返航"
        }
        return hints[action] || "等待任务状态更新"
    }

    function syncStrictSelection() {
        if (!root.strictTasks.length) { root.selectedTaskId = ""; return }
        for (var i = 0; i < root.strictTasks.length; ++i)
            if (root.strictTasks[i].taskId === root.selectedTaskId) return
        root.selectedTaskId = String(root.strictTasks[0].taskId || "")
    }

    function createStrictTask() {
        var commander = root.strictSeat("commander")
        var recon = root.strictSeat("recon")
        var attack = root.strictSeat("attack")
        var ground = root.strictSeat("ground")
        if (!root.controller || root.controller.matchPhase !== "running") {
            root.notice = "推演启动后才能创建 VMF 任务"
            return
        }
        if (!commander.seatId || !recon.seatId || !attack.seatId || !ground.seatId
                || !root.strictTargetId) {
            root.notice = "任务绑定或目标不完整"
            return
        }
        var suffix = String(Date.now())
        var result = root.controller.sendVmfTaskCommand({
            requestId: "vmf-task-create-" + suffix,
            taskId: root.side + "-strike-" + suffix,
            expectedTaskRevision: 0,
            action: "createTask", messages: [],
            commanderSeatId: commander.seatId, reconSeatId: recon.seatId,
            attackSeatId: attack.seatId, groundSeatId: ground.seatId,
            targetId: root.strictTargetId, correlationId: "corr-" + suffix
        })
        root.strictSubmitting = result && result.accepted === true
        root.notice = result && result.message ? String(result.message)
            : root.strictSubmitting ? "任务创建请求已发送" : "任务未提交"
        if (root.strictSubmitting) strictSubmitTimer.restart()
    }

    function advanceStrictTask() {
        var task = root.strictTask()
        var action = root.strictActionFor(task)
        if (!task.taskId || !action) return
        var result = root.controller.sendVmfTaskCommand({
            requestId: "vmf-task-action-" + String(Date.now()),
            taskId: task.taskId,
            expectedTaskRevision: Number(task.taskRevision || 0),
            action: action, messages: []
        })
        root.strictSubmitting = result && result.accepted === true
        root.notice = result && result.message ? String(result.message)
            : root.strictSubmitting ? root.strictActionLabel(action) + "已提交" : "操作未提交"
        if (root.strictSubmitting) strictSubmitTimer.restart()
    }

    function strictRoleLabel(role) {
        var labels = { commander: "指挥", recon: "侦察", attack: "攻击", ground: "地面" }
        return labels[String(role || "")] || "待定"
    }

    function strictStepIndex(stage) {
        var steps = {
            awaitingTargetReport: 0, targetReported: 0,
            dispatchPending: 1, enRoute: 1,
            groundGuidancePending: 2, rendezvousReady: 2,
            identityHandshakePending: 2, guidancePackagePending: 2,
            routeAcceptancePending: 2, attackLanePending: 2,
            attackAuthorizationPending: 3, engaging: 3,
            damageReportPending: 4, damageAssessmentPending: 4,
            reconConfirmationPending: 4, targetDestroyed: 4,
            withdrawPending: 5, returning: 5, completed: 5
        }
        return steps[String(stage || "")] === undefined ? -1 : steps[String(stage || "")]
    }

    function stageLabel(stage) {
        var labels = {
            idle: "等待目标报告",
            awaitingTargetReport: "等待目标报告",
            targetReported: "目标已报告",
            dispatchPending: "派单等待确认",
            enRoute: "攻击机航渡中",
            strikeDispatched: "打击已派出",
            groundGuidancePending: "等待地面引导",
            rendezvousReady: "会合点就绪",
            identityHandshakePending: "身份握手中",
            guidancePackagePending: "等待引导包",
            routeAcceptancePending: "等待航路接受",
            attackLanePending: "等待攻击就绪",
            attackAuthorizationPending: "等待攻击授权",
            engaging: "攻击进行中",
            damageReportPending: "等待毁伤报告",
            damageAssessmentPending: "等待毁伤评估",
            reconConfirmationPending: "等待侦察确认",
            targetDestroyed: "目标已摧毁",
            withdrawPending: "撤离等待确认",
            returning: "返航中",
            completed: "任务完成",
            withdrawn: "已下达撤离",
            failed: "工作流失败"
        }
        return labels[stage] || stage || "未启动"
    }

    function stageColor(stage) {
        if (stage === "failed") return root.danger
        if (stage === "targetDestroyed" || stage === "withdrawn" || stage === "completed")
            return root.success
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
            if (String(options[i].id || options[i].taskId || "") === String(id || "")) return i
        return -1
    }

    function keepSelection() {
        var recon = root.units("reconuav")
        if (root.selectedReconId === "" || root.findOptionIndex(recon, root.selectedReconId) < 0)
            root.selectedReconId = recon.length > 0 ? recon[0].id : root.currentSeatUnitId()
        var targets = root.targetOptions()
        if (root.strictProfile
                && root.findOptionIndex(targets, root.strictTargetId) < 0)
            root.strictTargetId = targets.length > 0 ? targets[0].id : ""
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
        function onVmfTasksChanged() {
            root.strictSubmitting = false
            strictSubmitTimer.stop()
            root.syncStrictSelection()
        }
    }

    Timer {
        id: strictSubmitTimer
        interval: 6000
        onTriggered: root.strictSubmitting = false
    }

    Rectangle {
        visible: root.strictProfile
        anchors.fill: parent
        color: root.panel
        border.color: root.line
        radius: 5

        ColumnLayout {
            id: strictBody
            anchors.fill: parent
            anchors.margins: 9
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Rectangle { Layout.preferredWidth: 3; Layout.preferredHeight: 24; color: root.cyan; radius: 2 }
                ColumnLayout { Layout.fillWidth: true; spacing: 1
                    Text { text: "VMF 任务"; color: root.ink; font.pixelSize: 13; font.bold: true }
                    Text { text: "按高亮步骤操作 · 航路自动生成"; color: root.dim; font.pixelSize: 9 }
                }
                Rectangle {
                    Layout.preferredWidth: vmfPhaseText.implicitWidth + 14
                    Layout.preferredHeight: 24
                    color: root.controller && root.controller.matchPhase === "running"
                        ? "#123b35" : root.panelAlt
                    border.color: root.controller && root.controller.matchPhase === "running"
                        ? root.cyan : root.line
                    radius: 3
                    Text { id: vmfPhaseText; anchors.centerIn: parent; text: root.controller && root.controller.matchPhase === "running" ? "运行" : "待启动"; color: root.controller && root.controller.matchPhase === "running" ? root.cyan : root.dim; font.pixelSize: 9; font.bold: true }
                }
            }

            RowLayout {
                visible: root.controller && root.controller.currentSeatSide === "red"
                    && root.controller.currentSeatType === "commander"
                Layout.fillWidth: true; spacing: 5
                ComboBox {
                    id: strictTargetCombo
                    Layout.fillWidth: true
                    model: root.targetOptions(); textRole: "callsign"; valueRole: "id"
                    onActivated: root.strictTargetId = currentValue || ""
                    contentItem: Text { text: strictTargetCombo.currentText || "选择已发现托管目标"; color: root.ink; leftPadding: 6; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight; font.pixelSize: 9 }
                    background: Rectangle { color: root.page; border.color: root.line; radius: 3 }
                }
                Button {
                    id: createStrictButton
                    text: "新建任务"
                    enabled: root.controller && root.controller.matchPhase === "running"
                        && root.strictTargetId.length > 0 && !root.strictSubmitting
                    onClicked: root.createStrictTask()
                    contentItem: Text { text: createStrictButton.text; color: createStrictButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 9; font.bold: true }
                    background: Rectangle { color: createStrictButton.enabled ? root.cyan : root.line; radius: 3 }
                }
            }

            ComboBox {
                id: strictTaskCombo
                visible: root.strictTasks.length > 1
                Layout.fillWidth: true
                model: root.strictTasks; textRole: "taskId"; valueRole: "taskId"
                currentIndex: root.findOptionIndex(model, root.selectedTaskId)
                onActivated: root.selectedTaskId = currentValue || ""
                contentItem: Text { text: strictTaskCombo.currentText || "暂无绑定任务"; color: root.ink; leftPadding: 6; verticalAlignment: Text.AlignVCenter; elide: Text.ElideMiddle; font.pixelSize: 9 }
                background: Rectangle { color: root.page; border.color: root.line; radius: 3 }
            }

            Text {
                visible: root.strictTasks.length === 0
                Layout.fillWidth: true
                text: root.controller && root.controller.matchPhase === "running"
                    ? "等待指挥战位创建任务" : "网页端启动推演后可创建任务"
                color: root.orange
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Repeater {
                    model: ["发现", "派单", "引导", "打击", "评估", "返航"]
                    delegate: ColumnLayout {
                        id: strictStep
                        required property string modelData
                        required property int index
                        Layout.fillWidth: true
                        spacing: 3
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 4
                            radius: 2
                            color: strictStep.index <= root.strictStepIndex(root.strictTask().stage)
                                ? root.cyan : root.line
                            Behavior on color { ColorAnimation { duration: 180 } }
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: strictStep.modelData
                            color: strictStep.index === root.strictStepIndex(root.strictTask().stage)
                                ? root.ink : root.dim
                            font.pixelSize: 8
                            font.bold: strictStep.index === root.strictStepIndex(root.strictTask().stage)
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 126
                color: root.panelAlt; border.color: root.strictTask().health === "blocked" ? root.orange : root.line; radius: 4
                ColumnLayout { anchors.fill: parent; anchors.margins: 9; spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Text { Layout.fillWidth: true; text: root.stageLabel(root.strictTask().stage); color: root.stageColor(root.strictTask().stage); font.pixelSize: 12; font.bold: true; elide: Text.ElideRight }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "目标  " + (root.strictTask().targetId || "未绑定")
                            + (root.strictTask().route && root.strictTask().route.length > 0
                                ? "  ·  航路 " + root.strictTask().route.length + " 点" : "")
                        color: root.ink; font.pixelSize: 9; elide: Text.ElideMiddle
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.strictTask().health === "blocked"
                            ? "任务受阻 · " + (root.strictTask().blockCode || "BLOCKED")
                            : "当前操作者  " + root.strictRoleLabel(root.strictActionRole(
                                root.strictStageAction(root.strictTask().stage)))
                                + " · " + root.strictActorStatus(root.strictTask())
                        color: root.strictTask().health === "blocked" ? root.orange : root.dim
                        font.pixelSize: 9; elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.strictActionHint(root.strictStageAction(root.strictTask().stage))
                        color: root.dim; font.pixelSize: 9; elide: Text.ElideRight
                    }
                    Button {
                        id: strictActionButton
                        Layout.fillWidth: true
                        property string actionName: root.strictActionFor(root.strictTask())
                        text: actionName.length > 0 ? root.strictActionLabel(actionName)
                            : root.strictActorSeat(root.strictTask()).controlMode === "vmf-auto"
                                ? "服务器自动推进" : root.strictTask().taskId
                                    ? "等待当前战位" : "等待任务"
                        enabled: actionName.length > 0 && !root.strictSubmitting
                            && root.strictTask().stage !== "completed"
                        onClicked: root.advanceStrictTask()
                        contentItem: Text { text: strictActionButton.text; color: strictActionButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 9; font.bold: true }
                        background: Rectangle { color: strictActionButton.enabled ? root.cyan : root.line; radius: 3 }
                    }
                }
            }

            GhostButton {
                text: root.strictDiagnosticsExpanded ? "收起通信诊断" : "通信诊断"
                iconName: "network"
                onClicked: root.strictDiagnosticsExpanded = !root.strictDiagnosticsExpanded
            }
            ColumnLayout {
                visible: root.strictDiagnosticsExpanded
                Layout.fillWidth: true
                spacing: 2
                Text { Layout.fillWidth: true; text: root.strictTrace.traceId ? (root.strictTrace.vmfMessage + " · " + Number(root.strictTrace.wireBitLength || root.strictTrace.bitLength || 0) + " bit · " + (root.strictTrace.roundTripEqual ? "往返一致" : "待校验")) : "暂无 VMF trace"; color: root.strictTrace.roundTripEqual ? root.success : root.dim; font.pixelSize: 9; elide: Text.ElideRight }
                Text { Layout.fillWidth: true; text: root.strictTrace.catalogId ? ("Catalog " + root.strictTrace.catalogId + " · " + Number(root.strictTrace.fieldCount || 0) + " fields") : root.lastCommunicationText(); color: root.dim; font.pixelSize: 8; elide: Text.ElideRight }
            }

            Text { visible: root.notice.length > 0; Layout.fillWidth: true; text: root.notice; color: root.dim; font.pixelSize: 9; wrapMode: Text.WordWrap }
        }
    }

    Rectangle {
        visible: !root.strictProfile
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
