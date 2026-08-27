pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    property var controller: null
    property var demoState: controller ? controller.demoState : ({})
    property var trace: controller ? controller.vmfTrace : ({})
    property bool lightTheme: false
    property int inspectorTab: 0
    property bool advancedXml: false
    property bool requestPending: false
    property int pendingRevision: 0
    property bool scriptEditorOpen: false
    property string notice: ""
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
        && !requestPending
        && (!advancedXml || xmlEditor.text.trim().length > 0)

    visible: demoProfile
    implicitHeight: content.implicitHeight

    function seatLabel(value) {
        return ({ commander: "指挥席", recon: "侦察席", attack: "攻击席",
                  ground: "地面引导席" })[String(value || "")] || "等待流程"
    }

    function actionLabel(value) {
        return ({ reportTarget: "发送目标报告", planRoute: "提交攻击航路",
                  acceptRoute: "确认攻击航路", issueGuidance: "下达引导命令",
                  acknowledgeGuidance: "确认引导命令",
                  confirmGroundGuidance: "完成地面引导", reportDamage: "上报毁伤结果",
                  confirmDestroyed: "确认目标摧毁", orderReturn: "下达返航命令",
                  confirmReturned: "确认返航完成" })[String(value || "")] || "流程已完成"
    }

    function fieldPayload() {
        var payload = {
            targetId: targetIdField.text.trim(),
            targetType: targetTypeBox.currentValue || "fixed-ground-target",
            reportText: reportText.text.trim(),
            damage: Number(damageSpin.value)
        }
        var x = Number(routeX.text)
        var y = Number(routeY.text)
        if (isFinite(x) && isFinite(y) && routeX.text.length > 0 && routeY.text.length > 0)
            payload.waypoints = [{ x: x, y: y }]
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
            Rectangle {
                Layout.preferredWidth: statusText.implicitWidth + 14
                Layout.preferredHeight: 24
                radius: 4
                color: root.demoState.status === "completed" ? Qt.alpha(root.accent, 0.16)
                    : root.demoState.status === "paused" ? Qt.alpha(root.warning, 0.16)
                    : root.page
                border.color: root.demoState.status === "paused" ? root.warning : root.line
                Text {
                    id: statusText
                    anchors.centerIn: parent
                    text: root.demoState.status === "completed" ? "已完成"
                        : root.demoState.status === "paused" ? "已暂停" : "进行中"
                    color: root.demoState.status === "paused" ? root.warning : root.accent
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
                        Text { Layout.fillWidth: true; text: root.seatLabel(root.demoState.activeSeat) + " · 版本 " + Number(root.demoState.revision || 0); color: root.dim; font.pixelSize: 9; elide: Text.ElideRight }
                    }
                    Text {
                        text: root.canSubmit ? "由我执行" : root.demoState.activeSeat === root.controller.currentSeatType ? "等待开始" : "自动/他席"
                        color: root.canSubmit ? root.accent : root.warning
                        font.pixelSize: 9
                        font.bold: true
                    }
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: root.compact ? 1 : 2
                    columnSpacing: 6
                    rowSpacing: 6
                    TextField {
                        id: targetIdField
                        Layout.fillWidth: true
                        placeholderText: "固定靶 ID（留空自动选择）"
                        color: root.ink
                        selectByMouse: true
                        background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                    }
                    ComboBox {
                        id: targetTypeBox
                        Layout.fillWidth: root.compact
                        Layout.preferredWidth: root.compact ? -1 : 126
                        model: [
                            { text: "固定地面目标", value: "fixed-ground-target" },
                            { text: "设施目标", value: "facility" },
                            { text: "车辆目标", value: "vehicle" }
                        ]
                        textRole: "text"
                        valueRole: "value"
                        background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                        contentItem: Text { text: targetTypeBox.currentText; color: root.ink; verticalAlignment: Text.AlignVCenter; leftPadding: 6; elide: Text.ElideRight; font.pixelSize: 9 }
                    }
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: root.compact ? 2 : 3
                    columnSpacing: 6
                    rowSpacing: 6
                    TextField {
                        id: routeX
                        Layout.fillWidth: true
                        placeholderText: "航点 X"
                        validator: DoubleValidator {}
                        color: root.ink
                        background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                    }
                    TextField {
                        id: routeY
                        Layout.fillWidth: true
                        placeholderText: "航点 Y"
                        validator: DoubleValidator {}
                        color: root.ink
                        background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                    }
                    SpinBox {
                        id: damageSpin
                        from: 0
                        to: 100
                        value: 100
                        editable: true
                        Layout.columnSpan: root.compact ? 2 : 1
                        Layout.fillWidth: root.compact
                        Layout.preferredWidth: root.compact ? -1 : 86
                        Accessible.name: "毁伤百分比"
                        textFromValue: function(value, locale) { return Number(value).toLocaleString(locale, 'f', 0) + "%" }
                        valueFromText: function(text, locale) { return Number.fromLocaleString(locale, text.replace("%", "")) }
                    }
                }
                TextArea {
                    id: reportText
                    Layout.fillWidth: true
                    Layout.preferredHeight: 58
                    placeholderText: "报告内容（可选择后补充明文说明）"
                    wrapMode: TextEdit.Wrap
                    color: root.ink
                    selectByMouse: true
                    background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                }
                CheckBox {
                    id: advancedXmlCheck
                    text: "高级 XML 输入"
                    checked: root.advancedXml
                    onToggled: root.advancedXml = checked
                    contentItem: Text { text: advancedXmlCheck.text; color: root.dim; leftPadding: 24; verticalAlignment: Text.AlignVCenter; font.pixelSize: 9 }
                }
                TextArea {
                    id: xmlEditor
                    visible: root.advancedXml
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

        TextArea {
            visible: root.inspectorTab === 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 150 : 0
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
            TextArea {
                Layout.fillWidth: true
                Layout.preferredHeight: 90
                readOnly: true
                text: String(root.trace.hexPreview || "")
                color: root.ink
                wrapMode: TextEdit.WrapAnywhere
                font.family: "monospace"
                font.pixelSize: 9
                selectByMouse: true
                background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
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
}
