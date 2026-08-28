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
    property color line: "#2e464c"
    property color accent: "#48d6b0"
    modal: true
    title: "任务报告详情"
    width: Math.min(460, parent ? parent.width - 24 : 460)
    height: Math.min(620, parent ? parent.height - 24 : 620)
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    background: Rectangle { color: root.panel; border.color: root.line; radius: 6 }

    function detailSummary(report) {
        var details = (report || {}).details || ({})
        var target = details.target || ({})
        var position = target.position || details.position || ({})
        var items = []
        var id = String(target.targetId || details.targetId || "")
        if (id) items.push((target.targetKind === "position" ? "位置" : "目标") + " " + id)
        var type = String(target.targetType || details.targetType || "")
        if (type) items.push("类型 " + type)
        if (position.x !== undefined && position.y !== undefined)
            items.push("坐标 " + Math.round(Number(position.x)) + ", " + Math.round(Number(position.y)))
        var points = details.route && details.route.points ? details.route.points
            : details.waypoints
        if (points && points.length > 1) items.push("航点 " + points.length)
        if (details.damage !== undefined) items.push("毁伤 " + Number(details.damage).toFixed(0) + "%")
        if (details.reportText) items.push(String(details.reportText))
        return items.length > 0 ? items.join(" · ") : "已完成此流程动作"
    }

    contentItem: ColumnLayout {
        anchors.margins: 14
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "已接收 " + root.reports.length + " 条流程报告"; color: root.ink; font.pixelSize: 13; font.bold: true }
            Button { text: "关闭"; onClicked: root.close() }
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: root.reports
            delegate: Rectangle {
                id: reportDelegate
                required property var modelData
                width: list.width
                implicitHeight: details.implicitHeight + 16
                color: "#1d2d33"
                border.color: root.line
                radius: 4
                ColumnLayout {
                    id: details
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 3
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: String(reportDelegate.modelData.action || "报告"); color: root.accent; font.pixelSize: 11; font.bold: true }
                        Text { Layout.fillWidth: true; text: String(reportDelegate.modelData.seatType || ""); color: root.muted; font.pixelSize: 9; elide: Text.ElideRight }
                        Text { text: reportDelegate.modelData.createdAt !== undefined ? Number(reportDelegate.modelData.createdAt).toFixed(1) + " s" : ""; color: root.muted; font.pixelSize: 9 }
                    }
                    Text { Layout.fillWidth: true; text: String(reportDelegate.modelData.reportId || ""); color: root.muted; font.family: "monospace"; font.pixelSize: 8; elide: Text.ElideRight }
                    Text {
                        Layout.fillWidth: true
                        text: root.detailSummary(reportDelegate.modelData)
                        color: root.ink; font.pixelSize: 10; wrapMode: Text.WordWrap
                    }
                }
            }
            Text { anchors.centerIn: parent; visible: list.count === 0; text: "暂无报告"; color: root.muted; font.pixelSize: 10 }
        }
    }
}
