pragma ComponentBehavior: Bound
import QtQuick
// MapTileRenderer 在 main.cpp 中以运行时方式注册，qmllint 无法静态解析该模块。
// qmllint disable import
import Wargame 1.0
// qmllint enable import

Item {
    id: root
    property var controller: null
    property var editor: null
    property string sideFilter: "red"
    property bool   showAllSides: false
    property var visibleUnitIds: null
    property string focusUnitId: ""
    property string actionTargetId: ""
    property var selectedUnitIds: []
    // null keeps the legacy all-friendly behavior; an array limits ranges to those units.
    property var rangeUnitIds: null
    property bool allowRightClickActions: false
    property bool followSuspended: false
    property bool showDetectRange: true
    property bool showAttackRange: true
    property bool showCommRange: false
    property bool showRoutes: true
    property bool showRecentPaths: true
    property bool showProjectiles: true
    onShowProjectilesChanged: innerCanvas.requestPaint()
    property bool showEnemyHp: true
    property bool showCoordinateGrid: false   // controlled by settings
    property bool showCoordinateReadout: false
    property double simTime: 0.0
    property var discoveryUnits: ([])
    property var recentPathsByUnit: ({})
    property var detectedEnemyIds: ([])
    property string trackingTargetId: ""
    property string pursueSourceId: ""
    property var pursueSourcePos: null
    property var trackingTargetPos: null
    property bool trackingTargetAlive: false

    function setTrackingTarget(srcId, tgtId) {
        root.pursueSourceId = srcId
        if (tgtId) {
            root.trackingTargetId = tgtId
        } else {
            root.trackingTargetId = ""
            root.pursueSourceId = ""
            root.pursueSourcePos = null
            root.trackingTargetPos = null
            root.trackingTargetAlive = false
        }
        refresh()
    }

    signal clickedMap(var logicalPos)
    signal rightClickedMap(var logicalPos)
    signal unitClicked(string unitId, var button, int modifiers)
    signal guidePointPicked(var logicalPos, string targetId)
    signal guideSourceChanged(string unitId)
    signal guideCancelled()
    signal mapMarkerClicked(var marker)
    signal doubleClickedUnit(string unitId)
    signal doubleClickedMap(var logicalPos)

    property double destroyFlashX: -1
    property double destroyFlashY: -1
    property bool destroyFlashActive: false
    property double destroyLogicalX: -1
    property double destroyLogicalY: -1

    function flashDestroyAt(logicalX, logicalY) {
        root.destroyLogicalX = logicalX
        root.destroyLogicalY = logicalY
        root.destroyFlashActive = true
        destroyFlashAnim.restart()
    }

    // 路径引导：进入后点击地图为目标点；命中敌方用敌方当前坐标
    property bool   guideMode: false
    property string guideSourceUnitId: ""

    function startGuideMode(sourceUnitId) {
        root.guideSourceUnitId = sourceUnitId || ""
        root.guideMode = true
        innerCanvas.requestPaint()
    }
    function stopGuideMode() {
        if (!root.guideMode) return
        root.guideMode = false
        root.guideSourceUnitId = ""
        innerCanvas.requestPaint()
    }
    onGuideModeChanged: innerCanvas.requestPaint()

    property double zoom: 1.0
    property var center: ({x: 10000, y: 7500})
    property var mapSize: ({w: 20000, h: 15000})
    property double mapOriginLon: 119.30
    property double mapOriginLat: 25.40
    property int mapTileZoom: 12
    property int mapRevision: 0
    property var routes: []
    property var mapMarkers: []
    property string selectedMapMarkerId: ""
    property var abilityCooldownByUnit: ({})
    property var abilityEffects: []
    property bool pointerInside: false
    property var pointerLogicalPos: ({x: 0, y: 0})
    property bool actionPulseActive: false
    property double actionPulseX: -1
    property double actionPulseY: -1
    property color actionPulseColor: "#3bd6bd"

    // 公共刷新方法
    function refresh() { innerCanvas.requestPaint() }
    function rebuildRecentPaths() {
        var map = ({})
        if (root.controller && root.controller.networked) {
            if (root.controller.isObserver) {
                var projected = root.controller.observerTrajectories || ({})
                var trails = projected.trails || []
                for (var ti = 0; ti < trails.length; ti++) {
                    var trail = trails[ti]
                    if (trail && trail.unitId && trail.points && trail.points.length > 0)
                        map[trail.unitId] = trail.points
                }
            }
            root.recentPathsByUnit = map
            innerCanvas.requestPaint()
            return
        }
        var all = root.controller ? root.controller.allUnits() : []
        for (var i = 0; i < all.length; i++) {
            var snap = all[i]
            if (snap && snap.recentPath && snap.recentPath.length > 0)
                map[snap.id] = snap.recentPath
        }
        root.recentPathsByUnit = map
        innerCanvas.requestPaint()
    }
    function pulseActionAt(logicalPos, color) {
        if (!logicalPos) return
        root.actionPulseX = Number(logicalPos.x)
        root.actionPulseY = Number(logicalPos.y)
        root.actionPulseColor = color || t.actionPulse
        root.actionPulseActive = true
        actionPulseAnimation.restart()
    }

    function observeAbilityTransitions(units) {
        var previous = root.abilityCooldownByUnit || ({})
        var next = ({})
        var effects = root.abilityEffects ? root.abilityEffects.slice() : []
        var now = Date.now()
        for (var i = 0; i < units.length; i++) {
            var unit = units[i]
            if (!unit || !unit.id || !unit.position || unit.position.length < 2) continue
            var abilities = unit.abilities || ({})
            var observedAbilities = ["countermeasure", "scan"]
            for (var abilityIndex = 0; abilityIndex < observedAbilities.length; abilityIndex++) {
                var abilityName = observedAbilities[abilityIndex]
                var ability = abilities[abilityName]
                if (!ability || ability.cooldownRemaining === undefined) continue
                var key = unit.id + ":" + abilityName
                var cooldown = Number(ability.cooldownRemaining || 0)
                next[key] = cooldown
                if (previous[key] !== undefined && cooldown > Number(previous[key]) + 1) {
                    effects.push({
                        kind: abilityName,
                        x: Number(unit.position[0]),
                        y: Number(unit.position[1]),
                        range: Number(ability.range || 0),
                        side: unit.side || "",
                        started: now,
                        duration: abilityName === "scan" ? 850 : 520
                    })
                }
            }
        }
        root.abilityCooldownByUnit = next
        if (effects.length > 0) {
            root.abilityEffects = effects.slice(Math.max(0, effects.length - 12))
            abilityEffectTimer.start()
        }
    }

    function applyMapInfo(recenter) {
        var info = root.controller.mapInfo
        if (!info) return
        var revision = Number(info.mapRevision)
        var w = Number(info.widthMeters)
        var h = Number(info.heightMeters)
        var lon = Number(info.originLon)
        var lat = Number(info.originLat)
        var z = Number(info.tileZoom)

        if (!Number.isInteger(revision) || revision < 0
                || !isFinite(w) || w <= 0 || !isFinite(h) || h <= 0
                || !isFinite(lon) || lon < -180 || lon > 180
                || !isFinite(lat) || lat < -85.05112878 || lat > 85.05112878
                || !Number.isInteger(z) || z < 0 || z > 22) return
        var sameMetadata = revision === root.mapRevision
                && root.mapSize.w === w && root.mapSize.h === h
                && root.mapOriginLon === lon && root.mapOriginLat === lat
                && root.mapTileZoom === z
        if ((revision === 0 && root.mapRevision > 0)
                || (revision > 0 && revision < root.mapRevision)
                || sameMetadata) return

        var mapSizeChanged = root.mapSize.w !== w || root.mapSize.h !== h
        root.mapSize = ({w: w, h: h})
        if (recenter || mapSizeChanged) root.center = ({x: w / 2, y: h / 2})
        root.mapOriginLon = lon
        root.mapOriginLat = lat
        root.mapTileZoom = z
        if (revision > 0) root.mapRevision = revision
        refresh()
    }

    function refreshTrackingPos() {
        if (root.trackingTargetId && root.pursueSourceId) {
            var s = root.controller.unitAt(root.pursueSourceId)
            root.pursueSourcePos = (s && s.position && s.position.length >= 2) ? {x: s.position[0], y: s.position[1]} : null
            var t = root.controller.unitAt(root.trackingTargetId)
            root.trackingTargetPos = (t && t.position && t.position.length >= 2) ? {x: t.position[0], y: t.position[1]} : null
            root.trackingTargetAlive = !!(t && t.alive)
            if (!root.trackingTargetAlive || !root.pursueSourcePos || !root.trackingTargetPos) {
                root.setTrackingTarget("", "")
            }
        }
        trackingRefreshTimer.running = !!root.trackingTargetId
        refresh()
    }

    onTrackingTargetIdChanged: {
        if (!root.trackingTargetId) {
            root.pursueSourceId = ""
            root.pursueSourcePos = null
            root.trackingTargetPos = null
            root.trackingTargetAlive = false
        }
        refresh()
    }
    onSideFilterChanged: refresh()
    onShowAllSidesChanged: refresh()
    onRoutesChanged: refresh()
    onShowRoutesChanged: refresh()
    onShowDetectRangeChanged: refresh()
    onShowAttackRangeChanged: refresh()
    onShowCommRangeChanged: refresh()
    onShowRecentPathsChanged: refresh()
    onShowEnemyHpChanged: refresh()
    onRecentPathsByUnitChanged: refresh()
    onFocusUnitIdChanged: {
        if (root.focusUnitId) root.followSuspended = false
        refresh()
    }
    onSelectedUnitIdsChanged: refresh()
    onActionTargetIdChanged: refresh()
    onDiscoveryUnitsChanged: refresh()
    onDetectedEnemyIdsChanged: refresh()
    onSimTimeChanged: refresh()
    onZoomChanged: refresh()
    onCenterChanged: refresh()
    onMapSizeChanged: refresh()
    onMapMarkersChanged: refresh()
    onSelectedMapMarkerIdChanged: refresh()

    Connections {
        target: root.controller
        // 联网快照会定期通知地图信息；只有地图尺寸变化时才重新居中，不能覆盖用户拖拽。
        function onMapInfoForward() { root.applyMapInfo(false) }
    }

    QtObject {
        id: t
        property color bg: AppContext.page
        property color grid: AppContext.softLine
        property color land: "#0a0f1e"
        property color label: AppContext.textStrong
        property color labelShadow: "#000000"
        property color red: AppContext.red
        property color blue: AppContext.blue
        property color dead: "#4a5268"
        property color focus: "#ffd240"
        property color detect: AppContext.info
        property color comm: "#5a6a88"
        property color attack: "#f06050"
        property color route: AppContext.success
        property color routePending: "#5a6a88"
        property color enemy: "#f06050"
        property color alertBg: AppContext.warning
        property color coordinateBg: "#081219cc"
        property color coordinateBorder: "#2c4651"
        property color coordinateText: "#c3d2d8"
        property color markerRed: AppContext.red
        property color markerBlue: AppContext.blue
        property color markerStroke: "#081219"
        property color markerText: AppContext.text
        property color rangeCommunication: AppContext.rangeCommunication
        property color rangeDetection: AppContext.rangeDetection
        property color rangeAttack: AppContext.rangeAttack
        property color markerSelf: AppContext.markSelf
        property color markerCommander: AppContext.markCommander
        property color markerManeuver: AppContext.signal
        property color markerAttack: AppContext.warning
        property color markerWithdrawal: AppContext.danger
        property color markerOrder: AppContext.info
        property color actionPulse: AppContext.signal
    }

    function logicalFromPixel(px, py) {
        var point = tileMap.screenToSim(px, py)
        return { x: Number(point.x), y: Number(point.y) }
    }
    function toPixel(lx, ly) {
        return tileMap.simToScreen(lx, ly)
    }
    function commandMarkerAtPixel(px, py) {
        var markers = root.mapMarkers || []
        for (var i = markers.length - 1; i >= 0; i--) {
            var marker = markers[i]
            if (!marker || marker.category !== "command" || !marker.position) continue
            var markerX = marker.position.x !== undefined
                ? Number(marker.position.x) : Number(marker.position[0])
            var markerY = marker.position.y !== undefined
                ? Number(marker.position.y) : Number(marker.position[1])
            if (!isFinite(markerX) || !isFinite(markerY)) continue
            var point = root.toPixel(markerX, markerY)
            var dx = px - point.x
            var dy = py - point.y
            if (dx * dx + dy * dy <= 24 * 24) return marker
        }
        return null
    }
    function isVisible(u) {
        if (!u) return false
        if (root.visibleUnitIds !== null && root.visibleUnitIds.indexOf(u.id) < 0) return false
        if (showAllSides) return true
        if (u.side === sideFilter) return true
        if (root.detectedEnemyIds && root.detectedEnemyIds.length > 0) {
            if (root.detectedEnemyIds.indexOf(u.id) >= 0) return true
        }
        if (root.discoveryUnits && root.discoveryUnits.length > 0) {
            if (root.discoveryUnits.indexOf(u.id) >= 0) return true
        }
        return false
    }
    function centerOn(lx, ly) {
        root.center = ({x: lx, y: ly})
        refresh()
    }
    // 所有视角共用此入口，确保拖拽后重新点击同一单元也能恢复居中。
    function focusOnUnit(unitId) {
        root.followSuspended = false
        if (!unitId || !root.controller) return false
        var unit = root.controller.unitAt(unitId)
        if (!unit || !unit.position || unit.position.length < 2) return false
        centerOn(unit.position[0], unit.position[1])
        return true
    }
    function focusAt(lx, ly) {
        root.followSuspended = false
        centerOn(lx, ly)
    }

    // GIS tile map background (uses C++ MapTileRenderer)
    // qmllint disable import
    // qmllint disable unresolved-type
    // qmllint disable unqualified
    MapTileRenderer {
        id: tileMap
        anchors.fill: root
        z: 0
        centerX: root.center.x
        centerY: root.center.y
        zoom: Math.max(0.05, root.zoom)
        originLon: root.mapOriginLon
        originLat: root.mapOriginLat
        logicalWidthMeters: root.mapSize.w
        logicalHeightMeters: root.mapSize.h
        tileZoom: root.mapTileZoom
    }
    // qmllint enable unqualified
    // qmllint enable unresolved-type
    // qmllint enable import

    // Semi-transparent overlay so tiles show through but we keep grid border
    Rectangle { anchors.fill: parent; color: "transparent"; border.color: "#2a3a56"; border.width: 1 }

    // 引导模式激活时显示绿色边框
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: root.guideMode ? "#46d29a" : "transparent"
        border.width: root.guideMode ? 4 : 0
        radius: 6
        z: 5
        Behavior on border.width { NumberAnimation { duration: 150 } }
    }

    Canvas {
        id: innerCanvas
        anchors.fill: parent
        property var units: []
        property var detections: []
        property var enemyDetections: []
        property var projectiles: []
        property var previousProjectiles: []
        property double projectileSampleStartedMs: 0
        property double projectileSampleIntervalMs: 100
        property double projectileLastArrivalMs: 0
        property point _guideHover: Qt.point(-1, -1)

        function acceptProjectileSample(sample) {
            var now = Date.now()
            if (projectileLastArrivalMs > 0) {
                projectileSampleIntervalMs = Math.max(33,
                    Math.min(1000, now - projectileLastArrivalMs))
            }
            previousProjectiles = projectiles || []
            projectiles = sample || []
            projectileLastArrivalMs = now
            projectileSampleStartedMs = now
            if (root.showProjectiles) projectileRenderTimer.restart()
            requestPaint()
        }

        Connections {
            target: root.controller
            function onUnitsForward() {
                var projectedUnits = root.controller.units || []
                root.observeAbilityTransitions(projectedUnits)
                innerCanvas.units = projectedUnits
                if (root.controller.networked) {
                    root.rebuildRecentPaths()
                    return
                }
                // Only rebuild the recentPaths map if at least one path changed.
                // Otherwise we trigger a full repaint on every 16ms engine tick.
                var map = ({})
                var prev = root.recentPathsByUnit || ({})
                var changed = false
                var all = root.controller.allUnits()
                for (var i = 0; i < all.length; i++) {
                    var snap = all[i]
                    if (snap && snap.recentPath && snap.recentPath.length > 0) {
                        map[all[i].id] = snap.recentPath
                        if (!changed) {
                            var old = prev[all[i].id]
                            var last = snap.recentPath[snap.recentPath.length - 1]
                            var oldLast = old && old.length > 0 ? old[old.length - 1] : null
                            if (!old || old.length !== snap.recentPath.length || !oldLast
                                    || oldLast.x !== last.x || oldLast.y !== last.y) changed = true
                        }
                    }
                }
                if (!changed && Object.keys(prev).length !== Object.keys(map).length) changed = true
                if (changed) root.recentPathsByUnit = map
                innerCanvas.requestPaint()
            }
            function onObserverTrajectoriesChanged() {
                root.rebuildRecentPaths()
            }
            function onProjectilesForward() {
                innerCanvas.acceptProjectileSample(root.controller.projectiles)
            }
        }

        Timer {
            id: projectileRenderTimer
            interval: 33
            repeat: true
            running: false
            onTriggered: {
                innerCanvas.requestPaint()
                if (Date.now() - innerCanvas.projectileSampleStartedMs
                        >= innerCanvas.projectileSampleIntervalMs) {
                    if (innerCanvas.projectiles.length === 0)
                        innerCanvas.previousProjectiles = []
                    stop()
                }
            }
        }

        Timer {
            id: abilityEffectTimer
            interval: 33
            repeat: true
            running: false
            onTriggered: {
                var now = Date.now()
                var active = (root.abilityEffects || []).filter(function(effect) {
                    return now - effect.started < effect.duration
                })
                root.abilityEffects = active
                innerCanvas.requestPaint()
                if (active.length === 0) stop()
            }
        }

    Timer {
        id: trackingRefreshTimer
        interval: 350; repeat: true; running: false
        onTriggered: {
            if (root.trackingTargetId) root.refreshTrackingPos()
        }
    }

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            // Background: transparent to show tile map
            ctx.clearRect(0, 0, width, height)
            if (root.showCoordinateGrid) {
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
            }
            ctx.fillStyle = "rgba(5, 12, 20, 0.74)"
            ctx.fillRect(6, 4, 148, 40)
            ctx.font = "bold 12px sans-serif"
            ctx.fillStyle = "rgba(255,255,255,0.92)"
            ctx.fillText("画布 " + Math.round(root.mapSize.w/1000) + " km × " + Math.round(root.mapSize.h/1000) + " km", 12, 18)
            ctx.fillStyle = "rgba(220,232,242,0.88)"
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

            // 手动情报标记：只由服务器投影到当前阵营/战位，绝不自动传播。
            var markers = root.mapMarkers || []
            for (var mi = 0; mi < markers.length; mi++) {
                var marker = markers[mi]
                var position = marker && marker.position
                if (!position) continue
                var markerX = position.x !== undefined ? Number(position.x) : Number(position[0])
                var markerY = position.y !== undefined ? Number(position.y) : Number(position[1])
                if (!isFinite(markerX) || !isFinite(markerY)) continue
                var mp = root.toPixel(markerX, markerY)
                var commandMarker = marker.category === "command"
                var commandKind = marker.commandKind || ""
                var projectedMarkType = marker.markType || ""
                var currentSeatId = root.controller ? root.controller.currentSeatId : ""
                var participantMarker = projectedMarkType === "self"
                    || (!projectedMarkType && currentSeatId && marker.seatId === currentSeatId)
                var ownMarker = participantMarker && currentSeatId && marker.seatId === currentSeatId
                var commanderMarker = projectedMarkType === "commander"
                    || (!projectedMarkType && marker.seatId
                        && String(marker.seatId).indexOf("_commander") > 0)
                ctx.save()
                var commandColor = commandKind === "attack" ? t.markerAttack
                    : commandKind === "withdrawal" ? t.markerWithdrawal
                    : commandKind === "text" ? t.markerOrder : t.markerManeuver
                ctx.fillStyle = commandMarker ? commandColor
                    : commanderMarker ? t.markerCommander
                    : participantMarker ? t.markerSelf
                    : (marker.side === "red" ? t.markerRed : t.markerBlue)
                ctx.strokeStyle = t.markerStroke
                ctx.lineWidth = commanderMarker ? 3 : 2
                ctx.beginPath()
                if (commandMarker && commandKind === "attack") {
                    ctx.arc(mp.x, mp.y, 9, 0, Math.PI * 2)
                } else if (commandMarker && commandKind === "withdrawal") {
                    ctx.moveTo(mp.x, mp.y + 10)
                    ctx.lineTo(mp.x - 9, mp.y - 7)
                    ctx.lineTo(mp.x + 9, mp.y - 7)
                    ctx.closePath()
                } else if (commandMarker && commandKind === "text") {
                    ctx.rect(mp.x - 8, mp.y - 8, 16, 16)
                } else if (commandMarker) {
                    ctx.arc(mp.x, mp.y, 8, 0, Math.PI * 2)
                } else if (commanderMarker) {
                    var markerRadius = 17
                    for (var oi = 0; oi < 8; oi++) {
                        var oa = -Math.PI / 8 + oi * Math.PI / 4
                        var ox = mp.x + Math.cos(oa) * markerRadius
                        var oy = mp.y + Math.sin(oa) * markerRadius
                        if (oi === 0) ctx.moveTo(ox, oy); else ctx.lineTo(ox, oy)
                    }
                    ctx.closePath()
                } else if (participantMarker) {
                    var ts = 8
                    ctx.moveTo(mp.x, mp.y + ts)
                    ctx.lineTo(mp.x - ts, mp.y - ts * 0.7)
                    ctx.lineTo(mp.x + ts, mp.y - ts * 0.7)
                    ctx.closePath()
                } else {
                    ctx.arc(mp.x, mp.y, 6, 0, Math.PI * 2)
                }
                ctx.fill(); ctx.stroke()
                if (commandMarker && commandKind === "maneuver") {
                    ctx.strokeStyle = "#ffffff"
                    ctx.lineWidth = 2
                    ctx.beginPath()
                    ctx.moveTo(mp.x - 4, mp.y + 3)
                    ctx.lineTo(mp.x + 4, mp.y - 4)
                    ctx.moveTo(mp.x, mp.y - 4)
                    ctx.lineTo(mp.x + 4, mp.y - 4)
                    ctx.lineTo(mp.x + 4, mp.y)
                    ctx.stroke()
                } else if (commandMarker && commandKind === "attack") {
                    ctx.strokeStyle = "#ffffff"
                    ctx.lineWidth = 1.5
                    ctx.beginPath()
                    ctx.moveTo(mp.x - 13, mp.y); ctx.lineTo(mp.x + 13, mp.y)
                    ctx.moveTo(mp.x, mp.y - 13); ctx.lineTo(mp.x, mp.y + 13)
                    ctx.stroke()
                } else if (commandMarker && commandKind === "text") {
                    ctx.strokeStyle = "#ffffff"
                    ctx.lineWidth = 1.5
                    ctx.beginPath()
                    ctx.moveTo(mp.x - 4, mp.y - 3); ctx.lineTo(mp.x + 4, mp.y - 3)
                    ctx.moveTo(mp.x - 4, mp.y + 1); ctx.lineTo(mp.x + 4, mp.y + 1)
                    ctx.moveTo(mp.x - 4, mp.y + 5); ctx.lineTo(mp.x + 2, mp.y + 5)
                    ctx.stroke()
                }
                if (commanderMarker) {
                    ctx.strokeStyle = t.markerCommander
                    ctx.lineWidth = 3
                    var frameRadius = commandMarker ? 20 : 23
                    ctx.beginPath()
                    ctx.rect(mp.x - frameRadius, mp.y - frameRadius,
                             frameRadius * 2, frameRadius * 2)
                    ctx.stroke()
                    if (commandMarker) {
                        ctx.fillStyle = t.markerCommander
                        ctx.beginPath()
                        ctx.arc(mp.x + frameRadius - 2, mp.y - frameRadius + 2,
                                8, 0, Math.PI * 2)
                        ctx.fill()
                        ctx.strokeStyle = t.markerStroke
                        ctx.lineWidth = 2
                        ctx.stroke()
                        ctx.fillStyle = "#081219"
                        ctx.font = "bold 10px sans-serif"
                        ctx.textAlign = "center"
                        ctx.textBaseline = "middle"
                        ctx.fillText("令", mp.x + frameRadius - 2,
                                     mp.y - frameRadius + 3)
                    } else {
                        ctx.fillStyle = "#081219"
                        ctx.font = "bold 13px sans-serif"
                        ctx.textAlign = "center"
                        ctx.textBaseline = "middle"
                        ctx.fillText("令", mp.x, mp.y + 1)
                    }
                    ctx.textAlign = "start"
                    ctx.textBaseline = "alphabetic"
                } else if (participantMarker && !commandMarker) {
                    ctx.strokeStyle = t.markerSelf
                    ctx.lineWidth = 1
                    ctx.beginPath(); ctx.arc(mp.x, mp.y, 11, 0, Math.PI * 2); ctx.stroke()
                    ctx.fillStyle = "#ffffff"
                    ctx.globalAlpha = 0.7
                    ctx.beginPath(); ctx.arc(mp.x, mp.y - 1, 2.5, 0, Math.PI * 2); ctx.fill()
                    ctx.globalAlpha = 1.0
                }
                if (commandMarker && marker.id && marker.id === root.selectedMapMarkerId) {
                    ctx.strokeStyle = t.focus
                    ctx.lineWidth = 2
                    ctx.beginPath(); ctx.arc(mp.x, mp.y, 25, 0, Math.PI * 2); ctx.stroke()
                }
                ctx.fillStyle = t.markerText
                ctx.font = "10px sans-serif"
                var markerLabel = marker.label || "标记"
                if (commandMarker) {
                    var commandLabel = commandKind === "attack" ? "攻击"
                        : commandKind === "withdrawal" ? "撤离"
                        : commandKind === "text" ? "命令" : "机动"
                    markerLabel = "[指挥·" + commandLabel + "]"
                }
                else if (commanderMarker) markerLabel = "[指挥令] " + markerLabel
                else if (ownMarker) {
                    if (marker.category !== "selfMove")
                        markerLabel = "[我的点] " + markerLabel
                }
                else if (participantMarker) markerLabel = "[下属报告] " + markerLabel
                if (marker.status) markerLabel += " · " + marker.status
                var labelOffsetX = commanderMarker ? 28 : participantMarker ? 14 : 9
                var labelOffsetY = commanderMarker ? -12 : -7
                ctx.fillText(markerLabel, mp.x + labelOffsetX, mp.y + labelOffsetY)
                ctx.restore()
            }

            // 路径引导模式：从源单元到鼠标位置的预览连线
            if (root.guideMode && root.guideSourceUnitId && innerCanvas._guideHover
                && innerCanvas._guideHover.x >= 0 && innerCanvas._guideHover.y >= 0) {
                var srcSnap = root.controller.unitAt(root.guideSourceUnitId)
                if (srcSnap && srcSnap.position) {
                    var srcPx = root.toPixel(srcSnap.position[0], srcSnap.position[1])
                    var hov = innerCanvas._guideHover
                    ctx.strokeStyle = "#46d29a"
                    ctx.lineWidth = 2
                    ctx.setLineDash([6, 4])
                    ctx.beginPath()
                    ctx.moveTo(srcPx.x, srcPx.y)
                    ctx.lineTo(hov.x, hov.y)
                    ctx.stroke()
                    ctx.setLineDash([])
                    ctx.fillStyle = "#46d29a"
                    ctx.beginPath(); ctx.arc(hov.x, hov.y, 6, 0, Math.PI*2); ctx.fill()
                    ctx.strokeStyle = "#0a1428"; ctx.lineWidth = 2; ctx.stroke()
                }
            }

            // 实时路径曲线（仅己方，在 unit 圆圈之前绘制）
            if (root.showRecentPaths) {
                var rpMap = root.recentPathsByUnit || ({})
                for (var rpi in rpMap) {
                    var rpPts = rpMap[rpi]
                    if (!rpPts || rpPts.length < 2) continue
                    var rpColor = "#8899b0"
                    for (var rj = 0; rj < innerCanvas.units.length; rj++) {
                        if (innerCanvas.units[rj].id === rpi) {
                            if (innerCanvas.units[rj].side !== root.sideFilter && !root.showAllSides) {
                                rpPts = null; break
                            }
                            rpColor = innerCanvas.units[rj].side === "red" ? "#ff6b7a" : "#6ba3ff"
                            break
                        }
                    }
                    if (!rpPts) continue
                    ctx.save()
                    ctx.globalAlpha = 0.7
                    ctx.lineWidth = 2.5
                    ctx.lineCap = "round"
                    ctx.lineJoin = "round"
                    ctx.shadowColor = rpColor
                    ctx.shadowBlur = 6
                    ctx.strokeStyle = rpColor
                    ctx.beginPath()
                    var rp0 = root.toPixel(rpPts[0].x, rpPts[0].y)
                    ctx.moveTo(rp0.x, rp0.y)
                    for (var rsi = 1; rsi < rpPts.length; rsi++) {
                        var cur = root.toPixel(rpPts[rsi].x, rpPts[rsi].y)
                        if (rsi + 1 < rpPts.length) {
                            var nxt = root.toPixel(rpPts[rsi+1].x, rpPts[rsi+1].y)
                            var midX = (cur.x + nxt.x) / 2
                            var midY = (cur.y + nxt.y) / 2
                            ctx.quadraticCurveTo(cur.x, cur.y, midX, midY)
                        } else {
                            ctx.lineTo(cur.x, cur.y)
                        }
                    }
                    ctx.stroke()
                    ctx.restore()
                }
            }

            // Ability effects are derived only from cooldown transitions in projected
            // authoritative unit state. They never reveal an unprojected activation.
            var abilityEffects = root.abilityEffects || []
            for (var aei = 0; aei < abilityEffects.length; aei++) {
                var effect = abilityEffects[aei]
                var effectProgress = Math.max(0, Math.min(1,
                    (Date.now() - effect.started) / Math.max(1, effect.duration)))
                var effectPixel = root.toPixel(effect.x, effect.y)
                var effectColor = effect.side === "red" ? "#ffc15a" : "#effcff"
                ctx.save()
                ctx.globalAlpha = 1 - effectProgress
                ctx.strokeStyle = effectColor
                ctx.lineWidth = 2
                if (effect.kind === "scan") {
                    ctx.beginPath()
                    ctx.arc(effectPixel.x, effectPixel.y,
                            Math.max(6, effect.range * root.zoom * effectProgress),
                            0, Math.PI * 2)
                    ctx.stroke()
                } else {
                    var fragmentRadius = Math.max(12,
                        Math.min(130, effect.range * root.zoom)
                        * (0.25 + effectProgress * 0.75))
                    for (var fragment = 0; fragment < 14; fragment++) {
                        var fragmentAngle = fragment / 14 * Math.PI * 2
                            + effectProgress * 0.7
                        var innerRadius = fragmentRadius * 0.72
                        ctx.beginPath()
                        ctx.moveTo(effectPixel.x + Math.cos(fragmentAngle) * innerRadius,
                                   effectPixel.y + Math.sin(fragmentAngle) * innerRadius)
                        ctx.lineTo(effectPixel.x + Math.cos(fragmentAngle) * fragmentRadius,
                                   effectPixel.y + Math.sin(fragmentAngle) * fragmentRadius)
                        ctx.stroke()
                    }
                }
                ctx.restore()
            }

            // Projectile positions are rendered one authoritative sample behind.
            // The interpolation factor is clamped, so a delayed server update can
            // never turn into client-side trajectory prediction.
            if (root.showProjectiles) {
                var projectileAlpha = innerCanvas.projectileSampleIntervalMs > 0
                    ? Math.max(0, Math.min(1,
                        (Date.now() - innerCanvas.projectileSampleStartedMs)
                        / innerCanvas.projectileSampleIntervalMs)) : 1
                var previousById = ({})
                var currentById = ({})
                var previousProjectiles = innerCanvas.previousProjectiles || []
                var currentProjectiles = innerCanvas.projectiles || []
                for (var ppi = 0; ppi < previousProjectiles.length; ppi++) {
                    var previousProjectile = previousProjectiles[ppi]
                    if (previousProjectile && previousProjectile.id)
                        previousById[previousProjectile.id] = previousProjectile
                }
                for (var cpi = 0; cpi < currentProjectiles.length; cpi++) {
                    var projectile = currentProjectiles[cpi]
                    if (!projectile || !projectile.id || !projectile.position
                        || projectile.position.length < 2) continue
                    currentById[projectile.id] = projectile
                    var previous = previousById[projectile.id]
                    var px = Number(projectile.position[0])
                    var py = Number(projectile.position[1])
                    var heading = Number(projectile.headingRad !== undefined
                        ? projectile.headingRad : projectile.heading || 0)
                    if (previous && previous.position && previous.position.length >= 2) {
                        px = Number(previous.position[0])
                            + (px - Number(previous.position[0])) * projectileAlpha
                        py = Number(previous.position[1])
                            + (py - Number(previous.position[1])) * projectileAlpha
                        var oldHeading = Number(previous.headingRad !== undefined
                            ? previous.headingRad : previous.heading || heading)
                        var headingDelta = heading - oldHeading
                        while (headingDelta > Math.PI) headingDelta -= Math.PI * 2
                        while (headingDelta < -Math.PI) headingDelta += Math.PI * 2
                        heading = oldHeading + headingDelta * projectileAlpha
                    }
                    if (!isFinite(px) || !isFinite(py) || !isFinite(heading)) continue
                    var projectilePixel = root.toPixel(px, py)
                    var redProjectile = projectile.side === "red"
                    var bodyColor = redProjectile ? "#ff5b3f" : "#35c8ff"
                    var edgeColor = redProjectile ? "#ffc15a" : "#effcff"
                    var tailLength = Math.max(8, Math.min(24,
                        Number(projectile.speed || 360) * root.zoom * 0.12))
                    ctx.save()
                    ctx.globalAlpha = 0.92
                    ctx.strokeStyle = bodyColor
                    ctx.lineWidth = 2.5
                    ctx.beginPath()
                    ctx.moveTo(projectilePixel.x - Math.cos(heading) * tailLength,
                               projectilePixel.y - Math.sin(heading) * tailLength)
                    ctx.lineTo(projectilePixel.x, projectilePixel.y)
                    ctx.stroke()
                    ctx.translate(projectilePixel.x, projectilePixel.y)
                    ctx.rotate(heading)
                    ctx.fillStyle = bodyColor
                    ctx.strokeStyle = edgeColor
                    ctx.lineWidth = 1.5
                    ctx.beginPath()
                    if (redProjectile) {
                        ctx.moveTo(9, 0); ctx.lineTo(-6, -4.5)
                        ctx.lineTo(-3, 0); ctx.lineTo(-6, 4.5)
                    } else {
                        ctx.moveTo(8, 0); ctx.lineTo(0, -4.5)
                        ctx.lineTo(-7, 0); ctx.lineTo(0, 4.5)
                    }
                    ctx.closePath(); ctx.fill(); ctx.stroke()
                    ctx.restore()
                }
                // A projectile missing from the new authoritative array is terminal.
                // Keep only a short visual fade at its last confirmed position.
                for (var dpi = 0; dpi < previousProjectiles.length; dpi++) {
                    var deletedProjectile = previousProjectiles[dpi]
                    if (!deletedProjectile || currentById[deletedProjectile.id]
                        || !deletedProjectile.position
                        || deletedProjectile.position.length < 2) continue
                    var deletedX = Number(deletedProjectile.position[0])
                    var deletedY = Number(deletedProjectile.position[1])
                    if (!isFinite(deletedX) || !isFinite(deletedY)) continue
                    var deletedPixel = root.toPixel(deletedX, deletedY)
                    ctx.save()
                    ctx.globalAlpha = Math.max(0, 1 - projectileAlpha) * 0.75
                    ctx.strokeStyle = deletedProjectile.side === "red"
                        ? "#ffc15a" : "#effcff"
                    ctx.lineWidth = 2
                    ctx.beginPath(); ctx.arc(deletedPixel.x, deletedPixel.y,
                                             4 + projectileAlpha * 8, 0, Math.PI * 2)
                    ctx.stroke()
                    ctx.restore()
                }
            }

            // 追踪目标：静态高亮 + 进攻路线（无闪烁）
            if (root.trackingTargetId && root.pursueSourcePos && root.trackingTargetPos && root.trackingTargetAlive) {
                var spx = root.toPixel(root.pursueSourcePos.x, root.pursueSourcePos.y)
                var tpx = root.toPixel(root.trackingTargetPos.x, root.trackingTargetPos.y)
                ctx.strokeStyle = "rgba(255,90,50,0.55)"
                ctx.lineWidth = 1.5
                ctx.setLineDash([10, 6])
                ctx.beginPath()
                ctx.moveTo(spx.x, spx.y)
                ctx.lineTo(tpx.x, tpx.y)
                ctx.stroke()
                ctx.setLineDash([])
                ctx.strokeStyle = "rgba(255,80,40,0.65)"
                ctx.lineWidth = 2
                ctx.beginPath(); ctx.arc(tpx.x, tpx.y, 16, 0, Math.PI*2); ctx.stroke()
                ctx.fillStyle = "rgba(255,70,40,0.7)"
                ctx.font = "bold 9px sans-serif"
                ctx.fillText("追踪", tpx.x + 5, tpx.y - 14)
            }

            for (var i = 0; i < innerCanvas.units.length; i++) {
                var u = innerCanvas.units[i]
                if (!u || !u.position || u.position.length < 2) continue
                var visible = root.isVisible(u)
                var dead = !u.alive
                var isEnemy = root.showAllSides ? false : (u.side !== root.sideFilter)
                var p = root.toPixel(u.position[0], u.position[1])
                var threatCount = Number(u.incomingThreatCount || 0)
                if (!dead && visible && threatCount > 0) {
                    ctx.save()
                    ctx.strokeStyle = u.side === "red" ? "#ffc15a" : "#effcff"
                    ctx.lineWidth = 2
                    ctx.setLineDash([4, 3])
                    ctx.beginPath(); ctx.arc(p.x, p.y, 18, 0, Math.PI * 2); ctx.stroke()
                    ctx.setLineDash([])
                    ctx.restore()
                }
                var showsRanges = root.rangeUnitIds === null
                    || root.rangeUnitIds.indexOf(u.id) >= 0

                if (showsRanges && !dead && !isEnemy && root.showDetectRange && u.detectRange > 0) {
                    ctx.save()
                    ctx.strokeStyle = t.rangeDetection
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
                if (showsRanges && !dead && !isEnemy && root.showCommRange && u.commRange > 0) {
                    ctx.save()
                    ctx.strokeStyle = t.rangeCommunication
                    ctx.globalAlpha = 0.72
                    ctx.lineWidth = 1.5
                    ctx.setLineDash([4, 8])
                    ctx.beginPath()
                    ctx.arc(p.x, p.y, Math.max(2, u.commRange * root.zoom), 0, Math.PI*2)
                    ctx.stroke()
                    ctx.setLineDash([])
                    ctx.restore()
                }
                if (showsRanges && !dead && !isEnemy && root.showAttackRange && u.attackRange > 0) {
                    ctx.save()
                    ctx.strokeStyle = t.rangeAttack
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
                    var color
                    if (dead) color = "#5a6068"
                    else if (isEnemy) color = "#ff7a59"
                    else color = u.side === "red" ? t.red : t.blue

                    // 敌方标识环（静态）
                    if (!dead && isEnemy) {
                        ctx.strokeStyle = "rgba(255,110,70,0.35)"
                        ctx.lineWidth = 2
                        ctx.beginPath(); ctx.arc(p.x, p.y, 14, 0, Math.PI*2); ctx.stroke()
                    }

                    if (!dead && root.actionTargetId === u.id) {
                        ctx.save()
                        ctx.fillStyle = "rgba(255,113,128,0.16)"
                        ctx.beginPath(); ctx.arc(p.x, p.y, 24, 0, Math.PI*2); ctx.fill()
                        ctx.strokeStyle = t.markerRed
                        ctx.lineWidth = 3
                        ctx.setLineDash([7, 4])
                        ctx.beginPath(); ctx.arc(p.x, p.y, 19, 0, Math.PI*2); ctx.stroke()
                        ctx.setLineDash([])
                        ctx.fillStyle = t.markerRed
                        ctx.font = "bold 10px sans-serif"
                        ctx.fillText("攻击目标", p.x + 16, p.y - 17)
                        ctx.restore()
                    }

                    // 摧毁视觉：红色X + 碎光 + 旋转碎片
                    if (dead) {
                        var dcx = p.x, dcy = p.y
                        ctx.fillStyle = "rgba(255,60,80,0.2)"
                        ctx.beginPath(); ctx.arc(dcx, dcy, 17, 0, Math.PI*2); ctx.fill()
                        ctx.strokeStyle = "rgba(255,60,80,0.55)"
                        ctx.lineWidth = 3
                        var xsz = 10
                        ctx.beginPath()
                        ctx.moveTo(dcx - xsz, dcy - xsz)
                        ctx.lineTo(dcx + xsz, dcy + xsz)
                        ctx.moveTo(dcx + xsz, dcy - xsz)
                        ctx.lineTo(dcx - xsz, dcy + xsz)
                        ctx.stroke()
                        ctx.strokeStyle = "rgba(255,60,80,0.25)"
                        ctx.lineWidth = 1
                        ctx.setLineDash([3, 4])
                        ctx.beginPath()
                        ctx.moveTo(dcx - 3, dcy - xsz - 3)
                        ctx.lineTo(dcx - 3, dcy + xsz + 3)
                        ctx.moveTo(dcx - xsz - 3, dcy - 3)
                        ctx.lineTo(dcx + xsz + 3, dcy - 3)
                        ctx.stroke()
                        ctx.setLineDash([])
                        color = "#5a6068"
                    }

                    // 选中单元的发光效果
                    if (root.focusUnitId === u.id || root.selectedUnitIds.indexOf(u.id) >= 0) {
                        ctx.fillStyle = "rgba(255,210,63,0.3)"
                        ctx.beginPath(); ctx.arc(p.x, p.y, 22, 0, Math.PI*2); ctx.fill()
                        ctx.strokeStyle = t.focus
                        ctx.lineWidth = 3
                        ctx.beginPath(); ctx.arc(p.x, p.y, 18, 0, Math.PI*2); ctx.stroke()
                        ctx.strokeStyle = "#ffffff"
                        ctx.lineWidth = 1.5
                        ctx.beginPath(); ctx.arc(p.x, p.y, 12, 0, Math.PI*2); ctx.stroke()
                    }

                    // 单元主体圆
                    ctx.fillStyle = color
                    ctx.beginPath(); ctx.arc(p.x, p.y, dead ? 5 : (isEnemy ? 10 : 10), 0, Math.PI*2); ctx.fill()
                    ctx.strokeStyle = dead ? "rgba(255,77,109,0.6)"
                                    : (isEnemy ? "rgba(255,255,255,0.25)" : "#0a1428")
                    ctx.lineWidth = dead ? 1.5 : (isEnemy ? 2.5 : 2)
                    ctx.stroke()

                    // 摧毁时中央小白点
                    if (dead) {
                        ctx.fillStyle = "#ffffff"
                        ctx.globalAlpha = 0.5
                        ctx.beginPath(); ctx.arc(p.x, p.y, 2.5, 0, Math.PI*2); ctx.fill()
                        ctx.globalAlpha = 1.0
                    }

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
                    if (isEnemy) {
                        // 敌方标签背景光晕
                        var labelMetrics = ctx.measureText(u.callsign)
                        if (labelMetrics.width !== undefined) {
                            var bw = labelMetrics.width + 10
                            ctx.fillStyle = "rgba(255,80,50,0.15)"
                            var lx = p.x + 10, ly = p.y - 18, lh = 14
                            ctx.beginPath()
                            ctx.moveTo(lx + 3, ly)
                            ctx.lineTo(lx + bw - 3, ly)
                            ctx.arcTo(lx + bw, ly, lx + bw, ly + 3, 3)
                            ctx.lineTo(lx + bw, ly + lh - 3)
                            ctx.arcTo(lx + bw, ly + lh, lx + bw - 3, ly + lh, 3)
                            ctx.lineTo(lx + 3, ly + lh)
                            ctx.arcTo(lx, ly + lh, lx, ly + lh - 3, 3)
                            ctx.lineTo(lx, ly + 3)
                            ctx.arcTo(lx, ly, lx + 3, ly, 3)
                            ctx.closePath()
                            ctx.fill()
                        }
                        ctx.fillStyle = "#f3f6fb"
                        ctx.font = "bold 10px sans-serif"
                    } else {
                        ctx.fillStyle = t.labelShadow
                        ctx.fillText(u.callsign, p.x + 13, p.y - 6)
                        ctx.fillText(u.callsign, p.x + 14, p.y - 5)
                        ctx.fillStyle = t.label
                    }
                    ctx.fillText(u.callsign, p.x + 13, p.y - 7)

                    // HP bar：所有存活且血量非满的单元都显示；非满血时血条变短，方便指挥员快速识别受损单元
                    if (root.showEnemyHp && !dead && u.maxHp && u.hp !== undefined && u.hp < u.maxHp) {
                        var hpRatio = Math.max(0, Math.min(1, u.hp / u.maxHp))
                        var hpBarW = 28, hpBarH = 5
                        var hpBx = p.x - hpBarW / 2, hpBy = p.y + 14
                        ctx.fillStyle = "rgba(0,0,0,0.65)"
                        ctx.beginPath()
                        ctx.moveTo(hpBx + 2, hpBy)
                        ctx.lineTo(hpBx + hpBarW - 2, hpBy)
                        ctx.arcTo(hpBx + hpBarW, hpBy, hpBx + hpBarW, hpBy + 2, 2)
                        ctx.lineTo(hpBx + hpBarW, hpBy + hpBarH - 2)
                        ctx.arcTo(hpBx + hpBarW, hpBy + hpBarH, hpBx + hpBarW - 2, hpBy + hpBarH, 2)
                        ctx.lineTo(hpBx + 2, hpBy + hpBarH)
                        ctx.arcTo(hpBx, hpBy + hpBarH, hpBx, hpBy + hpBarH - 2, 2)
                        ctx.lineTo(hpBx, hpBy + 2)
                        ctx.arcTo(hpBx, hpBy, hpBx + 2, hpBy, 2)
                        ctx.closePath()
                        ctx.fill()
                        var hpFillW = Math.max(2, hpBarW * hpRatio)
                        var hpColor = hpRatio > 0.5 ? "#46d29a" : (hpRatio > 0.25 ? "#ffb24d" : "#ff4d6d")
                        ctx.fillStyle = hpColor
                        ctx.beginPath()
                        ctx.moveTo(hpBx + 1, hpBy + 1)
                        ctx.lineTo(hpBx + hpFillW - 1, hpBy + 1)
                        ctx.arcTo(hpBx + hpFillW, hpBy + 1, hpBx + hpFillW, hpBy + 2, 1)
                        ctx.lineTo(hpBx + hpFillW, hpBy + hpBarH - 1)
                        ctx.arcTo(hpBx + hpFillW, hpBy + hpBarH, hpBx + hpFillW - 1, hpBy + hpBarH, 1)
                        ctx.lineTo(hpBx + 1, hpBy + hpBarH)
                        ctx.arcTo(hpBx + 1, hpBy + hpBarH, hpBx + 1, hpBy + hpBarH - 1, 1)
                        ctx.lineTo(hpBx + 1, hpBy + 1)
                        ctx.arcTo(hpBx + 1, hpBy, hpBx + 2, hpBy, 1)
                        ctx.closePath()
                        ctx.fill()
                    }
                }
            }

            // 已知目标探测标记（仅己方视角）
            if (!root.showAllSides && root.focusUnitId) {
                var snap = root.controller.unitAt(root.focusUnitId)
                    if (snap && snap.detections) {
                        ctx.font = "10px sans-serif"
                        for (var di2 = 0; di2 < snap.detections.length; di2++) {
                            var d = snap.detections[di2]
                            if (!d || !d.position || d.position.length < 2) continue
                        var dp = root.toPixel(d.position[0], d.position[1])
                        ctx.strokeStyle = "#ff7a59"
                        ctx.lineWidth = 2
                        ctx.setLineDash([3, 3])
                        ctx.beginPath()
                        ctx.arc(dp.x, dp.y, 10, 0, Math.PI*2)
                        ctx.stroke()
                        ctx.setLineDash([])
                        ctx.fillStyle = "rgba(255,122,89,0.25)"
                        ctx.beginPath(); ctx.arc(dp.x, dp.y, 10, 0, Math.PI*2); ctx.fill()
                        ctx.fillStyle = "rgba(0,0,0,0.7)"
                        ctx.fillText(d.callsign, dp.x + 13, dp.y + 4)
                    }
                }
            }
        }

        MouseArea {
            id: canvasMouse
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            hoverEnabled: true
            cursorShape: root.guideMode ? Qt.CrossCursor : Qt.ArrowCursor
            // 拖拽平移画布。拖动期间解除单元追踪，避免追踪箭头跟随视口平移产生误导
            property bool _panning: false
            // 从按下点计算的实际移动距离；超过阈值后才进入平移。
            property real _totalDrag: 0
            property bool _dragActive: false
            property point _panAnchor: Qt.point(0, 0)
            property point _centerAnchor: Qt.point(0, 0)
            // 仅在常规（非引导）模式下允许拖拽平移
            property bool _panningEnabled: !root.guideMode
            // 拖拽结束后 onClicked 仍会被触发；用此标记让 onClicked 跳过
            property bool _dragJustEnded: false

            onPositionChanged: function(mouse) {
                root.pointerLogicalPos = root.logicalFromPixel(mouse.x, mouse.y)
                root.pointerInside = true
                if (root.guideMode) {
                    innerCanvas._guideHover = Qt.point(mouse.x, mouse.y)
                    innerCanvas.requestPaint()
                } else if (_panning) {
                    var dx = (mouse.x - _panAnchor.x)
                    var dy = (mouse.y - _panAnchor.y)
                    _totalDrag = Math.sqrt(dx * dx + dy * dy)
                    if (!_dragActive && _totalDrag <= 6) return
                    _dragActive = true
                    root.followSuspended = true
                    // 把像素位移转换为逻辑米位移，再平移 center
                    var ldx = dx / root.zoom
                    var ldy = dy / root.zoom
                    // 画布 y 轴向下，逻辑 y 轴向上；故 dy 取反
                    root.center = ({ x: _centerAnchor.x - ldx, y: _centerAnchor.y + ldy })
                    innerCanvas.requestPaint()
                }
            }
            onExited: {
                root.pointerInside = false
                if (root.guideMode) {
                    innerCanvas._guideHover = Qt.point(-1, -1)
                    innerCanvas.requestPaint()
                }
                // MouseArea 在按下后会持有鼠标抓取；即使指针移出画布，
                // 也要等到 release/cancel 再结束，避免释放被误判为新点击。
            }
            onCanceled: {
                _panning = false
                _totalDrag = 0
                _dragActive = false
                _dragJustEnded = false
            }
            onPressed: function(mouse) {
                if (root.guideMode) return
                // acceptedButtons already limits this area to primary/secondary clicks.
                _panning = true
                _totalDrag = 0
                _dragActive = false
                _panAnchor = Qt.point(mouse.x, mouse.y)
                _centerAnchor = Qt.point(root.center.x, root.center.y)
                // 平移期间解除追踪
                if (root.trackingTargetId) root.setTrackingTarget("", "")
            }
            onReleased: function(mouse) {
                if (_panning) {
                    _panning = false
                    var wasDragged = _dragActive
                    if (wasDragged) {
                        // 拖拽释放只结束平移，不命中单元、不弹出菜单、不恢复跟随。
                        _dragJustEnded = true
                        _totalDrag = 0
                        _dragActive = false
                        Qt.callLater(function() { canvasMouse._dragJustEnded = false })
                        return
                    }
                    if (mouse.button === Qt.LeftButton) {
                        var markerHit = root.commandMarkerAtPixel(mouse.x, mouse.y)
                        if (markerHit) {
                            _dragJustEnded = true
                            _totalDrag = 0
                            _dragActive = false
                            root.mapMarkerClicked(markerHit)
                            return
                        }
                    }
                    var hit = null
                    for (var i = 0; i < innerCanvas.units.length; i++) {
                        var u = innerCanvas.units[i]
                        if (!u || !u.alive || !root.isVisible(u)) continue
                        var p = root.toPixel(u.position[0], u.position[1])
                        var dx = mouse.x - p.x, dy = mouse.y - p.y
                        if (dx*dx + dy*dy < 14*14) { hit = u; break }
                    }
                    if (hit) {
                        if (mouse.button === Qt.RightButton) {
                            _dragJustEnded = true
                            if (root.allowRightClickActions) {
                                root.focusOnUnit(hit.id)
                                root.unitClicked(hit.id, "right", mouse.modifiers)
                            }
                        } else {
                            root.focusOnUnit(hit.id)
                            _dragJustEnded = true
                            root.unitClicked(hit.id, "left", mouse.modifiers)
                        }
                    }
                    _totalDrag = 0
                    _dragActive = false
                }
            }
            onWheel: function(wheel) {
                var delta = wheel.angleDelta.y > 0 ? 1.1 : 1/1.1
                root.zoom = Math.max(0.05, Math.min(20.0, root.zoom * delta))
                innerCanvas.requestPaint()
            }
            onClicked: function(mouse) {
                // release 已处理单元点击，或本次交互是拖拽，则忽略 onClicked。
                if (_dragJustEnded) { _dragJustEnded = false; return }
                var lp = root.logicalFromPixel(mouse.x, mouse.y)

                if (root.guideMode) {
                    // 引导源 unit 已死/离场则直接退出引导模式
                    var guideSource = root.guideSourceUnitId
                                      ? root.controller.unitAt(root.guideSourceUnitId) : null
                    if (!guideSource || !guideSource.alive) {
                        root.stopGuideMode()
                        root.guideCancelled()
                        return
                    }
                    // 检查是否点击了友方可移动单位 → 切换引导目标
                    for (var gf = 0; gf < innerCanvas.units.length; gf++) {
                        var gfu = innerCanvas.units[gf]
                        if (!root.isVisible(gfu)) continue
                        if (!gfu.alive) continue
                        if (gfu.side !== root.sideFilter) continue
                        if (gfu.kind === "commandpost") continue
                        var gfp = root.toPixel(gfu.position[0], gfu.position[1])
                        var gfdx = mouse.x - gfp.x, gfdy = mouse.y - gfp.y
                        if (gfdx*gfdx + gfdy*gfdy < 14*14) {
                            root.guideSourceUnitId = gfu.id
                            root.controller.setFocusedUnitId(gfu.id)
                            root.guideSourceChanged(gfu.id)
                            if (gfu.position) root.centerOn(gfu.position[0], gfu.position[1])
                            innerCanvas.requestPaint()
                            return
                        }
                    }
                    var pickedPos = lp
                    var pickedTarget = ""
                    for (var gi = 0; gi < innerCanvas.units.length; gi++) {
                        var gu = innerCanvas.units[gi]
                        if (!root.isVisible(gu)) continue
                        // 引导目标仅命中敌方（不含己方）；通过侧边判定敌我
                        if (gu.side === root.sideFilter) continue
                        var gp = root.toPixel(gu.position[0], gu.position[1])
                        var gdx = mouse.x - gp.x, gdy = mouse.y - gp.y
                        if (gdx*gdx + gdy*gdy < 14*14) {
                            pickedPos = { x: gu.position[0], y: gu.position[1] }
                            pickedTarget = gu.id
                            break
                        }
                    }
                    root.guidePointPicked(pickedPos, pickedTarget)
                    return
                }

                // 区分"点击"与"拖拽后释放"：鼠标按下与释放位置接近 → 视为点击
                // onReleased 已经处理拖拽；这里只处理未发生平移的情况
                if (_panning) return
                var hit = null
                for (var ci = 0; ci < innerCanvas.units.length; ci++) {
                    var cu = innerCanvas.units[ci]
                    if (!cu || !cu.alive || !root.isVisible(cu)) continue
                    var cp = root.toPixel(cu.position[0], cu.position[1])
                    var cdx = mouse.x - cp.x, cdy = mouse.y - cp.y
                    if (cdx*cdx + cdy*cdy < 14*14) { hit = cu; break }
                }
                if (mouse.button === Qt.RightButton) {
                    if (root.allowRightClickActions) {
                        if (hit) root.unitClicked(hit.id, "right", mouse.modifiers)
                        else root.rightClickedMap(lp)
                    }
                } else {
                    if (hit) root.unitClicked(hit.id, "left", mouse.modifiers)
                    else root.clickedMap(lp)
                }
            }
            onDoubleClicked: function(mouse) {
                var hit = null
                for (var i = 0; i < innerCanvas.units.length; i++) {
                    var u = innerCanvas.units[i]
                    if (!u || !u.alive || !root.isVisible(u)) continue
                    var p = root.toPixel(u.position[0], u.position[1])
                    var dx = mouse.x - p.x, dy = mouse.y - p.y
                    if (dx*dx + dy*dy < 14*14) { hit = u; break }
                }
                if (hit) root.doubleClickedUnit(hit.id)
                else root.doubleClickedMap(root.logicalFromPixel(mouse.x, mouse.y))
            }
        }
    }

    Rectangle {
        visible: root.showCoordinateReadout && root.pointerInside
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 10
        z: 60
        implicitWidth: coordinateText.implicitWidth + 16
        implicitHeight: coordinateText.implicitHeight + 10
        radius: 4
        color: t.coordinateBg
        border.color: t.coordinateBorder
        opacity: visible ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
        Text {
            id: coordinateText
            anchors.centerIn: parent
            text: "X " + Math.round(root.pointerLogicalPos.x) + "  Y " + Math.round(root.pointerLogicalPos.y) + " m"
            color: t.coordinateText
            font.pixelSize: 10
        }
    }

    Rectangle {
        id: actionPulse
        visible: root.actionPulseActive
        x: root.toPixel(root.actionPulseX, root.actionPulseY).x - width / 2
        y: root.toPixel(root.actionPulseX, root.actionPulseY).y - height / 2
        width: 30
        height: 30
        radius: width / 2
        color: "transparent"
        border.color: root.actionPulseColor
        border.width: 2
        z: 55
        scale: 0.35
        opacity: 1
    }
    ParallelAnimation {
        id: actionPulseAnimation
        running: false
        NumberAnimation { target: actionPulse; property: "scale"; from: 0.35; to: 1.8; duration: 360 }
        NumberAnimation { target: actionPulse; property: "opacity"; from: 1; to: 0; duration: 360 }
        onStopped: root.actionPulseActive = false
    }

    Component.onCompleted: {
        root.applyMapInfo(true)
        innerCanvas.units = root.controller.units
        root.observeAbilityTransitions(innerCanvas.units || [])
        innerCanvas.acceptProjectileSample(root.controller.projectiles)
        root.rebuildRecentPaths()
        innerCanvas.requestPaint()
    }

    // 全局 ESC 监听：退出路径引导模式
    // 使用两个 Shortcut 确保兼容：捕获 Escape 和 Qt.Key_Escape（所有平台）
    Shortcut {
        sequence: "Escape"
        enabled: root.guideMode
        context: Qt.WindowShortcut
        onActivated: {
            root.stopGuideMode()
            root.guideCancelled()
        }
    }

    // 摧毁闪光动画
    Rectangle {
        id: destroyFlashRect
        visible: root.destroyFlashActive
        x: {
            var px = root.toPixel(root.destroyLogicalX, root.destroyLogicalY)
            return px.x - 30
        }
        y: {
            var py = root.toPixel(root.destroyLogicalX, root.destroyLogicalY)
            return py.y - 30
        }
        width: 60; height: 60; radius: 30
        color: "transparent"
        border.color: "#ff4060"; border.width: 4
        z: 100
        scale: 0.3
        opacity: 1.0
        SequentialAnimation {
            id: destroyFlashAnim
            running: false
            ParallelAnimation {
                NumberAnimation { target: destroyFlashRect; property: "scale"; from: 0.3; to: 2.5; duration: 700 }
                NumberAnimation { target: destroyFlashRect; property: "opacity"; from: 1.0; to: 0.0; duration: 700 }
            }
            onStopped: root.destroyFlashActive = false
        }
    }
    Rectangle {
        visible: root.guideMode
        anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 18
        implicitHeight: 28
        implicitWidth: hintRow.implicitWidth + 28
        radius: 14
        color: "#1a1f2b"
        border.color: "#46d29a"
        z: 50
        Row {
            id: hintRow
            anchors.centerIn: parent
            spacing: 8
            Rectangle { width: 8; height: 8; radius: 4; color: "#46d29a"; anchors.verticalCenter: parent.verticalCenter }
            Text {
                text: "路径引导模式：点击地图 / 敌方单位指定目标 (ESC 取消)"
                color: "#f3f6fb"
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
                renderType: Text.NativeRendering
            }
        }
    }
}
