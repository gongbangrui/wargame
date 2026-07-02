import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"

Item {
    id: root

    property string side: controller.viewMode === "commandpost-blue" ? "blue" : "red"
    property string enemySide: root.side === "red" ? "blue" : "red"
    property var snap: controller.unitAt(controller.focusedUnitId)

    QtObject {
        id: theme
        property color panel: "#1a1f2b"
        property color panelAlt: "#222838"
        property color border: "#3a455a"
        property color text: "#f3f6fb"
        property color textStrong: "#ffffff"
        property color textDim: "#d4dbe6"
        property color muted: "#b0b8c4"
        property color dimmer: "#6e7687"
        property color red: "#ff5566"
        property color blue: "#4d9bff"
        property color accent: "#4f9dff"
        property color accentSoft: "#2a4f86"
        property color success: "#46d29a"
        property color warning: "#ffb24d"
        property color danger: "#ff4d6d"
        property color disabled: "#4a5161"
        property color switchOn: "#46d29a"
    }

    property var discoverySet: ([])
    property bool canAttack: false
    property bool canGuide: false
    property bool simEnded: false

    // 检查指挥部是否被摧毁
    function checkCommandPostAlive() {
        var cpId = root.side === "red" ? "red_cp" : "blue_cp"
        var cp = controller.unitAt(cpId)
        if (cp && !cp.alive && cp.hp !== undefined && cp.hp <= 0) {
            root.simEnded = true
            controller.setRunning(false)
            endDialog.open()
            return true
        }
        return false
    }

    Timer {
        id: cpCheckTimer
        interval: 1000; running: controller.running; repeat: true
        onTriggered: { if (root.checkCommandPostAlive()) stop() }
    }

    function autoFitZoom() {
        var units = controller.unitOptions("", root.side)
        if (!units || units.length < 2) return
        var minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9
        for (var i = 0; i < units.length; i++) {
            var u = controller.unitAt(units[i].id)
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

    Timer {
        id: centerTimer
        interval: 250; running: controller.running && controller.focusedUnitId; repeat: true
        onTriggered: {
            if (controller.running && controller.focusedUnitId) {
                var u = controller.unitAt(controller.focusedUnitId)
                if (u && u.position) canvas.centerOn(u.position[0], u.position[1])
            }
        }
    }

    ListModel { id: eventList }
    Connections {
        target: controller.engine
        function onEventPosted(title, body, level) {
            eventList.append({title: title, body: body, level: level, ts: Date.now()})
            while (eventList.count > 30) eventList.remove(0)
            // 指挥部被摧毁时弹出结束
            if (title === "单元被摧毁") root.checkCommandPostAlive()
        }
    }
    Connections {
        target: controller
        function onUnitsForward() {
            root.canAttack = root.hasTargetInAttackRange()
            root.canGuide = root.hasTargetInDetectShared()
        }
        function onMessagesForward() {
            var msgs = controller.messages
            var newDisc = root.discoverySet.slice()
            for (var i = msgs.length - 1; i >= 0 && i >= msgs.length - 20; i--) {
                var m = msgs[i]
                if (m.type === "TargetDetect" && m.sender) {
                    if (newDisc.indexOf(m.sender) < 0) newDisc.push(m.sender)
                }
            }
            if (newDisc.length > 20) newDisc = newDisc.slice(newDisc.length - 20)
            root.discoverySet = newDisc
        }
    }

    function detectedEnemyOptions(attackerId) {
        var all = controller.unitOptions("", root.enemySide)
        var detected = []
        var reconUnits = controller.unitOptions("reconuav", root.side)
        // 如果指定了攻击机，获取其攻击范围
        var atkRange = -1
        var atkPos = null
        if (attackerId) {
            var atk = controller.unitAt(attackerId)
            if (atk && atk.position) { atkRange = atk.attackRange || 0; atkPos = atk.position }
        }
        for (var ei = 0; ei < all.length; ei++) {
            var enemy = controller.unitAt(all[ei].id)
            if (!enemy || !enemy.alive) continue
            var foundByRecon = false
            for (var ri = 0; ri < reconUnits.length; ri++) {
                var recon = controller.unitAt(reconUnits[ri].id)
                if (!recon || !recon.position) continue
                var dx = recon.position[0] - enemy.position[0]
                var dy = recon.position[1] - enemy.position[1]
                if (Math.sqrt(dx*dx + dy*dy) <= (recon.detectRange || 0)) { foundByRecon = true; break }
            }
            // 如果在攻击范围内也视为可攻击
            var inAtkRange = false
            if (atkPos && atkRange > 0) {
                var adx = atkPos[0] - enemy.position[0]
                var ady = atkPos[1] - enemy.position[1]
                if (Math.sqrt(adx*adx + ady*ady) <= atkRange) inAtkRange = true
            }
            if (foundByRecon || inAtkRange) detected.push(all[ei])
        }
        return detected
    }

    function hasTargetInAttackRange() {
        if (!controller.focusedUnitId) return false
        var u = controller.unitAt(controller.focusedUnitId)
        if (!u || u.kind !== "attackuav") return false
        var atkRange = u.attackRange || 0, myPos = u.position
        var enemies = controller.unitOptions("", root.enemySide)
        for (var i = 0; i < enemies.length; i++) {
            var e = controller.unitAt(enemies[i].id)
            if (!e || !e.position || !e.alive) continue
            var dx = myPos[0] - e.position[0], dy = myPos[1] - e.position[1]
            if (Math.sqrt(dx*dx + dy*dy) <= atkRange) return true
        }
        return false
    }

    function hasTargetInDetectShared() {
        if (!controller.focusedUnitId) return false
        var u = controller.unitAt(controller.focusedUnitId)
        if (!u || u.kind !== "groundscout") return false
        var friendlies = controller.unitOptions("reconuav", root.side)
        var enemies = controller.unitOptions("", root.enemySide)
        for (var ei = 0; ei < enemies.length; ei++) {
            var e = controller.unitAt(enemies[ei].id)
            if (!e || !e.position || !e.alive) continue
            for (var fi = 0; fi < friendlies.length; fi++) {
                var f = controller.unitAt(friendlies[fi].id)
                if (!f || !f.position) continue
                var dx = f.position[0] - e.position[0], dy = f.position[1] - e.position[1]
                if (Math.sqrt(dx*dx + dy*dy) <= (f.detectRange || 0)) return true
            }
        }
        return false
    }

    RowLayout {
        anchors.fill: parent; spacing: 0

        Item {
            Layout.fillHeight: true; Layout.fillWidth: true
            MapCanvas {
                id: canvas
                anchors.fill: parent; anchors.margins: 12
                sideFilter: root.side; showAllSides: false
                focusUnitId: controller.focusedUnitId
                showRoutes: routeToggle.checked
                showDetectRange: detectRangeToggle.checked
                showAttackRange: attackRangeToggle.checked
                simTime: controller.simTime
                discoveryUnits: root.discoverySet

                onUnitClicked: function(uid, btn) {
                    if (btn === "right") {
                        actionMenu.unitId = uid; actionMenu.popup()
                    } else {
                        controller.setFocusedUnitId(uid)
                        var u = controller.unitAt(uid)
                        if (u && u.position) canvas.centerOn(u.position[0], u.position[1])
                        unitActionPanel.showFor(uid)
                    }
                }
                onClickedMap: function(lp) {
                    if (controller.focusedUnitId)
                        controller.command("moveTo", { unitId: controller.focusedUnitId, pos: lp })
                }
            }

            // 控制栏
            Rectangle {
                anchors.right: parent.right; anchors.top: parent.top
                anchors.rightMargin: 24; anchors.topMargin: 24
                color: "#1a1f2b"; border.color: "#3a455a"; radius: 6
                implicitHeight: 32; implicitWidth: 380; z: 10
                RowLayout {
                    anchors.centerIn: parent; spacing: 10
                    Text { text: "路径"; color: theme.textDim; font.pixelSize: 11; renderType: Text.NativeRendering }
                    Switch { id: routeToggle; checked: true }
                    Text {
                        text: routeToggle.checked ? "开" : "关"
                        color: routeToggle.checked ? theme.switchOn : theme.muted
                        font.pixelSize: 11; renderType: Text.NativeRendering
                    }
                    Text { text: "侦察范围"; color: theme.textDim; font.pixelSize: 11; renderType: Text.NativeRendering }
                    Switch { id: detectRangeToggle; checked: true }
                    Text {
                        text: detectRangeToggle.checked ? "开" : "关"
                        color: detectRangeToggle.checked ? theme.switchOn : theme.muted
                        font.pixelSize: 11; renderType: Text.NativeRendering
                    }
                    Text { text: "攻击范围"; color: theme.textDim; font.pixelSize: 11; renderType: Text.NativeRendering }
                    Switch { id: attackRangeToggle; checked: true }
                    Text {
                        text: attackRangeToggle.checked ? "开" : "关"
                        color: attackRangeToggle.checked ? theme.switchOn : theme.muted
                        font.pixelSize: 11; renderType: Text.NativeRendering
                    }
                }
            }

            Rectangle {
                anchors.right: parent.right; anchors.top: parent.top
                anchors.rightMargin: 24; anchors.topMargin: 68
                color: "#1a1f2b"; border.color: "#3a455a"; radius: 4
                implicitHeight: 26; implicitWidth: 80; z: 10
                Text {
                    anchors.centerIn: parent; text: "自适应缩放"
                    color: "#4f9dff"; font.pixelSize: 10; renderType: Text.NativeRendering
                }
                MouseArea {
                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                    onClicked: root.autoFitZoom()
                }
            }

            Menu {
                id: actionMenu
                property string unitId: ""
                MenuItem { text: "查看"; onTriggered: controller.setFocusedUnitId(actionMenu.unitId) }
                MenuItem { text: "下达攻击命令"; onTriggered: attackDialog.openWithTarget(actionMenu.unitId) }
                MenuItem { text: "发送机动指令"; onTriggered: controller.setFocusedUnitId(actionMenu.unitId) }
            }
        }

        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: 400
            color: theme.panel
            Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 1; color: theme.border }

            ColumnLayout {
                anchors.fill: parent; anchors.margins: 14; spacing: 8

                ColumnLayout {
                    spacing: 2
                    Text {
                        text: (root.side === "red" ? "红方" : "蓝方") + " · 指挥所"
                        color: theme.textStrong; font.pixelSize: 18; font.bold: true; renderType: Text.NativeRendering
                    }
                    Text {
                        text: "实时态势 / 事件队列 / 指令下达"
                        color: theme.muted; font.pixelSize: 11; renderType: Text.NativeRendering
                    }
                }

                SectionTitle { text: "选择己方单元" }
                ComboBox {
                    Layout.fillWidth: true
                    model: controller.unitOptions("", root.side)
                    textRole: "callsign"; valueRole: "id"
                    onActivated: function(idx) {
                        controller.setFocusedUnitId(model[idx].id)
                        var u = controller.unitAt(model[idx].id)
                        if (u && u.position) canvas.centerOn(u.position[0], u.position[1])
                    }
                }

                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 200
                    color: theme.panelAlt; border.color: theme.border; radius: 6
                    UnitPanel { anchors.fill: parent; anchors.margins: 12; snap: root.snap }
                }

                SectionTitle { text: "单元操作" }
                Flow {
                    Layout.fillWidth: true; spacing: 6

                    TonalButton {
                        text: "规划路径"; base: theme.success
                        visible: controller.focusedUnitId
                            && (controller.unitAt(controller.focusedUnitId).kind !== "commandpost")
                        onClicked: {
                            var snap = controller.unitAt(controller.focusedUnitId)
                            if (snap) routeDialog.openFor(snap)
                        }
                    }

                    TonalButton {
                        text: "下达攻击"
                        base: root.canAttack ? theme.danger : theme.disabled
                        visible: controller.focusedUnitId
                            && (controller.unitAt(controller.focusedUnitId).kind === "attackuav")
                        enabled: visible && root.canAttack
                        onClicked: attackDialog.openForAttacker(controller.focusedUnitId)
                    }

                    TonalButton {
                        text: "引导打击"
                        base: root.canGuide ? theme.warning : theme.disabled
                        visible: controller.focusedUnitId
                            && (controller.unitAt(controller.focusedUnitId).kind === "groundscout")
                        enabled: visible && root.canGuide
                        onClicked: guideDialog.open()
                    }

                    GhostButton {
                        text: "机动到点击点"
                        enabled: controller.focusedUnitId
                            && (controller.unitAt(controller.focusedUnitId).movable !== false)
                    }
                    GhostButton {
                        text: "撤离"
                        enabled: controller.focusedUnitId ? true : false
                        onClicked: {
                            if (controller.focusedUnitId)
                                controller.command("withdraw", { unitId: controller.focusedUnitId })
                        }
                    }
                }

                SectionTitle { text: "事件队列" }
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 140
                    color: theme.panelAlt; border.color: theme.border; radius: 6
                    ListView {
                        anchors.fill: parent; anchors.margins: 6; clip: true; model: eventList
                        delegate: Rectangle {
                            width: ListView.view.width; implicitHeight: 42
                            color: index % 2 === 0 ? "#1a1f2b" : "#222838"
                            Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 3
                                color: model.level === "success" ? theme.success : (model.level === "warn" ? theme.warning : theme.accent) }
                            Column {
                                anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                Text { text: model.title; color: theme.textStrong; font.bold: true; font.pixelSize: 12; renderType: Text.NativeRendering }
                                Text { text: model.body; color: theme.textDim; font.pixelSize: 11; wrapMode: Text.WordWrap; width: parent.width - 12; renderType: Text.NativeRendering }
                            }
                            MouseArea { anchors.fill: parent; onClicked: eventList.remove(index) }
                        }
                    }
                }

                SectionTitle { text: "实时消息流" }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: theme.panelAlt; border.color: theme.border; radius: 6
                    MessageLog { anchors.fill: parent; anchors.margins: 1; messages: controller.messages; sideFilter: root.side }
                }
            }
        }
    }

    // ===== 友方单元点击面板 =====
    Dialog {
        id: unitActionPanel
        property string uid: ""
        property var unitData: ({})
        function showFor(id) { unitActionPanel.uid = id; unitActionPanel.unitData = controller.unitAt(id); if (unitActionPanel.unitData && unitActionPanel.unitData.id) unitActionPanel.open() }
        title: "单元操作 - " + (unitActionPanel.unitData.callsign || unitActionPanel.uid)
        modal: false; width: 460; height: 360; anchors.centerIn: parent; standardButtons: Dialog.NoButton
        background: Rectangle { color: "#1a1f2b"; border.color: "#3a455a"; radius: 6 }
        contentItem: ColumnLayout {
            spacing: 8; anchors.margins: 12
            SectionTitle { text: "侦察详情" }
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 120; color: "#0e1217"; border.color: "#2a3142"; radius: 4
                ListView {
                    anchors.fill: parent; anchors.margins: 4; clip: true
                    model: unitActionPanel.unitData.detections || []
                    delegate: Rectangle {
                        width: ListView.view.width; implicitHeight: 26
                        color: index % 2 === 0 ? "#141a21" : "#1a1f2b"
                        Row {
                            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 8
                            Rectangle { width: 8; height: 8; radius: 4; anchors.verticalCenter: parent.verticalCenter; color: modelData.side === "red" ? "#ff5566" : "#4d9bff" }
                            Text { text: modelData.callsign + " (" + modelData.kind + ") · " + Math.round(modelData.distance) + " m"; color: "#d4dbe6"; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter; renderType: Text.NativeRendering }
                        }
                    }
                    Text { visible: (!unitActionPanel.unitData.detections || unitActionPanel.unitData.detections.length === 0); anchors.centerIn: parent; text: "暂无侦察目标"; color: "#b0b8c4"; font.pixelSize: 12; renderType: Text.NativeRendering }
                }
            }
            SectionTitle { text: "快速操作" }
            Flow {
                Layout.fillWidth: true; spacing: 8
                TonalButton { text: "查看详情"; base: theme.accent; onClicked: { controller.setFocusedUnitId(unitActionPanel.uid); var u = controller.unitAt(unitActionPanel.uid); if (u && u.position) canvas.centerOn(u.position[0], u.position[1]); unitActionPanel.close() } }
                TonalButton { text: "规划路径"; base: theme.success; visible: unitActionPanel.unitData.kind !== "commandpost"; onClicked: { var snap = controller.unitAt(unitActionPanel.uid); unitActionPanel.close(); if (snap) routeDialog.openFor(snap) } }
                TonalButton { text: "下达攻击"; base: theme.danger; visible: unitActionPanel.unitData.kind === "attackuav"; onClicked: { var uid = unitActionPanel.uid; unitActionPanel.close(); attackDialog.openForAttacker(uid) } }
                GhostButton { text: "关闭"; onClicked: unitActionPanel.close() }
            }
        }
    }

    // ===== 路径规划 =====
    RoutePlannerDialog {
        id: routeDialog
        onRouteAccepted: function(points) {
            if (!controller.focusedUnitId) return
            controller.setUnitSchedule(controller.focusedUnitId, points)
            var wps = []
            for (var i = 0; i < points.length; i++) wps.push({ x: points[i].x, y: points[i].y })
            controller.command("setFlightPlan", { attackerId: controller.focusedUnitId, waypoints: wps })
        }
    }

    // ===== 攻击对话框（含目标和攻击机双选）=====
    Dialog {
        id: attackDialog
        property string targetId: ""
        property string attackerId: ""
        function openWithTarget(tid) { attackDialog.targetId = tid; attackDialog.attackerId = ""; attackDialog.open() }
        function openForAttacker(aid) { attackDialog.attackerId = aid; attackDialog.targetId = ""; attackDialog.open() }
        title: "下达攻击命令"; modal: true; anchors.centerIn: parent; standardButtons: Dialog.NoButton
        background: Rectangle { color: "#1a1f2b"; border.color: "#3a455a"; radius: 6 }
        ColumnLayout {
            spacing: 10; anchors.margins: 16
            Text { text: "攻击机"; color: theme.muted; font.pixelSize: 11; renderType: Text.NativeRendering }
            ComboBox {
                id: attackerCombo; Layout.fillWidth: true
                model: controller.unitOptions("attackuav", root.side)
                textRole: "callsign"; valueRole: "id"
                Component.onCompleted: {
                    if (attackDialog.attackerId) {
                        for (var i = 0; i < model.length; i++) {
                            if (model[i].id === attackDialog.attackerId) { currentIndex = i; break }
                        }
                    }
                }
            }
            Text { text: "目标"; color: theme.muted; font.pixelSize: 11; renderType: Text.NativeRendering }
            ComboBox {
                id: targetCombo; Layout.fillWidth: true
                model: root.detectedEnemyOptions(attackDialog.attackerId); textRole: "callsign"; valueRole: "id"; Component.onCompleted: { if (attackDialog.targetId) {
                        for (var i = 0; i < model.length; i++) {
                            if (model[i].id === attackDialog.targetId) { currentIndex = i; break }
                        }
                    }
                }
            }
        }
        footer: DialogButtonBox {
            TonalButton {
                text: "下达"; base: "#ff4d6d"
                onClicked: {
                    if (attackerCombo.currentIndex < 0 || targetCombo.currentIndex < 0) return
                    var aid = attackerCombo.model[attackerCombo.currentIndex].id
                    var tid = targetCombo.model[targetCombo.currentIndex].id
                    controller.command("assignTarget", { attackerId: aid, targetId: tid })
                    var s = controller.unitAt(tid)
                    if (s && s.position) {
                        controller.command("setFlightPlan", { attackerId: aid, waypoints: [{ x: s.position[0], y: s.position[1] }] })
                    }
                    eventList.append({title: "攻击命令已下达", body: aid + " → " + tid, level: "info", ts: Date.now()})
                    attackDialog.close()
                }
            }
            GhostButton { text: "取消"; onClicked: attackDialog.close() }
        }
    }

    // ===== 引导打击 =====
    Dialog {
        id: guideDialog
        title: "引导打击"; modal: true; anchors.centerIn: parent; standardButtons: Dialog.NoButton
        background: Rectangle { color: "#1a1f2b"; border.color: "#3a455a"; radius: 6 }
        ColumnLayout {
            spacing: 10; anchors.margins: 16
            Text { text: "选择攻击机与目标"; color: theme.textStrong; font.pixelSize: 13; renderType: Text.NativeRendering }
            Text { text: "攻击机"; color: theme.muted; font.pixelSize: 11 }
            ComboBox { id: gAttackerCombo2; Layout.fillWidth: true; model: controller.unitOptions("attackuav", root.side); textRole: "callsign"; valueRole: "id" }
            Text { text: "目标"; color: theme.muted; font.pixelSize: 11 }
            ComboBox { id: gTargetCombo2; Layout.fillWidth: true; model: root.detectedEnemyOptions(controller.focusedUnitId); textRole: "callsign"; valueRole: "id" }
        }
        footer: DialogButtonBox {
            TonalButton {
                text: "发送航路"; base: theme.danger
                onClicked: {
                    if (gTargetCombo2.currentIndex < 0 || gAttackerCombo2.currentIndex < 0) return
                    var tid = gTargetCombo2.model[gTargetCombo2.currentIndex].id
                    var aid = gAttackerCombo2.model[gAttackerCombo2.currentIndex].id
                    var s = controller.unitAt(tid)
                    controller.command("guideAttack", { guideId: controller.focusedUnitId, attackerId: aid, targetId: tid, targetPos: { x: s.position[0], y: s.position[1] } })
                    guideDialog.close()
                }
            }
            GhostButton { text: "取消"; onClicked: guideDialog.close() }
        }
    }

    // ===== 推演结束弹窗 =====
    Dialog {
        id: endDialog
        title: "推演结束"; modal: true; anchors.centerIn: parent; standardButtons: Dialog.NoButton
        background: Rectangle { color: "#1a1f2b"; border.color: "#ff4d6d"; radius: 8 }
        ColumnLayout {
            spacing: 16; anchors.margins: 24
            Text { text: "⛔ 推演结束"; color: "#ff4d6d"; font.pixelSize: 22; font.bold: true; renderType: Text.NativeRendering }
            Text {
                text: "己方指挥部已被摧毁，推演终止。"
                color: "#f3f6fb"; font.pixelSize: 14; wrapMode: Text.WordWrap; Layout.preferredWidth: 300; renderType: Text.NativeRendering
            }
            TonalButton { text: "确认"; base: theme.accent; Layout.alignment: Qt.AlignHCenter; onClicked: endDialog.close() }
        }
    }

    Component.onCompleted: {
        autoFitZoom()
        var cpId = root.side === "red" ? "red_cp" : "blue_cp"
        var cp = controller.unitAt(cpId)
        if (cp && cp.position) canvas.centerOn(cp.position[0], cp.position[1])
    }

    Connections {
        target: controller
        function onViewModeChanged() {
            autoFitZoom()
            var cpId = root.side === "red" ? "red_cp" : "blue_cp"
            var cp = controller.unitAt(cpId)
            if (cp && cp.position) canvas.centerOn(cp.position[0], cp.position[1])
        }
    }
}
