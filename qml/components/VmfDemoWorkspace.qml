pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "."

Item {
    id: root
    property var controller: null
    property var mapCanvas: null
    property var demoState: controller ? controller.demoState : ({})
    property var trace: controller ? controller.vmfTrace : ({})
    property bool lightTheme: false
    property int inspectorTab: 0
    property bool advancedXml: false
    property bool requestPending: false
    property int pendingRevision: 0
    property bool scriptEditorOpen: false
    property string notice: ""
    property var selectedTarget: null
    property var selectedPosition: null
    property bool targetPicking: false
    property var highlightedUnitIds: []
    property int seenReportCount: 0
    property real lastGeneration: 0
    readonly property bool compact: width < 420

    readonly property color surface: lightTheme ? "#f7f9fa" : AppContext.panel
    readonly property color raised: lightTheme ? "#ffffff" : AppContext.raised
    readonly property color page: lightTheme ? "#e9eef1" : AppContext.page
    readonly property color ink: lightTheme ? "#172126" : AppContext.text
    readonly property color dim: lightTheme ? "#66747b" : AppContext.muted
    readonly property color line: lightTheme ? "#c9d2d6" : AppContext.line
    readonly property color accent: lightTheme ? "#087f70" : AppContext.signal
    readonly property color warning: lightTheme ? "#a65d08" : AppContext.warning
    readonly property color danger: lightTheme ? "#b63043" : AppContext.danger
    readonly property bool demoProfile: controller
        && controller.protocolProfile === "vmf-demo-v2"
    readonly property bool director: controller
        && (controller.isRoomAdmin
            || ["admin", "room_admin", "director"].indexOf(controller.userRole) >= 0)
    readonly property bool canSubmit: demoProfile && controller && !controller.isObserver
        && controller.currentSeatSide === "red"
        && controller.currentSeatType === String(demoState.activeSeat || "")
        && controller.matchPhase === "running" && demoState.status === "active"
        && String(demoState.expectedAction || "").length > 0
        && String(demoState.expectedAction || "") !== "planRoute"
        && !requestPending
        && root.actionFormValid()
        && (!advancedXml || xmlEditor.text.trim().length > 0)

    visible: demoProfile
    implicitHeight: content.implicitHeight

    function seatLabel(value) {
        return ({ commander: "指挥席", recon: "侦察席", attack: "攻击席",
                  ground: "地面引导席" })[String(value || "")] || "等待流程"
    }

    function actionLabel(value) {
        return ({ reportTarget: "发送目标报告", planRoute: "编辑攻击航路",
                  acceptRoute: "确认攻击航路", issueGuidance: "下达引导命令",
                  acknowledgeGuidance: "确认引导命令",
                  identityHello: "发送身份报告", identityConfirm: "确认身份",
                  sendGuidancePackage: "发送引导包", acceptGuidance: "确认引导包",
                  reportAttackReady: "报告攻击准备就绪", authorizeAttack: "授权攻击",
                  simulateAttack: "执行模拟攻击", reportBattleDamage: "报告模拟战果",
                  confirmDamageAssessment: "确认毁伤评估",
                  confirmTargetDestroyed: "确认目标摧毁", withdraw: "下达返航命令",
                  confirmReturned: "确认返航完成" })[String(value || "")] || "流程已完成"
    }

    function taskTarget() { return (demoState.task || ({})).target || ({}) }
    function targetEffect() { return (demoState.task || ({})).effect || ({}) }
    function pointOf(value) {
        var position = value && value.position
        if (position && position.length >= 2)
            return { x: Number(position[0]), y: Number(position[1]) }
        if (position && position.x !== undefined)
            return { x: Number(position.x), y: Number(position.y) }
        if (value && value.x !== undefined && value.y !== undefined)
            return { x: Number(value.x), y: Number(value.y) }
        return null
    }
    function targetOverlayUnit() {
        var target = root.taskTarget()
        var taskTargetId = String(target.targetId || target.id || "")
        if (!taskTargetId && root.selectedTarget) target = root.selectedTarget
        var point = root.pointOf(target)
        var targetId = String(target.targetId || target.id || "")
        if (!targetId || !point || !isFinite(point.x) || !isFinite(point.y)) return null
        var effect = targetId === taskTargetId ? root.targetEffect() : ({})
        var destroyed = effect.destroyed === true || effect.outcome === "destroyed"
        var damageState = String(effect.damageState || "")
        return {
            id: targetId,
            targetId: targetId,
            callsign: String(target.callsign || target.label || targetId),
            kind: "groundtarget",
            targetType: String(target.targetType || "fixed-ground-target"),
            side: "blue",
            position: [point.x, point.y, Number((target.position || {}).alt || 0)],
            alive: !destroyed,
            hp: destroyed ? 0 : damageState === "damaged" ? 50 : 100,
            maxHp: 100,
            movable: false,
            collisionRadius: Number(target.collisionRadius || 50),
            demoTarget: true,
            targetSnapshot: true
        }
    }
    function targetOverlayUnits() {
        var proxy = root.targetOverlayUnit()
        return proxy ? [proxy] : []
    }
    function targetSelectionForId(unitId) {
        var target = root.taskTarget()
        var targetId = String(target.targetId || target.id || "")
        if (!targetId && root.selectedTarget) {
            target = root.selectedTarget
            targetId = String(target.targetId || target.id || "")
        }
        if (!targetId || String(unitId || "") !== targetId) return null
        var selected = ({})
        for (var key in target) selected[key] = target[key]
        selected.id = targetId
        selected.targetId = targetId
        selected.targetKind = target.targetKind || "entity"
        selected.targetType = target.targetType || "fixed-ground-target"
        return selected
    }
    function demoSeatUnit(seatId) {
        if (!controller) return ({})
        var seats = controller.onlineSeats || []
        for (var i = 0; i < seats.length; ++i) {
            var seat = seats[i] || ({})
            if (String(seat.seatId || "") === String(seatId || ""))
                return controller.unitAt(String(seat.unitId || "")) || ({})
        }
        return ({})
    }
    function routeEndpointsReady() {
        var attack = root.demoSeatUnit("red_attack_1")
        var target = root.taskTarget()
        var attackPoint = root.pointOf(attack)
        var targetPoint = root.pointOf(target)
        return Boolean(attackPoint && targetPoint
                       && isFinite(attackPoint.x) && isFinite(attackPoint.y)
                       && isFinite(targetPoint.x) && isFinite(targetPoint.y)
                       && String(target.targetId || target.id || "").length > 0)
    }
    function reportUnitId() {
        var seats = controller ? (controller.onlineSeats || []) : []
        for (var i = 0; i < seats.length; ++i) {
            var seat = seats[i] || ({})
            if (seat.side === "red" && seat.seatType === "recon" && seat.unitId)
                return String(seat.unitId)
        }
        return "red_recon_1"
    }
    function mapHighlightedUnitIds() {
        var result = (root.highlightedUnitIds || []).slice()
        if (String(root.demoState.expectedAction || "") === "reportTarget") {
            var reconId = root.reportUnitId()
            if (reconId && result.indexOf(reconId) < 0) result.unshift(reconId)
        }
        return result
    }
    function reportList() {
        var source = demoState.reports || []
        var result = []
        for (var i = source.length - 1; i >= 0; --i) result.push(source[i])
        return result
    }
    function selectTarget(target) {
        if (!target) return
        root.selectedTarget = target
        var selectedId = String(target.targetId || target.id || "")
        root.highlightedUnitIds = selectedId.length > 0 ? [selectedId] : []
        root.selectedPosition = null
        root.targetPicking = false
        if (targetIdField) targetIdField.text = selectedId
        var targetPoint = root.pointOf(target)
        if (targetPoint && routeX && routeY) {
            routeX.text = String(targetPoint.x)
            routeY.text = String(targetPoint.y)
        }
        if (targetTypeBox) {
            var kind = String(target.targetType || "")
            var wanted = kind === "groundtarget" ? "fixed-ground-target"
                : kind === "position" ? "position" : kind === "vehicle" ? "vehicle" : "facility"
            for (var i = 0; i < targetTypeBox.model.length; ++i) {
                if (targetTypeBox.model[i].value === wanted) { targetTypeBox.currentIndex = i; break }
            }
        }
        root.notice = "已选择目标 · " + String(target.targetId || target.id || "")
    }
    function selectPosition(point) {
        if (!point) return
        root.selectedTarget = null
        root.highlightedUnitIds = []
        root.selectedPosition = { x: Number(point.x), y: Number(point.y) }
        root.targetPicking = false
        if (targetTypeBox) targetTypeBox.currentIndex = targetTypeBox.model.length - 1
        if (routeX && routeY) {
            routeX.text = String(root.selectedPosition.x)
            routeY.text = String(root.selectedPosition.y)
        }
        root.notice = "已选择位置目标 · " + Math.round(point.x) + ", " + Math.round(point.y)
    }
    function beginTargetPick() {
        root.targetPicking = true
        root.notice = "请在地图上单击敌方侦察目标，或单击空白处提交位置目标"
    }
    function clearTargetSelection() {
        root.selectedTarget = null
        root.selectedPosition = null
        root.highlightedUnitIds = []
        root.targetPicking = false
        if (targetIdField) targetIdField.clear()
        if (routeX) routeX.clear()
        if (routeY) routeY.clear()
    }

    function actionFormValid() {
        var action = String(demoState.expectedAction || "")
        if (action === "reportTarget") {
            return Boolean(root.selectedTarget || root.selectedPosition
                           || (targetIdField && targetIdField.text.trim().length > 0))
        }
        if (action === "confirmTargetDestroyed")
            return Boolean(reportForm && reportForm.valid())
        return true
    }

    function fieldPayload() {
        var action = String(demoState.expectedAction || "")
        var report = reportForm ? reportForm.details() : ({})
        var payload = { reportDetails: report }
        if (report.reportText !== undefined) payload.reportText = report.reportText
        if (action === "reportTarget") {
            if (root.selectedPosition) {
                payload.targetKind = "position"
                payload.position = root.selectedPosition
            } else {
                var selected = root.selectedTarget || ({})
                payload.targetId = String(selected.targetId || selected.id || targetIdField.text.trim())
                payload.intelId = String(selected.intelId || "")
            }
            payload.targetType = targetTypeBox.currentValue || "fixed-ground-target"
        } else if (action === "reportBattleDamage") {
            payload.targetId = String(root.taskTarget().targetId || "")
        } else if (action === "confirmTargetDestroyed") {
            payload.targetId = String(root.taskTarget().targetId || "")
            payload.outcome = report.outcome || "destroyed"
            payload.damageState = report.damageState || "unknown"
            if (report.confidence !== undefined) payload.confidence = report.confidence
            if (report.evidence !== undefined) payload.evidence = report.evidence
            if (report.notes !== undefined) payload.notes = report.notes
        } else {
            payload.targetId = String(root.taskTarget().targetId || "")
        }
        if (action === "reportTarget") {
            var x = Number(routeX.text)
            var y = Number(routeY.text)
            if (isFinite(x) && isFinite(y) && routeX.text.length > 0 && routeY.text.length > 0)
                payload.waypoints = [{ x: x, y: y }]
        }
        return payload
    }

    function submitCurrent() {
        if (!canSubmit) return
        var payload = root.fieldPayload()
        if (advancedXml) payload.xml = xmlEditor.text.trim()
        var result = controller.sendDemoAction({
            action: String(demoState.expectedAction || ""),
            inputMode: advancedXml ? "xml" : "template",
            payload: payload
        })
        root.requestPending = Boolean(result.accepted)
        root.pendingRevision = Number(root.demoState.revision || 0)
        if (root.requestPending) requestTimeout.restart()
        root.notice = result.accepted
            ? "消息已发送，等待服务器确认"
            : String(result.message || "消息发送失败")
    }

    function submitRoute(points) {
        var result = controller.sendDemoAction({
            action: "planRoute", inputMode: "template",
            payload: { targetId: String(root.taskTarget().targetId || ""), route: { points: points } }
        })
        root.requestPending = Boolean(result.accepted)
        root.pendingRevision = Number(root.demoState.revision || 0)
        if (root.requestPending) requestTimeout.restart()
        root.notice = result.accepted ? "航路已提交，等待服务器确认" : String(result.message || "航路提交失败")
    }

    function control(action, payload) {
        if (requestPending) return
        var result = controller.sendDemoControl(action, payload || ({}))
        root.requestPending = Boolean(result.accepted)
        root.pendingRevision = Number(root.demoState.revision || 0)
        if (root.requestPending) requestTimeout.restart()
        root.notice = result.accepted ? "导演控制已提交" : String(result.message || "控制失败")
    }

    function applyTargetScript() {
        try {
            var script = JSON.parse(targetScriptEditor.text)
            if (!script || Array.isArray(script) || typeof script !== "object")
                throw new Error("脚本根节点必须是对象")
            root.control("setTargetScript", { script: script })
        } catch (error) {
            root.notice = "脚本 JSON 无效 · " + String(error.message || error)
        }
    }

    Connections {
        target: root.controller
        function onDemoStateChanged() {
            var generation = Number(root.demoState.generation || 0)
            if (generation !== root.lastGeneration) {
                root.lastGeneration = generation
                root.clearTargetSelection()
            }
            if (!root.requestPending) return
            if (Number(root.demoState.revision || 0) !== root.pendingRevision) {
                root.requestPending = false
                requestTimeout.stop()
                root.notice = "服务器已确认"
            }
        }
        function onErrorForward(message) {
            if (!root.requestPending) return
            root.requestPending = false
            requestTimeout.stop()
            root.notice = String(message || "服务器拒绝了请求")
        }
        function onDemoCommandCompleted() {
            if (!root.requestPending) return
            root.requestPending = false
            requestTimeout.stop()
            root.notice = "服务器已确认"
        }
    }

    Timer {
        id: requestTimeout
        interval: 10000
        repeat: false
        onTriggered: {
            root.requestPending = false
            root.notice = "等待确认超时，请刷新状态后重试"
        }
    }

    ColumnLayout {
        id: content
        width: root.width
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: "演示模式"
                color: root.ink
                font.pixelSize: 15
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            Button {
                id: reportButton
                visible: (root.demoState.reports || []).length > 0
                text: "报告"
                onClicked: reportDrawer.open()
                contentItem: Text { text: reportButton.text; color: root.ink; font.pixelSize: 9; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                Rectangle {
                    visible: root.controller && root.controller.unreadDemoReports > 0
                    width: 8; height: 8; radius: 4; color: root.danger
                    anchors.right: parent.right; anchors.top: parent.top; anchors.rightMargin: -2; anchors.topMargin: -2
                }
            }
            Button {
                id: commanderCancelButton
                visible: root.controller && root.controller.currentSeatType === "commander"
                    && root.demoState.status !== "completed"
                    && root.demoState.status !== "cancelled"
                enabled: !root.requestPending
                text: "终止任务"
                onClicked: root.control("cancel", ({}))
                contentItem: Text { text: commanderCancelButton.text; color: root.danger; font.pixelSize: 9; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { color: Qt.alpha(root.danger, 0.12); border.color: root.danger; radius: 4 }
            }
            Button {
                id: commanderStartButton
                visible: root.controller && root.controller.currentSeatType === "commander"
                    && root.demoState.status === "completed"
                enabled: !root.requestPending
                text: "开始新任务"
                onClicked: root.control("start", ({}))
                contentItem: Text { text: commanderStartButton.text; color: root.accent; font.pixelSize: 9; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { color: Qt.alpha(root.accent, 0.12); border.color: root.accent; radius: 4 }
            }
            Rectangle {
                Layout.preferredWidth: statusText.implicitWidth + 14
                Layout.preferredHeight: 24
                radius: 4
                color: root.demoState.status === "completed" ? Qt.alpha(root.accent, 0.16)
                    : root.demoState.status === "cancelled" ? Qt.alpha(root.danger, 0.16)
                    : root.demoState.status === "paused" ? Qt.alpha(root.warning, 0.16)
                    : root.page
                border.color: root.demoState.status === "cancelled" ? root.danger
                    : root.demoState.status === "paused" ? root.warning : root.line
                Text {
                    id: statusText
                    anchors.centerIn: parent
                    text: root.demoState.status === "completed" ? "已完成"
                        : root.demoState.status === "cancelled" ? "任务已取消"
                        : root.demoState.status === "paused" ? "已暂停" : "进行中"
                    color: root.demoState.status === "cancelled" ? root.danger
                        : root.demoState.status === "paused" ? root.warning : root.accent
                    font.pixelSize: 9
                    font.bold: true
                }
            }
            Switch {
                id: themeSwitch
                checked: root.lightTheme
                Accessible.name: checked ? "切换为战术深色主题" : "切换为演示浅色主题"
                onToggled: root.lightTheme = checked
                ToolTip.visible: hovered
                ToolTip.text: checked ? "演示浅色主题" : "战术深色主题"
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: root.compact ? 1 : 2
            columnSpacing: 6
            rowSpacing: 6
            Repeater {
                model: root.demoState.phases || []
                delegate: Rectangle {
                    id: phaseDelegate
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    Layout.preferredHeight: 42
                    radius: 4
                    color: phaseDelegate.modelData.status === "completed"
                        ? Qt.alpha(root.accent, 0.12) : root.raised
                    border.color: phaseDelegate.modelData.status === "active"
                        || phaseDelegate.modelData.status === "paused" ? root.accent : root.line
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 7
                        spacing: 7
                        Rectangle {
                            Layout.preferredWidth: 21
                            Layout.preferredHeight: 21
                            radius: 3
                            color: phaseDelegate.modelData.status === "completed"
                                ? root.accent : root.page
                            border.color: root.line
                            Text {
                                anchors.centerIn: parent
                                text: phaseDelegate.modelData.status === "completed"
                                    ? "✓" : String(phaseDelegate.index + 1)
                                color: phaseDelegate.modelData.status === "completed"
                                    ? root.page : root.dim
                                font.pixelSize: 9
                                font.bold: true
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: String(phaseDelegate.modelData.title || "")
                            color: phaseDelegate.modelData.status === "pending" ? root.dim : root.ink
                            font.pixelSize: 9
                            font.bold: phaseDelegate.modelData.status !== "pending"
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: actionColumn.implicitHeight + 18
            radius: 5
            color: root.raised
            border.color: root.canSubmit ? root.accent : root.line
            ColumnLayout {
                id: actionColumn
                anchors.fill: parent
                anchors.margins: 9
                spacing: 7
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text { Layout.fillWidth: true; text: root.actionLabel(root.demoState.expectedAction); color: root.ink; font.pixelSize: 12; font.bold: true; elide: Text.ElideRight }
                        Text {
                            Layout.fillWidth: true
                            text: root.seatLabel(root.demoState.activeSeat)
                                + (root.demoState.task && root.demoState.task.strikeAttempt > 0
                                   ? " · 第 " + Number(root.demoState.task.strikeAttempt) + " 次攻击"
                                   : "")
                            color: root.dim
                            font.pixelSize: 9
                            elide: Text.ElideRight
                        }
                    }
                    Text {
                        text: root.canSubmit ? "由我执行" : root.demoState.activeSeat === root.controller.currentSeatType ? "等待开始" : "自动/他席"
                        color: root.canSubmit ? root.accent : root.warning
                        font.pixelSize: 9
                        font.bold: true
                    }
                }
                GridLayout {
                    visible: root.demoState.expectedAction === "reportTarget"
                    Layout.preferredHeight: visible ? implicitHeight : 0
                    Layout.fillWidth: true
                    columns: root.compact ? 1 : 2
                    columnSpacing: 6
                    rowSpacing: 6
                    TextField {
                        id: targetIdField
                        Layout.fillWidth: true
                        placeholderText: "地图选择后自动填充目标 ID"
                        color: root.ink
                        selectByMouse: true
                        background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                    }
                    ComboBox {
                        id: targetTypeBox
                        Layout.fillWidth: true
                        Layout.preferredWidth: root.compact ? -1 : 150
                        model: [
                            { text: "固定地面目标", value: "fixed-ground-target" },
                            { text: "设施目标", value: "facility" },
                            { text: "车辆目标", value: "vehicle" },
                            { text: "位置目标", value: "position" }
                        ]
                        textRole: "text"
                        valueRole: "value"
                        background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                        contentItem: Text {
                            text: targetTypeBox.currentText
                            color: root.ink
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 7
                            elide: Text.ElideRight
                            font.pixelSize: 9
                        }
                    }
                    TextField {
                        id: routeX
                        Layout.fillWidth: true
                        placeholderText: "位置 X"
                        validator: DoubleValidator {}
                        color: root.ink
                        background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                    }
                    TextField {
                        id: routeY
                        Layout.fillWidth: true
                        placeholderText: "位置 Y"
                        validator: DoubleValidator {}
                        color: root.ink
                        background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                    }
                }

                VmfDemoReportForm {
                    id: reportForm
                    visible: root.demoState.expectedAction !== "planRoute"
                    Layout.fillWidth: true
                    action: String(root.demoState.expectedAction || "")
                    compact: root.compact
                    ink: root.ink; muted: root.dim; panel: root.raised; page: root.page
                    line: root.line; accent: root.accent; warning: root.warning; danger: root.danger
                }

                CheckBox {
                    id: advancedXmlCheck
                    visible: root.demoState.expectedAction === "reportTarget"
                    text: "高级 XML 输入"
                    checked: root.advancedXml
                    onToggled: root.advancedXml = checked
                    contentItem: Text { text: advancedXmlCheck.text; color: root.dim; leftPadding: 24; verticalAlignment: Text.AlignVCenter; font.pixelSize: 9 }
                }
                TextArea {
                    id: xmlEditor
                    visible: root.advancedXml && root.demoState.expectedAction === "reportTarget"
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? 112 : 0
                    placeholderText: "粘贴与当前消息类型匹配的 VMF XML"
                    wrapMode: TextEdit.NoWrap
                    color: root.ink
                    font.family: "monospace"
                    font.pixelSize: 9
                    selectByMouse: true
                    background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                }
                Button {
                    id: submitButton
                    visible: root.demoState.expectedAction !== "planRoute"
                    Layout.fillWidth: true
                    enabled: root.canSubmit
                    text: root.requestPending ? "等待服务器确认"
                                  : enabled ? root.actionLabel(root.demoState.expectedAction)
                                  : "等待 " + root.seatLabel(root.demoState.activeSeat)
                    Accessible.name: text
                    onClicked: root.submitCurrent()
                    contentItem: Text { text: submitButton.text; color: submitButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.bold: true; font.pixelSize: 10; elide: Text.ElideRight }
                    background: Rectangle { color: submitButton.enabled ? root.accent : root.line; radius: 4 }
                }
                Text { visible: root.notice.length > 0; Layout.fillWidth: true; text: root.notice; color: root.warning; wrapMode: Text.WordWrap; font.pixelSize: 9 }
                Button {
                    id: pickTargetButton
                    visible: root.demoState.expectedAction === "reportTarget"
                    Layout.fillWidth: true
                    text: root.targetPicking ? "等待地图选择…" : (root.selectedTarget || root.selectedPosition ? "重新选择目标" : "从地图选择目标")
                    enabled: !root.requestPending
                    onClicked: root.beginTargetPick()
                }
                TonalButton {
                    id: routeButton
                    visible: root.demoState.expectedAction === "planRoute"
                    Layout.fillWidth: true
                    enabled: !root.requestPending && root.controller.currentSeatType === "commander"
                        && root.routeEndpointsReady()
                    text: "打开航路编辑器"
                    iconName: "edit"
                    base: root.accent
                    textColor: root.page
                    Accessible.name: "打开攻击航路编辑器"
                    onClicked: {
                        var attack = root.demoSeatUnit("red_attack_1")
                        if (!routeDialog.begin(attack, root.taskTarget()))
                            root.notice = "攻击机或目标位置尚未同步"
                    }
                }
            }
        }

        Rectangle {
            visible: root.director
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? directorColumn.implicitHeight + 16 : 0
            color: root.page
            border.color: root.line
            radius: 4
            ColumnLayout {
                id: directorColumn
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6
                GridLayout {
                    Layout.fillWidth: true
                    columns: root.compact ? 2 : 4
                    columnSpacing: 6
                    rowSpacing: 6
                    ComboBox {
                        id: jumpPhase
                        Layout.fillWidth: true
                        model: root.demoState.phases || []
                        textRole: "title"
                        valueRole: "id"
                    }
                    Button { enabled: !root.requestPending; text: "跳转"; onClicked: root.control("jump", { phase: jumpPhase.currentValue }) }
                    Button { enabled: !root.requestPending; Layout.fillWidth: root.compact; text: root.demoState.status === "paused" ? "继续" : "暂停"; onClicked: root.control(root.demoState.status === "paused" ? "resume" : "pause", ({})) }
                    Button { enabled: !root.requestPending; Layout.fillWidth: root.compact; text: "重置"; onClicked: root.control("reset", ({})) }
                    Button { visible: root.demoState.status !== "completed" && root.demoState.status !== "cancelled" && root.controller.currentSeatType !== "commander"; enabled: !root.requestPending; text: "终止任务"; onClicked: root.control("cancel", ({})) }
                    Button { visible: root.demoState.status === "completed" && root.controller.currentSeatType !== "commander"; enabled: !root.requestPending; text: "开始新任务"; onClicked: root.control("start", ({})) }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { Layout.fillWidth: true; text: "固定靶脚本"; color: root.ink; font.pixelSize: 9; font.bold: true }
                    Text { text: String(root.demoState.targetScriptHash || "").slice(0, 10); color: root.dim; font.family: "monospace"; font.pixelSize: 8 }
                    Button {
                        text: root.scriptEditorOpen ? "收起" : "编辑"
                        onClicked: root.scriptEditorOpen = !root.scriptEditorOpen
                    }
                }
                TextArea {
                    id: targetScriptEditor
                    visible: root.scriptEditorOpen
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? 150 : 0
                    text: '{\n  "version": 1,\n  "targets": [],\n  "timeline": []\n}'
                    wrapMode: TextEdit.NoWrap
                    color: root.ink
                    font.family: "monospace"
                    font.pixelSize: 9
                    selectByMouse: true
                    background: Rectangle { color: root.raised; border.color: root.line; radius: 4 }
                }
                Button {
                    visible: root.scriptEditorOpen
                    enabled: !root.requestPending && targetScriptEditor.text.trim().length > 0
                    Layout.fillWidth: true
                    text: root.requestPending ? "等待服务器确认" : "应用固定靶脚本"
                    onClicked: root.applyTargetScript()
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            Rectangle { anchors.fill: parent; color: root.page; border.color: root.line; radius: 4 }
            RowLayout {
                anchors.fill: parent
                anchors.margins: 2
                spacing: 2
                Repeater {
                    model: ["消息明文", "编码检查", "字段布局"]
                    delegate: Button {
                        id: tabButton
                        required property string modelData
                        required property int index
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        onClicked: root.inspectorTab = tabButton.index
                        contentItem: Text { text: tabButton.modelData; color: root.inspectorTab === tabButton.index ? root.ink : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 9; font.bold: root.inspectorTab === tabButton.index }
                        background: Rectangle { color: root.inspectorTab === tabButton.index ? root.raised : "transparent"; radius: 3 }
                    }
                }
            }
        }

        ScrollView {
            id: plainTextScroll
            visible: root.inspectorTab === 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 170 : 0
            clip: true
            contentWidth: availableWidth
            contentHeight: plainTextArea.contentHeight + 16
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
            TextArea {
                id: plainTextArea
                width: plainTextScroll.availableWidth
                height: Math.max(plainTextScroll.height, plainTextArea.contentHeight + 16)
                readOnly: true
                text: root.trace && root.trace.plainText
                    ? JSON.stringify(root.trace.plainText, null, 2) : "尚无已确认消息"
                color: root.ink
                wrapMode: TextEdit.WrapAnywhere
                font.family: "monospace"
                font.pixelSize: 9
                selectByMouse: true
                background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
            }
        }
        ColumnLayout {
            visible: root.inspectorTab === 1
            Layout.fillWidth: true
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                Text { Layout.fillWidth: true; text: String(root.trace.vmfMessage || "等待编码"); color: root.ink; font.pixelSize: 10; font.bold: true; elide: Text.ElideRight }
                Text { text: Number(root.trace.wireBitLength || 0) + " bit"; color: root.dim; font.pixelSize: 9 }
                Text { text: root.trace.roundTripEqual === true ? "往返一致" : "未校验"; color: root.trace.roundTripEqual === true ? root.accent : root.warning; font.pixelSize: 9; font.bold: true }
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 110
                clip: true
                contentWidth: availableWidth
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                TextArea {
                    width: parent.availableWidth
                    height: Math.max(parent.height, contentHeight + 16)
                    readOnly: true
                    text: String(root.trace.hexPreview || "")
                    color: root.ink
                    wrapMode: TextEdit.WrapAnywhere
                    font.family: "monospace"
                    font.pixelSize: 9
                    selectByMouse: true
                    background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: root.trace.requiresAck ? "ACK 必需" : "无需 ACK"; color: root.trace.requiresAck ? root.warning : root.dim; font.pixelSize: 9 }
                Text { Layout.fillWidth: true; text: root.trace.automaticAck ? "自动回执" : "人工回执"; color: root.accent; font.pixelSize: 9 }
            }
        }
        ListView {
            id: fieldList
            visible: root.inspectorTab === 2
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? Math.min(190, Math.max(40, contentHeight)) : 0
            model: root.trace.fields || []
            clip: true
            spacing: 3
            delegate: Rectangle {
                id: fieldDelegate
                required property var modelData
                width: fieldList.width
                height: 34
                color: root.page
                border.color: root.line
                radius: 3
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 6
                    Text { Layout.fillWidth: true; text: String(fieldDelegate.modelData.path || fieldDelegate.modelData.name || "字段"); color: root.ink; font.family: "monospace"; font.pixelSize: 8; elide: Text.ElideMiddle }
                    Text { text: "@" + Number(fieldDelegate.modelData.bitOffset || 0) + " / " + Number(fieldDelegate.modelData.bits || 0) + "b"; color: root.dim; font.family: "monospace"; font.pixelSize: 8 }
                }
            }
        }
    }

    VmfDemoReportDrawer {
        id: reportDrawer
        reports: root.reportList()
        ink: root.ink; muted: root.dim; panel: root.raised; line: root.line; accent: root.accent
        onOpened: {
            root.seenReportCount = (root.demoState.reports || []).length
            if (root.controller) root.controller.markDemoReportsRead()
        }
    }
    VmfDemoRouteDialog {
        id: routeDialog
        controller: root.controller
        mapCanvas: root.mapCanvas
        ink: root.ink; muted: root.dim; panel: root.raised; line: root.line; accent: root.accent
        onRouteAccepted: function(points) { root.submitRoute(points) }
    }
}
