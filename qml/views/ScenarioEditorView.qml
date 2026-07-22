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
    property string forcedSide: ""
    property bool rosterMode: false
    property bool editable: !root.controller.networked
        || (root.controller.matchPhase === "preparing"
            && (root.rosterMode ? root.controller.canEditOwnRoster : root.controller.canEditScenario))
    property string pendingFocusUnitId: ""
    property var selectedIds: []
    property var clipboardUnits: []
    property var validationIssues: []
    property int gridSize: 100

    function nudgeSelected(offsetX, offsetY) {
        if (!root.editable) return
        var ids = root.selectedOrCurrent()
        if (ids.length === 0) return
        var previous = JSON.stringify(root.controller.unitsJson())
        if (!root.controller.batchUpdateUnits(ids, { offsetX: offsetX, offsetY: offsetY })) return
        root.undoStack.push(previous)
        root.redoStack = []
        if (root.undoStack.length > 50) root.undoStack.shift()
        root.pendingFocusUnitId = ids[0]
        root.reload()
    }

    Shortcut {
        sequence: "W"
        context: Qt.WindowShortcut
        enabled: root.editable && root.selectedOrCurrent().length > 0
        onActivated: root.nudgeSelected(0, root.gridSize)
    }
    Shortcut {
        sequence: "S"
        context: Qt.WindowShortcut
        enabled: root.editable && root.selectedOrCurrent().length > 0
        onActivated: root.nudgeSelected(0, -root.gridSize)
    }
    Shortcut {
        sequence: "A"
        context: Qt.WindowShortcut
        enabled: root.editable && root.selectedOrCurrent().length > 0
        onActivated: root.nudgeSelected(-root.gridSize, 0)
    }
    Shortcut {
        sequence: "D"
        context: Qt.WindowShortcut
        enabled: root.editable && root.selectedOrCurrent().length > 0
        onActivated: root.nudgeSelected(root.gridSize, 0)
    }

    function isSelected(id) { return root.selectedIds.indexOf(id) >= 0 }
    function selectUnit(id, index, modifiers) {
        var next = root.selectedIds.slice()
        if (modifiers & Qt.ControlModifier) {
            var existing = next.indexOf(id)
            if (existing >= 0) next.splice(existing, 1)
            else next.push(id)
        } else if ((modifiers & Qt.ShiftModifier) && list.currentIndex >= 0) {
            next = []
            var first = Math.min(list.currentIndex, index)
            var last = Math.max(list.currentIndex, index)
            for (var i = first; i <= last; i++) next.push(root.units[i].id)
        } else next = [id]
        root.selectedIds = next
        list.currentIndex = index
    }

    function selectedOrCurrent() {
        if (root.selectedIds.length > 0) return root.selectedIds
        if (list.currentIndex >= 0 && root.units[list.currentIndex]) return [root.units[list.currentIndex].id]
        return []
    }

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
        var available = ({})
        for (var a = 0; a < root.units.length; a++) available[root.units[a].id] = true
        var retained = []
        for (var s = 0; s < root.selectedIds.length; s++) {
            if (available[root.selectedIds[s]]) retained.push(root.selectedIds[s])
        }
        root.selectedIds = retained
        root.validationIssues = root.controller.scenarioValidationIssues()

        // 保留列表选择，但只有新建/编辑后的显式焦点才移动视口。
        // 网络快照和本地刷新不能覆盖用户刚完成的画布拖拽。
        var focusId = root.pendingFocusUnitId
        if (focusId) {
            for (var i = 0; i < root.units.length; i++) {
                if (root.units[i].id === focusId) {
                    list.currentIndex = i
                    canvas.focusAt(root.units[i].x, root.units[i].y)
                    root.pendingFocusUnitId = ""
                    break
                }
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
                    Text { text: root.editable ? "双击地图放置新单元；右键单元删除；选中后可用 W/S/A/D 微调位置" : "推演已经开始，初始场景现为只读状态"; color: root.editable ? t.muted : t.danger; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true; renderType: Text.NativeRendering }
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
                            color: root.isSelected(scenarioRow.modelData.id) ? "#3478c1" : (scenarioRow.index % 2 === 0 ? "#1b3554" : "#234160")
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
                                onClicked: function(mouse) {
                                    root.selectUnit(scenarioRow.modelData.id, scenarioRow.index, mouse.modifiers)
                                    if (scenarioRow.modelData.x !== undefined && scenarioRow.modelData.y !== undefined)
                                        canvas.focusAt(scenarioRow.modelData.x, scenarioRow.modelData.y)
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
                    TonalButton { text: "删除"; base: t.danger; enabled: root.editable && root.selectedOrCurrent().length > 0; onClicked: root.removeSelected() }
                    GhostButton { visible: !root.rosterMode; text: "↶ 撤销"; enabled: root.editable && root.undoStack.length > 0; onClicked: root.undo() }
                    GhostButton { visible: !root.rosterMode; text: "↷ 重做"; enabled: root.editable && root.redoStack.length > 0; onClicked: root.redo() }
                    GhostButton { visible: !root.rosterMode; text: "保存"; onClicked: root.saveToFile() }
                    GhostButton { visible: !root.rosterMode; text: "读取"; enabled: root.editable; onClicked: root.loadFromFile() }
                }

                Flow {
                    Layout.fillWidth: true; spacing: 8
                    GhostButton {
                        text: "复制 " + root.selectedOrCurrent().length
                        enabled: root.selectedOrCurrent().length > 0
                        onClicked: root.clipboardUnits = root.controller.copyUnits(root.selectedOrCurrent())
                    }
                    GhostButton {
                        text: "粘贴"; enabled: root.editable && root.clipboardUnits.length > 0
                        onClicked: {
                            root.pushUndo()
                            var ids = root.controller.pasteUnits(root.clipboardUnits, root.gridSize, root.gridSize,
                                                                 root.restrictedSide)
                            root.selectedIds = ids
                            root.reload()
                        }
                    }
                    GhostButton { visible: !root.restrictedSide; text: "批量编辑"; enabled: root.editable && root.selectedOrCurrent().length > 0; onClicked: batchDialog.open() }
                    GhostButton { visible: !root.restrictedSide; text: "对齐与吸附"; enabled: root.editable && root.selectedOrCurrent().length > 1; onClicked: alignDialog.open() }
                    GhostButton {
                        text: root.validationIssues.length === 0 ? "场景校验 ✓" : "场景校验 " + root.validationIssues.length
                        onClicked: { root.validationIssues = root.controller.scenarioValidationIssues(); validationDialog.open() }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    ComboBox {
                        id: templateCombo
                        Layout.fillWidth: true; implicitHeight: 32
                        model: root.controller.unitTemplates(); textRole: "templateName"
                    }
                    TonalButton {
                        text: "从模板新增"; base: t.success; enabled: root.editable
                        onClicked: root.createFromTemplate(templateCombo.currentValue || templateCombo.model[templateCombo.currentIndex])
                    }
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
                selectedUnitIds: root.selectedIds
                allowRightClickActions: true
                routes: root.visibleRoutes()
                onUnitClicked: function(uid, btn, modifiers) {
                    for (var i = 0; i < root.units.length; i++) {
                        if (root.units[i].id === uid) {
                            root.selectUnit(uid, i, modifiers)
                            canvas.focusAt(root.units[i].x, root.units[i].y)
                            break
                        }
                    }
                    if (btn === "right" && root.editable) { confirmDelete.uids = [uid]; confirmDelete.open() }
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
                    text: root.editable ? "双击地图新增 · 右键单元删除 · W/S/A/D 微调 100m" : "推演进行中，初始阵容已锁定"
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
    function createFromTemplate(value) {
        if (!root.editable || !value) return
        var data = JSON.parse(JSON.stringify(value))
        data.id = ""
        data.callsign = "新" + (data.templateName || "单元")
        delete data.templateName
        data.x = canvas.center.x; data.y = canvas.center.y
        data.side = root.restrictedSide || root.controller.focusedSide || "red"
        root.pushUndo()
        var id = root.controller.upsertUnit(data)
        root.selectedIds = [id]; root.pendingFocusUnitId = id; root.reload()
    }
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
        var ids = root.selectedOrCurrent()
        if (ids.length === 0) return
        confirmDelete.uids = ids
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
        forcedSide: root.forcedSide
        onFormAccepted: function(data) {
            if (!root.editable) return
            if (root.restrictedSide) data.side = root.restrictedSide
            root.pushUndo()
            var targetId = root.controller.upsertUnit(data)
            root.pendingFocusUnitId = targetId
            root.reload()
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
        id: batchDialog
        title: "批量编辑 " + root.selectedOrCurrent().length + " 个单元"
        modal: true; anchors.centerIn: parent; width: Math.min(480, root.width - 32)
        standardButtons: Dialog.NoButton
        background: Rectangle { color: t.panel; border.color: t.border; radius: 6 }
        contentItem: GridLayout {
            columns: 3; columnSpacing: 10; rowSpacing: 8
            Text { text: "坐标偏移"; color: t.textDim; font.pixelSize: 11 }
            SpinBox { id: offsetXSpin; from: -100000; to: 100000; stepSize: 100; editable: true; value: 0; Layout.fillWidth: true }
            SpinBox { id: offsetYSpin; from: -100000; to: 100000; stepSize: 100; editable: true; value: 0; Layout.fillWidth: true }

            CheckBox { id: sideCheck; text: "统一阵营" }
            ComboBox { id: batchSide; model: [{text: "红方", value: "red"}, {text: "蓝方", value: "blue"}]; textRole: "text"; valueRole: "value"; Layout.columnSpan: 2; Layout.fillWidth: true }

            CheckBox { id: speedCheck; text: "速度 (m/s)" }
            SpinBox { id: batchSpeed; from: 0; to: 1000; value: 60; editable: true; Layout.columnSpan: 2; Layout.fillWidth: true }

            CheckBox { id: armorCheck; text: "装甲 (%)" }
            SpinBox { id: batchArmor; from: 0; to: 90; value: 10; editable: true; Layout.columnSpan: 2; Layout.fillWidth: true }

            CheckBox { id: detectCheck; text: "探测半径 (m)" }
            SpinBox { id: batchDetect; from: 0; to: 100000; value: 5000; stepSize: 100; editable: true; Layout.columnSpan: 2; Layout.fillWidth: true }
        }
        footer: DialogButtonBox {
            TonalButton {
                text: "应用"; iconName: "check"; base: t.accent
                onClicked: {
                    var changes = { offsetX: offsetXSpin.value, offsetY: offsetYSpin.value }
                    if (sideCheck.checked) changes.side = batchSide.currentValue
                    if (speedCheck.checked) changes.speed = batchSpeed.value
                    if (armorCheck.checked) changes.armor = batchArmor.value / 100
                    if (detectCheck.checked) changes.detectRange = batchDetect.value
                    root.pushUndo()
                    root.controller.batchUpdateUnits(root.selectedOrCurrent(), changes)
                    root.reload(); batchDialog.close()
                }
            }
            GhostButton { text: "取消"; iconName: "close"; onClicked: batchDialog.close() }
        }
    }

    Dialog {
        id: alignDialog
        title: "对齐、分布与吸附"
        modal: true; anchors.centerIn: parent; width: Math.min(520, root.width - 32)
        standardButtons: Dialog.NoButton
        background: Rectangle { color: t.panel; border.color: t.border; radius: 6 }
        function applyOperation(operation, value) {
            root.pushUndo()
            root.controller.transformUnits(root.selectedOrCurrent(), operation, value || 0)
            root.reload()
        }
        contentItem: ColumnLayout {
            spacing: 10
            Text { text: "对齐"; color: t.text; font.bold: true; font.pixelSize: 12 }
            Flow {
                Layout.fillWidth: true; spacing: 6
                GhostButton { text: "左对齐"; onClicked: alignDialog.applyOperation("alignLeft", 0) }
                GhostButton { text: "右对齐"; onClicked: alignDialog.applyOperation("alignRight", 0) }
                GhostButton { text: "上对齐"; onClicked: alignDialog.applyOperation("alignTop", 0) }
                GhostButton { text: "下对齐"; onClicked: alignDialog.applyOperation("alignBottom", 0) }
                GhostButton { text: "水平居中"; onClicked: alignDialog.applyOperation("alignCenterX", 0) }
                GhostButton { text: "垂直居中"; onClicked: alignDialog.applyOperation("alignCenterY", 0) }
            }
            Text { text: "等距分布"; color: t.text; font.bold: true; font.pixelSize: 12 }
            Flow {
                Layout.fillWidth: true; spacing: 6
                GhostButton { text: "水平等距"; enabled: root.selectedOrCurrent().length >= 3; onClicked: alignDialog.applyOperation("distributeX", 0) }
                GhostButton { text: "垂直等距"; enabled: root.selectedOrCurrent().length >= 3; onClicked: alignDialog.applyOperation("distributeY", 0) }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: "网格"; color: t.textDim; font.pixelSize: 11 }
                SpinBox { id: gridSpin; from: 10; to: 5000; stepSize: 10; value: root.gridSize; editable: true; Layout.fillWidth: true }
                TonalButton { text: "吸附"; base: t.success; onClicked: { root.gridSize = gridSpin.value; alignDialog.applyOperation("snap", gridSpin.value) } }
            }
        }
        footer: DialogButtonBox { GhostButton { text: "完成"; onClicked: alignDialog.close() } }
    }

    Dialog {
        id: validationDialog
        title: "场景校验"
        modal: true; anchors.centerIn: parent
        width: Math.min(620, root.width - 32); height: Math.min(520, root.height - 32)
        standardButtons: Dialog.NoButton
        background: Rectangle { color: t.panel; border.color: t.border; radius: 6 }
        contentItem: ColumnLayout {
            spacing: 8
            Text {
                text: root.validationIssues.length === 0 ? "未发现场景问题" : "发现 " + root.validationIssues.length + " 个问题"
                color: root.validationIssues.length === 0 ? t.success : t.danger
                font.pixelSize: 13; font.bold: true
            }
            ListView {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 4
                model: root.validationIssues
                delegate: Rectangle {
                    id: issueRow
                    required property var modelData
                    width: ListView.view.width; implicitHeight: 46; radius: 4
                    color: "#172941"; border.color: issueRow.modelData.severity === "error" ? t.danger : "#8b7439"
                    RowLayout {
                        anchors.fill: parent; anchors.margins: 8
                        Text { text: issueRow.modelData.severity === "error" ? "错误" : "警告"; color: issueRow.modelData.severity === "error" ? t.danger : "#f4c95d"; font.bold: true; font.pixelSize: 11 }
                        Text { text: issueRow.modelData.message; color: t.text; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight }
                        GhostButton {
                            visible: !!issueRow.modelData.unitId; text: "定位"; implicitHeight: 26
                            onClicked: {
                                for (var i = 0; i < root.units.length; i++) if (root.units[i].id === issueRow.modelData.unitId) {
                                    root.selectUnit(issueRow.modelData.unitId, i, 0)
                                    canvas.focusAt(root.units[i].x, root.units[i].y); break
                                }
                                validationDialog.close()
                            }
                        }
                    }
                }
            }
        }
        footer: DialogButtonBox { GhostButton { text: "关闭"; onClicked: validationDialog.close() } }
    }

    Dialog {
        id: confirmDelete
        property var uids: []
        title: "删除确认"
        modal: true; anchors.centerIn: parent
        standardButtons: Dialog.NoButton
        background: Rectangle { color: t.panel; border.color: t.border; radius: 6 }
        Column {
            anchors.margins: 16
            Text { text: "确定要删除选中的 " + confirmDelete.uids.length + " 个单元？"; color: t.text; font.pixelSize: 14; renderType: Text.NativeRendering }
        }
        footer: DialogButtonBox {
            TonalButton { text: "删除"; base: t.danger; enabled: root.editable; onClicked: { if (root.editable && confirmDelete.uids.length) { root.pushUndo(); root.controller.removeUnits(confirmDelete.uids); root.selectedIds = []; root.reload(); confirmDelete.uids = [] } confirmDelete.close() } }
            GhostButton { text: "取消"; onClicked: confirmDelete.close() }
        }
    }
}
