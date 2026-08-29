pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: root

    property var reports: []
    property color ink: "#eef4f5"
    property color muted: "#91a4a8"
    property color panel: "#162329"
    property color page: "#0c171b"
    property color line: "#2e464c"
    property color accent: "#48d6b0"
    property color warning: "#e5a54a"

    modal: true
    title: "任务报告详情"
    parent: Overlay.overlay
    width: Overlay.overlay ? Math.max(360, Math.min(720, Overlay.overlay.width - 28)) : 660
    height: Overlay.overlay ? Math.max(420, Math.min(760, Overlay.overlay.height - 28)) : 660
    anchors.centerIn: Overlay.overlay
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: root.panel
        border.color: root.line
        radius: 8
    }

    function actionLabel(action) {
        return ({
            reportTarget: "目标报告",
            planRoute: "攻击航路",
            acceptRoute: "航路确认",
            issueGuidance: "下达引导",
            acknowledgeGuidance: "确认引导",
            identityHello: "身份报告",
            identityConfirm: "身份确认",
            sendGuidancePackage: "发送引导包",
            acceptGuidance: "接收引导包",
            reportAttackReady: "攻击准备",
            authorizeAttack: "攻击授权",
            simulateAttack: "模拟攻击",
            reportBattleDamage: "战果报告",
            confirmDamageAssessment: "毁伤评估",
            confirmTargetDestroyed: "效果确认",
            withdraw: "返航命令",
            confirmReturned: "返航确认"
        })[String(action || "")] || "流程报告"
    }

    function seatLabel(seat) {
        return ({ commander: "指挥席", recon: "侦察席", attack: "攻击席", ground: "地面引导席" })[
            String(seat || "")] || "未知战位"
    }

    function outcomeLabel(report) {
        var details = (report || {}).details || ({})
        var outcome = String(details.outcome || "")
        if (outcome === "notDestroyed") return "目标未摧毁"
        if (outcome === "destroyed") return "目标已摧毁"
        var damageState = String(details.damageState || details.status || "")
        if (damageState === "damaged") return "目标已损伤"
        if (damageState === "intact") return "目标仍完整"
        return "已提交"
    }

    function targetSummary(report) {
        var details = (report || {}).details || ({})
        var target = details.target || ({})
        var id = String(target.targetId || details.targetId || "")
        var position = target.position || details.position || ({})
        var result = id ? "目标 " + id : "未指定目标"
        if (position.x !== undefined && position.y !== undefined)
            result += " · " + Math.round(Number(position.x)) + ", " + Math.round(Number(position.y)) + " m"
        var route = details.route && details.route.points ? details.route.points : details.waypoints
        if (route && route.length > 1) result += " · " + route.length + " 个航路点"
        return result
    }

    function reportText(report) {
        var details = (report || {}).details || ({})
        var lines = []
        if (details.reportDetails) {
            var form = details.reportDetails
            for (var key in form) {
                if (form[key] === undefined || form[key] === null || String(form[key]).length === 0) continue
                lines.push(String(key) + ": " + String(form[key]))
            }
        }
        if (details.evidence) lines.push("依据: " + String(details.evidence))
        if (details.notes) lines.push("备注: " + String(details.notes))
        if (details.reportText) lines.push(String(details.reportText))
        return lines.join("\n")
    }

    contentItem: ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Icon { name: "history"; iconSize: 18; iconColor: root.accent }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text { Layout.fillWidth: true; text: "任务报告详情"; color: root.ink; font.pixelSize: 14; font.bold: true }
                Text { Layout.fillWidth: true; text: root.reports.length + " 条已确认记录"; color: root.muted; font.pixelSize: 9; elide: Text.ElideRight }
            }
            GhostButton { text: "关闭"; iconName: "close"; textColor: root.muted; onClicked: root.close() }
        }

        ScrollView {
            id: reportScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ListView {
                id: list
                width: reportScroll.availableWidth
                model: root.reports
                spacing: 7
                clip: true
                delegate: Rectangle {
                    id: reportDelegate
                    required property var modelData
                    required property int index
                    property bool expanded: false
                    width: list.width
                    implicitHeight: reportDetails.implicitHeight + 18
                    color: reportDelegate.expanded ? "#1e3438" : "#1a2a30"
                    border.color: reportDelegate.expanded ? root.accent : root.line
                    radius: 6

                    ColumnLayout {
                        id: reportDetails
                        anchors.fill: parent
                        anchors.margins: 9
                        spacing: 5
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 7
                            Rectangle {
                                Layout.preferredWidth: 24
                                Layout.preferredHeight: 24
                                radius: 12
                                color: root.accent
                                Text { anchors.centerIn: parent; text: reportDelegate.index + 1; color: "#071014"; font.pixelSize: 9; font.bold: true }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Text {
                                    Layout.fillWidth: true
                                    text: root.actionLabel(reportDelegate.modelData.action)
                                    color: root.ink
                                    font.pixelSize: 11
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.seatLabel(reportDelegate.modelData.seatType)
                                        + " · " + String(reportDelegate.modelData.phaseTitle || "")
                                    color: root.muted
                                    font.pixelSize: 8
                                    elide: Text.ElideRight
                                }
                            }
                            Rectangle {
                                Layout.preferredWidth: outcomeText.implicitWidth + 14
                                Layout.preferredHeight: 22
                                radius: 11
                                color: root.outcomeLabel(reportDelegate.modelData) === "目标未摧毁"
                                    ? "#5b472c" : "#23453d"
                                Text {
                                    id: outcomeText
                                    anchors.centerIn: parent
                                    text: root.outcomeLabel(reportDelegate.modelData)
                                    color: root.outcomeLabel(reportDelegate.modelData) === "目标未摧毁"
                                        ? root.warning : root.accent
                                    font.pixelSize: 8
                                    font.bold: true
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root.targetSummary(reportDelegate.modelData)
                            color: root.ink
                            font.pixelSize: 9
                            wrapMode: Text.WordWrap
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: "第 " + Number(reportDelegate.modelData.strikeAttempt || 0) + " 次攻击"
                                color: root.muted
                                font.pixelSize: 8
                            }
                            Text {
                                text: reportDelegate.modelData.createdAt !== undefined
                                    ? Number(reportDelegate.modelData.createdAt).toFixed(1) + " s" : ""
                                color: root.muted
                                font.pixelSize: 8
                            }
                            GhostButton {
                                text: reportDelegate.expanded ? "收起" : "详情"
                                iconName: reportDelegate.expanded ? "chevron-down" : "chevron-right"
                                iconSize: 11
                                textColor: root.accent
                                onClicked: reportDelegate.expanded = !reportDelegate.expanded
                            }
                        }
                        Text {
                            visible: reportDelegate.expanded && root.reportText(reportDelegate.modelData).length > 0
                            Layout.fillWidth: true
                            text: root.reportText(reportDelegate.modelData)
                            color: root.ink
                            font.pixelSize: 9
                            wrapMode: Text.WordWrap
                        }
                        ScrollView {
                            visible: reportDelegate.expanded
                            Layout.fillWidth: true
                            Layout.preferredHeight: visible ? 96 : 0
                            clip: true
                            contentWidth: availableWidth
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            TextArea {
                                width: parent.availableWidth
                                height: Math.max(parent.height, contentHeight + 12)
                                readOnly: true
                                text: JSON.stringify(reportDelegate.modelData.details || ({}), null, 2)
                                color: root.ink
                                wrapMode: TextEdit.WrapAnywhere
                                font.family: "monospace"
                                font.pixelSize: 8
                                selectByMouse: true
                                background: Rectangle { color: root.page; border.color: root.line; radius: 4 }
                            }
                        }
                        Text {
                            visible: reportDelegate.expanded
                            Layout.fillWidth: true
                            text: String(reportDelegate.modelData.reportId || "")
                            color: root.muted
                            font.family: "monospace"
                            font.pixelSize: 7
                            elide: Text.ElideMiddle
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        z: -1
                        onClicked: reportDelegate.expanded = !reportDelegate.expanded
                    }
                }
            }
        }
    }
}
