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
    property bool mapDominant: false
    property bool editable: !root.controller.networked
        || (root.controller.matchPhase === "preparing"
            && (root.rosterMode ? root.controller.canEditOwnRoster : root.controller.canEditScenario))
    property string pendingFocusUnitId: ""
    property var selectedIds: []
    property var clipboardUnits: []
    property var validationIssues: []
    property int gridSize: 100
    property bool compactLayout: width < 820
    property bool narrowLayout: width < 560
    property real compactUnitPanelHeight: Math.max(180,
        Math.min(root.narrowLayout ? 230 : 260, root.height * 0.32))
    property real compactMapHeight: Math.max(260,
        root.height - root.compactUnitPanelHeight)
    property bool positionPickMode: false
    property string positionStatus: ""
    property bool positionCommitPending: false
    onEditableChanged: if (!root.editable) root.positionPickMode = false

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
        property color panel: AppContext.panel; property color panelAlt: AppContext.raised
        property color border: AppContext.line; property color text: AppContext.text
        property color textDim: AppContext.textDim; property color muted: AppContext.muted
        property color accent: AppContext.signal; property color danger: AppContext.danger
        property color red: AppContext.red; property color blue: AppContext.blue
        property color success: AppContext.success
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
        // The scenario roster is a QML var and may be backed by a Qt list.
        // Push it explicitly so the canvas does not depend on reference-change
        // notification timing for its internal Canvas model.
        if (canvas) canvas.refreshUnitSource()

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

    function currentPositionUnit() {
        var index = list.currentIndex
        if (index >= 0 && root.units[index]) return root.units[index]
        if (root.selectedIds.length === 1) {
            for (var i = 0; i < root.units.length; ++i)
                if (root.units[i].id === root.selectedIds[0]) return root.units[i]
        }
        return null
    }

    function applyPickedPosition(point) {
        if (!root.positionPickMode || !root.editable || !point) return
        var current = root.currentPositionUnit()
        if (!current) {
            root.positionStatus = "请先选择一个单位"
            root.positionPickMode = false
            return
        }
        var x = Number(point.x)
        var y = Number(point.y)
        if (!isFinite(x) || !isFinite(y)) return
        x = Math.max(0, Math.min(canvas.mapSize.w, x))
        y = Math.max(0, Math.min(canvas.mapSize.h, y))
        var data = JSON.parse(JSON.stringify(current))
        data.x = Math.round(x)
        data.y = Math.round(y)
        root.pushUndo()
        var networked = Boolean(root.controller.networked)
        root.positionCommitPending = networked
        var targetId = root.controller.upsertUnit(data)
        if (!targetId) {
            root.positionCommitPending = false
            root.positionStatus = "初始位置提交失败"
            return
        }
        root.pendingFocusUnitId = targetId
        root.selectedIds = [targetId]
        root.positionPickMode = false
        if (!networked || root.positionCommitPending)
            root.positionStatus = networked ? "已提交位置，等待服务器同步 · " + data.x + ", " + data.y
                : "初始位置已保存 · " + data.x + ", " + data.y
        root.reload()
    }

    function togglePositionPickMode() {
        if (!root.editable) return
        if (root.positionPickMode) {
            root.positionPickMode = false
            root.positionStatus = "已取消选点"
            return
        }
        if (!root.currentPositionUnit()) {
            root.positionStatus = "请先选择一个单位"
            return
        }
        root.positionStatus = "请在 GIS 地图上点击新的初始位置"
        root.positionPickMode = true
    }

    function fitScenarioCanvas() {
        if (!canvas || canvas.width <= 0 || canvas.height <= 0) return
        var source = root.units || []
        var minX = canvas.mapSize.w
        var maxX = 0
        var minY = canvas.mapSize.h
        var maxY = 0
        var valid = 0
        for (var i = 0; i < source.length; ++i) {
            var unit = source[i] || ({})
            var x = Number(unit.x)
            var y = Number(unit.y)
            if (!isFinite(x) || !isFinite(y)) continue
            minX = Math.min(minX, x); maxX = Math.max(maxX, x)
            minY = Math.min(minY, y); maxY = Math.max(maxY, y)
            ++valid
        }
        if (valid === 0) {
            minX = 0; maxX = canvas.mapSize.w; minY = 0; maxY = canvas.mapSize.h
        }
        var spanX = Math.max(3000, maxX - minX + 3000)
        var spanY = Math.max(2400, maxY - minY + 2400)
        var fit = Math.min((canvas.width - 24) / spanX, (canvas.height - 24) / spanY)
        canvas.zoom = Math.max(0.005, Math.min(0.65, fit * 0.9))
        canvas.centerOn((minX + maxX) / 2, (minY + maxY) / 2)
    }
    Component.onCompleted: {
        reload()
        Qt.callLater(root.fitScenarioCanvas)
    }
    Timer { id: layoutFitTimer; interval: 120; repeat: false; onTriggered: root.fitScenarioCanvas() }
    onWidthChanged: layoutFitTimer.restart()
    onHeightChanged: layoutFitTimer.restart()
    Connections {
        target: root.controller
        function onCommandExecuted() { root.reload() }
        function onUnitsForward() {
            if (root.positionCommitPending) {
                root.positionCommitPending = false
                root.positionStatus = "初始位置已同步"
            }
            root.requestReload()
        }
        function onCommandStatusChanged() {
            if (!root.positionCommitPending || root.controller.lastCommandAction !== "scenarioUpsert") return
            if (root.controller.lastCommandStatus === "accepted") {
                root.positionCommitPending = false
                root.positionStatus = "初始位置已同步"
            } else if (root.controller.lastCommandStatus === "rejected"
                       || root.controller.lastCommandStatus === "unknown") {
                root.positionCommitPending = false
                root.positionStatus = root.controller.lastCommandMessage || "初始位置提交失败"
            }
        }
        function onErrorForward(message) {
            if (!root.positionCommitPending) return
            root.positionCommitPending = false
            root.positionStatus = message || "初始位置提交失败"
        }
    }

    Timer {
        id: reloadTimer
        interval: 300; repeat: false
        onTriggered: root.reload()
    }
    function requestReload() {
        if (!reloadTimer.running) reloadTimer.start()
    }

    GridLayout {
        anchors.fill: parent; rowSpacing: 0; columnSpacing: 0
        columns: root.mapDominant || root.compactLayout ? 1 : 2

        Rectangle {
            visible: !root.mapDominant
            Layout.row: root.compactLayout ? 1 : 0
            Layout.column: 0
            Layout.fillWidth: true
            Layout.fillHeight: !root.compactLayout
            Layout.preferredWidth: root.compactLayout ? -1 : Math.min(360, Math.max(280, root.width * 0.36))
            Layout.preferredHeight: root.compactLayout ? root.compactUnitPanelHeight : -1
            Layout.minimumHeight: root.compactLayout ? Math.min(180, root.compactUnitPanelHeight) : 0
            color: t.panel
            Rectangle { anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 1; color: t.border }

            ColumnLayout {
                anchors.fill: parent; anchors.margins: 16; spacing: 12

                ColumnLayout {
                    spacing: 4
                    Text { text: root.rosterMode ? (root.restrictedSide === "red" ? "红方初始阵容" : "蓝方初始阵容") : "GIS 场景编辑器"; color: t.text; font.pixelSize: 18; font.bold: true; renderType: Text.NativeRendering }
                    Text {
                        text: root.editable ? "编辑"
                            : root.controller.networked && root.controller.isRoomAdmin
                                && root.controller.matchPhase === "preparing"
                                ? "未开放编辑"
                                : "只读"
                        color: root.editable ? t.muted : t.danger; font.pixelSize: 11
                        wrapMode: Text.WordWrap; Layout.fillWidth: true; renderType: Text.NativeRendering
                    }
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
                    TonalButton { text: root.positionPickMode ? "取消" : "选点"; iconName: root.positionPickMode ? "close" : "locate"; base: root.positionPickMode ? t.danger : t.success; enabled: root.editable && !root.positionCommitPending && (!!root.currentPositionUnit() || root.positionPickMode); onClicked: root.togglePositionPickMode() }
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
            Layout.row: 0
            Layout.column: root.mapDominant || root.compactLayout ? 0 : 1
            Layout.fillHeight: true
            Layout.fillWidth: true
            Layout.minimumHeight: root.compactLayout
                ? Math.min(root.compactMapHeight, Math.max(220, root.height * 0.5)) : 0
            Layout.preferredHeight: root.compactLayout ? root.compactMapHeight : -1
            MapCanvas { controller: root.controller; editor: root.editor;
                id: canvas
                anchors.fill: parent; anchors.margins: 12
                scenarioUnits: root.units
                sideFilter: root.restrictedSide || root.controller.focusedSide
                showAllSides: !root.rosterMode
                focusUnitId: list.currentIndex >= 0 && root.units[list.currentIndex] ? root.units[list.currentIndex].id : ""
                selectedUnitIds: root.selectedIds
                pointPickMode: root.positionPickMode
                showCoordinateGrid: true
                showCoordinateReadout: true
                allowRightClickActions: !root.positionPickMode
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
                onDoubleClickedUnit: function(uid) {
                    if (!root.editable) return
                    for (var i = 0; i < root.units.length; ++i) {
                        if (root.units[i].id !== uid) continue
                        root.selectUnit(uid, i, Qt.NoModifier)
                        editDialog.openWith(root.units[i])
                        break
                    }
                }
                onClickedMap: function(lp) { root.applyPickedPosition(lp) }
                onRightClickedMap: function(lp) {
                    if (root.editable && !root.positionPickMode)
                        editDialog.openNew(lp.x, lp.y, root.restrictedSide || root.controller.focusedSide)
                }
                onDoubleClickedMap: function(lp) {
                    if (root.positionPickMode) root.applyPickedPosition(lp)
                    else if (root.editable) editDialog.openNew(lp.x, lp.y, root.restrictedSide || root.controller.focusedSide)
                }
            }

            Rectangle {
                visible: root.mapDominant
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.leftMargin: 18
                anchors.topMargin: 18
                width: Math.max(220, Math.min(parent.width - 180, 620))
                height: mapToolbar.implicitHeight + 12
                color: "#08121dee"
                border.color: "#31506b"
                radius: 6
                z: 24

                Flow {
                    id: mapToolbar
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 6

                    TonalButton { text: "新增"; iconName: "plus"; base: t.accent; enabled: root.editable; onClicked: root.addNew() }
                    TonalButton { text: "编辑"; iconName: "edit"; base: "#2a4f86"; enabled: root.editable && root.currentPositionUnit(); onClicked: root.editSelected() }
                    TonalButton {
                        text: "路径"; iconName: "command"; base: t.success
                        enabled: root.editable && !!root.currentPositionUnit()
                            && root.currentPositionUnit().kind !== "commandpost"
                        onClicked: root.planRouteSelected()
                    }
                    TonalButton {
                        text: root.positionPickMode ? "取消选点" : "初始位置"
                        iconName: root.positionPickMode ? "close" : "locate"
                        base: root.positionPickMode ? t.danger : t.success
                        enabled: root.editable && !root.positionCommitPending
                            && (!!root.currentPositionUnit() || root.positionPickMode)
                        onClicked: root.togglePositionPickMode()
                    }
                    TonalButton { text: "删除"; iconName: "delete"; base: t.danger; enabled: root.editable && root.selectedOrCurrent().length > 0; onClicked: root.removeSelected() }
                    GhostButton { text: "全图"; iconName: "scan"; onClicked: root.fitScenarioCanvas() }
                }
            }

            Rectangle {
                anchors.top: parent.top; anchors.right: parent.right
                anchors.topMargin: 18; anchors.rightMargin: 20
                color: "#08121dcc"; border.color: "#31506b"; radius: 4
                implicitWidth: canvasStatus.implicitWidth + 18; implicitHeight: 24
                z: 20
                Text {
                    id: canvasStatus
                    anchors.centerIn: parent
                    text: "GIS 地图 · " + root.units.length + " 个单位"
                    color: root.units.length > 0 ? t.success : t.muted
                    font.pixelSize: 10; font.bold: true
                }
            }

            Rectangle {
                id: mapHint
                visible: root.positionPickMode || root.positionStatus.length > 0
                         || (!root.editable && root.controller && root.controller.networked
                             && root.controller.isRoomAdmin
                             && root.controller.matchPhase === "preparing")
                anchors.left: parent.left; anchors.bottom: parent.bottom
                anchors.right: root.narrowLayout ? parent.right : undefined
                anchors.leftMargin: 24; anchors.bottomMargin: 24
                anchors.rightMargin: root.narrowLayout ? 24 : 0
                color: "#0c1122"; border.color: "#1e2d4a"; radius: 8
                implicitHeight: visible ? Math.max(36, hintText.implicitHeight + 16) : 0
                implicitWidth: visible ? Math.min(parent.width - 48, hintText.implicitWidth + 28) : 0
                Text {
                    id: hintText
                    anchors.fill: parent; anchors.margins: 8
                    text: root.positionPickMode ? "选点"
                        : root.positionStatus.length > 0 ? root.positionStatus
                        : root.editable ? ""
                        : root.controller.networked && root.controller.isRoomAdmin
                            && root.controller.matchPhase === "preparing"
                            ? "服务器未开放编辑权限"
                            : "推演进行中，初始阵容已锁定"
                    color: "#c2cad8"; font.pixelSize: 11
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.WordWrap
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
        var unit = root.currentPositionUnit()
        if (!unit) return
        editDialog.openWith(unit)
    }
    function planRouteSelected() {
        var u = root.currentPositionUnit()
        if (!u) return
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
            SpinBox { id: batchSpeed; from: 0; to: 360; value: 120; editable: true; Layout.columnSpan: 2; Layout.fillWidth: true }

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
