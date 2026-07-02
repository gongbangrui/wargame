import QtQuick
import QtQuick.Controls.Basic

Item {
    id: root
    property string sideFilter: "red"
    property bool   showAllSides: false
    property string focusUnitId: ""
    property bool showAllUnits: true
    property bool showFriendly: true
    property bool showEnemy: true
    property bool showDetectRange: true
    property bool showCommRange: true
    property bool showAttackRange: true
    property bool showRoutes: true
    property double simTime: 0.0
    property var discoveryUnits: ([])
    property bool routePlanningMode: false
    property var plannedWaypoints: ([])

    signal clickedMap(var logicalPos)
    signal rightClickedMap(var logicalPos)
    signal unitClicked(string unitId, var button)
    signal routePointAdded(var logicalPos)

    property double zoom: 1.0
    property var center: ({x: 20000, y: 15000})
    property var mapSize: ({w: 40000, h: 30000})
    property var routes: []

    // 公共刷新方法
    function refresh() { innerCanvas.requestPaint() }
    onSideFilterChanged: refresh()
    onShowAllSidesChanged: refresh()
    onRoutesChanged: refresh()
    onPlannedWaypointsChanged: refresh()
    onShowRoutesChanged: refresh()
    onShowDetectRangeChanged: refresh()
    onShowAttackRangeChanged: refresh()
    onShowCommRangeChanged: refresh()

    QtObject {
        id: t
        property color bg: "#0a1428"
        property color grid: "#1d3252"
        property color land: "#102039"
        property color label: "#ffffff"
        property color labelShadow: "#000000"
        property color red: "#ff5566"
        property color blue: "#4d9bff"
        property color dead: "#6c7280"
        property color focus: "#ffd23f"
        property color detect: "#62b4ff"
        property color comm: "#8593a8"
        property color attack: "#ff7a59"
        property color route: "#46d29a"
        property color routePending: "#8a93a6"
        property color enemy: "#ff7a59"
        property color alertBg: "#ffb24d"
    }

    function logicalFromPixel(px, py) {
        return { x: center.x + (px - width/2) / zoom, y: center.y - (py - height/2) / zoom }
    }
    function toPixel(lx, ly) {
        return Qt.point(width/2 + (lx - center.x) * zoom, height/2 - (ly - center.y) * zoom)
    }
    function isVisible(u) {
        if (!u || !u.alive) return false
        if (showAllSides) return true
        return u.side === sideFilter
    }
    function centerOn(lx, ly) {
        root.center = ({x: lx, y: ly})
        refresh()
    }

    Rectangle { anchors.fill: parent; color: t.bg; border.color: "#3a455a" }

    Canvas {
        id: innerCanvas
        anchors.fill: parent
        property var units: []
        property var detections: []
        property var enemyDetections: []

        Connections {
            target: controller
            function onUnitsForward() { innerCanvas.units = controller.units; innerCanvas.requestPaint() }
        }

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.fillStyle = t.land
            ctx.fillRect(0, 0, width, height)
            ctx.strokeStyle = t.grid
            ctx.lineWidth = 1
            var step = 2000 * root.zoom
            if (step < 8) step = 8
            var offX = ((root.center.x * root.zoom) % step + step) % step
            var offY = ((root.center.y * root.zoom) % step + step) % step
            for (var x = -offX; x < width; x += step) {
                ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, height); ctx.stroke()
            }
            for (var y = offY; y < height; y += step) {
                ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke()
            }
            ctx.font = "bold 12px sans-serif"
            ctx.fillStyle = "rgba(255,255,255,0.7)"
            ctx.fillText("画布 " + Math.round(root.mapSize.w/1000) + " km × " + Math.round(root.mapSize.h/1000) + " km", 12, 18)
            ctx.fillStyle = "rgba(255,255,255,0.4)"
            ctx.font = "11px sans-serif"
            ctx.fillText("缩放 " + root.zoom.toFixed(2) + "x", 12, 36)

            // 绘制路径：已完成（实线）+ 未完成（虚线）
            if (root.showRoutes) {
                for (var ri = 0; ri < root.routes.length; ri++) {
                    var route = root.routes[ri]
                    if (!route || !route.points || route.points.length < 2) continue
                    var completeColor = route.color || t.route
                    var pendingColor = route.pendingColor || t.routePending

                    for (var si = 0; si < route.points.length - 1; si++) {
                        var segStart = route.points[si]
                        var segEnd = route.points[si + 1]
                        var segCompleted = (segEnd.time <= root.simTime)

                        ctx.strokeStyle = segCompleted ? completeColor : pendingColor
                        ctx.lineWidth = segCompleted ? 2.5 : 1.5
                        if (!segCompleted) ctx.setLineDash([6, 4])
                        else ctx.setLineDash([])

                        var ps = root.toPixel(segStart.x, segStart.y)
                        var pe = root.toPixel(segEnd.x, segEnd.y)
                        ctx.beginPath()
                        ctx.moveTo(ps.x, ps.y)
                        ctx.lineTo(pe.x, pe.y)
                        ctx.stroke()
                        ctx.setLineDash([])
                    }

                    for (var pj = 0; pj < route.points.length; pj++) {
                        var pt = route.points[pj]
                        var pp = root.toPixel(pt.x, pt.y)
                        var ptCompleted = (pt.time <= root.simTime)
                        ctx.fillStyle = ptCompleted ? (route.color || t.route) : pendingColor
                        ctx.beginPath(); ctx.arc(pp.x, pp.y, 4, 0, Math.PI*2); ctx.fill()
                        ctx.fillStyle = "#fff"
                        ctx.font = "9px sans-serif"
                        ctx.fillText("t=" + (pt.time).toFixed(0), pp.x + 6, pp.y - 4)
                    }
                }
            }

            // 绘制规划中的航路点
            if (root.routePlanningMode && root.plannedWaypoints.length > 0) {
                ctx.strokeStyle = "#46d29a"
                ctx.lineWidth = 2
                ctx.setLineDash([5, 5])
                ctx.beginPath()
                for (var wi = 0; wi < root.plannedWaypoints.length; wi++) {
                    var wp = root.plannedWaypoints[wi]
                    var wpp = root.toPixel(wp.x, wp.y)
                    if (wi === 0) ctx.moveTo(wpp.x, wpp.y)
                    else ctx.lineTo(wpp.x, wpp.y)
                }
                ctx.stroke()
                ctx.setLineDash([])
                for (var wj = 0; wj < root.plannedWaypoints.length; wj++) {
                    var wpt = root.plannedWaypoints[wj]
                    var wpp2 = root.toPixel(wpt.x, wpt.y)
                    ctx.fillStyle = "#46d29a"
                    ctx.beginPath(); ctx.arc(wpp2.x, wpp2.y, 6, 0, Math.PI*2); ctx.fill()
                    ctx.fillStyle = "#000"
                    ctx.font = "bold 10px sans-serif"
                    ctx.fillText("" + (wj + 1), wpp2.x - 3, wpp2.y + 4)
                }
            }

            for (var i = 0; i < innerCanvas.units.length; i++) {
                var u = innerCanvas.units[i]
                var visible = root.isVisible(u)
                var dead = !u.alive
                var p = root.toPixel(u.position[0], u.position[1])

                if (visible && root.showDetectRange && u.detectRange > 0) {
                    ctx.save()
                    ctx.strokeStyle = "#4d9bff"
                    ctx.lineWidth = 1.5
                    ctx.setLineDash([8, 6])
                    ctx.beginPath()
                    ctx.arc(p.x, p.y, Math.max(2, u.detectRange * root.zoom), 0, Math.PI*2)
                    ctx.stroke()
                    ctx.setLineDash([])
                    // 半透明填充
                    ctx.fillStyle = "rgba(77,155,255,0.06)"
                    ctx.fill()
                    ctx.restore()
                }
                if (visible && root.showCommRange && u.commRange > 0) {
                    ctx.save()
                    ctx.strokeStyle = "rgba(160,175,200,0.35)"
                    ctx.lineWidth = 1
                    ctx.setLineDash([4, 8])
                    ctx.beginPath()
                    ctx.arc(p.x, p.y, Math.max(2, u.commRange * root.zoom), 0, Math.PI*2)
                    ctx.stroke()
                    ctx.setLineDash([])
                    ctx.restore()
                }
                if (visible && root.showAttackRange && u.attackRange > 0) {
                    ctx.save()
                    ctx.strokeStyle = "#ff6b4a"
                    ctx.lineWidth = 2
                    ctx.setLineDash([6, 4])
                    ctx.beginPath()
                    ctx.arc(p.x, p.y, Math.max(2, u.attackRange * root.zoom), 0, Math.PI*2)
                    ctx.stroke()
                    ctx.setLineDash([])
                    ctx.fillStyle = "rgba(255,107,74,0.08)"
                    ctx.fill()
                    ctx.restore()
                }

                if (visible) {
                    var color = u.side === "red" ? t.red : t.blue
                    if (dead) {
                        color = t.dead
                        // 绘制 X 标记
                        ctx.strokeStyle = "#ff4d6d"
                        ctx.lineWidth = 2.5
                        var xSize = 7
                        ctx.beginPath()
                        ctx.moveTo(p.x - xSize, p.y - xSize)
                        ctx.lineTo(p.x + xSize, p.y + xSize)
                        ctx.moveTo(p.x + xSize, p.y - xSize)
                        ctx.lineTo(p.x - xSize, p.y + xSize)
                        ctx.stroke()
                    }

                    // 选中单元的发光效果
                    if (root.focusUnitId === u.id) {
                        ctx.fillStyle = "rgba(255,210,63,0.25)"
                        ctx.beginPath(); ctx.arc(p.x, p.y, 22, 0, Math.PI*2); ctx.fill()
                        ctx.strokeStyle = t.focus
                        ctx.lineWidth = 3
                        ctx.beginPath(); ctx.arc(p.x, p.y, 18, 0, Math.PI*2); ctx.stroke()
                        // 内环
                        ctx.strokeStyle = "#ffffff"
                        ctx.lineWidth = 1.5
                        ctx.beginPath(); ctx.arc(p.x, p.y, 12, 0, Math.PI*2); ctx.stroke()
                    }

                    ctx.fillStyle = color
                    ctx.beginPath(); ctx.arc(p.x, p.y, dead ? 5 : 9, 0, Math.PI*2); ctx.fill()
                    ctx.strokeStyle = "#0a1428"; ctx.lineWidth = 2
                    ctx.stroke()
                    if (root.focusUnitId === u.id) {
                        ctx.strokeStyle = t.focus
                        ctx.lineWidth = 2.5
                        ctx.setLineDash([5, 3])
                        ctx.beginPath(); ctx.arc(p.x, p.y, 24, 0, Math.PI*2); ctx.stroke()
                        ctx.setLineDash([])
                    }

                    // 绘制发现提示标记
                    var hasDiscovery = false
                    for (var di = 0; di < root.discoveryUnits.length; di++) {
                        if (root.discoveryUnits[di] === u.id) { hasDiscovery = true; break }
                    }
                    if (hasDiscovery) {
                        var badgeX = p.x + 20
                        var badgeY = p.y - 28
                        var bx = badgeX - 18, by = badgeY - 8, bw = 32, bh = 18, br = 4
                        ctx.fillStyle = t.alertBg
                        ctx.beginPath()
                        ctx.moveTo(bx + br, by)
                        ctx.lineTo(bx + bw - br, by)
                        ctx.arcTo(bx + bw, by, bx + bw, by + br, br)
                        ctx.lineTo(bx + bw, by + bh - br)
                        ctx.arcTo(bx + bw, by + bh, bx + bw - br, by + bh, br)
                        ctx.lineTo(bx + br, by + bh)
                        ctx.arcTo(bx, by + bh, bx, by + bh - br, br)
                        ctx.lineTo(bx, by + br)
                        ctx.arcTo(bx, by, bx + br, by, br)
                        ctx.closePath()
                        ctx.fill()
                        ctx.fillStyle = "#000"
                        ctx.font = "bold 10px sans-serif"
                        ctx.fillText("\u6d88\u606f", badgeX - 14, badgeY + 5)
                    }

                    ctx.font = "bold 11px sans-serif"
                    ctx.fillStyle = t.labelShadow
                    ctx.fillText(u.callsign, p.x + 13, p.y - 6)
                    ctx.fillText(u.callsign, p.x + 14, p.y - 5)
                    ctx.fillStyle = t.label
                    ctx.fillText(u.callsign, p.x + 13, p.y - 7)
                }
            }

            // 未知目标探测
            if (!root.showAllSides && root.focusUnitId) {
                var snap = controller.unitAt(root.focusUnitId)
                if (snap && snap.detections) {
                    ctx.font = "10px sans-serif"
                    for (var di = 0; di < snap.detections.length; di++) {
                        var d = snap.detections[di]
                        var dp = root.toPixel(d.position[0], d.position[1])
                        ctx.fillStyle = t.enemy
                        ctx.beginPath()
                        ctx.moveTo(dp.x, dp.y - 7)
                        ctx.lineTo(dp.x - 6, dp.y + 4)
                        ctx.lineTo(dp.x + 6, dp.y + 4)
                        ctx.closePath()
                        ctx.fill()
                        ctx.fillStyle = "rgba(0,0,0,0.7)"
                        ctx.fillText(d.callsign, dp.x + 8, dp.y + 12)
                    }
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            hoverEnabled: true
            onWheel: function(wheel) {
                var delta = wheel.angleDelta.y > 0 ? 1.1 : 1/1.1
                root.zoom = Math.max(0.05, Math.min(20.0, root.zoom * delta))
                innerCanvas.requestPaint()
            }
            onClicked: function(mouse) {
                var lp = root.logicalFromPixel(mouse.x, mouse.y)

                if (root.routePlanningMode) {
                    root.routePointAdded(lp)
                    innerCanvas.requestPaint()
                    return
                }

                var hit = null
                for (var i = 0; i < innerCanvas.units.length; i++) {
                    var u = innerCanvas.units[i]
                    if (!root.isVisible(u)) continue
                    var p = root.toPixel(u.position[0], u.position[1])
                    var dx = mouse.x - p.x, dy = mouse.y - p.y
                    if (dx*dx + dy*dy < 14*14) { hit = u; break }
                }
                if (mouse.button === Qt.RightButton) {
                    if (hit) root.unitClicked(hit.id, "right")
                    else root.rightClickedMap(lp)
                } else {
                    if (hit) root.unitClicked(hit.id, "left")
                    else root.clickedMap(lp)
                }
            }
        }
    }

    Component.onCompleted: {
        var info = controller.mapInfo
        if (info && info.widthMeters) {
            root.mapSize = ({w: info.widthMeters, h: info.heightMeters})
            root.center = ({x: info.widthMeters/2, y: info.heightMeters/2})
        }
        innerCanvas.units = controller.units
        innerCanvas.requestPaint()
    }
}
