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
    property var draftLimits: ({})
    property var draftParameters: ({})
    property bool compactLayout: width < 980
    property bool narrowLayout: width < 640
    property bool tinyLayout: width < 440

    readonly property var seatKeys: [
        "red_commander", "red_attack", "red_recon", "red_ground", "red_jammer",
        "blue_commander", "blue_attack", "blue_recon", "blue_ground", "blue_jammer"
    ]

    function seatTitle(key) {
        var parts = String(key).split("_")
        var side = parts[0] === "red" ? "红方" : "蓝方"
        var names = {
            commander: "指挥官", attack: "攻击机", recon: "侦察机",
            ground: "地面单位", jammer: "干扰机"
        }
        return side + " · " + (names[parts[1]] || parts[1])
    }

    function seatSide(key) {
        return String(key).split("_")[0] === "red" ? "red" : "blue"
    }

    function limitFor(key) {
        var value = Number(root.draftLimits ? root.draftLimits[key] : 0)
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

    function setLimit(key, value) {
        var next = JSON.parse(JSON.stringify(root.draftLimits || ({})))
        next[key] = Math.max(0, Math.min(64, Math.round(Number(value) || 0)))
        root.draftLimits = next
        if (!root.loadingDraft) {
            root.dirty = true
            root.statusKind = "dirty"
        }
    }

    function setParameter(key, name, text) {
        var next = JSON.parse(JSON.stringify(root.draftParameters || ({})))
        var parameters = next[key] || ({})
        var trimmed = String(text).trim()
        if (!trimmed) {
            delete parameters[name]
        } else {
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
        root.draftLimits = JSON.parse(JSON.stringify(root.controller.onlineSeatLimits || ({})))
        root.draftParameters = JSON.parse(JSON.stringify(root.controller.onlineSeatParameters || ({})))
        roomNameField.text = root.controller.roomName || ""
        roomDescriptionField.text = root.controller.roomDescription || ""
        scenarioIdField.text = root.controller.scenarioId || "default"
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
        for (var i = 0; i < root.seatKeys.length; ++i) {
            if (root.seatSide(root.seatKeys[i]) === side) total += root.limitFor(root.seatKeys[i])
        }
        return total
    }

    function sideOccupied(side) {
        var total = 0
        for (var i = 0; i < root.seatKeys.length; ++i) {
            if (root.seatSide(root.seatKeys[i]) === side)
                total += root.occupiedFor(root.seatKeys[i])
        }
        return total
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

    function seatStateLabel(key) {
        var occupied = root.occupiedFor(key)
        var limit = root.limitFor(key)
        if (limit === 0 && occupied > 0) return "超出容量"
        if (occupied === 0) return "空闲"
        if (occupied >= limit) return "已占满"
        return "有占用"
    }

    function seatStateColor(key) {
        var state = root.seatStateLabel(key)
        return state === "超出容量" ? root.danger
            : state === "已占满" ? root.orange : state === "有占用" ? root.cyan : root.dim
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
        var scenarioId = scenarioIdField.text.trim() || "default"
        var limits = ({})
        for (var i = 0; i < root.seatKeys.length; ++i) {
            var key = root.seatKeys[i]
            limits[key] = root.limitFor(key)
        }
        var parameters = JSON.parse(JSON.stringify(root.draftParameters || ({})))
        if (!root.validateParameterDraft(parameters)) return
        root.statusText = "正在保存房间配置..."
        root.statusKind = "saving"
        root.dirty = true
        root.configSaving = true
        root.controller.updateOnlineRoomConfig({
            name: name,
            description: roomDescriptionField.text,
            scenarioId: scenarioId,
            seatLimits: limits,
            seatParameters: parameters,
            expectedConfigVersion: Math.max(1, Number(root.controller.configVersion) || 1)
        })
    }

    function reloadConfig() {
        root.statusText = "已放弃未保存的修改，已重新载入服务器配置"
        root.statusKind = "info"
        root.loadDraft()
    }

    function requestLeaveRoom() {
        if (root.dirty) leaveRoomDialog.open()
        else root.controller.leaveOnlineRoom()
    }

    function openConfig() {
        if (!root.dirty) root.loadDraft()
        configDialog.open()
    }

    Rectangle {
        anchors.fill: parent
        color: root.page
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 3
        color: root.cyan
        opacity: 0.8
    }

    Connections {
        target: root.controller

        function onRoomStateChanged() {
            if (!root.dirty) root.loadDraft()
        }

        function onOnlineStateChanged() {
            if (root.controller.onlineStage === "roomAdmin" && !root.dirty)
                root.loadDraft()
        }

        function onCommandStatusChanged() {
            if (root.controller.lastCommandAction !== "updateRoomConfig") return
            if (root.controller.lastCommandStatus === "accepted") {
                root.configSaving = false
                root.dirty = false
                root.statusKind = "success"
                root.statusText = "房间配置已保存 · 版本 " + root.controller.configVersion
                root.loadDraft()
                configDialog.close()
            } else if (root.controller.lastCommandStatus === "rejected") {
                root.configSaving = false
                root.statusKind = "error"
                root.statusText = root.controller.lastCommandCode === "ROOM_CONFIG_VERSION_CONFLICT"
                    ? "保存冲突：服务器配置已变化，请先重载后再保存"
                    : (root.controller.lastCommandMessage || "房间配置保存失败")
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

    ScrollView {
        id: adminScroll
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            id: adminColumn
            x: 12
            y: 10
            width: Math.max(0, adminScroll.availableWidth - 24)
            spacing: 9

        GridLayout {
            Layout.fillWidth: true
            columns: root.tinyLayout ? 1 : 2
            rowSpacing: 7
            columnSpacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    text: "房间管理"
                    color: root.ink
                    font.pixelSize: root.tinyLayout ? 18 : 20
                    font.bold: true
                }
                Text {
                    Layout.fillWidth: true
                    text: (root.controller.roomName || root.controller.currentRoomId)
                        + " · 账号 " + (root.controller.username || "")
                    color: root.dim
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.alignment: root.tinyLayout ? Qt.AlignLeft : Qt.AlignRight
                spacing: 7

                Rectangle {
                    Layout.preferredHeight: 28
                    Layout.preferredWidth: phaseText.implicitWidth + 24
                    radius: 14
                    color: Qt.rgba(root.phaseColor().r, root.phaseColor().g,
                                   root.phaseColor().b, 0.12)
                    border.color: root.phaseColor()
                    Text {
                        id: phaseText
                        anchors.centerIn: parent
                        text: root.phaseLabel()
                        color: root.phaseColor()
                        font.pixelSize: 10
                        font.bold: true
                    }
                    Rectangle {
                        width: 5
                        height: 5
                        radius: 3
                        anchors.left: parent.left
                        anchors.leftMargin: 9
                        anchors.verticalCenter: parent.verticalCenter
                        color: root.phaseColor()
                        visible: root.controller.matchPhase === "preparing"
                        SequentialAnimation on opacity {
                            running: root.controller.matchPhase === "preparing"
                            loops: Animation.Infinite
                            NumberAnimation { from: 0.35; to: 1; duration: 850 }
                            NumberAnimation { from: 1; to: 0.35; duration: 850 }
                        }
                    }
                }
                Text {
                    text: "配置 v" + root.controller.configVersion
                    color: root.cyan
                    font.family: "Consolas"
                    font.pixelSize: 10
                }
                GhostButton {
                    text: root.controller.leaveRoomPending ? "正在退出..." : "退出房间"
                    iconName: "close"
                    enabled: !root.controller.leaveRoomPending
                    onClicked: root.requestLeaveRoom()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: noticeText.implicitHeight + 18
            color: root.occupiedSeatCount() > 0 ? "#2b2418" : "#102a29"
            border.color: root.occupiedSeatCount() > 0 ? root.orange : root.cyan
            radius: 5
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                anchors.topMargin: 7
                anchors.bottomMargin: 7
                spacing: 8
                Icon {
                    name: root.occupiedSeatCount() > 0 ? "warning" : "check"
                    iconColor: root.occupiedSeatCount() > 0 ? root.orange : root.cyan
                    iconSize: 15
                }
                Text {
                    id: noticeText
                    Layout.fillWidth: true
                    text: root.occupiedSeatCount() > 0
                        ? "已有 " + root.occupiedSeatCount()
                          + " 个战位占用。容量和通信参数可调整；初始场景需先清空战位。"
                        : "准备阶段可编辑房间配置和完整初始场景。保存采用配置版本校验，避免覆盖其他管理员的修改。"
                    color: root.occupiedSeatCount() > 0 ? root.orange : root.dim
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: root.tinyLayout ? 1 : root.compactLayout ? 2 : 4
            columnSpacing: 8
            rowSpacing: 8

            Repeater {
                model: [
                    { title: "战位占用", value: root.occupiedSeatCount() + " / " + root.totalCapacity(),
                      detail: "红 " + root.sideOccupied("red") + " · 蓝 " + root.sideOccupied("blue"), color: root.cyan },
                    { title: "初始场景", value: root.controller.units.length + " 个单元",
                      detail: root.controller.canEditScenario ? "可编辑" : "有占用，已锁定", color: root.controller.canEditScenario ? root.success : root.orange },
                    { title: "参数覆盖", value: root.parameterCount() + " 个战位",
                      detail: "留空沿用场景默认值", color: root.cyan },
                    { title: "双方就绪", value: (root.controller.redReady ? "红方已就绪" : "红方待命"),
                      detail: root.controller.blueReady ? "蓝方已就绪" : "蓝方待命", color: root.controller.redReady && root.controller.blueReady ? root.success : root.dim }
                ]
                delegate: Rectangle {
                    id: metricCard
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredHeight: 61
                    color: root.panel
                    border.color: root.line
                    radius: 5
                    Rectangle {
                        width: 3
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        color: metricCard.modelData.color
                    }
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 9
                        anchors.topMargin: 8
                        anchors.bottomMargin: 7
                        spacing: 2
                        Text { text: metricCard.modelData.title; color: root.dim; font.pixelSize: 9 }
                        Text { text: metricCard.modelData.value; color: root.ink; font.pixelSize: 14; font.bold: true; elide: Text.ElideRight }
                        Text { text: metricCard.modelData.detail; color: metricCard.modelData.color; font.pixelSize: 9; elide: Text.ElideRight }
                    }
                }
            }
        }

        GridLayout {
            id: workspaceLayout
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.minimumHeight: root.compactLayout
                ? 595 : 420
            Layout.preferredHeight: root.compactLayout
                ? Math.max(595, Math.max(350, Math.min(510, root.height * 0.62)) + 245)
                : Math.max(420, root.height - 285)
            columns: root.compactLayout ? 1 : 2
            columnSpacing: 10
            rowSpacing: 10

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: !root.compactLayout
                Layout.minimumHeight: root.compactLayout ? 350 : 0
                Layout.preferredHeight: root.compactLayout
                    ? Math.max(350, Math.min(510, root.height * 0.62)) : -1
                color: root.panel
                border.color: root.line
                radius: 6
                clip: true
                ScenarioEditorView {
                    anchors.fill: parent
                    anchors.margins: 1
                    controller: root.controller
                    editor: root.editor
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: !root.compactLayout
                Layout.preferredWidth: root.compactLayout ? -1 : 288
                Layout.minimumWidth: root.compactLayout ? 0 : 260
                Layout.preferredHeight: root.compactLayout ? 235 : -1
                color: root.panel
                border.color: root.line
                radius: 6

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 13
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Icon { name: "settings"; iconColor: root.cyan; iconSize: 16 }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text { text: "房间状态"; color: root.ink; font.pixelSize: 14; font.bold: true }
                            Text { text: "管理员控制台"; color: root.dim; font.pixelSize: 9 }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.controller.roomDescription || "暂无房间说明"
                        color: root.dim
                        font.pixelSize: 10
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        wrapMode: Text.WordWrap
                    }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.line }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 8
                        rowSpacing: 5
                        Text { text: "房间模式"; color: root.dim; font.pixelSize: 9 }
                        Text { text: root.controller.roomMode === "pve" ? "PVE 电脑对抗" : "PVP 对抗"; color: root.ink; font.pixelSize: 10; horizontalAlignment: Text.AlignRight; Layout.fillWidth: true }
                        Text { text: "场景标识"; color: root.dim; font.pixelSize: 9 }
                        Text { text: root.controller.scenarioId || "default"; color: root.cyan; font.pixelSize: 10; font.family: "Consolas"; horizontalAlignment: Text.AlignRight; Layout.fillWidth: true; elide: Text.ElideRight }
                    }

                    SectionTitle { text: "双方战位" }
                    Repeater {
                        model: ["red", "blue"]
                        delegate: Rectangle {
                            id: sideSummary
                            required property string modelData
                            Layout.fillWidth: true
                            Layout.preferredHeight: 39
                            color: root.page
                            border.color: sideSummary.modelData === "red" ? root.danger : root.cyan
                            radius: 4
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 9
                                anchors.rightMargin: 9
                                spacing: 8
                                Rectangle {
                                    Layout.preferredWidth: 6
                                    Layout.preferredHeight: 20
                                    radius: 3
                                    color: sideSummary.modelData === "red" ? root.danger : root.cyan
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text { text: sideSummary.modelData === "red" ? "红方" : "蓝方"; color: root.ink; font.pixelSize: 10; font.bold: true }
                                    Text { text: root.sideOccupied(sideSummary.modelData) + " / " + root.sideCapacity(sideSummary.modelData) + " 个战位"; color: root.dim; font.pixelSize: 9 }
                                }
                                Text {
                                    text: root.sideOccupied(sideSummary.modelData) > 0 ? "有占用" : "空闲"
                                    color: root.sideOccupied(sideSummary.modelData) > 0 ? root.cyan : root.dim
                                    font.pixelSize: 9
                                }
                            }
                        }
                    }

                    Item { Layout.fillHeight: true; Layout.minimumHeight: 2 }

                    Flow {
                        Layout.fillWidth: true
                        spacing: 7
                        TonalButton {
                            text: "编辑配置"
                            iconName: "settings"
                            base: root.cyan
                            enabled: root.controller.matchPhase === "preparing" && !root.configSaving
                            onClicked: root.openConfig()
                        }
                        GhostButton {
                            text: "重载"
                            iconName: "refresh"
                            enabled: !root.configSaving
                            onClicked: root.reloadConfig()
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.statusText || (root.dirty ? "有未保存的修改" : "配置与服务器同步")
                        color: root.statusKind === "error" ? root.danger
                            : root.statusKind === "success" ? root.success
                            : root.statusKind === "dirty" ? root.orange : root.dim
                        font.pixelSize: 9
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                    }
                }
            }
        }
        }
    }

    Dialog {
        id: configDialog
        title: "房间配置"
        modal: true
        anchors.centerIn: parent
        width: Math.min(760, Math.max(280, root.width - 16))
        height: Math.min(720, Math.max(300, root.height - 16))
        standardButtons: Dialog.NoButton
        background: Rectangle {
            color: root.panel
            border.color: root.cyan
            border.width: 1
            radius: 8
        }

        contentItem: ScrollView {
            id: configScroll
            clip: true
            contentWidth: availableWidth
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            ColumnLayout {
                id: configColumn
                width: configScroll.availableWidth
                spacing: 12

                Text {
                    Layout.fillWidth: true
                    text: "准备阶段修改会立即同步给房间内所有客户端。配置版本用于防止并发覆盖。"
                    color: root.dim
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: root.narrowLayout ? 1 : 2
                    columnSpacing: 10
                    rowSpacing: 8

                    ColumnLayout {
                        Layout.fillWidth: true
                        Text { text: "房间名称"; color: root.dim; font.pixelSize: 9 }
                        TextField {
                            id: roomNameField
                            Layout.fillWidth: true
                            implicitHeight: 32
                            placeholderText: "输入房间名称"
                            maximumLength: 96
                            selectByMouse: true
                            color: root.ink
                            placeholderTextColor: root.dim
                            background: Rectangle { color: root.panelAlt; border.color: roomNameField.activeFocus ? root.cyan : root.line; radius: 4 }
                            onTextChanged: if (!root.loadingDraft) { root.dirty = true; root.statusKind = "dirty" }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Text { text: "场景标识"; color: root.dim; font.pixelSize: 9 }
                        TextField {
                            id: scenarioIdField
                            Layout.fillWidth: true
                            implicitHeight: 32
                            placeholderText: "default"
                            maximumLength: 128
                            selectByMouse: true
                            color: root.ink
                            placeholderTextColor: root.dim
                            background: Rectangle { color: root.panelAlt; border.color: scenarioIdField.activeFocus ? root.cyan : root.line; radius: 4 }
                            onTextChanged: if (!root.loadingDraft) { root.dirty = true; root.statusKind = "dirty" }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.columnSpan: root.narrowLayout ? 1 : 2
                        Text { text: "房间说明"; color: root.dim; font.pixelSize: 9 }
                        TextArea {
                            id: roomDescriptionField
                            Layout.fillWidth: true
                            Layout.preferredHeight: 66
                            placeholderText: "补充参演规则或任务说明"
                            wrapMode: TextArea.Wrap
                            selectByMouse: true
                            color: root.ink
                            placeholderTextColor: root.dim
                            background: Rectangle { color: root.panelAlt; border.color: roomDescriptionField.activeFocus ? root.cyan : root.line; radius: 4 }
                            onTextChanged: {
                                if (text.length > 512) text = text.slice(0, 512)
                                if (!root.loadingDraft) { root.dirty = true; root.statusKind = "dirty" }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 42
                    color: root.occupiedSeatCount() > 0 ? "#2b2418" : root.panelAlt
                    border.color: root.occupiedSeatCount() > 0 ? root.orange : root.line
                    radius: 4
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 9
                        spacing: 8
                        Icon { name: "info"; iconColor: root.occupiedSeatCount() > 0 ? root.orange : root.cyan; iconSize: 14 }
                        Text {
                            Layout.fillWidth: true
                            text: root.occupiedSeatCount() > 0
                                ? "当前有战位占用，调整容量或参数可能使战位重新校验。"
                                : "指挥官战位固定为每方 1 个；留空的通信和侦察参数沿用场景默认值。"
                            color: root.dim
                            font.pixelSize: 9
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                SectionTitle { text: "战位容量" }
                GridLayout {
                    Layout.fillWidth: true
                    columns: root.narrowLayout ? 1 : 2
                    columnSpacing: 8
                    rowSpacing: 7
                    Repeater {
                        model: root.seatKeys
                        delegate: Rectangle {
                            id: limitCard
                            required property string modelData
                            Layout.fillWidth: true
                            Layout.preferredHeight: 45
                            color: root.panelAlt
                            border.color: root.line
                            radius: 4
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 9
                                anchors.rightMargin: 8
                                spacing: 8
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text { text: root.seatTitle(limitCard.modelData); color: root.ink; font.pixelSize: 10; elide: Text.ElideRight }
                                    Text { text: root.occupiedFor(limitCard.modelData) + " 个已占用 · " + root.seatStateLabel(limitCard.modelData); color: root.seatStateColor(limitCard.modelData); font.pixelSize: 8; elide: Text.ElideRight }
                                }
                                SpinBox {
                                    id: seatLimitBox
                                    from: 0
                                    to: 64
                                    stepSize: 1
                                    editable: true
                                    enabled: limitCard.modelData.indexOf("commander") < 0
                                        || root.limitFor(limitCard.modelData) === 1
                                    implicitWidth: 76
                                    implicitHeight: 30
                                    onValueChanged: if (!root.loadingDraft) root.setLimit(limitCard.modelData, value)
                                    Binding {
                                        target: seatLimitBox
                                        property: "value"
                                        value: root.limitFor(limitCard.modelData)
                                        when: !seatLimitBox.activeFocus
                                    }
                                }
                            }
                        }
                    }
                }

                SectionTitle { text: "通信与侦察覆盖" }
                Text {
                    Layout.fillWidth: true
                    text: "单位：米。可以按具体战位覆盖场景默认值；空白表示不覆盖。"
                    color: root.dim
                    font.pixelSize: 9
                    wrapMode: Text.WordWrap
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: root.narrowLayout ? 1 : 2
                    columnSpacing: 8
                    rowSpacing: 7
                    Repeater {
                        model: root.seatKeys
                        delegate: Rectangle {
                            id: parameterCard
                            required property string modelData
                            Layout.fillWidth: true
                            Layout.preferredHeight: root.tinyLayout ? 112 : 76
                            color: root.panelAlt
                            border.color: root.line
                            radius: 4
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 5
                                Text { text: root.seatTitle(parameterCard.modelData); color: root.ink; font.pixelSize: 10; font.bold: true }
                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: root.tinyLayout ? 1 : 2
                                    columnSpacing: 7
                                    rowSpacing: 4
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Text { text: "通信范围"; color: root.dim; font.pixelSize: 8 }
                                        IntelTextField {
                                            id: communicationField
                                            Layout.fillWidth: true
                                            controlHeight: 29
                                            placeholderText: "默认"
                                            validator: DoubleValidator { bottom: 0; top: 1000000; decimals: 2 }
                                            onEditingFinished: root.setParameter(parameterCard.modelData, "communicationRange", text)
                                            Binding {
                                                target: communicationField
                                                property: "text"
                                                value: root.parameterText(parameterCard.modelData, "communicationRange")
                                                when: !communicationField.activeFocus
                                            }
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Text { text: "侦察范围"; color: root.dim; font.pixelSize: 8 }
                                        IntelTextField {
                                            id: detectField
                                            Layout.fillWidth: true
                                            controlHeight: 29
                                            placeholderText: "默认"
                                            validator: DoubleValidator { bottom: 0; top: 1000000; decimals: 2 }
                                            onEditingFinished: root.setParameter(parameterCard.modelData, "detectRange", text)
                                            Binding {
                                                target: detectField
                                                property: "text"
                                                value: root.parameterText(parameterCard.modelData, "detectRange")
                                                when: !detectField.activeFocus
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: root.statusText
                    visible: text.length > 0
                    color: root.statusKind === "error" ? root.danger : root.statusKind === "success" ? root.success : root.dim
                    font.pixelSize: 9
                    wrapMode: Text.WordWrap
                }
            }
        }

        footer: DialogButtonBox {
            spacing: 7
            padding: 9
            background: Rectangle { color: root.panelAlt; radius: 6 }
            GhostButton {
                text: "重载"
                iconName: "refresh"
                enabled: !root.configSaving
                onClicked: root.reloadConfig()
            }
            GhostButton {
                text: "取消"
                iconName: "close"
                enabled: !root.configSaving
                onClicked: configDialog.close()
            }
            TonalButton {
                text: root.configSaving ? "保存中..." : "保存配置"
                iconName: root.configSaving ? "refresh" : "check"
                base: root.cyan
                enabled: !root.configSaving && root.controller.matchPhase === "preparing"
                onClicked: root.saveConfig()
            }
        }
    }

    Dialog {
        id: leaveRoomDialog
        title: "放弃未保存修改？"
        modal: true
        anchors.centerIn: parent
        width: Math.min(420, Math.max(280, root.width - 16))
        standardButtons: Dialog.NoButton
        background: Rectangle { color: root.panel; border.color: root.orange; radius: 7 }
        contentItem: ColumnLayout {
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: "房间配置中有未保存的修改。退出后这些修改会丢失。"
                color: root.ink
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
            Text { text: "服务器上的配置不会受到影响。"; color: root.dim; font.pixelSize: 9 }
        }
        footer: DialogButtonBox {
            spacing: 7
            padding: 9
            background: Rectangle { color: root.panelAlt; radius: 6 }
            GhostButton { text: "留在房间"; onClicked: leaveRoomDialog.close() }
            TonalButton {
                text: "放弃并退出"
                base: root.orange
                onClicked: {
                    leaveRoomDialog.close()
                    root.dirty = false
                    root.controller.leaveOnlineRoom()
                }
            }
        }
    }
}
