pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: root
    property var controller: null
    property var attacker: ({})
    property var target: ({})
    property var points: []
    property color ink: "#eef4f5"
    property color muted: "#91a4a8"
    property color panel: "#162329"
    property color line: "#2e464c"
    property color accent: "#48d6b0"
    modal: true
    title: "攻击航路编辑"
    width: Math.min(920, parent ? parent.width - 20 : 920)
    height: Math.min(600, parent ? parent.height - 20 : 600)
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    background: Rectangle { color: root.panel; border.color: root.line; radius: 6 }

    function positionOf(value) {
        var p = value && value.position
        if (p && p.length >= 2) return { x: Number(p[0]), y: Number(p[1]) }
        if (p && p.x !== undefined) return { x: Number(p.x), y: Number(p.y) }
        return { x: Number(value && value.x || 0), y: Number(value && value.y || 0) }
    }
    function begin(attackerValue, targetValue) {
        root.attacker = attackerValue || ({})
        root.target = targetValue || ({})
        var a = root.positionOf(root.attacker)
        var t = root.positionOf(root.target)
        root.points = [{ x: a.x, y: a.y, time: 0, locked: true },
                       { x: t.x, y: t.y, time: 60, locked: true }]
        open()
        Qt.callLater(function() {
            if (!routeMap) return
            routeMap.refresh()
            routeMap.focusAt((a.x + t.x) / 2, (a.y + t.y) / 2)
        })
    }
    function editablePoints() {
        var result = []
        for (var i = 0; i < root.points.length; ++i) result.push(root.points[i])
        return result
    }
    function addPoint(point) {
        if (!point || root.points.length < 2) return
        var copy = editablePoints()
        var end = copy.pop()
        copy.push({ x: Number(point.x), y: Number(point.y), time: copy.length * 30 })
        copy.push(end)
        root.points = copy
    }
    function finish() {
        if (root.points.length < 2) return
        root.routeAccepted(root.editablePoints())
        close()
    }
    signal routeAccepted(var points)

    contentItem: ColumnLayout {
        anchors.margins: 12
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "起点 " + String(root.attacker.callsign || root.attacker.id || "攻击机") + "  →  " + String(root.target.targetId || root.target.id || "目标"); color: root.ink; font.pixelSize: 12; font.bold: true }
            Text { text: "首尾点已锁定"; color: root.accent; font.pixelSize: 9 }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8
            Rectangle {
                Layout.preferredWidth: 190
                Layout.fillHeight: true
                color: "#101a1f"; border.color: root.line; radius: 4
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 8; spacing: 5
                    Text { text: "航路点"; color: root.ink; font.pixelSize: 11; font.bold: true }
                    ListView {
                        id: pointsList
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 4
                        model: root.points
                        delegate: Rectangle {
                            id: pointDelegate
                            required property var modelData
                            required property int index
                            width: pointsList.width; height: 34
                            color: index === 0 || index === pointsList.count - 1 ? "#203d3a" : "#1b2b31"
                            border.color: root.line; radius: 3
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 5; spacing: 4
                                Text { Layout.fillWidth: true; text: (pointDelegate.index === 0 ? "起点" : pointDelegate.index === pointsList.count - 1 ? "终点" : "中继") + "  " + Math.round(pointDelegate.modelData.x) + ", " + Math.round(pointDelegate.modelData.y); color: root.ink; font.pixelSize: 9; elide: Text.ElideRight }
                                Button { visible: pointDelegate.index > 0 && pointDelegate.index < pointsList.count - 1; text: "×"; onClicked: { var copy = root.editablePoints(); copy.splice(pointDelegate.index, 1); root.points = copy } }
                            }
                        }
                    }
                    Button { Layout.fillWidth: true; text: "清除中继点"; enabled: root.points.length > 2; onClicked: { var a = root.points[0]; var b = root.points[root.points.length - 1]; root.points = [a, b] } }
                }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true
                color: "#0c171b"; border.color: root.line; radius: 4
                MapCanvas {
                    id: routeMap
                    anchors.fill: parent; anchors.margins: 2
                    controller: root.controller
                    sideFilter: "red"
                    showAllSides: true
                    zoom: 0.05
                    showRoutes: true
                    routes: [{ points: root.points, color: root.accent, pendingColor: root.accent }]
                    onClickedMap: function(point) { root.addPoint(point) }
                    Rectangle {
                        property var markerPoint: root.points.length > 0
                            ? routeMap.toPixel(root.points[0].x, root.points[0].y)
                            : ({ x: -100, y: -100 })
                        x: markerPoint.x - 7; y: markerPoint.y - 7
                        width: 14; height: 14; radius: 7
                        color: root.accent; border.color: "#071014"; border.width: 2
                        z: 30
                        Text { anchors.left: parent.right; anchors.leftMargin: 5; anchors.verticalCenter: parent.verticalCenter; text: "攻击机"; color: root.ink; font.pixelSize: 9 }
                    }
                    Rectangle {
                        property var markerPoint: root.points.length > 1
                            ? routeMap.toPixel(root.points[root.points.length - 1].x,
                                               root.points[root.points.length - 1].y)
                            : ({ x: -100, y: -100 })
                        x: markerPoint.x - 7; y: markerPoint.y - 7
                        width: 14; height: 14; radius: 2
                        color: "#f06b58"; border.color: "#071014"; border.width: 2
                        z: 30
                        Text { anchors.left: parent.right; anchors.leftMargin: 5; anchors.verticalCenter: parent.verticalCenter; text: "目标"; color: root.ink; font.pixelSize: 9 }
                    }
                }
                Text { anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 10; text: "单击地图添加中继点"; color: root.muted; font.pixelSize: 9 }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button { text: "取消"; onClicked: root.close() }
            Button { text: "完成航路"; enabled: root.points.length >= 2; onClicked: root.finish() }
        }
    }
}
