pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: dlg

    property var controller: null
    property var snap: ({})
    property bool interactionEnabled: true
    readonly property bool narrow: dlg.width < 620
    readonly property color sideAccent: dlg.snap.side === "red" ? AppContext.red
        : dlg.snap.side === "blue" ? AppContext.blue : AppContext.signal
    readonly property color healthAccent: dlg.healthRatio > 0.55 ? AppContext.success
        : dlg.healthRatio > 0.25 ? AppContext.warning : AppContext.danger
    readonly property real healthRatio: Math.max(0, Math.min(1,
        Number(dlg.snap.hp || 0) / Math.max(1, Number(dlg.snap.maxHp || 1))))

    function actionEntry(action) {
        var actions = dlg.snap.actions || dlg.snap.actionCapabilities || ({})
        return actions[action]
    }

    function actionVisible(action, localFallback) {
        if (!dlg.interactionEnabled || !dlg.snap.alive) return false
        var entry = dlg.actionEntry(action)
        if (entry !== undefined) {
            if (typeof entry === "object" && entry !== null)
                return entry.visible !== false
            return Boolean(entry)
        }
        return dlg.controller && !dlg.controller.networked && Boolean(localFallback)
    }

    function actionAllowed(action, localFallback) {
        if (!dlg.interactionEnabled || !dlg.snap.alive) return false
        var entry = dlg.actionEntry(action)
        if (entry !== undefined) {
            if (typeof entry === "object" && entry !== null) {
                if (entry.enabled !== undefined) return Boolean(entry.enabled)
                if (entry.allowed !== undefined) return Boolean(entry.allowed)
                return entry.visible !== false
            }
            return Boolean(entry)
        }
        return dlg.controller && !dlg.controller.networked && Boolean(localFallback)
    }

    function ability(name) {
        var abilities = dlg.snap.abilities || ({})
        return abilities[name] || ({})
    }

    function kindLabel(kind) {
        var labels = {
            commandpost: "指挥所",
            attackuav: "攻击无人机",
            reconuav: "侦察无人机",
            jammeruav: "干扰无人机",
            groundscout: "地面侦察",
            groundtarget: "地面静态目标"
        }
        return labels[kind] || kind || "未知单元"
    }

    function kindIcon(kind) {
        var icons = {
            commandpost: "command",
            attackuav: "missile",
            reconuav: "scan",
            jammeruav: "countermeasure",
            groundscout: "unit",
            groundtarget: "locate"
        }
        return icons[kind] || "unit"
    }

    function statusLabel() {
        if (!dlg.snap.alive) return "已摧毁"
        if (dlg.snap.serviceRequested) return "指挥所补给中"
        if (dlg.snap.disabled) return "系统失效"
        return dlg.snap.status || "在线"
    }

    function cooldownRatio(data) {
        var total = Number(data.cooldownSec || 0)
        if (total <= 0) return 1
        return Math.max(0, Math.min(1,
            1 - Number(data.cooldownRemaining || 0) / total))
    }

    function abilityRows() {
        var rows = []
        var countermeasure = dlg.ability("countermeasure")
        if (Number(countermeasure.range || 0) > 0) {
            rows.push({
                name: "干扰弹", icon: "countermeasure",
                action: "activateCountermeasure", data: countermeasure
            })
        }
        var scan = dlg.ability("scan")
        if (dlg.snap.kind === "reconuav" && Number(scan.range || 0) > 0) {
            rows.push({
                name: "主动扫描", icon: "scan",
                action: "activateScan", data: scan
            })
        }
        var repair = dlg.ability("fieldRepair")
        if (repair.cooldownRemaining !== undefined) {
            rows.push({
                name: "战场修理", icon: "repair",
                action: "attemptFieldRepair", data: repair
            })
        }
        return rows
    }

    modal: true
    closePolicy: Popup.CloseOnEscape
    standardButtons: Dialog.NoButton
    width: Math.max(320, Math.min(760, (parent ? parent.width : 784) - 24))
    height: Math.max(460, Math.min(680, (parent ? parent.height : 704) - 24))
    anchors.centerIn: parent
    padding: 0

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: AppContext.stateMotion; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: AppContext.stateMotion; easing.type: Easing.OutBack }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: AppContext.fastMotion; easing.type: Easing.InCubic }
            NumberAnimation { property: "scale"; from: 1; to: 0.98; duration: AppContext.fastMotion; easing.type: Easing.InCubic }
        }
    }
    Overlay.modal: Rectangle { color: "#05080dcc" }

    background: Rectangle {
        color: AppContext.panel
        border.color: AppContext.line
        radius: dlg.narrow ? 0 : AppContext.radius
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            color: "transparent"
            border.color: Qt.rgba(dlg.sideAccent.r, dlg.sideAccent.g, dlg.sideAccent.b, 0.28)
            radius: Math.max(0, (dlg.narrow ? 0 : AppContext.radius) - 1)
        }
    }

    header: Rectangle {
        implicitHeight: 78
        color: AppContext.raised
        border.color: AppContext.line

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 4
            color: dlg.sideAccent
            opacity: dlg.snap.alive ? 1 : 0.45
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 12
            spacing: 12

            Rectangle {
                Layout.preferredWidth: 46
                Layout.preferredHeight: 46
                radius: 8
                color: Qt.rgba(dlg.sideAccent.r, dlg.sideAccent.g, dlg.sideAccent.b, 0.13)
                border.color: Qt.rgba(dlg.sideAccent.r, dlg.sideAccent.g, dlg.sideAccent.b, 0.65)
                Icon {
                    anchors.centerIn: parent
                    name: dlg.kindIcon(dlg.snap.kind)
                    iconColor: dlg.sideAccent
                    iconSize: 25
                }
                Rectangle {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: 10
                    height: 10
                    radius: 5
                    color: dlg.snap.alive ? AppContext.success : AppContext.danger
                    border.color: AppContext.raised
                    border.width: 2
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3
                Text {
                    Layout.fillWidth: true
                    text: dlg.snap.callsign || dlg.snap.id || "单位详情"
                    color: AppContext.textStrong
                    font.pixelSize: 17
                    font.bold: true
                    elide: Text.ElideRight
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Text {
                        text: dlg.kindLabel(dlg.snap.kind) + "  ·  "
                              + (dlg.snap.side === "red" ? "红方" : dlg.snap.side === "blue" ? "蓝方" : "中立")
                        color: AppContext.textDim
                        font.pixelSize: 10
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.preferredWidth: headerStatusText.implicitWidth + 18
                        Layout.preferredHeight: 22
                        radius: 11
                        color: Qt.rgba(dlg.healthAccent.r, dlg.healthAccent.g, dlg.healthAccent.b, 0.14)
                        border.color: Qt.rgba(dlg.healthAccent.r, dlg.healthAccent.g, dlg.healthAccent.b, 0.62)
                        Text {
                            id: headerStatusText
                            anchors.centerIn: parent
                            text: dlg.statusLabel()
                            color: dlg.healthAccent
                            font.pixelSize: 10
                            font.bold: true
                            elide: Text.ElideRight
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 3
                    radius: 2
                    color: AppContext.page
                    Rectangle {
                        width: parent.width * dlg.healthRatio
                        height: parent.height
                        radius: 2
                        color: dlg.healthAccent
                        Behavior on width { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
                    }
                }
            }
            GhostButton {
                text: ""
                iconName: "close"
                iconSize: 19
                implicitWidth: 40
                implicitHeight: 38
                onClicked: dlg.close()
                ToolTip.visible: hovered
                ToolTip.text: "关闭"
                Accessible.name: ToolTip.text
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: dlg.narrow ? 92 : 68
            color: AppContext.page
            border.color: AppContext.line

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                anchors.topMargin: dlg.narrow ? 12 : 14
                anchors.bottomMargin: dlg.narrow ? 12 : 14
                spacing: dlg.narrow ? 7 : 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: !dlg.narrow
                    Layout.preferredHeight: dlg.narrow ? 34 : -1
                    spacing: 14

                    ColumnLayout {
                        Layout.preferredWidth: 96
                        spacing: 2
                        Text { text: "生命状态"; color: AppContext.muted; font.pixelSize: 9 }
                        Text {
                            text: Math.round(Number(dlg.snap.hp || 0)) + " / " + Math.round(Number(dlg.snap.maxHp || 0))
                            color: dlg.healthAccent
                            font.pixelSize: 16
                            font.bold: true
                            font.family: "Consolas"
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 12
                        radius: 6
                        color: AppContext.raised
                        border.color: AppContext.line
                        Rectangle {
                            width: parent.width * dlg.healthRatio
                            height: parent.height
                            radius: 6
                            color: dlg.healthAccent
                            opacity: 0.9
                            Behavior on width { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
                        }
                    }
                    ColumnLayout {
                        visible: !dlg.narrow
                        Layout.preferredWidth: 82
                        spacing: 2
                        Text { text: "当前速度"; color: AppContext.muted; font.pixelSize: 9 }
                        Text {
                            text: dlg.snap.speed !== undefined ? Math.round(Number(dlg.snap.speed)) + " m/s" : "-"
                            color: AppContext.textStrong
                            font.pixelSize: 13
                            font.bold: true
                            font.family: "Consolas"
                        }
                    }
                    ColumnLayout {
                        visible: !dlg.narrow
                        Layout.preferredWidth: 76
                        spacing: 2
                        Text { text: "数据 revision"; color: AppContext.muted; font.pixelSize: 9 }
                        Text {
                            text: dlg.controller && dlg.controller.networked ? String(dlg.controller.unitStateRevision) : "LOCAL"
                            color: AppContext.signal
                            font.pixelSize: 11
                            font.bold: true
                            font.family: "Consolas"
                        }
                    }
                }

                RowLayout {
                    visible: dlg.narrow
                    Layout.fillWidth: true
                    Layout.preferredHeight: 27
                    spacing: 14

                    Item { Layout.fillWidth: true }
                    ColumnLayout {
                        Layout.preferredWidth: 82
                        spacing: 1
                        Text { text: "当前速度"; color: AppContext.muted; font.pixelSize: 9 }
                        Text {
                            text: dlg.snap.speed !== undefined ? Math.round(Number(dlg.snap.speed)) + " m/s" : "-"
                            color: AppContext.textStrong
                            font.pixelSize: 12
                            font.bold: true
                            font.family: "Consolas"
                        }
                    }
                    ColumnLayout {
                        Layout.preferredWidth: 76
                        spacing: 1
                        Text { text: "数据 revision"; color: AppContext.muted; font.pixelSize: 9 }
                        Text {
                            text: dlg.controller && dlg.controller.networked ? String(dlg.controller.unitStateRevision) : "LOCAL"
                            color: AppContext.signal
                            font.pixelSize: 10
                            font.bold: true
                            font.family: "Consolas"
                        }
                    }
                }
            }
        }

        Item {
            id: detailTabs
            Layout.fillWidth: true
            Layout.preferredHeight: 42
            property int currentIndex: 0
            clip: true

            Rectangle { anchors.fill: parent; color: AppContext.page }
            RowLayout {
                anchors.fill: parent
                anchors.margins: 2
                spacing: 2

                Repeater {
                    model: [
                        { label: "态势", icon: "locate" },
                        { label: "系统", icon: "settings" },
                        { label: "技能", icon: "countermeasure" },
                        { label: "交战", icon: "missile" },
                        { label: "链路", icon: "network" }
                    ]
                    delegate: Button {
                        id: detailTab
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumWidth: 0
                        padding: 0
                        leftPadding: 0
                        rightPadding: 0
                        topPadding: 0
                        bottomPadding: 0
                        property bool active: detailTabs.currentIndex === detailTab.index
                        Accessible.name: detailTab.modelData.label + "视图"
                        onClicked: detailTabs.currentIndex = detailTab.index

                        contentItem: Item {
                            anchors.fill: parent
                            Row {
                                id: detailTabContent
                                anchors.centerIn: parent
                                spacing: 4
                                Icon {
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: detailTab.modelData.icon
                                    iconSize: 14
                                    iconColor: detailTab.active ? AppContext.signal : AppContext.muted
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: detailTab.modelData.label
                                    color: detailTab.active ? AppContext.signal : AppContext.muted
                                    font.pixelSize: 10
                                    font.bold: detailTab.active
                                }
                            }
                        }
                        background: Rectangle {
                            color: detailTab.active ? AppContext.raised
                                                      : (detailTab.hovered ? AppContext.panel : "transparent")
                            radius: 3
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 2
                                color: detailTab.active ? AppContext.signal : "transparent"
                            }
                        }
                    }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: detailTabs.currentIndex

            ScrollView {
                id: overviewPage
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    x: 16
                    width: Math.max(0, overviewPage.availableWidth - 32)
                    spacing: 12

                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: 14
                        text: dlg.snap.status || "-"
                        color: AppContext.textStrong
                        font.pixelSize: 13
                        font.bold: true
                        wrapMode: Text.WordWrap
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: AppContext.line }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: dlg.narrow ? 1 : 2
                        columnSpacing: 24
                        rowSpacing: 10
                        Repeater {
                            model: [
                                { label: "位置", value: dlg.snap.position ? Math.round(dlg.snap.position[0]) + ", " + Math.round(dlg.snap.position[1]) + " m" : "-" },
                                { label: "速度", value: dlg.snap.speed !== undefined ? Math.round(dlg.snap.speed) + " m/s" : "-" },
                                { label: "探测半径", value: dlg.snap.detectRange !== undefined ? Math.round(dlg.snap.detectRange) + " m" : "-" },
                                { label: "通信半径", value: dlg.snap.commRange !== undefined ? Math.round(dlg.snap.commRange) + " m" : "-" },
                                { label: "攻击半径", value: dlg.snap.attackRange !== undefined ? Math.round(dlg.snap.attackRange) + " m" : "-" },
                                { label: "装甲", value: dlg.snap.armor !== undefined ? Math.round(Number(dlg.snap.armor) * 100) + "%" : "-" },
                                { label: "燃油", value: dlg.snap.fuelRemaining !== undefined ? Math.round(dlg.snap.fuelRemaining) + " / " + Math.round(dlg.snap.fuelCapacity) + " s" : "-" },
                                { label: "预计续航", value: Number(dlg.snap.estimatedEnduranceSec) >= 0 ? Math.round(dlg.snap.estimatedEnduranceSec) + " s" : "-" },
                                { label: "单位 ID", value: dlg.snap.id || "-" }
                            ]
                            delegate: RowLayout {
                                id: overviewMetric
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 10
                                Text { text: overviewMetric.modelData.label; color: AppContext.muted; font.pixelSize: 10; Layout.preferredWidth: 72 }
                                Text { Layout.fillWidth: true; text: overviewMetric.modelData.value; color: AppContext.text; font.pixelSize: 11; font.family: "Consolas"; elide: Text.ElideRight }
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            ScrollView {
                id: systemsPage
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: systemsPage.availableWidth
                    spacing: 14

                    Text { Layout.topMargin: 14; Layout.leftMargin: 16; text: "舰体与部位健康"; color: AppContext.textStrong; font.pixelSize: 13; font.bold: true }
                    Repeater {
                        model: [
                            { label: "舰体", value: Number(dlg.snap.hp || 0) / Math.max(1, Number(dlg.snap.maxHp || 1)) },
                            { label: "传感器", value: dlg.snap.subsystems ? Number(dlg.snap.subsystems.sensor) : 0 },
                            { label: "通信", value: dlg.snap.subsystems ? Number(dlg.snap.subsystems.comms) : 0 },
                            { label: "机动", value: dlg.snap.subsystems ? Number(dlg.snap.subsystems.mobility) : 0 },
                            { label: "武器", value: dlg.snap.subsystems ? Number(dlg.snap.subsystems.weapon) : 0 }
                        ]
                        delegate: RowLayout {
                            id: systemHealth
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            spacing: 12
                            property real ratio: Math.max(0, Math.min(1, Number(systemHealth.modelData.value || 0)))
                            Text { text: systemHealth.modelData.label; color: AppContext.muted; font.pixelSize: 11; Layout.preferredWidth: 58 }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 9
                                radius: 4
                                color: AppContext.raised
                                Rectangle {
                                    width: parent.width * systemHealth.ratio
                                    height: parent.height
                                    radius: 4
                                    color: systemHealth.ratio > 0.6 ? AppContext.success : systemHealth.ratio > 0.3 ? AppContext.warning : AppContext.danger
                                }
                            }
                            Text { text: Math.round(systemHealth.ratio * 100) + "%"; color: AppContext.text; font.pixelSize: 10; font.family: "Consolas"; Layout.preferredWidth: 40; horizontalAlignment: Text.AlignRight }
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            ScrollView {
                id: abilitiesPage
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: abilitiesPage.availableWidth
                    spacing: 10

                    Repeater {
                        model: dlg.abilityRows()
                        delegate: Rectangle {
                            id: abilityDelegate
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: 14
                            Layout.rightMargin: 14
                            Layout.topMargin: abilityDelegate.index === 0 ? 14 : 0
                            Layout.preferredHeight: 106
                            property real readyRatio: dlg.cooldownRatio(abilityDelegate.modelData.data)
                            property color abilityColor: abilityDelegate.modelData.data.available === true
                                ? AppContext.success : AppContext.warning
                            color: AppContext.page
                            border.color: abilityDelegate.modelData.data.available === true
                                ? Qt.rgba(AppContext.success.r, AppContext.success.g, AppContext.success.b, 0.52)
                                : AppContext.line
                            radius: 6
                            Behavior on border.color { ColorAnimation { duration: 180 } }
                            required property int index

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 11
                                spacing: 12
                                Rectangle {
                                    Layout.preferredWidth: 48
                                    Layout.preferredHeight: 48
                                    radius: 9
                                    color: Qt.rgba(abilityDelegate.abilityColor.r,
                                                   abilityDelegate.abilityColor.g,
                                                   abilityDelegate.abilityColor.b, 0.12)
                                    border.color: Qt.rgba(abilityDelegate.abilityColor.r,
                                                         abilityDelegate.abilityColor.g,
                                                         abilityDelegate.abilityColor.b, 0.5)
                                    Icon {
                                        anchors.centerIn: parent
                                        name: abilityDelegate.modelData.icon
                                        iconColor: abilityDelegate.abilityColor
                                        iconSize: 25
                                    }
                                    Rectangle {
                                        visible: abilityDelegate.modelData.data.available === true
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        width: 10
                                        height: 10
                                        radius: 5
                                        color: AppContext.success
                                        border.color: AppContext.page
                                        border.width: 2
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 4
                                    Text { text: abilityDelegate.modelData.name; color: AppContext.textStrong; font.pixelSize: 13; font.bold: true }
                                    Text {
                                        Layout.fillWidth: true
                                        text: (abilityDelegate.modelData.data.range !== undefined ? "范围 " + Math.round(abilityDelegate.modelData.data.range) + " m  ·  " : "")
                                              + "冷却 " + Number(abilityDelegate.modelData.data.cooldownRemaining || 0).toFixed(0)
                                              + " / " + Number(abilityDelegate.modelData.data.cooldownSec || 0).toFixed(0) + " s"
                                        color: AppContext.textDim; font.pixelSize: 10; elide: Text.ElideRight
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 5
                                        radius: 3
                                        color: AppContext.raised
                                        Rectangle {
                                            width: parent.width * abilityDelegate.readyRatio
                                            height: parent.height
                                            radius: 3
                                            color: abilityDelegate.abilityColor
                                            Behavior on width { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }
                                        }
                                    }
                                    Text {
                                        visible: abilityDelegate.modelData.data.remaining !== undefined
                                        text: Number(abilityDelegate.modelData.data.remaining) < 0 ? "次数 无限" : "剩余 " + Number(abilityDelegate.modelData.data.remaining) + " / " + Number(abilityDelegate.modelData.data.capacity)
                                        color: AppContext.text; font.pixelSize: 10
                                    }
                                }
                                AbilityActionButton {
                                    Layout.preferredWidth: 52
                                    Layout.preferredHeight: 46
                                    iconName: abilityDelegate.modelData.icon
                                    actionLabel: abilityDelegate.modelData.name
                                    abilityData: abilityDelegate.modelData.data
                                    actionVisible: dlg.actionVisible(abilityDelegate.modelData.action, true)
                                    actionAllowed: dlg.actionAllowed(abilityDelegate.modelData.action,
                                                                      abilityDelegate.modelData.data.available === true)
                                    onClicked: dlg.controller.command(abilityDelegate.modelData.action,
                                                                      { unitId: dlg.snap.id })
                                }
                            }
                        }
                    }
                    Text {
                        visible: dlg.abilityRows().length === 0
                        Layout.margins: 16
                        text: "当前投影中没有可用技能信息"
                        color: AppContext.muted
                        font.pixelSize: 11
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            ScrollView {
                id: combatPage
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: combatPage.availableWidth
                    spacing: 12

                    Text { Layout.topMargin: 14; Layout.leftMargin: 16; text: "目标与交战规则"; color: AppContext.textStrong; font.pixelSize: 13; font.bold: true }
                    Text {
                        Layout.leftMargin: 16; Layout.rightMargin: 16; Layout.fillWidth: true
                        text: dlg.snap.kind === "attackuav" ? (dlg.snap.targetId ? "当前目标 · " + dlg.snap.targetId : "当前未指定目标") : "该单位无攻击任务"
                        color: AppContext.muted; font.pixelSize: 11; elide: Text.ElideMiddle
                    }
                    ComboBox {
                        id: detailTargetBox
                        visible: dlg.snap.kind === "attackuav" && dlg.interactionEnabled
                        Layout.fillWidth: true
                        Layout.leftMargin: 16; Layout.rightMargin: 16
                        model: dlg.controller && dlg.snap.id
                            ? dlg.controller.detectedEnemyOptions(dlg.snap.id, dlg.snap.side,
                                dlg.snap.side === "red" ? "blue" : "red") : []
                        textRole: "callsign"
                        valueRole: "id"
                        enabled: count > 0
                        background: Rectangle { color: AppContext.page; border.color: AppContext.line; radius: 4 }
                        contentItem: Text { text: detailTargetBox.currentText || "选择已掌握目标"; color: AppContext.text; verticalAlignment: Text.AlignVCenter; leftPadding: 8; elide: Text.ElideRight }
                    }
                    TonalButton {
                        visible: dlg.actionVisible("engageTarget", dlg.snap.kind === "attackuav")
                        Layout.fillWidth: true
                        Layout.leftMargin: 16; Layout.rightMargin: 16
                        text: "发射导弹 · 在途 "
                              + Number(dlg.snap.activeProjectileCount || 0) + " 枚"
                        iconName: "missile"
                        enabled: detailTargetBox.count > 0
                                 && dlg.actionAllowed("engageTarget",
                                    Number(dlg.snap.ammoRemaining || 0) > 0
                                    && Number(dlg.snap.cooldownRemaining || 0) <= 0
                                    && !dlg.snap.serviceRequested)
                        onClicked: dlg.controller.command("engageTarget", {
                            attackerId: dlg.snap.id,
                            targetId: detailTargetBox.currentValue
                        })
                    }
                    RowLayout {
                        visible: dlg.snap.kind === "attackuav"
                        Layout.fillWidth: true
                        Layout.leftMargin: 16; Layout.rightMargin: 16
                        spacing: 8
                        TonalButton {
                            Layout.fillWidth: true
                            text: "自由交战"
                            base: dlg.snap.rulesOfEngagement === "free" ? AppContext.success : AppContext.raised
                            visible: dlg.actionVisible("setRoe", true)
                            enabled: dlg.actionAllowed("setRoe", true)
                            onClicked: dlg.controller.command("setRoe", { unitId: dlg.snap.id, roe: "free" })
                        }
                        TonalButton {
                            Layout.fillWidth: true
                            text: "武器管制"
                            base: dlg.snap.rulesOfEngagement === "hold" ? AppContext.warning : AppContext.raised
                            visible: dlg.actionVisible("setRoe", true)
                            enabled: dlg.actionAllowed("setRoe", true)
                            onClicked: dlg.controller.command("setRoe", { unitId: dlg.snap.id, roe: "hold" })
                        }
                    }
                    GhostButton {
                        visible: dlg.actionVisible("cancelEngagement", dlg.snap.kind === "attackuav")
                        Layout.fillWidth: true
                        Layout.leftMargin: 16; Layout.rightMargin: 16
                        text: "取消交战"
                        iconName: "close"
                        enabled: dlg.actionAllowed("cancelEngagement", Boolean(dlg.snap.targetId))
                        onClicked: dlg.controller.command("cancelEngagement", { unitId: dlg.snap.id })
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            ScrollView {
                id: linkPage
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: linkPage.availableWidth
                    spacing: 10

                    Text { Layout.topMargin: 14; Layout.leftMargin: 16; text: "通信与标点"; color: AppContext.textStrong; font.pixelSize: 13; font.bold: true }
                    Repeater {
                        model: [
                            { label: "链路状态", value: dlg.controller && dlg.controller.networked ? (dlg.controller.communicationState || "disconnected") : "local" },
                            { label: "通信半径", value: Math.round(Number(dlg.snap.commRange || 0)) + " m" },
                            { label: "当前探测", value: String((dlg.snap.detections || []).length) },
                            { label: "共享情报", value: String(Object.keys(dlg.snap.sharedKnowledge || ({})).length) }
                        ]
                        delegate: RowLayout {
                            id: linkMetric
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: 16; Layout.rightMargin: 16
                            Text { text: linkMetric.modelData.label; color: AppContext.muted; font.pixelSize: 10; Layout.preferredWidth: 78 }
                            Text { Layout.fillWidth: true; text: linkMetric.modelData.value; color: AppContext.text; font.pixelSize: 11; font.family: "Consolas"; elide: Text.ElideRight }
                        }
                    }
                    Rectangle { Layout.fillWidth: true; Layout.leftMargin: 16; Layout.rightMargin: 16; Layout.preferredHeight: 1; color: AppContext.line }
                    Text { Layout.leftMargin: 16; text: "已投影地图标点"; color: AppContext.muted; font.pixelSize: 10 }
                    Repeater {
                        model: dlg.controller && dlg.controller.networked ? (dlg.controller.onlineMapMarks || []) : []
                        delegate: RowLayout {
                            id: markDelegate
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: 16; Layout.rightMargin: 16
                            spacing: 8
                            Icon { name: "locate"; iconColor: AppContext.signal; iconSize: 12 }
                            Text { Layout.fillWidth: true; text: markDelegate.modelData.label || markDelegate.modelData.category || "地图标点"; color: AppContext.text; font.pixelSize: 10; elide: Text.ElideRight }
                            Text {
                                text: markDelegate.modelData.position
                                    ? Math.round(markDelegate.modelData.position.x) + ", " + Math.round(markDelegate.modelData.position.y) : ""
                                color: AppContext.muted; font.pixelSize: 9; font.family: "Consolas"
                            }
                        }
                    }
                    Text {
                        visible: !dlg.controller || !dlg.controller.networked || dlg.controller.onlineMapMarks.length === 0
                        Layout.leftMargin: 16
                        text: "暂无可见标点"
                        color: AppContext.muted
                        font.pixelSize: 10
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }
    }
}
