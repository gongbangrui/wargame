pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "components"
import "views"

Item {
    id: root
    property var controller: null
    property var editor: null
    property var appWindow: null
    property real uiScale: appWindow ? Math.max(0.9, Math.min(1.25, Math.min(appWindow.width / 1360, appWindow.height / 860))) : 1.0
    property bool compactTopBar: width < 1120
    anchors.fill: parent
    focus: true
    activeFocusOnTab: true
    // 联网推演的开停、速率和单步由网页管理员控制；本地模式保持原有席位控制。
    property bool simulationControlAllowed: !root.controller.networked
    property bool unitControlAllowed: !root.controller.networked || (root.controller.currentSeatId !== "" && root.controller.matchPhase === "running")
    property bool directorCanStart: !root.controller.networked
    property var activePage: pageLoader.item
    property int chatLastSeenCount: 0
    readonly property int chatUnreadCount: root.controller.networked
        ? Math.max(0, root.controller.chatMessages.length - root.chatLastSeenCount) : 0

    function switchActiveUnit(direction) {
        if (root.activePage && root.activePage.switchUnit)
            root.activePage.switchUnit(direction)
    }

    function fitActivePage() {
        if (root.activePage && root.activePage.autoFitZoom)
            root.activePage.autoFitZoom()
    }

    function activeCanvas() {
        return root.activePage && root.activePage["canvas"] ? root.activePage["canvas"] : null
    }

    // --- Keyboard shortcuts (configurable via settings) ---
    // ks_actions / ks_seqs are parallel arrays; the order MUST stay in sync.
    // ks_seqs is reassigned whenever a shortcut changes so every Shortcut item
    // rebinds on the same frame.
    property var ks_actions: [
        "toggleRun", "speed0", "speed1", "speed2", "speed4", "speed8",
        "step", "speedUp", "speedDown",
        "prevUnit", "nextUnit", "nextUnitTab", "prevUnitSh",
        "autoFit", "cancelTrack"
    ]
    property var ks_defaults: ({
        toggleRun: "Space", speed0: "1", speed1: "2", speed2: "3", speed4: "4", speed8: "5",
        step: ".", speedUp: "W", speedDown: "S",
        prevUnit: "A,Left", nextUnit: "D,Right",
        nextUnitTab: "Tab", prevUnitSh: "Shift+Tab",
        autoFit: "Ctrl+F", cancelTrack: "P"
    })
    property var ks_seqs: ks_loadAll()

    function ks_loadAll() {
        var result = ({})
        for (var i = 0; i < ks_actions.length; i++) {
            var a = ks_actions[i]
            result[a] = root.controller.loadSetting("shortcuts/" + a, ks_defaults[a])
        }
        return result
    }

    function reloadAllShortcuts() { ks_seqs = ks_loadAll() }

    function toSeqList(s) {
        if (typeof s !== "string" || s.trim().length === 0) return []
        return s.split(",").map(function(p){ return p.trim() }).filter(function(p){ return p.length > 0 })
    }

    // 单一 Shortcut：每次快捷键序列变化时整体重建
    // 注意：Qt 的 Shortcut 不支持直接绑定到动态 sequences 属性，必须重新赋值
    // 整个 Shortcut 实例；这里采用简化方案：用分组 Shortcut，每个绑定 ks_seqs[action]
    // ks_seqs 整体重新赋值会触发所有 Shortcut 的 sequences 重新求值

    Shortcut { sequences: root.toSeqList(root.ks_seqs["toggleRun"]   || ""); context: Qt.WindowShortcut; enabled: root.simulationControlAllowed && root.directorCanStart; onActivated: { if (!root.controller.readyForSim) { cpIssueDialog.refreshAndOpen(); return } root.controller.setRunning(!root.controller.running) }}
    Shortcut { sequences: root.toSeqList(root.ks_seqs["speed0"]      || ""); context: Qt.WindowShortcut; enabled: root.simulationControlAllowed; onActivated: { root.controller.setSpeed(0); speedCombo.currentIndex = 0 }}
    Shortcut { sequences: root.toSeqList(root.ks_seqs["speed1"]      || ""); context: Qt.WindowShortcut; enabled: root.simulationControlAllowed; onActivated: { root.controller.setSpeed(1); speedCombo.currentIndex = 1 }}
    Shortcut { sequences: root.toSeqList(root.ks_seqs["speed2"]      || ""); context: Qt.WindowShortcut; enabled: root.simulationControlAllowed; onActivated: { root.controller.setSpeed(2); speedCombo.currentIndex = 2 }}
    Shortcut { sequences: root.toSeqList(root.ks_seqs["speed4"]      || ""); context: Qt.WindowShortcut; enabled: root.simulationControlAllowed; onActivated: { root.controller.setSpeed(4); speedCombo.currentIndex = 3 }}
    Shortcut { sequences: root.toSeqList(root.ks_seqs["speed8"]      || ""); context: Qt.WindowShortcut; enabled: root.simulationControlAllowed; onActivated: { root.controller.setSpeed(8); speedCombo.currentIndex = 4 }}
    Shortcut { sequences: root.toSeqList(root.ks_seqs["step"]        || ""); context: Qt.WindowShortcut; enabled: root.simulationControlAllowed; onActivated: root.controller.stepOnce() }
    Shortcut { sequences: root.toSeqList(root.ks_seqs["speedUp"]     || ""); context: Qt.WindowShortcut; enabled: root.unitControlAllowed && root.controller.viewMode !== "editor"; onActivated: root.adjustFocusedUnitSpeed(10) }
    Shortcut { sequences: root.toSeqList(root.ks_seqs["speedDown"]   || ""); context: Qt.WindowShortcut; enabled: root.unitControlAllowed && root.controller.viewMode !== "editor"; onActivated: root.adjustFocusedUnitSpeed(-10) }
    Shortcut { sequences: root.toSeqList(root.ks_seqs["prevUnit"]    || ""); context: Qt.WindowShortcut
        onActivated: root.switchActiveUnit(-1)
        enabled: root.controller.viewMode === "commandpost-red" || root.controller.viewMode === "commandpost-blue"
    }
    Shortcut { sequences: root.toSeqList(root.ks_seqs["nextUnit"]    || ""); context: Qt.WindowShortcut
        onActivated: root.switchActiveUnit(1)
        enabled: root.controller.viewMode === "commandpost-red" || root.controller.viewMode === "commandpost-blue"
    }
    Shortcut { sequences: root.toSeqList(root.ks_seqs["nextUnitTab"] || ""); context: Qt.WindowShortcut
        onActivated: root.switchActiveUnit(1)
    }
    Shortcut { sequences: root.toSeqList(root.ks_seqs["prevUnitSh"]  || ""); context: Qt.WindowShortcut
        onActivated: root.switchActiveUnit(-1)
    }
    Shortcut { sequences: root.toSeqList(root.ks_seqs["autoFit"]     || ""); context: Qt.WindowShortcut
        onActivated: root.fitActivePage()
        enabled: root.controller.viewMode === "commandpost-red" || root.controller.viewMode === "commandpost-blue"
    }
    Shortcut { sequences: root.toSeqList(root.ks_seqs["cancelTrack"] || ""); context: Qt.WindowShortcut
        onActivated: {
            var canvas = root.activeCanvas()
            if (canvas) canvas.setTrackingTarget("", "")
            if (root.controller.focusedUnitId) root.controller.command("halt", { unitId: root.controller.focusedUnitId })
        }
        enabled: root.controller.viewMode !== "editor"
    }

    // ── Settings state (loaded from QSettings) ──
    property bool settShowMinimap: true
    property bool settShowGrid: false
    property bool settAutoFollow: true

    function applySettings() {
        // 窗口尺寸
        var w = root.controller.loadSetting("window/width", 1360)
        var h = root.controller.loadSetting("window/height", 860)
        if (root.appWindow) { root.appWindow.width = w; root.appWindow.height = h }
        var op = root.controller.loadSetting("window/opacity", 1.0)
        if (root.appWindow) root.appWindow.opacity = op

        // 默认速率
        var ds = root.controller.loadSetting("sim/defaultSpeed", 1)
        var speeds = [0,1,2,4,8]
        var matched = false
        for (var i = 0; i < speeds.length; i++) {
            if (speeds[i] === ds) {
                speedCombo.currentIndex = i
                root.controller.setSpeed(ds)
                matched = true
                break
            }
        }
        if (!matched && speedCombo.currentIndex !== 1) {
            // Unknown speed (corrupt settings) — fall back to 1x
            speedCombo.currentIndex = 1
            root.controller.setSpeed(1)
        }

        // 显示选项
        settShowMinimap = root.controller.loadSetting("sim/showMinimap", true)
        settShowGrid = root.controller.loadSetting("sim/showGrid", false)
        settAutoFollow = root.controller.loadSetting("sim/autoFollowFocused", true)
    }

    function adjustFocusedUnitSpeed(delta) {
        if (!root.controller.focusedUnitId) return
        var u = root.controller.unitAt(root.controller.focusedUnitId)
        if (!u || !u.movable || !u.alive) return
        var newSpeed = Math.max(1, Math.round((u.speed || 10) + delta))
        root.controller.command("setSpeed", { unitId: root.controller.focusedUnitId, speed: newSpeed })
    }

    QtObject {
        id: theme
        property color bg:          AppContext.page
        property color panel:       AppContext.panel
        property color panelAlt:    AppContext.raised
        property color border:      AppContext.line
        property color borderSoft:  AppContext.softLine
        property color text:        AppContext.text
        property color textStrong:  AppContext.textStrong
        property color textDim:     AppContext.textDim
        property color textMuted:   AppContext.muted
        property color accent:      AppContext.signal
        property color accentSoft:  "#1d675e"
        property color red:         AppContext.red
        property color blue:        AppContext.blue
        property color success:     AppContext.success
        property color warning:     AppContext.warning
        property color danger:      AppContext.danger
    }

    Rectangle {
        id: shellBackground
        anchors.fill: parent
        z: -10
        color: theme.bg
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: AppContext.page }
            GradientStop { position: 0.55; color: "#0d161d" }
            GradientStop { position: 1.0; color: "#08131a" }
        }
    }

    Rectangle {
        id: topBar
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        height: 50
        color: theme.panel
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#0c151b" }
            GradientStop { position: 0.62; color: "#111d25" }
            GradientStop { position: 1.0; color: "#0c1820" }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: theme.border }
        Rectangle {
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            width: 3
            color: root.controller && root.controller.currentSeatSide === "red" ? theme.red
                 : root.controller && root.controller.currentSeatSide === "blue" ? theme.blue : theme.accent
            opacity: 0.9
            Behavior on color { ColorAnimation { duration: 220 } }
        }
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            height: 2
            opacity: 0.28
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: theme.accent }
                GradientStop { position: 0.45; color: theme.blue }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: root.compactTopBar ? 10 : 16; anchors.rightMargin: root.compactTopBar ? 10 : 16; anchors.topMargin: 8; anchors.bottomMargin: 8
            spacing: root.compactTopBar ? 8 : 14

            Text {
                text: "兵棋推演"
                color: theme.textStrong; font.bold: true
                font.pixelSize: root.compactTopBar ? 14 : 16
                Layout.rightMargin: root.compactTopBar ? 2 : 8; renderType: Text.NativeRendering
            }
            Rectangle { visible: !root.compactTopBar; Layout.preferredWidth: 1; Layout.preferredHeight: 24; color: theme.border; Layout.alignment: Qt.AlignVCenter }

            Text { visible: !root.controller.networked && !root.compactTopBar; text: "视角"; color: theme.textDim; font.pixelSize: 12 }
            ComboBox {
                id: viewCombo
                visible: !root.controller.networked
                Layout.preferredWidth: root.compactTopBar ? 100 : 130
                textRole: "label"
                model: ListModel {
                    ListElement { label: "编辑";   value: "editor" }
                    ListElement { label: "红方";   value: "commandpost-red" }
                    ListElement { label: "蓝方";   value: "commandpost-blue" }
                    ListElement { label: "导演席"; value: "director" }
                }
                currentIndex: {
                    for (var i = 0; i < model.count; i++) {
                        if (model.get(i).value === root.controller.viewMode) return i
                    }
                    return 0
                }
                onActivated: function(idx) { root.controller.viewMode = model.get(idx).value; root.forceActiveFocus() }
            }

            Rectangle {
                visible: root.controller.networked
                Layout.preferredHeight: 26; Layout.preferredWidth: Math.min(root.compactTopBar ? 104 : 220, onlineIdentity.implicitWidth + 22)
                radius: 5; color: root.controller.currentSeatSide === "red" ? "#3c2028" : root.controller.currentSeatSide === "blue" ? "#173248" : "#183632"
                border.color: root.controller.currentSeatSide === "red" ? theme.red : root.controller.currentSeatSide === "blue" ? theme.blue : theme.success
                border.width: 1
                Behavior on color { ColorAnimation { duration: 180 } }
                Text {
                    id: onlineIdentity; anchors.centerIn: parent
                text: root.compactTopBar
                          ? (root.controller.currentSeatId || "未选择战位")
                          : (root.controller.displayName || root.controller.username) + " · " + (root.controller.currentSeatId || "房间大厅")
                    elide: Text.ElideRight
                    color: theme.textStrong; font.pixelSize: 11; font.bold: true
                }
            }

            Rectangle {
                visible: root.controller.networked && !root.compactTopBar
                Layout.preferredHeight: 24; Layout.preferredWidth: statusText.implicitWidth + 18
                radius: 4
                color: root.controller.networkState === "connected" ? "#12352c"
                     : root.controller.networkState === "reconnecting" || root.controller.networkState === "synchronizing" ? "#3a2d17" : "#3a1e28"
                border.color: root.controller.networkState === "connected" ? theme.success
                            : root.controller.networkState === "reconnecting" || root.controller.networkState === "synchronizing" ? theme.warning : theme.danger
                Text { id: statusText; anchors.centerIn: parent; text: root.controller.networkState === "connected" ? (root.controller.matchPhase === "running" ? "在线 · 推演中" : "在线 · " + (root.controller.matchPhase === "preparing" ? "准备阶段" : "已结束")) : root.controller.networkStatus; color: theme.textDim; font.pixelSize: 10; elide: Text.ElideRight; maximumLineCount: 1 }
            }

            Rectangle { visible: !root.compactTopBar; Layout.preferredWidth: 1; Layout.preferredHeight: 24; color: theme.border; Layout.alignment: Qt.AlignVCenter }

            Text { visible: !root.compactTopBar; text: "时间"; color: theme.textDim; font.pixelSize: 12 }
            Text {
                text: root.controller.simTime.toFixed(1) + " s"
                color: theme.textStrong
                font.family: "Consolas"; font.pixelSize: 14
                Layout.preferredWidth: root.compactTopBar ? 62 : 80
                renderType: Text.NativeRendering
            }

            Rectangle { visible: !root.compactTopBar; Layout.preferredWidth: 1; Layout.preferredHeight: 24; color: theme.border; Layout.alignment: Qt.AlignVCenter }

            Row {
                visible: root.simulationControlAllowed
                spacing: 8
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    id: runSwitch
                    width: 44; height: 24; radius: 12
                    opacity: root.controller.readyForSim && root.directorCanStart ? 1.0 : 0.45
                    color: !root.controller.readyForSim ? theme.danger
                         : root.controller.running ? theme.success : "#3b4252"
                    border.color: !root.controller.readyForSim ? theme.danger
                         : root.controller.running ? theme.success : theme.border
                    Rectangle {
                        width: 20; height: 20; radius: 10
                        color: "#fff"
                        x: root.controller.running ? 22 : 2
                        anchors.verticalCenter: parent.verticalCenter
                        Behavior on x { NumberAnimation { duration: 120 } }
                    }
                    MouseArea {
                        anchors.fill: parent; enabled: root.directorCanStart; cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: {
                            if (!root.controller.readyForSim) {
                                cpIssueDialog.refreshAndOpen()
                                return
                            }
                            root.controller.setRunning(!root.controller.running)
                        }
                    }
                }
                Text {
                    visible: !root.compactTopBar
                    text: root.controller.networked && root.controller.matchPhase === "preparing" && !root.directorCanStart ? "等待双方"
                         : !root.controller.readyForSim ? "未就绪"
                         : root.controller.running ? "运行中" : "已暂停"
                    color: !root.controller.readyForSim ? theme.danger
                         : root.controller.running ? theme.success : theme.textDim
                    font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter
                    renderType: Text.NativeRendering
                }
            }

            Text { visible: root.simulationControlAllowed && !root.compactTopBar; text: "速率"; color: theme.textDim; font.pixelSize: 12 }
            ComboBox {
                id: speedCombo
                visible: root.simulationControlAllowed
                enabled: !root.controller.networked || root.controller.matchPhase === "running"
                Layout.preferredWidth: root.compactTopBar ? 72 : 90
                model: ["暂停", "1x", "2x", "4x", "8x"]
                currentIndex: 1
                onActivated: function(idx) { root.controller.setSpeed([0,1,2,4,8][idx]); root.forceActiveFocus() }
            }
            GhostButton {
                visible: root.simulationControlAllowed; enabled: !root.controller.networked || root.controller.matchPhase === "running"; text: "单步"; onClicked: root.controller.stepOnce()
            }

            Item { Layout.fillWidth: true }

            GhostButton {
                visible: !root.compactTopBar; text: "快捷键"; onClicked: shortcutHelpDialog.open()
            }
            GhostButton {
                visible: root.controller.networked && !root.compactTopBar
                text: {
                    var base = root.controller.currentSeatType === "commander"
                        ? "收件箱 · " + root.controller.chatMessages.length : "通信"
                    return (!chatPanel.visible && root.chatUnreadCount > 0)
                        ? base + " [" + root.chatUnreadCount + "]" : base
                }
                Accessible.name: root.controller.currentSeatType === "commander"
                                 ? "打开指挥官实时通信收件箱" : "打开指挥通信"
                onClicked: {
                    chatPanel.open()
                    root.chatLastSeenCount = root.controller.chatMessages.length
                }
            }
            Rectangle {
                id: networkDot
                visible: root.controller.networked
                Layout.preferredWidth: 9; Layout.preferredHeight: 9; radius: 5
                color: root.controller.networkState === "connected" ? theme.success
                     : root.controller.networkState === "reconnecting" || root.controller.networkState === "synchronizing" ? theme.warning
                     : theme.danger
                HoverHandler { id: networkStatusHover }
                ToolTip.visible: networkStatusHover.hovered
                ToolTip.text: root.controller.networkStatus
            }
            GhostButton {
                text: "设置"; onClicked: settingsPanel.open()
            }
            GhostButton {
                visible: !root.controller.networked && !root.compactTopBar; text: "加载默认"; onClicked: root.controller.loadDefault()
            }
        }
    }

    Loader {
        id: pageLoader
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: topBar.bottom; anchors.bottom: parent.bottom
        opacity: 1.0
        transform: Scale { id: pageScale; origin.x: pageLoader.width / 2; origin.y: pageLoader.height / 2; xScale: 1.0; yScale: 1.0 }
        onStatusChanged: {
            if (status === Loader.Ready) {
                pageLoader.opacity = 0
                pageEnter.restart()
            }
        }
        sourceComponent: {
            if (root.controller.networked) return onlinePage
            switch (root.controller.viewMode) {
            case "editor": return editorPage
            case "commandpost-red":
            case "commandpost-blue": return commandPostPage
            case "director": return directorPage
            }
            return commandPostPage
        }
    }

    SequentialAnimation {
        id: pageEnter
        running: false
        ParallelAnimation {
            NumberAnimation { target: pageLoader; property: "opacity"; to: 1; duration: 260; easing.type: Easing.OutCubic }
            NumberAnimation { target: pageScale; property: "xScale"; from: 0.985; to: 1; duration: 260; easing.type: Easing.OutCubic }
            NumberAnimation { target: pageScale; property: "yScale"; from: 0.985; to: 1; duration: 260; easing.type: Easing.OutCubic }
        }
    }

    Component { id: editorPage; ScenarioEditorView { controller: root.controller; editor: root.editor } }
    Component { id: commandPostPage; CommandPostView { controller: root.controller; editor: root.editor } }
    Component { id: directorPage; DirectorView { controller: root.controller; editor: root.editor } }
    Component { id: onlinePage; OnlineOperationsView { controller: root.controller; editor: root.editor; onOpenChatRequested: chatPanel.open() } }

    Minimap {
        id: miniMap
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: 16
        anchors.bottomMargin: 16
        z: 100
        visible: root.settShowMinimap
                 && root.controller.viewMode !== "editor"
                 && root.controller.viewMode !== "director"
                 && ((!root.controller.networked && root.controller.sessionMode !== "unselected")
                     || (root.controller.networked
                         && (root.controller.onlineStage === "deployment"
                             || root.controller.onlineStage === "battle")))
        sideFilter: {
            if (root.controller.viewMode === "commandpost-red") return "red"
            if (root.controller.viewMode === "commandpost-blue") return "blue"
            return ""
        }
        mapCenter: root.activeCanvas() ? root.activeCanvas().center : ({x: 10000, y: 7500})
        mapSize: root.controller.mapInfo ? {w: root.controller.mapInfo.widthMeters, h: root.controller.mapInfo.heightMeters} : {w: 20000, h: 15000}
        viewportRect: {
            var c = root.activeCanvas()
            return c ? { x: c.center.x - c.width / c.zoom / 2, y: c.center.y - c.height / c.zoom / 2,
                         w: c.width / c.zoom, h: c.height / c.zoom }
                     : { x: 0, y: 0, w: 1, h: 1 }
        }
        mainZoom: 1.0
        onSideFilterChanged: root.refreshDetectedEnemies()
        onMinimapClicked: function(lp) {
            var canvas = root.activeCanvas()
            if (canvas) {
                canvas.centerOn(lp.x, lp.y)
            }
        }
    }

    Connections {
        target: root.controller
        function onUnitsForward() {
            miniMap.units = root.controller.units
        }
    }

    function refreshDetectedEnemies() {
        var msgs = root.controller.messages
        var cache = {}
        for (var i = 0; i < Math.min(msgs.length, 60); i++) {
            var m = msgs[i]
            if (m.type !== "TargetDetect" || !m.payload) continue
            if (miniMap.sideFilter && m.senderSide !== miniMap.sideFilter) continue
            var tid = m.payload.targetId
            if (!tid || cache[tid]) continue
            cache[tid] = {
                x: m.payload.x,
                y: m.payload.y,
                callsign: m.payload.callsign || tid,
                targetId: tid
            }
        }
        var arr = []
        for (var k in cache) arr.push(cache[k])
        miniMap.detectedEnemies = arr
    }

    Connections {
        target: root.controller
        function onMessagesForward() { root.refreshDetectedEnemies() }
    }

    Component.onCompleted: {
        root.forceActiveFocus()
        miniMap.units = root.controller.units
        root.refreshDetectedEnemies()
        var defaultView = root.controller.loadSetting("sim/defaultView", "commandpost-red")
        if (root.controller.viewModeOptions().indexOf(defaultView) >= 0)
            root.controller.viewMode = defaultView
        applySettings()
        if (root.controller.sessionMode === "unselected") Qt.callLater(function() { sessionDialog.open() })
    }

    Component.onDestruction: {
        if (root.appWindow) {
            root.controller.saveSetting("window/width", root.appWindow.width)
            root.controller.saveSetting("window/height", root.appWindow.height)
            root.controller.saveSetting("window/opacity", root.appWindow.opacity)
        }
    }

    EventDialog {
        id: cpIssueDialog
        title: "无法运行推演"
        level: "warn"
        function refreshAndOpen() {
            var redCount = root.controller.unitOptions("commandpost", "red").length
            var blueCount = root.controller.unitOptions("commandpost", "blue").length
            cpIssueDialog.body = "每方必须恰好存在一个指挥所（且存活）。\n当前存活指挥所：红方 "
                    + redCount + " 个，蓝方 " + blueCount
                    + " 个。\n\n请在场景编辑器中增删指挥所后重新加载。"
            cpIssueDialog.open()
        }
    }

    property var errorQueue: []

    EventDialog {
        id: errorDialog
        title: "操作未完成"
        body: root.errorQueue.length > 0 ? root.errorQueue[0] : (root.controller.lastError || "未知错误")
        level: "error"
        onAckClicked: {
            if (root.errorQueue.length > 0) {
                root.errorQueue.shift()
                // Reassign to trigger binding updates
                root.errorQueue = root.errorQueue.slice()
            }
            if (root.errorQueue.length > 0) {
                errorDialog.body = root.errorQueue[0]
            } else {
                errorDialog.close()
            }
        }
        onRejectClicked: {
            root.errorQueue = []
            errorDialog.close()
        }
    }

    Connections {
        target: root.controller
        function onErrorForward(msg) {
            root.errorQueue.push(msg)
            errorDialog.body = root.errorQueue[0]
            errorDialog.open()
        }
    }

    EventDialog {
        id: shortcutHelpDialog
        title: "键盘快捷键"
        body: "Space: 暂停/继续\n1-5: 速率 (暂停/1x/2x/4x/8x)\nW: 加速选中单元  S: 减速选中单元\n. : 单步\nA/D / ← →: 前/后切换单元\nTab / Shift+Tab: 切换单元\nCtrl+F: 自适应缩放\nEsc: 退出引导模式\nP: 取消追踪/停止追击\nLeftClick: 选中/引导\nRightClick: 右键菜单"
        level: "info"
    }

    EventDialog {
        id: simEndDialog
        title: "推演结束"
        level: "warn"
        body: "推演已结束"
        onAckClicked: { body = "" }
    }

    Connections {
        target: root.controller
        function onSimEndForward(winner, loser) {
            simEndDialog.body = loser
                    ? loser + " 指挥所已被摧毁，" + winner + " 获胜！"
                    : winner
            simEndDialog.open()
        }
    }

    SettingsPanel {
        id: settingsPanel
        controller: root.controller
        editor: root.editor
        appWindow: root.appWindow
        onClosed: { root.applySettings() }
        onSessionChangeRequested: sessionDialog.open()
    }

    SessionDialog { id: sessionDialog; controller: root.controller; editor: root.editor }
    ChatPanel { id: chatPanel; controller: root.controller; editor: root.editor }

    Connections {
        target: root.controller
        // Triggered whenever a `shortcuts/*` setting changes. Lets the live
        // Shortcut items re-bind immediately, without waiting for the
        // settings panel to close.
        function onShortcutsChanged() { root.reloadAllShortcuts() }
    }

    Binding {
        target: root.activeCanvas()
        property: "showCoordinateGrid"
        value: root.settShowGrid
        when: !!root.activeCanvas()
    }

    Binding {
        target: root.activePage
        property: "autoFollow"
        value: root.settAutoFollow
        when: !!(root.activePage && root.activePage["autoFollow"] !== undefined)
    }
}
