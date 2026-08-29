pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root

    property string action: ""
    property bool compact: false
    property color ink: AppContext.text
    property color muted: AppContext.muted
    property color panel: AppContext.panel
    property color page: AppContext.page
    property color line: AppContext.line
    property color accent: AppContext.signal
    property color warning: AppContext.warning
    property color danger: AppContext.danger

    readonly property bool visibleForm: action !== "" && action !== "planRoute"
    readonly property bool needsConfidence: [
        "reportTarget", "reportBattleDamage", "confirmDamageAssessment",
        "confirmTargetDestroyed"
    ].indexOf(action) >= 0
    readonly property bool needsDamageState: [
        "reportBattleDamage", "confirmDamageAssessment", "confirmTargetDestroyed"
    ].indexOf(action) >= 0
    readonly property bool needsOutcome: action === "confirmTargetDestroyed"
    // Ground guidance is an operational command, not a choice of transport
    // method.  Its report must not invent or require a guidance type.
    readonly property bool needsContext: action !== "issueGuidance"
    readonly property bool needsFreeText: [
        "reportTarget", "reportBattleDamage", "confirmDamageAssessment",
        "confirmTargetDestroyed"
    ].indexOf(action) >= 0

    function actionTitle() {
        return ({
            reportTarget: "目标观察",
            acceptRoute: "航路确认",
            issueGuidance: "引导指令",
            acknowledgeGuidance: "接收回执",
            identityHello: "身份报告",
            identityConfirm: "身份核验",
            sendGuidancePackage: "引导包",
            acceptGuidance: "攻击接收",
            reportAttackReady: "攻击准备",
            authorizeAttack: "攻击授权",
            simulateAttack: "攻击执行",
            reportBattleDamage: "战果报告",
            confirmDamageAssessment: "毁伤评估",
            confirmTargetDestroyed: "效果确认",
            withdraw: "返航状态",
            confirmReturned: "返航回执"
        })[action] || "任务报告"
    }

    function contextLabel() {
        return ({
            reportTarget: "观测质量",
            acceptRoute: "航路结论",
            issueGuidance: "引导方式",
            acknowledgeGuidance: "接收状态",
            identityHello: "身份状态",
            identityConfirm: "核验结果",
            sendGuidancePackage: "引导包状态",
            acceptGuidance: "接收结论",
            reportAttackReady: "准备状态",
            authorizeAttack: "授权状态",
            simulateAttack: "执行状态",
            reportBattleDamage: "战果状态",
            confirmDamageAssessment: "评估结论",
            confirmTargetDestroyed: "目标效果",
            withdraw: "返航状态",
            confirmReturned: "到达状态"
        })[action] || "报告状态"
    }

    function contextKey() {
        return ({
            reportTarget: "observationQuality",
            acceptRoute: "routeDecision",
            issueGuidance: "guidanceMethod",
            acknowledgeGuidance: "receiptState",
            identityHello: "identityStatus",
            identityConfirm: "identityResult",
            sendGuidancePackage: "packageState",
            acceptGuidance: "acceptance",
            reportAttackReady: "readiness",
            authorizeAttack: "authorization",
            simulateAttack: "executionState",
            reportBattleDamage: "damageState",
            confirmDamageAssessment: "assessment",
            confirmTargetDestroyed: "outcome",
            withdraw: "returnState",
            confirmReturned: "arrivalState"
        })[action] || "status"
    }

    function contextOptions() {
        var options = {
            reportTarget: [
                { text: "清晰可确认", value: "clear" },
                { text: "有限可确认", value: "limited" },
                { text: "需要复核", value: "uncertain" }
            ],
            acceptRoute: [
                { text: "接受航路", value: "accepted" },
                { text: "要求修订", value: "revise" }
            ],
            issueGuidance: [
                { text: "地面引导", value: "ground" },
                { text: "持续引导", value: "continuous" },
                { text: "备用引导", value: "alternate" }
            ],
            acknowledgeGuidance: [
                { text: "已收到", value: "received" },
                { text: "收到但受限", value: "limited" }
            ],
            identityHello: [
                { text: "身份正常", value: "valid" },
                { text: "身份待核验", value: "pending" }
            ],
            identityConfirm: [
                { text: "核验通过", value: "confirmed" },
                { text: "核验失败", value: "rejected" }
            ],
            sendGuidancePackage: [
                { text: "已发送", value: "sent" },
                { text: "备用包", value: "alternate" }
            ],
            acceptGuidance: [
                { text: "接受引导", value: "accepted" },
                { text: "需要澄清", value: "clarify" }
            ],
            reportAttackReady: [
                { text: "已就绪", value: "ready" },
                { text: "部分就绪", value: "limited" },
                { text: "尚未就绪", value: "notReady" }
            ],
            authorizeAttack: [
                { text: "批准攻击", value: "authorized" },
                { text: "附条件批准", value: "conditional" }
            ],
            simulateAttack: [
                { text: "攻击已执行", value: "executed" },
                { text: "执行中止", value: "aborted" }
            ],
            reportBattleDamage: [
                { text: "命中并造成毁伤", value: "damaged" },
                { text: "效果不确定", value: "unknown" },
                { text: "未观察到效果", value: "intact" }
            ],
            confirmDamageAssessment: [
                { text: "评估确认", value: "confirmed" },
                { text: "需要再次核查", value: "review" }
            ],
            confirmTargetDestroyed: [
                { text: "目标已摧毁 · 结束攻击链路", value: "destroyed" },
                { text: "目标未摧毁 · 返回地面引导", value: "notDestroyed" }
            ],
            withdraw: [
                { text: "返航执行", value: "ordered" },
                { text: "等待返航", value: "holding" }
            ],
            confirmReturned: [
                { text: "已安全到达", value: "arrived" },
                { text: "到达但需检查", value: "inspection" }
            ]
        }
        return options[action] || [{ text: "已提交", value: "submitted" }]
    }

    function confidenceOptions() {
        return [
            { text: "高 · 0.90", value: 0.90 },
            { text: "中 · 0.70", value: 0.70 },
            { text: "低 · 0.45", value: 0.45 }
        ]
    }

    function damageOptions() {
        var options = [
            { text: "已损伤", value: "damaged" },
            { text: "完整", value: "intact" },
            { text: "未知", value: "unknown" }
        ]
        if (!root.needsOutcome || contextBox.currentValue === "destroyed")
            options.unshift({ text: "已摧毁", value: "destroyed" })
        return options
    }

    function valid() {
        return root.visibleForm && (!root.needsContext || contextBox.currentValue !== undefined)
            && (!root.needsOutcome || String(contextBox.currentValue || "").length > 0)
    }

    function details() {
        var result = {}
        if (root.needsContext) result[contextKey()] = contextBox.currentValue || "submitted"
        if (root.needsConfidence) result.confidence = Number(confidenceBox.currentValue || 0.7)
        if (root.needsDamageState) {
            result.damageState = damageBox.currentValue || "unknown"
            result.damagePercent = Number(damageSpin.value)
        }
        if (root.needsOutcome && result.outcome === "destroyed") {
            result.damageState = "destroyed"
            result.damagePercent = 100
        }
        if (evidenceField.text.trim().length > 0) result.evidence = evidenceField.text.trim()
        if (notesField.text.trim().length > 0) result.notes = notesField.text.trim()
        if (root.needsFreeText && reportTextField.text.trim().length > 0)
            result.reportText = reportTextField.text.trim()
        return result
    }

    visible: root.visibleForm
    implicitHeight: visible ? formColumn.implicitHeight : 0

    ColumnLayout {
        id: formColumn
        width: root.width
        spacing: 7

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Icon { name: "edit"; iconSize: 14; iconColor: root.accent }
            Text {
                Layout.fillWidth: true
                text: root.actionTitle()
                color: root.ink
                font.pixelSize: 11
                font.bold: true
                elide: Text.ElideRight
            }
            Text { text: "报告字段"; color: root.muted; font.pixelSize: 8 }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: root.compact ? 1 : 2
            columnSpacing: 6
            rowSpacing: 6

            ColumnLayout {
                visible: root.needsContext
                Layout.fillWidth: visible
                spacing: 3
                Text { text: root.contextLabel(); color: root.muted; font.pixelSize: 8 }
                ComboBox {
                    id: contextBox
                    Layout.fillWidth: true
                    model: root.contextOptions()
                    textRole: "text"
                    valueRole: "value"
                    background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                    contentItem: Text {
                        text: contextBox.currentText
                        color: root.ink
                        leftPadding: 7
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        font.pixelSize: 9
                    }
                }
            }

            ColumnLayout {
                visible: root.needsConfidence
                Layout.fillWidth: true
                spacing: 3
                Text { text: "可信度"; color: root.muted; font.pixelSize: 8 }
                ComboBox {
                    id: confidenceBox
                    Layout.fillWidth: true
                    model: root.confidenceOptions()
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: 1
                    background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                    contentItem: Text {
                        text: confidenceBox.currentText
                        color: root.ink
                        leftPadding: 7
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        font.pixelSize: 9
                    }
                }
            }

            ColumnLayout {
                visible: root.needsDamageState
                Layout.fillWidth: true
                spacing: 3
                Text { text: "毁伤状态"; color: root.muted; font.pixelSize: 8 }
                ComboBox {
                    id: damageBox
                    Layout.fillWidth: true
                    model: root.damageOptions()
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: root.needsOutcome && contextBox.currentValue === "destroyed" ? 0 : 1
                    enabled: !root.needsOutcome || contextBox.currentValue === "notDestroyed"
                    background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                    contentItem: Text {
                        text: damageBox.currentText
                        color: root.ink
                        leftPadding: 7
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        font.pixelSize: 9
                    }
                }
            }

            ColumnLayout {
                visible: root.needsDamageState
                Layout.fillWidth: true
                spacing: 3
                Text { text: "毁伤比例"; color: root.muted; font.pixelSize: 8 }
                SpinBox {
                    id: damageSpin
                    Layout.fillWidth: true
                    from: 0
                    to: 100
                    value: 100
                    editable: true
                    textFromValue: function(value, locale) {
                        return Number(value).toLocaleString(locale, "f", 0) + "%"
                    }
                    valueFromText: function(text, locale) {
                        return Number.fromLocaleString(locale, text.replace("%", ""))
                    }
                    background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                }
            }
        }

        TextArea {
            id: reportTextField
            visible: root.needsFreeText
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            placeholderText: root.action === "reportTarget"
                ? "报告明文（可选）：目标轮廓、活动迹象或识别依据"
                : root.action === "confirmTargetDestroyed"
                    ? "复核说明（可选）：现场观察或再次确认依据"
                    : "报告明文（可选）"
            wrapMode: TextEdit.Wrap
            color: root.ink
            selectByMouse: true
            background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
        }

        TextField {
            id: evidenceField
            Layout.fillWidth: true
            visible: root.needsConfidence || root.action === "sendGuidancePackage"
            placeholderText: root.action === "reportTarget" ? "观测依据，例如目标轮廓、活动迹象" : "补充依据或回执编号"
            color: root.ink
            selectByMouse: true
            background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
        }

        TextArea {
            id: notesField
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            placeholderText: "补充说明（可选）"
            wrapMode: TextEdit.Wrap
            color: root.ink
            selectByMouse: true
            background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
        }
    }
}
