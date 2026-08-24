pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"

Item {
    id: root

    property var controller: null
    property var editor: null
    property bool loadingDraft: false
    property bool dirty: false
    property bool configSaving: false
    property string statusText: ""
    property string statusKind: "idle"
    property color page: AppContext.page
    property color ink: AppContext.text
    property color dim: AppContext.muted
    property color panel: AppContext.panel
    property color panelAlt: AppContext.raised
    property color line: AppContext.line
    property color cyan: AppContext.signal
    property color orange: AppContext.warning
    property color danger: AppContext.danger
    property color success: AppContext.success
    property var draftParameters: ({})
    property bool compactLayout: width < 860
    property bool narrowLayout: width < 560
    property bool tinyLayout: width < 420
    readonly property bool strictVmf: root.controller
        && root.controller.protocolProfile === "vmf-guided-strike-v1"
    // A two-column admin workspace needs enough width for the editor's own
    // list/canvas split.  Below this point stack the panels so the canvas is
    // never reduced to a clipped sliver.
    property bool desktopWorkspace: width >= 1320
    // In the stacked layout the editor contains a roster and a canvas in
    // separate rows.  Reserve both rows in the panel; the outer Flickable
    // provides scrolling for short windows.
    property real workspaceHeight: root.desktopWorkspace
        ? Math.max(520, Math.min(760, height - 220))
        : Math.max(900, Math.min(1080, height - 140))

    readonly property var seatKeys: [
        "red_commander", "red_attack", "red_recon", "red_ground", "red_jammer",
        "blue_commander", "blue_attack", "blue_recon", "blue_ground", "blue_jammer"
    ]

    function seatTitle(key) {
        var parts = String(key).split("_")
        var side = parts[0] === "red" ? "红方" : "蓝方"
        var names = { commander: "指挥官", attack: "攻击机", recon: "侦察机",
            ground: "地面单位", jammer: "干扰机" }
        return side + " · " + (names[parts[1]] || parts[1])
    }

    function seatSide(key) { return String(key).split("_")[0] === "red" ? "red" : "blue" }

    function limitFor(key) {
        var limits = root.controller ? root.controller.onlineSeatLimits : ({})
        var value = Number(limits ? limits[key] : 0)
        return isFinite(value) ? Math.max(0, Math.min(64, Math.round(value))) : 0
    }

    function parameterFor(key) {
        var value = root.draftParameters ? root.draftParameters[key] : null
        return value && typeof value === "object" ? value : ({})
    }

    function parameterText(key, name) {
        var value = root.parameterFor(key)[name]
        return value === undefined || value === null ? "" : String(value)
    }

    function setParameter(key, name, text) {
        var next = JSON.parse(JSON.stringify(root.draftParameters || ({})))
        var parameters = next[key] || ({})
        var trimmed = String(text).trim()
        if (!trimmed) delete parameters[name]
        else {
            var number = Number(trimmed)
            parameters[name] = isFinite(number) ? number : trimmed
        }
        if (Object.keys(parameters).length === 0) delete next[key]
        else next[key] = parameters
        root.draftParameters = next
        if (!root.loadingDraft) {
            root.dirty = true
            root.statusKind = "dirty"
        }
    }

    function loadDraft() {
        if (!root.controller) return
        root.loadingDraft = true
        root.draftParameters = JSON.parse(JSON.stringify(root.controller.onlineSeatParameters || ({})))
        roomNameField.text = root.controller.roomName || ""
        roomDescriptionField.text = root.controller.roomDescription || ""
        scenarioIdField.text = root.controller.scenarioId || "default"
        protocolProfileCombo.currentIndex = root.controller.protocolProfile === "vmf-guided-strike-v1" ? 1 : 0
        root.dirty = false
        root.configSaving = false
        root.loadingDraft = false
    }

    function seatsForKey(key) {
        var result = []
        var seats = root.controller ? root.controller.onlineSeats : []
        var prefix = String(key) + "_"
        for (var i = 0; i < seats.length; ++i) {
            var seat = seats[i] || ({})
            var id = String(seat.seatId || "")
            if (id === key || id.indexOf(prefix) === 0) result.push(seat)
        }
        return result
    }

    function occupiedFor(key) {
        var count = 0
        var seats = root.seatsForKey(key)
        for (var i = 0; i < seats.length; ++i) if (seats[i].occupied) ++count
        return count
    }

    function occupiedSeatCount() {
        var count = 0
        var seats = root.controller ? root.controller.onlineSeats : []
        for (var i = 0; i < seats.length; ++i) if (seats[i].occupied) ++count
        return count
    }

    function totalCapacity() {
        var total = 0
        for (var i = 0; i < root.seatKeys.length; ++i) total += root.limitFor(root.seatKeys[i])
        return total
    }

    function sideCapacity(side) {
        var total = 0
        for (var i = 0; i < root.seatKeys.length; ++i)
            if (root.seatSide(root.seatKeys[i]) === side) total += root.limitFor(root.seatKeys[i])
        return total
    }

    function sideOccupied(side) {
        var total = 0
        for (var i = 0; i < root.seatKeys.length; ++i)
            if (root.seatSide(root.seatKeys[i]) === side) total += root.occupiedFor(root.seatKeys[i])
        return total
    }

    function scenarioCount() {
        if (!root.controller) return 0
        var scenario = root.controller.unitsJson() || ({})
        return Array.isArray(scenario.units) ? scenario.units.length : 0
    }

    function parameterCount() {
        var total = 0
        var parameters = root.draftParameters || ({})
        for (var key in parameters) {
            var value = parameters[key]
            if (value && typeof value === "object" && Object.keys(value).length > 0) ++total
        }
        return total
    }

    function phaseLabel() {
        var phase = root.controller ? root.controller.matchPhase : ""
        if (phase === "running") return "推演中"
        if (phase === "paused") return "已暂停"
        if (phase === "finished") return "已结束"
        return "准备阶段"
    }

    function phaseColor() {
        var phase = root.controller ? root.controller.matchPhase : ""
        if (phase === "running") return root.cyan
        if (phase === "finished") return root.danger
        return root.orange
    }

    function validateParameterDraft(parameters) {
        for (var key in parameters) {
            var value = parameters[key] || ({})
            for (var name in value) {
                var number = Number(value[name])
                if (!isFinite(number) || number < 0 || number > 1000000) {
                    root.statusText = "参数无效：" + root.seatTitle(key)
                    root.statusKind = "error"
                    return false
                }
            }
        }
        return true
    }

    function saveConfig() {
        if (!root.controller || !root.controller.isRoomAdmin || root.configSaving) return
        var name = roomNameField.text.trim()
        if (!name) {
            root.statusText = "房间名称不能为空"
            root.statusKind = "error"
            return
        }
        var parameters = JSON.parse(JSON.stringify(root.draftParameters || ({})))
        if (!root.validateParameterDraft(parameters)) return
        root.statusText = "正在保存配置..."
        root.statusKind = "saving"
        root.configSaving = true
        root.controller.updateOnlineRoomConfig({
            name: name,
            description: roomDescriptionField.text,
            scenarioId: scenarioIdField.text.trim() || "default",
            protocolProfile: protocolProfileCombo.currentValue || "native",
            seatParameters: parameters,
            expectedConfigVersion: Math.max(1, Number(root.controller.configVersion) || 1)
        })
    }

    function reloadConfig() {
        root.statusText = "已重新载入服务器配置"
        root.statusKind = "info"
        root.loadDraft()
    }

    function openConfig() {
        if (!root.dirty) root.loadDraft()
        configDialog.open()
    }

    function requestLeaveRoom() {
        if (root.dirty) leaveRoomDialog.open()
        else root.controller.leaveOnlineRoom()
    }

    Connections {
        target: root.controller
        function onRoomStateChanged() { if (!root.dirty) root.loadDraft() }
        function onOnlineStateChanged() {
            if (root.controller.onlineStage === "roomAdmin" && !root.dirty) root.loadDraft()
        }
        function onCommandStatusChanged() {
            if (root.controller.lastCommandAction !== "updateRoomConfig") return
            if (root.controller.lastCommandStatus === "accepted") {
                root.configSaving = false
                root.dirty = false
                root.statusKind = "success"
                root.statusText = "配置已保存"
                root.loadDraft()
                configDialog.close()
            } else if (root.controller.lastCommandStatus === "rejected") {
                root.configSaving = false
                root.statusKind = "error"
                root.statusText = root.controller.lastCommandCode === "ROOM_CONFIG_VERSION_CONFLICT"
                    ? "保存冲突：服务器已更新，请重载后再保存"
                    : (root.controller.lastCommandMessage || "配置保存失败")
            }
        }
        function onErrorForward(message) {
            if (!root.configSaving) return
            root.configSaving = false
            root.statusKind = "error"
            root.statusText = message
        }
    }

    Component.onCompleted: Qt.callLater(root.loadDraft)

    Rectangle { anchors.fill: parent; color: root.page }
    Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 3; color: root.cyan }

    Flickable {
        id: adminFlick
        anchors.fill: parent
        anchors.margins: 14
        clip: true
        contentWidth: width
        contentHeight: adminColumn.implicitHeight + 18
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: adminColumn
            width: adminFlick.width
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text { text: "房间管理"; color: root.ink; font.pixelSize: root.narrowLayout ? 19 : 22; font.bold: true }
                    Text { Layout.fillWidth: true; text: (root.controller.roomName || root.controller.currentRoomId) + " · " + root.controller.username; color: root.dim; font.pixelSize: 10; elide: Text.ElideRight }
                }
                Rectangle {
                    Layout.preferredHeight: 28
                    Layout.preferredWidth: phaseText.implicitWidth + 24
                    color: Qt.rgba(root.phaseColor().r, root.phaseColor().g, root.phaseColor().b, 0.13)
                    border.color: root.phaseColor(); radius: 14
                    Text { id: phaseText; anchors.centerIn: parent; text: root.phaseLabel(); color: root.phaseColor(); font.pixelSize: 10; font.bold: true }
                }
                GhostButton { text: "退出"; iconName: "close"; enabled: !root.controller.leaveRoomPending; onClicked: root.requestLeaveRoom() }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: root.width >= 900 ? 4 : root.width >= 520 ? 2 : 1
                columnSpacing: 8; rowSpacing: 8
                Repeater {
                    model: [
                        { title: "战位", value: root.occupiedSeatCount() + " / " + root.totalCapacity(), detail: root.strictVmf ? ("红 " + root.sideOccupied("red") + " · 蓝 " + root.sideOccupied("blue")) : ("红 " + root.sideOccupied("red") + " · 蓝 " + root.sideOccupied("blue")), color: root.cyan },
                        { title: "场景", value: root.scenarioCount() + " 个单位", detail: root.controller.canEditScenario ? "可编辑" : "已锁定", color: root.controller.canEditScenario ? root.success : root.orange },
                        { title: "参数", value: root.parameterCount() + " 个战位", detail: "空白为默认", color: root.cyan },
                        { title: "就绪", value: root.strictVmf ? (root.controller.redReady ? "红方已就绪" : "等待红方") : root.controller.redReady && root.controller.blueReady ? "双方已就绪" : "等待就绪", detail: root.controller.readyForSim ? "可开始" : "未满足条件", color: root.controller.readyForSim ? root.success : root.orange }
                    ]
                    delegate: Rectangle {
                        id: metricCard
                        required property var modelData
                        Layout.fillWidth: true; Layout.preferredHeight: 58
                        color: root.panel; border.color: root.line; radius: 5
                        Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 3; color: metricCard.modelData.color }
                        ColumnLayout { anchors.fill: parent; anchors.leftMargin: 11; anchors.rightMargin: 8; anchors.topMargin: 7; anchors.bottomMargin: 6; spacing: 1
                            Text { text: metricCard.modelData.title; color: root.dim; font.pixelSize: 9 }
                            Text { text: metricCard.modelData.value; color: root.ink; font.pixelSize: 14; font.bold: true; elide: Text.ElideRight }
                            Text { text: metricCard.modelData.detail; color: metricCard.modelData.color; font.pixelSize: 9; elide: Text.ElideRight }
                        }
                    }
                }
            }

            GridLayout {
                id: workspace
                Layout.fillWidth: true
                Layout.preferredHeight: root.workspaceHeight
                Layout.minimumHeight: root.workspaceHeight
                columns: root.desktopWorkspace ? 2 : 1
                columnSpacing: 12; rowSpacing: 12
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: root.desktopWorkspace ? 720 : 0
                    Layout.minimumHeight: root.workspaceHeight
                    Layout.preferredHeight: root.workspaceHeight
                    color: root.panel; border.color: root.line; radius: 6; clip: true
                    ScenarioEditorView {
                        anchors.fill: parent
                        anchors.margins: 1
                        controller: root.controller
                        editor: root.editor
                        mapDominant: true
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: root.desktopWorkspace ? 318 : -1
                    Layout.minimumWidth: root.desktopWorkspace ? 300 : 0
                    Layout.minimumHeight: root.workspaceHeight
                    Layout.preferredHeight: root.workspaceHeight
                    color: root.panel; border.color: root.line; radius: 6
                    ColumnLayout { anchors.fill: parent; anchors.margins: 14; spacing: 10
                        RowLayout { Layout.fillWidth: true; spacing: 8
                            Icon { name: "settings"; iconColor: root.cyan; iconSize: 16 }
                            ColumnLayout { Layout.fillWidth: true; spacing: 1
                                Text { text: "场景"; color: root.ink; font.pixelSize: 14; font.bold: true }
                                Text { text: root.strictVmf ? "VMF" : root.controller.roomMode === "pve" ? "PVE" : "PVP"; color: root.dim; font.pixelSize: 9 }
                            }
                        }
                        Text { visible: root.controller.roomDescription.length > 0; Layout.fillWidth: true; text: root.controller.roomDescription; color: root.dim; font.pixelSize: 10; maximumLineCount: 3; elide: Text.ElideRight; wrapMode: Text.WordWrap }
                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.line }
                        SectionTitle { text: root.strictVmf ? "红方参演与蓝方固定靶" : "双方战位" }
                        Repeater {
                            model: ["red", "blue"]
                            delegate: Rectangle {
                                id: sideSummary
                                required property string modelData
                                Layout.fillWidth: true; Layout.preferredHeight: 42
                                color: root.page; border.color: sideSummary.modelData === "red" ? root.danger : root.cyan; radius: 4
                                RowLayout { anchors.fill: parent; anchors.margins: 9; spacing: 8
                                    Rectangle { Layout.preferredWidth: 5; Layout.preferredHeight: 22; color: sideSummary.modelData === "red" ? root.danger : root.cyan; radius: 2 }
                                    ColumnLayout { Layout.fillWidth: true; spacing: 1
                                        Text { text: sideSummary.modelData === "red" ? (root.strictVmf ? "红方参演战位" : "红方") : (root.strictVmf ? "蓝方固定靶" : "蓝方"); color: root.ink; font.pixelSize: 10; font.bold: true }
                                        Text { text: root.sideOccupied(sideSummary.modelData) + " / " + root.sideCapacity(sideSummary.modelData) + (root.strictVmf && sideSummary.modelData === "blue" ? " 个托管目标" : " 个战位"); color: root.dim; font.pixelSize: 9 }
                                    }
                                    Text { text: root.strictVmf && sideSummary.modelData === "blue" ? "固定不行动" : root.sideOccupied(sideSummary.modelData) ? "有占用" : "空闲"; color: root.sideOccupied(sideSummary.modelData) ? root.cyan : root.dim; font.pixelSize: 9 }
                                }
                            }
                        }
                        Item { Layout.fillHeight: true; Layout.minimumHeight: 2 }
                        Flow { Layout.fillWidth: true; spacing: 7
                            TonalButton { text: "编辑配置"; iconName: "settings"; base: root.cyan; enabled: root.controller.canEditScenario && !root.configSaving; onClicked: root.openConfig() }
                            GhostButton { text: "重载"; iconName: "refresh"; enabled: !root.configSaving; onClicked: root.reloadConfig() }
                        }
                        Text { Layout.fillWidth: true; text: root.statusText || (root.dirty ? "有未保存的修改" : "配置已同步"); color: root.statusKind === "error" ? root.danger : root.statusKind === "success" ? root.success : root.statusKind === "dirty" ? root.orange : root.dim; font.pixelSize: 9; wrapMode: Text.WordWrap; maximumLineCount: 2 }
                    }
                }
            }
        }
    }

    Dialog {
        id: configDialog
        title: "编辑房间配置"
        modal: true
        anchors.centerIn: parent
        width: Math.min(760, Math.max(300, root.width - 20))
        height: Math.min(720, Math.max(340, root.height - 20))
        standardButtons: Dialog.NoButton
        background: Rectangle { color: root.panel; border.color: root.cyan; radius: 7 }
        contentItem: ScrollView {
            clip: true; contentWidth: availableWidth
            ScrollBar.vertical.policy: ScrollBar.AsNeeded
            ColumnLayout {
                width: configDialog.width - 28
                spacing: 12
                GridLayout {
                    Layout.fillWidth: true; columns: root.narrowLayout ? 1 : 2; columnSpacing: 10; rowSpacing: 8
                    ColumnLayout { Layout.fillWidth: true; Text { text: "房间名称"; color: root.dim; font.pixelSize: 9 }
                        TextField {
                            id: roomNameField
                            Layout.fillWidth: true; implicitHeight: 32; maximumLength: 96
                            placeholderText: "输入房间名称"; selectByMouse: true
                            color: root.ink; placeholderTextColor: root.dim
                            background: Rectangle { color: root.panelAlt; border.color: roomNameField.activeFocus ? root.cyan : root.line; radius: 4 }
                            onTextChanged: if (!root.loadingDraft) { root.dirty = true; root.statusKind = "dirty" }
                        }
                    }
                    ColumnLayout { Layout.fillWidth: true; Text { text: "场景标识"; color: root.dim; font.pixelSize: 9 }
                        TextField {
                            id: scenarioIdField
                            Layout.fillWidth: true; implicitHeight: 32; maximumLength: 128
                            placeholderText: "default"; selectByMouse: true
                            color: root.ink; placeholderTextColor: root.dim
                            background: Rectangle { color: root.panelAlt; border.color: scenarioIdField.activeFocus ? root.cyan : root.line; radius: 4 }
                            onTextChanged: if (!root.loadingDraft) { root.dirty = true; root.statusKind = "dirty" }
                        }
                    }
                    ColumnLayout { Layout.fillWidth: true; Text { text: "协议"; color: root.dim; font.pixelSize: 9 }
                        ComboBox {
                            id: protocolProfileCombo
                            Layout.fillWidth: true; implicitHeight: 32
                            model: [
                                { text: "标准", value: "native" },
                                { text: "VMF", value: "vmf-guided-strike-v1" }
                            ]
                            textRole: "text"; valueRole: "value"
                            contentItem: Text { text: protocolProfileCombo.currentText; color: root.ink; verticalAlignment: Text.AlignVCenter; leftPadding: 8; elide: Text.ElideRight; font.pixelSize: 9 }
                            background: Rectangle { color: root.panelAlt; border.color: protocolProfileCombo.activeFocus ? root.cyan : root.line; radius: 4 }
                            onActivated: if (!root.loadingDraft) { root.dirty = true; root.statusKind = "dirty" }
                        }
                    }
                    ColumnLayout { Layout.fillWidth: true; Layout.columnSpan: root.narrowLayout ? 1 : 2; Text { text: "说明"; color: root.dim; font.pixelSize: 9 }
                        TextArea {
                            id: roomDescriptionField
                            Layout.fillWidth: true; Layout.preferredHeight: 66
                            placeholderText: ""; wrapMode: TextArea.Wrap; selectByMouse: true
                            color: root.ink; placeholderTextColor: root.dim
                            background: Rectangle { color: root.panelAlt; border.color: roomDescriptionField.activeFocus ? root.cyan : root.line; radius: 4 }
                            onTextChanged: {
                                if (text.length > 512) text = text.slice(0, 512)
                                if (!root.loadingDraft) { root.dirty = true; root.statusKind = "dirty" }
                            }
                        }
                    }
                }
                SectionTitle { text: "单位与战位" }
                GridLayout {
                    Layout.fillWidth: true; columns: root.narrowLayout ? 1 : 2; columnSpacing: 8; rowSpacing: 7
                    Repeater {
                        model: root.seatKeys
                        delegate: Rectangle {
                            id: limitCard
                            required property string modelData
                            Layout.fillWidth: true; Layout.preferredHeight: 48; color: root.panelAlt; border.color: root.line; radius: 4
                            RowLayout { anchors.fill: parent; anchors.leftMargin: 9; anchors.rightMargin: 8; spacing: 8
                                ColumnLayout { Layout.fillWidth: true; spacing: 1
                                    Text { text: root.seatTitle(limitCard.modelData); color: root.ink; font.pixelSize: 10; elide: Text.ElideRight }
                                    Text { text: root.occupiedFor(limitCard.modelData) + " 个已占用"; color: root.occupiedFor(limitCard.modelData) ? root.orange : root.dim; font.pixelSize: 8 }
                                }
                                Rectangle {
                                    Layout.preferredWidth: 82; Layout.preferredHeight: 30; color: root.page; border.color: root.line; radius: 4
                                    RowLayout { anchors.centerIn: parent; spacing: 6
                                        Icon { name: "unit"; iconColor: root.cyan; iconSize: 13 }
                                        Text { text: root.limitFor(limitCard.modelData); color: root.ink; font.pixelSize: 13; font.bold: true }
                                    }
                                }
                            }
                        }
                    }
                }
                SectionTitle { text: "通信与侦察覆盖" }
                Text { Layout.fillWidth: true; text: "单位：米"; color: root.dim; font.pixelSize: 9 }
                GridLayout {
                    Layout.fillWidth: true; columns: root.narrowLayout ? 1 : 2; columnSpacing: 8; rowSpacing: 7
                    Repeater {
                        model: root.seatKeys
                        delegate: Rectangle {
                            id: parameterCard
                            required property string modelData
                            Layout.fillWidth: true; Layout.preferredHeight: root.tinyLayout ? 112 : 78; color: root.panelAlt; border.color: root.line; radius: 4
                            ColumnLayout { anchors.fill: parent; anchors.margins: 8; spacing: 5
                                Text { text: root.seatTitle(parameterCard.modelData); color: root.ink; font.pixelSize: 10; font.bold: true }
                                GridLayout { Layout.fillWidth: true; columns: root.tinyLayout ? 1 : 2; columnSpacing: 7; rowSpacing: 4
                                    ColumnLayout { Layout.fillWidth: true; spacing: 2; Text { text: "通信范围"; color: root.dim; font.pixelSize: 8 }
                                        TextField {
                                            id: communicationField
                                            Layout.fillWidth: true; implicitHeight: 28; placeholderText: "默认"
                                            validator: DoubleValidator { bottom: 0; top: 1000000; decimals: 2 }
                                            onEditingFinished: root.setParameter(parameterCard.modelData, "communicationRange", text)
                                            Binding { target: communicationField; property: "text"; value: root.parameterText(parameterCard.modelData, "communicationRange"); when: !communicationField.activeFocus }
                                        }
                                    }
                                    ColumnLayout { Layout.fillWidth: true; spacing: 2; Text { text: "侦察范围"; color: root.dim; font.pixelSize: 8 }
                                        TextField {
                                            id: detectField
                                            Layout.fillWidth: true; implicitHeight: 28; placeholderText: "默认"
                                            validator: DoubleValidator { bottom: 0; top: 1000000; decimals: 2 }
                                            onEditingFinished: root.setParameter(parameterCard.modelData, "detectRange", text)
                                            Binding { target: detectField; property: "text"; value: root.parameterText(parameterCard.modelData, "detectRange"); when: !detectField.activeFocus }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                Text { Layout.fillWidth: true; text: root.statusText; visible: text.length > 0; color: root.statusKind === "error" ? root.danger : root.statusKind === "success" ? root.success : root.dim; font.pixelSize: 9; wrapMode: Text.WordWrap }
            }
        }
        footer: DialogButtonBox {
            spacing: 7; padding: 9; background: Rectangle { color: root.panelAlt; radius: 6 }
            GhostButton { text: "重载"; iconName: "refresh"; enabled: !root.configSaving; onClicked: root.reloadConfig() }
            GhostButton { text: "取消"; iconName: "close"; enabled: !root.configSaving; onClicked: configDialog.close() }
            TonalButton { text: root.configSaving ? "保存中..." : "保存配置"; iconName: root.configSaving ? "refresh" : "check"; base: root.cyan; enabled: !root.configSaving && root.controller.canEditScenario; onClicked: root.saveConfig() }
        }
    }

    Dialog {
        id: leaveRoomDialog
        title: "放弃未保存修改？"
        modal: true
        anchors.centerIn: parent
        width: Math.min(420, Math.max(290, root.width - 20))
        standardButtons: Dialog.NoButton
        background: Rectangle { color: root.panel; border.color: root.orange; radius: 7 }
        contentItem: ColumnLayout { spacing: 8; Text { Layout.fillWidth: true; text: "未保存修改将丢失。"; color: root.ink; font.pixelSize: 11; wrapMode: Text.WordWrap } }
        footer: DialogButtonBox {
            spacing: 7; padding: 9; background: Rectangle { color: root.panelAlt; radius: 6 }
            GhostButton { text: "留在房间"; onClicked: leaveRoomDialog.close() }
            TonalButton { text: "放弃并退出"; base: root.orange; onClicked: { leaveRoomDialog.close(); root.dirty = false; root.controller.leaveOnlineRoom() } }
        }
    }
}
