pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: root

    property var controller: null
    property var mapCanvas: null
    property var attacker: ({})
    property var target: ({})
    property var targetOverlayUnit: ({})
    property var points: []
    property int selectedIndex: -1
    property real mapZoom: 0.05
    property var mapCenter: ({x: 10000, y: 7500})
    property color ink: "#eef4f5"
    property color muted: "#91a4a8"
    property color panel: "#162329"
    property color page: "#0c171b"
    property color line: "#2e464c"
    property color accent: "#48d6b0"
    property color warning: "#e5a54a"
    property bool compact: width < 760

    // The dialog is created before the overlay has a final size.  Delay the
    // first fit until the map can actually render, then keep one retry for
    // platforms that settle Popup geometry a frame later.
    Timer {
        id: routeFitTimer
        interval: 40
        repeat: false
        onTriggered: root.fitWhenReady()
    }

    modal: true
    title: "攻击航路编辑"
    parent: Overlay.overlay
    width: Overlay.overlay ? Math.max(340, Math.min(1080, Overlay.overlay.width - 28)) : 960
    height: Overlay.overlay ? Math.max(420, Math.min(760, Overlay.overlay.height - 28)) : 680
    anchors.centerIn: Overlay.overlay
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    background: Rectangle {
        color: root.panel
        border.color: root.accent
        border.width: 1
        radius: 8
    }

    function positionOf(value) {
        var p = value && (value.lockedPosition || value.position)
        if (p && p.length >= 2) return { x: Number(p[0]), y: Number(p[1]) }
        if (p && p.x !== undefined) return { x: Number(p.x), y: Number(p.y) }
        return { x: Number(value && value.x || 0), y: Number(value && value.y || 0) }
    }

    function begin(attackerValue, targetValue) {
        root.attacker = attackerValue || ({})
        root.target = targetValue || ({})
        var a = root.positionOf(root.attacker)
        var t = root.positionOf(root.target)
        var targetId = String(root.target.targetId || root.target.id || "")
        if (!targetId || !isFinite(a.x) || !isFinite(a.y) || !isFinite(t.x) || !isFinite(t.y))
            return false
        root.targetOverlayUnit = targetId && isFinite(t.x) && isFinite(t.y) ? {
            id: targetId,
            targetId: targetId,
            callsign: String(root.target.callsign || root.target.label || targetId),
            kind: "groundtarget",
            side: "blue",
            position: [t.x, t.y, Number((root.target.position || {}).alt || 0)],
            alive: true,
            hp: 100,
            maxHp: 100,
            movable: false,
            collisionRadius: Number(root.target.collisionRadius || 50),
            demoTarget: true,
            targetSnapshot: true
        } : ({})
        root.points = [
            { x: a.x, y: a.y, time: 0, locked: true, kind: "start" },
            { x: t.x, y: t.y, time: 60, locked: true, kind: "target" }
        ]
        root.selectedIndex = -1
        open()
        root.scheduleFit()
        return true
    }

    function editablePoints() {
        var result = []
        for (var i = 0; i < root.points.length; ++i) result.push(root.points[i])
        return result
    }

    function normalizedPoints(source) {
        var result = []
        var previousTime = 0
        for (var i = 0; i < source.length; ++i) {
            var point = source[i] || ({})
            var requestedTime = Number(point.time)
            var time = i === 0 ? 0 : Math.max(i * 30, previousTime + 30,
                                                isFinite(requestedTime) ? requestedTime : 0)
            var copy = {}
            for (var key in point) copy[key] = point[key]
            copy.time = time
            result.push(copy)
            previousTime = time
        }
        return result
    }

    function fitRoute() {
        if (!routeMap || root.points.length < 2) return
        var minX = Number.POSITIVE_INFINITY, minY = Number.POSITIVE_INFINITY
        var maxX = Number.NEGATIVE_INFINITY, maxY = Number.NEGATIVE_INFINITY
        for (var i = 0; i < root.points.length; ++i) {
            var point = root.points[i] || ({})
            var x = Number(point.x), y = Number(point.y)
            if (!isFinite(x) || !isFinite(y)) continue
            minX = Math.min(minX, x); minY = Math.min(minY, y)
            maxX = Math.max(maxX, x); maxY = Math.max(maxY, y)
        }
        if (!isFinite(minX) || !isFinite(minY)) return
        var spanX = Math.max(500, maxX - minX)
        var spanY = Math.max(500, maxY - minY)
        var usableWidth = Math.max(160, routeMap.width - 80)
        var usableHeight = Math.max(160, routeMap.height - 80)
        root.mapZoom = Math.max(0.02, Math.min(4.0,
            Math.min(usableWidth / spanX, usableHeight / spanY)))
        root.mapCenter = ({x: (minX + maxX) / 2, y: (minY + maxY) / 2})
    }

    function fitWhenReady() {
        if (!root.visible || !routeMap || routeMap.width < 80 || routeMap.height < 80) {
            if (root.visible) routeFitTimer.restart()
            return
        }
        root.fitRoute()
        routeMap.refresh()
    }

    function scheduleFit() {
        routeFitTimer.restart()
    }

    onVisibleChanged: {
        if (visible) root.scheduleFit()
        else routeFitTimer.stop()
    }
    onWidthChanged: if (visible) root.scheduleFit()
    onHeightChanged: if (visible) root.scheduleFit()

    function addPoint(point) {
        if (!point || root.points.length < 2) return
        var copy = root.editablePoints()
        var end = copy.pop()
        copy.push({ x: Number(point.x), y: Number(point.y), time: copy.length * 30,
                    locked: false, kind: "waypoint" })
        copy.push(end)
        root.points = root.normalizedPoints(copy)
        root.selectedIndex = copy.length - 2
    }

    function movePoint(index, point) {
        if (!point || index <= 0 || index >= root.points.length - 1) return
        var copy = root.editablePoints()
        copy[index] = { x: Number(point.x), y: Number(point.y),
                        time: Number(copy[index].time || index * 30),
                        locked: false, kind: "waypoint" }
        root.points = root.normalizedPoints(copy)
        root.selectedIndex = index
    }

    function removePoint(index) {
        if (index <= 0 || index >= root.points.length - 1) return
        var copy = root.editablePoints()
        copy.splice(index, 1)
        root.points = root.normalizedPoints(copy)
        root.selectedIndex = Math.min(index, copy.length - 2)
    }

    function focusPoint(index) {
        if (index < 0 || index >= root.points.length) return
        root.selectedIndex = index
        var point = root.points[index]
        if (point) routeMap.focusAt(Number(point.x), Number(point.y))
    }

    function finish() {
        if (root.points.length < 2) return
        root.routeAccepted(root.editablePoints())
        close()
    }

    signal routeAccepted(var points)

    contentItem: ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Icon { name: "locate"; iconSize: 18; iconColor: root.accent }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    Layout.fillWidth: true
                    text: String(root.attacker.callsign || root.attacker.id || "攻击机")
                        + "  →  " + String(root.target.targetId || root.target.id || "目标")
                    color: root.ink
                    font.pixelSize: 13
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: "起点和目标终点已锁定 · 可编辑中继航路点"
                    color: root.muted
                    font.pixelSize: 9
                    elide: Text.ElideRight
                }
            }
            GhostButton {
                text: "全图"
                iconName: "scan"
                textColor: root.muted
                onClicked: root.fitRoute()
            }
        }

        GridLayout {
            id: editorBody
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: root.compact ? 1 : 2
            columnSpacing: 10
            rowSpacing: 10

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: root.compact
                Layout.preferredWidth: root.compact ? -1 : 230
                Layout.minimumWidth: root.compact ? 0 : 210
                Layout.maximumWidth: root.compact ? -1 : 260
                Layout.minimumHeight: root.compact ? 132 : 0
                Layout.alignment: root.compact ? Qt.AlignLeft : Qt.AlignTop
                color: root.page
                border.color: root.line
                radius: 6

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6
                    RowLayout {
                        Layout.fillWidth: true
                        Text { Layout.fillWidth: true; text: "航路点"; color: root.ink; font.pixelSize: 11; font.bold: true }
                        Text { text: root.points.length + " 点"; color: root.muted; font.pixelSize: 8 }
                    }
                    ListView {
                        id: pointsList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 4
                        model: root.points
                        delegate: Rectangle {
                            id: pointDelegate
                            required property var modelData
                            required property int index
                            width: pointsList.width
                            height: 38
                            color: root.selectedIndex === pointDelegate.index ? "#24483f"
                                : pointDelegate.index === 0 || pointDelegate.index === pointsList.count - 1
                                    ? "#1d3835" : "#1b2b31"
                            border.color: root.selectedIndex === pointDelegate.index ? root.accent : root.line
                            radius: 4
                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 6
                                Rectangle {
                                    Layout.preferredWidth: 22
                                    Layout.preferredHeight: 22
                                    radius: 11
                                    color: pointDelegate.index === pointsList.count - 1 ? "#f06b58" : root.accent
                                    Text {
                                        anchors.centerIn: parent
                                        text: pointDelegate.index === 0 ? "起" : pointDelegate.index === pointsList.count - 1 ? "终" : pointDelegate.index
                                        color: "#071014"
                                        font.pixelSize: 8
                                        font.bold: true
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Text {
                                        Layout.fillWidth: true
                                        text: pointDelegate.index === 0 ? "攻击机起点"
                                            : pointDelegate.index === pointsList.count - 1 ? "目标终点" : "中继点 " + pointDelegate.index
                                        color: root.ink
                                        font.pixelSize: 9
                                        font.bold: true
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: Math.round(Number(pointDelegate.modelData.x)) + ", "
                                            + Math.round(Number(pointDelegate.modelData.y)) + " m"
                                        color: root.muted
                                        font.pixelSize: 8
                                        elide: Text.ElideRight
                                    }
                                }
                                GhostButton {
                                    visible: pointDelegate.index > 0 && pointDelegate.index < pointsList.count - 1
                                    text: ""
                                    iconName: "delete"
                                    iconSize: 12
                                    textColor: root.warning
                                    onClicked: root.removePoint(pointDelegate.index)
                                    ToolTip.visible: hovered
                                    ToolTip.text: "删除中继点"
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                z: -1
                                onClicked: root.focusPoint(pointDelegate.index)
                            }
                        }
                    }
                    GhostButton {
                        Layout.fillWidth: true
                        text: "清除中继点"
                        iconName: "delete"
                        textColor: root.muted
                        enabled: root.points.length > 2
                        onClicked: {
                            var a = root.points[0]
                            var b = root.points[root.points.length - 1]
                            root.points = root.normalizedPoints([a, b])
                            root.selectedIndex = -1
                        }
                    }
                }
            }

            Rectangle {
                id: mapPanel
                Layout.fillWidth: true
                Layout.fillHeight: true
                // GridLayout does not infer a useful width for a Rectangle
                // whose content is an anchored item. Without an explicit
                // column size the map column collapses to zero and its
                // overlays are projected at the far-right edge of the dialog.
                Layout.preferredWidth: root.compact ? -1 : 640
                Layout.minimumWidth: root.compact ? 0 : 420
                Layout.minimumHeight: root.compact ? 230 : 0
                color: "#0a1418"
                border.color: root.line
                radius: 6
                onWidthChanged: if (root.visible) root.scheduleFit()
                onHeightChanged: if (root.visible) root.scheduleFit()

                MapCanvas {
                    id: routeMap
                    anchors.fill: parent
                    anchors.margins: 2
                    controller: root.controller
                    mapInfoOverride: root.mapCanvas ? root.mapCanvas.mapConfiguration : null
                    tileCacheDirOverride: root.mapCanvas
                        ? root.mapCanvas.resolvedMapTileCacheDir : ""
                    sideFilter: "red"
                    showAllSides: true
                    visibleUnitIds: null
                    detectedEnemyIds: []
                    overlayUnits: root.targetOverlayUnit && root.targetOverlayUnit.id
                        ? [root.targetOverlayUnit] : []
                    selectedUnitIds: root.targetOverlayUnit && root.targetOverlayUnit.id
                        ? [root.targetOverlayUnit.id] : []
                    actionTargetId: String(root.target.targetId || root.target.id || "")
                    center: root.mapCenter
                    zoom: root.mapZoom
                    showRoutes: true
                    showRecentPaths: false
                    routes: [{ points: root.points, color: root.accent, pendingColor: root.accent }]
                    onClickedMap: function(point) { root.addPoint(point) }

                    Repeater {
                        model: root.points
                        delegate: Item {
                            id: waypointHandle
                            required property var modelData
                            required property int index
                            property var pixel: routeMap.toPixel(Number(modelData.x), Number(modelData.y))
                            x: pixel.x - width / 2
                            y: pixel.y - height / 2
                            width: index === 0 || index === root.points.length - 1 ? 28 : 24
                            height: width
                            z: 40

                            Rectangle {
                                anchors.fill: parent
                                radius: width / 2
                                color: waypointHandle.index === root.points.length - 1 ? "#f06b58" : root.accent
                                border.color: waypointHandle.index === root.selectedIndex ? "#ffffff" : "#071014"
                                border.width: waypointHandle.index === root.selectedIndex ? 2 : 1
                                opacity: waypointHandle.index > 0 && waypointHandle.index < root.points.length - 1 ? 0.96 : 1
                            }
                            Text {
                                anchors.centerIn: parent
                                text: waypointHandle.index === 0 ? "起" : waypointHandle.index === root.points.length - 1 ? "终" : waypointHandle.index
                                color: "#071014"
                                font.pixelSize: 9
                                font.bold: true
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: waypointHandle.index > 0 && waypointHandle.index < root.points.length - 1
                                cursorShape: Qt.OpenHandCursor
                                onPressed: root.selectedIndex = waypointHandle.index
                                onPositionChanged: function(mouse) {
                                    if (!pressed) return
                                    var point = routeMap.logicalFromPixel(waypointHandle.x + mouse.x,
                                                                           waypointHandle.y + mouse.y)
                                    root.movePoint(waypointHandle.index, point)
                                }
                                onReleased: cursorShape = Qt.OpenHandCursor
                            }
                            Text {
                                visible: waypointHandle.index === 0 || waypointHandle.index === root.points.length - 1
                                anchors.left: parent.right
                                anchors.leftMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                text: waypointHandle.index === 0 ? "攻击机" : "目标 · " + String(root.target.targetId || "")
                                color: root.ink
                                font.pixelSize: 9
                                font.bold: waypointHandle.index === root.points.length - 1
                            }
                        }
                    }
                }
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 10
                    implicitWidth: mapHint.implicitWidth + 18
                    implicitHeight: mapHint.implicitHeight + 10
                    color: "#07151add"
                    border.color: root.line
                    radius: 4
                    Text {
                        id: mapHint
                        anchors.centerIn: parent
                        text: "单击地图添加中继点 · 拖动点位微调"
                        color: root.muted
                        font.pixelSize: 8
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: root.points.length > 2 ? "已规划 " + root.points.length + " 个航路点" : "请添加至少一个中继点或直接完成"
                color: root.muted
                font.pixelSize: 9
                elide: Text.ElideRight
            }
            GhostButton {
                text: "取消"
                iconName: "close"
                textColor: root.muted
                onClicked: root.close()
            }
            TonalButton {
                text: "完成"
                iconName: "check"
                base: root.accent
                textColor: "#071014"
                enabled: root.points.length >= 2
                onClicked: root.finish()
            }
        }
    }
}
