pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"

Item {
    id: root
    property var controller: null
    property var editor: null
    property var units: []
    property var undoStack: []
    property var redoStack: []
    property string restrictedSide: ""
    property bool rosterMode: false
    property bool editable: !root.controller.networked || root.controller.matchPhase === "preparing"

    function pushUndo() {
        undoStack.push(JSON.stringify(root.controller.unitsJson()))
        redoStack = []
        if (undoStack.length > 50) undoStack.shift()
    }
    function undo() {
        if (undoStack.length === 0) return
        redoStack.push(JSON.stringify(root.controller.unitsJson()))
        var prev = JSON.parse(undoStack.pop())
        var unitsArr = Object.prototype.toString.call(prev) === "[object Array]" ? prev : (prev.units || [])
        if (root.controller.replaceUnits(unitsArr)) reload()
    }
    function redo() {
        if (redoStack.length === 0) return
        undoStack.push(JSON.stringify(root.controller.unitsJson()))
        var next = JSON.parse(redoStack.pop())
        var unitsArr = Object.prototype.toString.call(next) === "[object Array]" ? next : (next.units || [])
        if (root.controller.replaceUnits(unitsArr)) reload()
    }

    QtObject {
        id: t
        property color panel: "#0b1728"; property color panelAlt: "#142943"
        property color border: "#315173"; property color text: "#f5f8fc"
        property color textDim: "#d6e1ef"; property color muted: "#bdcde0"
        property color accent: "#4090ff"; property color danger: "#f04760"
        property color red: "#f04760"; property color blue: "#4090ff"
        property color success: "#36c98a"
    }

    function reload() {
        var prevId = list.currentIndex >= 0 && root.units[list.currentIndex] ? root.units[list.currentIndex].id : ""
        var source = root.controller.unitsJson().units || []
        if (root.restrictedSide) {
            var filtered = []
            for (var n = 0; n < source.length; n++) {
                if (source[n].side === root.restrictedSide) filtered.push(source[n])
            }
            root.units = filtered
        } else {
            root.units = source
        }

        if (prevId) {
            for (var i = 0; i < root.units.length; i++) {
                if (root.units[i].id === prevId) { list.currentIndex = i; break }
            }
        }
    }
    Component.onCompleted: {
        reload()
        canvas.zoom = 0.15
        canvas.centerOn(canvas.mapSize.w / 2, canvas.mapSize.h / 2)
    }
    Connections {
        target: root.controller
        function onCommandExecuted() { root.reload() }
        function onUnitsForward() { root.requestReload() }
    }

    Timer {
        id: reloadTimer
        interval: 300; repeat: false
        onTriggered: root.reload()
    }
    function requestReload() {
        if (!reloadTimer.running) reloadTimer.start()
    }

    RowLayout {
        anchors.fill: parent; spacing: 0

        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: Math.min(360, Math.max(280, root.width * 0.36))
            color: t.panel
            Rectangle { anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 1; color: t.border }

            ColumnLayout {
                anchors.fill: parent; anchors.margins: 16; spacing: 12

                ColumnLayout {
                    spacing: 4
                    Text { text: root.rosterMode ? (root.restrictedSide === "red" ? "红方初始阵容" : "蓝方初始阵容") : "场景编辑器"; color: t.text; font.pixelSize: 18; font.bold: true; renderType: Text.NativeRendering }
                    Text { text: root.editable ? "双击地图放置新单元；右键单元删除；选中后可修改参数或规划路径" : "推演已经开始，初始场景现为只读状态"; color: root.editable ? t.muted : t.danger; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true; renderType: Text.NativeRendering }
                }

                SectionTitle { text: "场景单元" }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: t.panelAlt; border.color: t.border; radius: 6
                    ListView {
                        id: list
                        anchors.fill: parent; anchors.margins: 4
                        clip: true; model: root.units
                        delegate: Rectangle {
                            id: scenarioRow
                            required property int index
                            required property var modelData
                            width: list.width; implicitHeight: 40
                            color: ListView.isCurrentItem ? "#3478c1" : (scenarioRow.index % 2 === 0 ? "#1b3554" : "#234160")
                            radius: 3
                            Row {
                                anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10
                                spacing: 10
                                Rectangle { width: 10; height: 10; radius: 5; anchors.verticalCenter: parent.verticalCenter
                                    color: scenarioRow.modelData.side === "red" ? t.red : t.blue }
                                Text { text: scenarioRow.modelData.callsign; color: t.text; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter; renderType: Text.NativeRendering }
                                Text { text: scenarioRow.modelData.kind === "commandpost" ? "指挥所" : (scenarioRow.modelData.kind === "reconuav" ? "侦察" : (scenarioRow.modelData.kind === "attackuav" ? "攻击" : (scenarioRow.modelData.kind === "jammeruav" ? "干扰" : "地面"))); color: t.textDim; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter; renderType: Text.NativeRendering }
                                Item { width: 6 }
                                Rectangle {
                                    visible: scenarioRow.modelData.kind !== "commandpost"
                                    width: 40; height: 18; radius: 4
                                    color: "#315f99"
                                    anchors.verticalCenter: parent.verticalCenter
                                    Text {
                                        anchors.centerIn: parent
                                        text: (scenarioRow.modelData.schedule ? scenarioRow.modelData.schedule.length : 0) + " 点"
                                        color: "#f3f6fb"; font.pixelSize: 10
                                        renderType: Text.NativeRendering
                                    }
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    list.currentIndex = scenarioRow.index
                                    if (scenarioRow.modelData.x !== undefined && scenarioRow.modelData.y !== undefined)
                                        canvas.centerOn(scenarioRow.modelData.x, scenarioRow.modelData.y)
                                }
                                onDoubleClicked: { if (root.editable) { list.currentIndex = scenarioRow.index; editDialog.openWith(scenarioRow.modelData) } }
                            }
                        }
                    }
                }

                Flow {
                    Layout.fillWidth: true; spacing: 8
                    TonalButton { text: "新增"; base: t.accent; enabled: root.editable; onClicked: root.addNew() }
                    TonalButton { text: "编辑选中"; base: "#2a4f86"; enabled: root.editable && list.currentIndex >= 0; onClicked: root.editSelected() }
                    TonalButton {
                        text: "规划路径"
                        base: t.success
                        enabled: root.editable && list.currentIndex >= 0 && root.units[list.currentIndex] && root.units[list.currentIndex].kind !== "commandpost"
                        onClicked: root.planRouteSelected()
                    }
                    TonalButton { text: "删除"; base: t.danger; enabled: root.editable && list.currentIndex >= 0; onClicked: root.removeSelected() }
                    GhostButton { visible: !root.rosterMode; text: "↶ 撤销"; enabled: root.editable && root.undoStack.length > 0; onClicked: root.undo() }
                    GhostButton { visible: !root.rosterMode; text: "↷ 重做"; enabled: root.editable && root.redoStack.length > 0; onClicked: root.redo() }
                    GhostButton { visible: !root.rosterMode; text: "保存"; onClicked: root.saveToFile() }
                    GhostButton { visible: !root.rosterMode; text: "读取"; enabled: root.editable; onClicked: root.loadFromFile() }
                }
            }
        }

        Item {
            Layout.fillHeight: true; Layout.fillWidth: true
            MapCanvas { controller: root.controller; editor: root.editor;
                id: canvas
                anchors.fill: parent; anchors.margins: 12
                sideFilter: root.restrictedSide || root.controller.focusedSide
                showAllSides: !root.rosterMode
                focusUnitId: list.currentIndex >= 0 && root.units[list.currentIndex] ? root.units[list.currentIndex].id : ""
                routes: root.visibleRoutes()
                onUnitClicked: function(uid, btn) {
                    for (var i = 0; i < root.units.length; i++) {
                        if (root.units[i].id === uid) {
                            list.currentIndex = i
                            canvas.centerOn(root.units[i].x, root.units[i].y)
                            break
                        }
                    }
                    if (btn === "right" && root.editable) { confirmDelete.uid = uid; confirmDelete.open() }
                }
                onRightClickedMap: function(lp) { if (root.editable) editDialog.openNew(lp.x, lp.y, root.restrictedSide || root.controller.focusedSide) }
                onDoubleClickedMap: function(lp) { if (root.editable) editDialog.openNew(lp.x, lp.y, root.restrictedSide || root.controller.focusedSide) }
            }

            Rectangle {
                anchors.left: parent.left; anchors.bottom: parent.bottom
                anchors.leftMargin: 24; anchors.bottomMargin: 24
                color: "#0c1122"; border.color: "#1e2d4a"; radius: 8
                implicitHeight: 36; implicitWidth: hintText.implicitWidth + 28
                Text {
                    id: hintText
                    anchors.centerIn: parent
                    text: root.editable ? "双击地图新增 · 右键单元删除 · 选中单元后规划路径" : "推演进行中，初始阵容已锁定"
                    color: "#c2cad8"; font.pixelSize: 11
                    renderType: Text.NativeRendering
                }
            }
        }
    }

    function visibleRoutes() {
        var rs = []
        for (var i = 0; i < root.units.length; i++) {
            var u = root.units[i]
            if (u.kind === "commandpost") continue
            if (u.side !== (root.restrictedSide || root.controller.focusedSide)) continue
            if (!u.schedule || u.schedule.length < 1) continue
            rs.push({ color: u.side === "red" ? "#ff6675" : "#62aaff", pendingColor: "#9fb0c5", points: u.schedule })
        }
        return rs
    }

    function addNew() { if (root.editable) editDialog.openNew(canvas.mapSize.w / 2, canvas.mapSize.h / 2, root.restrictedSide || "red") }
    function editSelected() {
        if (list.currentIndex < 0) return
        editDialog.openWith(root.units[list.currentIndex])
    }
    function planRouteSelected() {
        if (list.currentIndex < 0) return
        var u = root.units[list.currentIndex]
        if (u.kind === "commandpost") return
        var snap = root.controller.unitAt(u.id)
        routeDialog.openFor(snap)
    }
    function removeSelected() {
        if (list.currentIndex < 0) return
        confirmDelete.uid = root.units[list.currentIndex].id
        confirmDelete.open()
    }
    function saveToFile() {
        if (!root.editor) return
        root.editor.saveJsonText(root.editor.scenarioDir() + "/editing.json", JSON.stringify(root.controller.unitsJson(), null, 2))
        reload()
    }
    function loadFromFile() {
        if (!root.editor) return
        var txt = root.editor.loadJsonText(root.editor.scenarioDir() + "/editing.json")
        if (txt) {
            try {
                var obj = JSON.parse(txt)
                if (obj && obj.units) {
                    pushUndo()
                    root.controller.replaceScenario(obj)
                }
            } catch (e) { console.warn("parse err", e) }
        }
        reload()
    }

    UnitEditDialog {
        id: editDialog
        onFormAccepted: function(data) {
            if (!root.editable) return
            if (root.restrictedSide) data.side = root.restrictedSide
            var prevId = list.currentIndex >= 0 && root.units[list.currentIndex] ? root.units[list.currentIndex].id : ""
            root.pushUndo()
            root.controller.upsertUnit(data)
            root.reload()
            // 重新定位到新建/编辑的 unit
            var targetId = data.id || prevId
            if (targetId) {
                for (var i = 0; i < root.units.length; i++) {
                    if (root.units[i].id === targetId) {
                        list.currentIndex = i
                        if (root.units[i].x !== undefined && root.units[i].y !== undefined)
                            canvas.centerOn(root.units[i].x, root.units[i].y)
                        break
                    }
                }
            }
        }
    }

    RoutePlannerDialog {
        id: routeDialog
        controller: root.controller
        editor: root.editor
        onRouteAccepted: function(points) {
            if (!root.editable) return
            var cur = root.units[list.currentIndex]
            if (!cur) return
            var data = JSON.parse(JSON.stringify(cur))
            data.schedule = points
            root.pushUndo()
            root.controller.upsertUnit(data)
            if (!root.controller.networked) root.controller.setUnitSchedule(cur.id, points)
            root.reload()
        }
    }

    Dialog {
        id: confirmDelete
        property string uid: ""
        title: "删除确认"
        modal: true; anchors.centerIn: parent
        standardButtons: Dialog.NoButton
        background: Rectangle { color: t.panel; border.color: t.border; radius: 6 }
        Column {
            anchors.margins: 16
            Text { text: "确定要删除该单元？"; color: t.text; font.pixelSize: 14; renderType: Text.NativeRendering }
        }
        footer: DialogButtonBox {
            TonalButton { text: "删除"; base: t.danger; enabled: root.editable; onClicked: { if (root.editable && confirmDelete.uid) { root.pushUndo(); root.controller.removeUnit(confirmDelete.uid); root.reload(); confirmDelete.uid = "" } confirmDelete.close() } }
            GhostButton { text: "取消"; onClicked: confirmDelete.close() }
        }
    }
}
