pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"

Item {
    id: root
    property var controller: null
    property var editor: null

    property string side: root.controller.viewMode === "commandpost-blue" ? "blue" : "red"
    property string enemySide: root.side === "red" ? "blue" : "red"
    property var snap: ({})
    property bool autoFollow: true   // controlled by settings
    property var ownUnitOptions: ([])
    property bool ownReady: root.side === "red" ? root.controller.redReady : root.controller.blueReady
    property bool commandsEnabled: !root.controller.networked || root.controller.matchPhase === "running"

    function refreshOwnUnitOptions() {
        var next = root.controller.unitOptions("", root.side)
        var current = root.ownUnitOptions || []
        var changed = next.length !== current.length
        for (var i = 0; !changed && i < next.length; i++) {
            if (next[i].id !== current[i].id || next[i].callsign !== current[i].callsign
                    || next[i].kind !== current[i].kind || next[i].side !== current[i].side)
                changed = true
        }
        if (changed) root.ownUnitOptions = next
    }

    function switchUnit(dir) {
        var opts = root.controller.unitOptions("", root.side)
        if (!opts || opts.length === 0) return
        var idx = -1
        for (var i = 0; i < opts.length; i++) {
            if (opts[i].id === root.controller.focusedUnitId) { idx = i; break }
        }
        if (idx < 0) idx = 0
        idx = (idx + dir + opts.length) % opts.length
        root.controller.setFocusedUnitId(opts[idx].id)
        canvas.focusOnUnit(opts[idx].id)
    }

    function refreshSnap() {
        root.snap = root.controller.unitAt(root.controller.focusedUnitId) || {}
    }
    function refreshDetected() {
        if (!canvas) return
        canvas.detectedEnemyIds = root.controller.detectedEnemyIds(root.side)
    }
    Connections {
        target: root.controller
        function onFocusedUnitIdChanged() {
            // Force re-center on the new focus unit next tick.
            root._lastFollowPos = Qt.point(NaN, NaN)
            root.refreshSnap()
            canvas.focusOnUnit(root.controller.focusedUnitId)
        }
        function onUnitsForward() {
            root.refreshSnap()
            root.refreshDetected()
            root.refreshOwnUnitOptions()
        }
        function onMessagesForward() { root.refreshDetected() }
    }

    QtObject {
        id: theme
        property color panel: "#0e1322"
        property color panelAlt: "#131a2c"
        property color border: "#1e2d4a"
        property color text: "#e8edf5"
        property color textStrong: "#ffffff"
        property color textDim: "#bcc8de"
        property color muted: "#8896b8"
        property color dimmer: "#5a6a88"
        property color red: "#f04760"
        property color blue: "#4090ff"
        property color accent: "#4090ff"
        property color accentSoft: "#1e4080"
        property color success: "#36c98a"
        property color warning: "#f0a040"
        property color danger: "#f04760"
        property color disabled: "#3b4458"
        property color switchOn: "#36c98a"
    }

    property var discoverySet: ([])
    property bool canAttack: false
    property bool canGuide: false
    property bool simEnded: false
    property bool cpDead: false
    property string destroyedCpName: ""
    property string destroyedAttacker: ""

    // 检查指挥部是否被摧毁
    function checkCommandPostAlive() {
        // SimulationRoot owns the authoritative end-of-simulation dialog.
        // Once the engine has stopped, do not open a second local modal.
        if (!root.controller.running) return false
        var cpId = root.controller.commandPostIdFor(root.side)
        var cp = root.controller.unitAt(cpId)
        if (cp && !cp.alive && cp.hp !== undefined && cp.hp <= 0) {
            root.simEnded = true
            root.destroyedCpName = cp.callsign || cpId
            root.controller.setRunning(false)
            canvas.setTrackingTarget("", "")
            endDialog.open()
            return true
        }
        return false
    }

    Timer {
        id: cpCheckTimer
        interval: 2000; running: root.controller.running && !root.cpDead; repeat: true
        onTriggered: { if (root.checkCommandPostAlive()) root.cpDead = true }
    }
    // Combined timer: refresh attack/guide eligibility and detection map in
    // one tick instead of two. 1.0s is sufficient for human-paced decisions.
    Timer {
        id: statusRefreshTimer
        interval: 1000; running: true; repeat: true
        onTriggered: {
            root.canAttack = root.hasTargetInAttackRange()
            root.canGuide = root.hasTargetInDetectShared()
            root.refreshDetected()
        }
    }

    function autoFitZoom() {
        var units = root.controller.unitOptions("", root.side)
        if (!units || units.length < 2) return
        var minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9
        for (var i = 0; i < units.length; i++) {
            var u = root.controller.unitAt(units[i].id)
            if (!u || !u.position) continue
            var px = u.position[0], py = u.position[1]
            if (px < minX) minX = px; if (px > maxX) maxX = px
            if (py < minY) minY = py; if (py > maxY) maxY = py
        }
        if (maxX <= minX || maxY <= minY) return
        var spanX = (maxX - minX) * 1.3 + 2000
        var spanY = (maxY - minY) * 1.3 + 2000
        var zx = canvas.width / spanX, zy = canvas.height / spanY
        canvas.zoom = Math.max(0.1, Math.min(5.0, Math.min(zx, zy)))
        canvas.centerOn((minX + maxX) / 2, (minY + maxY) / 2)
    }

    // Event-driven follow: only re-center when the focused unit has moved
    // by more than ~50m (one pixel at typical zoom) since the last follow.
    property point _lastFollowPos: Qt.point(NaN, NaN)
    Timer {
        id: centerTimer
        interval: 250; running: root.autoFollow && !canvas.followSuspended
                 && root.controller.running && root.controller.focusedUnitId.length > 0; repeat: true
        onTriggered: {
            if (!root.controller.running || !root.controller.focusedUnitId) return
            var u = root.controller.unitAt(root.controller.focusedUnitId)
            if (!u || !u.position) return
            var px = u.position[0], py = u.position[1]
            if (!isNaN(root._lastFollowPos.x)) {
                var dx = px - root._lastFollowPos.x, dy = py - root._lastFollowPos.y
                if (dx*dx + dy*dy < 50*50) return
            }
            root._lastFollowPos = Qt.point(px, py)
            canvas.centerOn(px, py)
        }
    }

    ListModel { id: eventList }
    Connections {
        target: root.controller
        function onEventForward(title, body, level, sourceUnitId) {
            var u = root.controller.unitAt(sourceUnitId)
            if (u && u.side !== root.side) return
            eventList.append({title: title, body: body, level: level, ts: Date.now()})
            while (eventList.count > 30) eventList.remove(0)
            if (title === "单元被摧毁") {
                // 提取攻击者名称
                var m = body.match(/被 (.+) 摧毁/)
                if (m) root.destroyedAttacker = m[1]
                root.checkCommandPostAlive()
            }
        }
        function onTargetDestroyedVisual(unitId, x, y) {
            canvas.refresh()
            canvas.flashDestroyAt(x, y)
            if (canvas.trackingTargetId === unitId) {
                canvas.setTrackingTarget("", "")
            }
        }
    }
    Connections {
        target: root.controller
        function onUnitsForward() {
            root.canAttack = root.hasTargetInAttackRange()
            root.canGuide = root.hasTargetInDetectShared()
        }
        function onMessagesForward() {
            var msgs = root.controller.messages
            var newDisc = root.discoverySet.slice()
            for (var i = msgs.length - 1; i >= 0 && i >= msgs.length - 20; i--) {
                var m = msgs[i]
                var targetId = m.payload ? m.payload.targetId : ""
                if (m.type === "TargetDetect" && m.senderSide === root.side && targetId) {
                    if (newDisc.indexOf(targetId) < 0) newDisc.push(targetId)
                }
            }
            if (newDisc.length > 20) newDisc = newDisc.slice(newDisc.length - 20)
            root.discoverySet = newDisc
        }
    }


    function attackableTargets(attackerId) {
        return root.controller.attackableTargets(attackerId, root.enemySide)
    }
    function detectedEnemyOptions(attackerId) {
        return root.controller.detectedEnemyOptions(attackerId, root.side, root.enemySide)
    }

    function hasTargetInAttackRange() {
        return root.controller.hasTargetInAttackRange(root.controller.focusedUnitId, root.enemySide)
    }

    function hasTargetInDetectShared() {
        return root.controller.hasTargetInDetectShared(root.controller.focusedUnitId, root.side, root.enemySide)
    }

    function hasAnyAttackUav() {
        var opts = root.controller.unitOptions("attackuav", root.side)
        return !!(opts && opts.length > 0)
    }

    RowLayout {
        anchors.fill: parent; spacing: 0

        Item {
            Layout.fillHeight: true; Layout.fillWidth: true
            MapCanvas { controller: root.controller; editor: root.editor;
                id: canvas
                anchors.fill: parent; anchors.margins: 12
                sideFilter: root.side; showAllSides: false
                focusUnitId: root.controller.focusedUnitId
                showRoutes: false
                showDetectRange: detectRangeToggle.checked
                showAttackRange: attackRangeToggle.checked
                showRecentPaths: recentPathToggle.checked
                simTime: root.controller.simTime
                discoveryUnits: root.discoverySet

                onUnitClicked: function(uid, btn) {
                    if (!root.commandsEnabled) return
                    var u = root.controller.unitAt(uid)
                    if (!u || !u.alive) return
                    if (u.side !== root.side) {
                        // click enemy → attack dialog
                        attackDialog.openWithTarget(uid)
                        return
                    }
                    // 指挥所不可移动：不进入引导模式
                    if (u.kind === "commandpost") {
                        root.controller.setFocusedUnitId(uid)
                        canvas.focusOnUnit(uid)
                        return
                    }
                    // 友方机动单位：直接进入路径引导模式，不弹详情面板
                    root.controller.setFocusedUnitId(uid)
                    canvas.focusOnUnit(uid)
                    canvas.startGuideMode(uid)
                }
                onGuidePointPicked: function(lp, tid) {
                    if (!root.commandsEnabled) { canvas.stopGuideMode(); return }
                    var srcId = canvas.guideSourceUnitId
                    if (!srcId) { canvas.stopGuideMode(); return }
                    var srcUnit = root.controller.unitAt(srcId)
                    if (tid) {
                        var tgt = root.controller.unitAt(tid)
                        if (tgt && tgt.alive) {
                            if (srcUnit && srcUnit.kind === "attackuav") {
                                root.controller.command("assignTarget", { attackerId: srcId, targetId: tid })
                                root.controller.command("setFlightPlan", { attackerId: srcId, waypoints: [{ x: lp.x, y: lp.y }] })
                                canvas.setTrackingTarget(srcId, tid)
                                canvas.refreshTrackingPos()
                            } else {
                                root.controller.command("moveTo", { unitId: srcId, pos: lp })
                            }
                        } else {
                            root.controller.command("moveTo", { unitId: srcId, pos: lp })
                        }
                    } else {
                        root.controller.command("moveTo", { unitId: srcId, pos: lp })
                    }
                    canvas.stopGuideMode()
                    root.controller.setFocusedUnitId(srcId)
                    canvas.focusOnUnit(srcId)
                }
                onGuideCancelled: {
                    // 视角切换时也会触发，源单元恢复为当前选中
                    if (canvas.guideSourceUnitId) root.controller.setFocusedUnitId(canvas.guideSourceUnitId)
                }
                onClickedMap: function(lp) {
                    if (!root.commandsEnabled || canvas.guideMode) return
                    if (root.controller.focusedUnitId)
                        root.controller.command("moveTo", { unitId: root.controller.focusedUnitId, pos: lp })
                }
                onDoubleClickedUnit: function(uid) {
                    if (!canvas.guideMode) return
                    var atk = root.controller.unitAt(root.controller.focusedUnitId)
                    if (!atk || atk.kind !== "attackuav" || !atk.alive) return
                    var tgt = root.controller.unitAt(uid)
                    if (!tgt || !tgt.alive) return
                    if (tgt.side === atk.side) return
                    root.controller.command("pursue", { attackerId: root.controller.focusedUnitId, targetId: uid })
                    canvas.setTrackingTarget(root.controller.focusedUnitId, uid)
                    canvas.refreshTrackingPos()
                    canvas.stopGuideMode()
                }
            }

            // 控制栏
            Rectangle {
                anchors.right: parent.right; anchors.top: parent.top
                anchors.rightMargin: 24; anchors.topMargin: 24
                color: "#0c1122"; border.color: "#1e2d4a"; radius: 8
                implicitHeight: 34; z: 10
                width: Math.min(canvas.width * 0.42, 400)
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
                    spacing: 8
                    Text { text: "侦察"; color: theme.textDim; font.pixelSize: 11; renderType: Text.NativeRendering }
                    Switch {
                        id: detectRangeToggle; checked: true
                        Layout.preferredWidth: 36
                        indicator: Rectangle {
                            implicitWidth: 34; implicitHeight: 18; radius: 9
                            color: detectRangeToggle.checked ? theme.accent : "#3b4252"
                            border.color: detectRangeToggle.checked ? theme.accent : theme.border
                            Rectangle { x: detectRangeToggle.checked ? 18 : 2; y: 2; width: 14; height: 14; radius: 7; color: "#fff"
                                Behavior on x { NumberAnimation { duration: 120 } }
                            }
                        }
                    }
                    Text { text: "攻击"; color: theme.textDim; font.pixelSize: 11; renderType: Text.NativeRendering }
                    Switch {
                        id: attackRangeToggle; checked: true
                        Layout.preferredWidth: 36
                        indicator: Rectangle {
                            implicitWidth: 34; implicitHeight: 18; radius: 9
                            color: attackRangeToggle.checked ? theme.danger : "#3b4252"
                            border.color: attackRangeToggle.checked ? theme.danger : theme.border
                            Rectangle { x: attackRangeToggle.checked ? 18 : 2; y: 2; width: 14; height: 14; radius: 7; color: "#fff"
                                Behavior on x { NumberAnimation { duration: 120 } }
                            }
                        }
                    }
                    Text { text: "轨迹"; color: theme.textDim; font.pixelSize: 11; renderType: Text.NativeRendering }
                    Switch {
                        id: recentPathToggle; checked: true
                        Layout.preferredWidth: 36
                        indicator: Rectangle {
                            implicitWidth: 34; implicitHeight: 18; radius: 9
                            color: recentPathToggle.checked ? theme.warning : "#3b4252"
                            border.color: recentPathToggle.checked ? theme.warning : theme.border
                            Rectangle { x: recentPathToggle.checked ? 18 : 2; y: 2; width: 14; height: 14; radius: 7; color: "#fff"
                                Behavior on x { NumberAnimation { duration: 120 } }
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                    TonalButton {
                        text: "自适应缩放"; base: theme.accent
                        implicitHeight: 24
                        onClicked: root.autoFitZoom()
                    }
                }
            }

            Rectangle {
                anchors.fill: parent; z: 50
                visible: root.controller.networked && root.controller.matchPhase !== "running"
                color: "#090e16b8"
                Rectangle {
                    anchors.centerIn: parent; width: Math.min(430, parent.width - 40); height: 86
                    color: "#121b26"; border.color: "#314155"; radius: 6
                    Column {
                        anchors.centerIn: parent; spacing: 6
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: root.controller.matchPhase === "preparing" ? "推演尚未开始" : "本轮推演已结束"; color: theme.textStrong; font.pixelSize: 17; font.bold: true }
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: root.controller.matchPhase === "preparing" ? "请在右侧确认阵容并提交就绪" : "等待导演结束并重置推演"; color: theme.muted; font.pixelSize: 12 }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: 400
            color: theme.panel
            Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 1; color: theme.border }

            ColumnLayout {
                anchors.fill: parent; anchors.margins: 14; spacing: 6

                RowLayout {
                    spacing: 8
                    Rectangle {
                        Layout.preferredWidth: 4; Layout.preferredHeight: 20; radius: 2
                        color: root.side === "red" ? theme.red : theme.blue
                        Layout.alignment: Qt.AlignVCenter
                    }
                    ColumnLayout {
                        spacing: 1
                        Text {
                            text: (root.side === "red" ? "红方" : "蓝方") + " · 指挥所"
                            color: theme.textStrong; font.pixelSize: 18; font.bold: true; renderType: Text.NativeRendering
                        }
                        Text {
                            text: "实时态势 / 事件队列 / 指令下达"
                            color: theme.muted; font.pixelSize: 11; renderType: Text.NativeRendering
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.border; Layout.topMargin: 4; Layout.bottomMargin: 2 }

                Rectangle {
                    visible: root.controller.networked
                    Layout.fillWidth: true; Layout.preferredHeight: 68
                    color: "#111b25"; border.color: root.ownReady ? theme.success : theme.border; radius: 6
                    RowLayout {
                        anchors.fill: parent; anchors.margins: 10; spacing: 8
                        ColumnLayout {
                            spacing: 2; Layout.fillWidth: true
                            Text { text: root.controller.matchPhase === "preparing" ? (root.ownReady ? "己方已就绪" : "准备初始阵容") : root.controller.matchPhase === "running" ? "推演进行中" : "等待导演结束推演"; color: root.ownReady ? theme.success : theme.textStrong; font.pixelSize: 13; font.bold: true }
                            Text { text: "红方 " + (root.controller.redReady ? "已就绪" : "未就绪") + " · 蓝方 " + (root.controller.blueReady ? "已就绪" : "未就绪"); color: theme.muted; font.pixelSize: 10 }
                        }
                        GhostButton { visible: root.controller.matchPhase === "preparing"; text: "阵容"; onClicked: rosterDialog.open() }
                        TonalButton {
                            visible: root.controller.matchPhase === "preparing"
                            text: root.ownReady ? "取消就绪" : "准备完毕"
                            base: root.ownReady ? theme.warning : theme.success
                            onClicked: root.controller.setReady(!root.ownReady)
                        }
                    }
                }

                SectionTitle { text: "选择己方单元" }
                ComboBox {
                    id: myCombo
                    Layout.fillWidth: true
                    model: root.ownUnitOptions
                    textRole: "callsign"; valueRole: "id"
                    onActivated: function(idx) {
                        root.controller.setFocusedUnitId(model[idx].id)
                        canvas.focusOnUnit(model[idx].id)
                    }
                    Connections {
                        target: root.controller
                        function onFocusedUnitIdChanged() {
                            var m = myCombo.model
                            if (!m) return
                            for (var i = 0; i < m.length; i++) {
                                if (m[i].id === root.controller.focusedUnitId) {
                                    if (myCombo.currentIndex !== i) myCombo.currentIndex = i
                                    return
                                }
                            }
                        }
                        function onUnitsForward() {
                            // model 重建后保持 currentIndex 同步
                            var m = myCombo.model
                            if (!m) return
                            for (var i = 0; i < m.length; i++) {
                                if (m[i].id === root.controller.focusedUnitId) {
                                    myCombo.currentIndex = i; return
                                }
                            }
                        }
                    }
                    Component.onCompleted: {
                        // 初次绑定时同步
                        var m = myCombo.model
                        if (!m) return
                        for (var i = 0; i < m.length; i++) {
                            if (m[i].id === root.controller.focusedUnitId) {
                                myCombo.currentIndex = i; return
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 180; Layout.maximumHeight: 220
                    color: theme.panelAlt; border.color: root.canAttack ? theme.danger : (root.canGuide ? theme.warning : theme.border); radius: 6
                    Behavior on border.color { ColorAnimation { duration: 200 } }
                    UnitPanel { controller: root.controller; editor: root.editor; anchors.fill: parent; anchors.margins: 12; snap: root.snap; interactionEnabled: root.commandsEnabled && root.snap.side === root.side }
                }

                SectionTitle { text: "事件队列" }
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 130; Layout.minimumHeight: 80; Layout.maximumHeight: 220
                    color: theme.panelAlt; border.color: theme.border; radius: 6
                    ListView {
                        anchors.fill: parent; anchors.margins: 6; clip: true; model: eventList
                        delegate: Rectangle {
                            id: eventRow
                            required property int index
                            required property string level
                            required property string title
                            required property string body
                            width: ListView.view.width; height: Math.max(40, eventBodyText.implicitHeight + eventTitleText.implicitHeight + 14)
                            color: eventRow.index % 2 === 0 ? "#1a1f2b" : "#1e2432"
                            radius: 3
                            Rectangle { anchors.left: parent.left; anchors.top: parent.top
                                anchors.topMargin: 3; anchors.bottom: parent.bottom; anchors.bottomMargin: 3
                                width: 3; radius: 2
                                color: eventRow.level === "success" ? theme.success
                                     : (eventRow.level === "warn" ? theme.warning : theme.accent) }
                            Rectangle {
                                anchors.left: parent.left; anchors.leftMargin: 7
                                anchors.verticalCenter: parent.verticalCenter
                                width: 8; height: 8; radius: 4
                                color: eventRow.level === "success" ? theme.success
                                     : (eventRow.level === "warn" ? theme.warning : theme.accent)
                            }
                            Column {
                                anchors.left: parent.left; anchors.right: parent.right
                                anchors.leftMargin: 22; anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                Text {
                                    id: eventTitleText
                                    text: eventRow.title; color: theme.textStrong; font.bold: true
                                    font.pixelSize: 12; renderType: Text.NativeRendering
                                }
                                Text {
                                    id: eventBodyText
                                    text: eventRow.body; color: theme.textDim; font.pixelSize: 11
                                    wrapMode: Text.WordWrap; width: ListView.view.width - 48
                                    renderType: Text.NativeRendering
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: { root.eventDetailId = eventRow.index; eventDetail.open() }
                                onPressAndHold: eventList.remove(eventRow.index)
                            }
                        }
                    }
                }

                SectionTitle { text: "实时消息流" }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 60
                    color: theme.panelAlt; border.color: theme.border; radius: 6
                    MessageLog { anchors.fill: parent; anchors.margins: 1; messages: root.controller.messages; sideFilter: root.side }
                }
            }
        }
    }

    // ===== 路径规划 =====
    RoutePlannerDialog {
        id: routeDialog
        controller: root.controller
        editor: root.editor
        onRouteAccepted: function(points) {
            if (!root.controller.focusedUnitId) return
            root.controller.setUnitSchedule(root.controller.focusedUnitId, points)
            var wps = []
            for (var i = 0; i < points.length; i++) wps.push({ x: points[i].x, y: points[i].y })
            root.controller.command("setFlightPlan", { attackerId: root.controller.focusedUnitId, waypoints: wps })
        }
    }

    RosterEditorDialog {
        id: rosterDialog
        controller: root.controller
        editor: root.editor
    }

    // ===== 攻击对话框（固定选中攻击机，仅选目标）=====
    Dialog {
        id: attackDialog
        property string targetId: ""
        property string forcedAttackerId: ""
        property var attackableList: ([])
        function resolveAttacker() {
            if (attackDialog.forcedAttackerId) return attackDialog.forcedAttackerId
            var opts = root.controller.unitOptions("attackuav", root.side)
            var firstId = (opts && opts.length > 0) ? opts[0].id : ""
            if (root.controller.focusedUnitId) {
                var fu = root.controller.unitAt(root.controller.focusedUnitId)
                if (fu && fu.kind === "attackuav" && fu.alive) return root.controller.focusedUnitId
            }
            return firstId
        }
        function openWithTarget(tid) {
            attackDialog.targetId = tid
            attackDialog.forcedAttackerId = ""
            attackDialog.attackableList = root.attackableTargets(attackDialog.resolveAttacker())
            attackDialog.open()
        }
        function openForAttacker(aid, tid) {
            attackDialog.targetId = tid
            attackDialog.forcedAttackerId = aid
            attackDialog.attackableList = root.attackableTargets(aid)
            attackDialog.open()
        }
        onOpened: {
            if (attackDialog.targetId && targetCombo.model && targetCombo.model.length) {
                for (var i = 0; i < targetCombo.model.length; i++) {
                    if (targetCombo.model[i].id === attackDialog.targetId) {
                        targetCombo.currentIndex = i; break
                    }
                }
            }
        }
        title: "下达攻击命令"; modal: true; anchors.centerIn: parent; standardButtons: Dialog.NoButton
        background: Rectangle { color: "#1a1f2b"; border.color: theme.danger; border.width: 2; radius: 8 }
        ColumnLayout {
            spacing: 12; anchors.margins: 20
            RowLayout {
                spacing: 10
                Rectangle { Layout.preferredWidth: 4; Layout.preferredHeight: 20; radius: 2; color: theme.danger; Layout.alignment: Qt.AlignVCenter }
                ColumnLayout { spacing: 1
                    Text {
                        text: {
                            var aid = attackDialog.forcedAttackerId || attackDialog.resolveAttacker()
                            var atk = root.controller.unitAt(aid)
                            return "攻击机： " + (atk ? atk.callsign : aid || "—")
                        }
                        color: theme.textStrong; font.pixelSize: 15; font.bold: true; renderType: Text.NativeRendering
                    }
                    Text {
                        text: {
                            var aid = attackDialog.forcedAttackerId || attackDialog.resolveAttacker()
                            var atk = root.controller.unitAt(aid)
                            var range = atk ? Math.round(atk.attackRange || 0) : 0
                            return "攻击范围 " + range + " m  |  范围内目标"
                        }
                        color: theme.muted; font.pixelSize: 11; renderType: Text.NativeRendering
                    }
                }
            }
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.border }
            Text { text: "\u76ee\u6807\uff08\u653b\u51fb\u8303\u56f4\u5185\uff09"; color: theme.muted; font.pixelSize: 11; renderType: Text.NativeRendering }
            ComboBox {
                id: targetCombo; Layout.fillWidth: true
                model: attackDialog.attackableList
                textRole: "callsign"; valueRole: "id"
            }
            Text {
                visible: attackDialog.attackableList.length === 0
                text: "\u2605 \u653b\u51fb\u8303\u56f4\u5185\u6682\u65e0\u654c\u65b9\u76ee\u6807"
                color: theme.warning; font.pixelSize: 12; renderType: Text.NativeRendering
            }
            Rectangle {
                visible: attackDialog.attackableList.length > 0 && targetCombo.currentIndex >= 0
                Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.border
                Text {
                    visible: parent.visible
                    text: {
                        if (targetCombo.currentIndex < 0) return ""
                        var t = attackDialog.attackableList[targetCombo.currentIndex]
                        var tid = t.id; var s = root.controller.unitAt(tid)
                        if (!s) return ""
                        var aid = attackDialog.forcedAttackerId || attackDialog.resolveAttacker()
                        var atk = root.controller.unitAt(aid)
                        var dist = 0
                        if (atk && atk.position && s.position) {
                            var dx = atk.position[0] - s.position[0]
                            var dy = atk.position[1] - s.position[1]
                            dist = Math.round(Math.sqrt(dx*dx + dy*dy))
                        }
                        return "\u8ddd\u79bb\uff1a" + dist + " m  \u00b7  " + (s.kind || "") + "  \u00b7  HP: " + Math.round(s.hp || 0) + "/" + Math.round(s.maxHp || 0)
                    }
                    color: theme.textDim; font.pixelSize: 11; renderType: Text.NativeRendering
                    anchors.left: parent.left; anchors.top: parent.top
                    anchors.topMargin: 6
                }
            }
        }
        footer: DialogButtonBox {
            TonalButton {
                text: "\u4e0b\u8fbe\u653b\u51fb"; base: theme.danger
                enabled: targetCombo.currentIndex >= 0
                onClicked: {
                    if (targetCombo.currentIndex < 0) return
                    var aid = attackDialog.forcedAttackerId || attackDialog.resolveAttacker()
                    var tid = attackDialog.attackableList[targetCombo.currentIndex].id
                    root.controller.command("assignTarget", { attackerId: aid, targetId: tid })
                    var s = root.controller.unitAt(tid)
                    if (s && s.position) {
                        root.controller.command("setFlightPlan", { attackerId: aid, waypoints: [{ x: s.position[0], y: s.position[1] }] })
                    }
                    eventList.append({title: "\u653b\u51fb\u547d\u4ee4\u5df2\u4e0b\u8fbe", body: aid + " \u2192 " + tid, level: "info", ts: Date.now()})
                    attackDialog.close()
                }
            }
            GhostButton { text: "\u53d6\u6d88"; onClicked: attackDialog.close() }
        }
    }

    // ===== 推演结束弹窗 =====
    Dialog {
        id: endDialog
        title: "推演结束"; modal: true; anchors.centerIn: parent; standardButtons: Dialog.NoButton
        background: Rectangle { color: "#0e1322"; border.color: theme.danger; border.width: 2; radius: 10 }
        ColumnLayout {
            spacing: 14; anchors.margins: 28
            Rectangle {
                Layout.preferredWidth: 360; Layout.preferredHeight: 4; radius: 2
                color: theme.danger
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                text: "指挥所已摧毁 · 推演结束"
                color: theme.danger; font.pixelSize: 20; font.bold: true
                Layout.alignment: Qt.AlignHCenter; renderType: Text.NativeRendering
            }
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.border }
            ColumnLayout {
                spacing: 6
                Text {
                    text: "已摧毁指挥所: " + (root.destroyedCpName || "—")
                    color: theme.textStrong; font.pixelSize: 14; font.bold: true; renderType: Text.NativeRendering
                }
                Text {
                    visible: root.destroyedAttacker.length > 0
                    text: "摧毁方: " + root.destroyedAttacker
                    color: theme.textDim; font.pixelSize: 13; renderType: Text.NativeRendering
                }
                Text {
                    text: {
                        var enemy = root.side === "red" ? "蓝方" : "红方"
                        return "己方（" + (root.side === "red" ? "红方" : "蓝方") + "）指挥所已被" + enemy + "摧毁，推演终止。"
                    }
                    color: theme.muted; font.pixelSize: 12; wrapMode: Text.WordWrap
                    Layout.preferredWidth: 340; renderType: Text.NativeRendering
                }
            }
            Row {
                Layout.alignment: Qt.AlignHCenter; spacing: 12
                TonalButton {
                    text: "确认"; base: theme.danger
                    onClicked: endDialog.close()
                }
                GhostButton {
                    text: "查看战报"
                    onClicked: { endDialog.close(); canvas.centerOn(canvas.mapSize.w/2, canvas.mapSize.h/2) }
                }
            }
        }
    }

    Component.onCompleted: {
        root.refreshSnap()
        root.refreshDetected()
        root.refreshOwnUnitOptions()
        root.cpDead = false
        root.simEnded = false
        initCenterTimer.start()
    }

    Timer {
        id: initCenterTimer
        interval: 100; repeat: false
        onTriggered: {
            canvas.zoom = 0.15
            root.autoFitZoom()
            var cpId = root.controller.commandPostIdFor(root.side)
            var cp = root.controller.unitAt(cpId)
            if (cp && cp.position) canvas.centerOn(cp.position[0], cp.position[1])
        }
    }

    property int eventDetailId: -1

    Dialog {
        id: eventDetail
        title: "事件详情"; modal: true; anchors.centerIn: parent; standardButtons: Dialog.Close
        background: Rectangle { color: "#1a1f2b"; border.color: theme.border; radius: 8 }
        property var item: (root.eventDetailId >= 0 && root.eventDetailId < eventList.count) ? eventList.get(root.eventDetailId) : null
        ColumnLayout {
            spacing: 12; anchors.margins: 20; width: 400
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 4; radius: 2
                color: eventDetail.item
                    ? (eventDetail.item.level === "success" ? theme.success
                    : (eventDetail.item.level === "warn" ? theme.warning : theme.accent))
                    : theme.accent
            }
            Text {
                text: eventDetail.item ? eventDetail.item.title : ""
                color: theme.textStrong; font.pixelSize: 16; font.bold: true
                renderType: Text.NativeRendering
            }
            Text {
                text: eventDetail.item ? eventDetail.item.body : ""
                color: theme.textDim; font.pixelSize: 13; wrapMode: Text.WordWrap
                Layout.fillWidth: true; renderType: Text.NativeRendering
            }
            Text {
                text: eventDetail.item ? "时间: " + new Date(eventDetail.item.ts).toLocaleTimeString() : ""
                color: theme.muted; font.pixelSize: 11; renderType: Text.NativeRendering
            }
        }
    }

    Connections {
        target: root.controller
        function onViewModeChanged() {
            canvas.stopGuideMode()
            canvas.setTrackingTarget("", "")
            root.refreshOwnUnitOptions()
            // Multiple sideFilter/property updates will trigger repaints.
            // Defer the centerOn/zoom work into a single 0-ms timer to coalesce.
            Qt.callLater(function() {
                canvas.zoom = 0.15
                root.autoFitZoom()
                root.cpDead = false
                root.simEnded = false
                var cpId = root.controller.commandPostIdFor(root.side)
                var cp = root.controller.unitAt(cpId)
                if (cp && cp.position) canvas.centerOn(cp.position[0], cp.position[1])
                root._lastFollowPos = Qt.point(NaN, NaN)
            })
        }
    }
}
