pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: dlg

    property var controller: null
    property var snap: ({})
    property bool interactionEnabled: true
    readonly property bool narrow: parent ? parent.width < 720 : false

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
            groundscout: "地面侦察"
        }
        return labels[kind] || kind || "未知单元"
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
    width: Math.max(300, Math.min(680, (parent ? parent.width : 704) - 24))
    height: Math.max(420, Math.min(620, (parent ? parent.height : 644) - 24))
    anchors.centerIn: parent
    padding: 0

    background: Rectangle {
        color: AppContext.panel
        border.color: AppContext.line
        radius: dlg.narrow ? 0 : AppContext.radius
    }

    header: Rectangle {
        implicitHeight: 54
        color: AppContext.raised
        border.color: AppContext.line

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 10
            spacing: 10

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    Layout.fillWidth: true
                    text: dlg.snap.callsign || dlg.snap.id || "单位详情"
                    color: AppContext.textStrong
                    font.pixelSize: 15
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    text: dlg.kindLabel(dlg.snap.kind)
                    color: AppContext.muted
                    font.pixelSize: 10
                }
            }
            GhostButton {
                text: ""
                iconName: "close"
                implicitWidth: 34
                implicitHeight: 32
                onClicked: dlg.close()
                ToolTip.visible: hovered
                ToolTip.text: "关闭"
                Accessible.name: ToolTip.text
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        TabBar {
            id: detailTabs
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            background: Rectangle { color: AppContext.page }

            Repeater {
                model: ["态势", "系统", "技能", "交战", "链路"]
                delegate: TabButton {
                    id: detailTab
                    required property string modelData
                    width: detailTabs.width / 5
                    text: detailTab.modelData
                    contentItem: Text {
                        text: detailTab.text
                        color: detailTab.checked ? AppContext.signal : AppContext.muted
                        font.pixelSize: 11
                        font.bold: detailTab.checked
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: detailTab.checked ? AppContext.raised : "transparent"
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 2
                            color: detailTab.checked ? AppContext.signal : "transparent"
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
                    width: overviewPage.availableWidth
                    spacing: 12
                    anchors.margins: 16

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
                            Layout.preferredHeight: 82
                            color: AppContext.page
                            border.color: AppContext.line
                            radius: 5
                            required property int index

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 12
                                Icon { name: abilityDelegate.modelData.icon; iconColor: AppContext.signal; iconSize: 19 }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3
                                    Text { text: abilityDelegate.modelData.name; color: AppContext.textStrong; font.pixelSize: 12; font.bold: true }
                                    Text {
                                        Layout.fillWidth: true
                                        text: (abilityDelegate.modelData.data.range !== undefined ? "范围 " + Math.round(abilityDelegate.modelData.data.range) + " m  ·  " : "")
                                              + "冷却 " + Number(abilityDelegate.modelData.data.cooldownRemaining || 0).toFixed(0)
                                              + " / " + Number(abilityDelegate.modelData.data.cooldownSec || 0).toFixed(0) + " s"
                                        color: AppContext.muted; font.pixelSize: 10; elide: Text.ElideRight
                                    }
                                    Text {
                                        visible: abilityDelegate.modelData.data.remaining !== undefined
                                        text: Number(abilityDelegate.modelData.data.remaining) < 0 ? "次数 无限" : "剩余 " + Number(abilityDelegate.modelData.data.remaining) + " / " + Number(abilityDelegate.modelData.data.capacity)
                                        color: AppContext.text; font.pixelSize: 10
                                    }
                                }
                                GhostButton {
                                    text: ""
                                    iconName: abilityDelegate.modelData.icon
                                    implicitWidth: 36
                                    implicitHeight: 34
                                    visible: dlg.actionVisible(abilityDelegate.modelData.action, true)
                                    enabled: dlg.actionAllowed(abilityDelegate.modelData.action,
                                                               abilityDelegate.modelData.data.available === true)
                                    onClicked: dlg.controller.command(abilityDelegate.modelData.action, { unitId: dlg.snap.id })
                                    ToolTip.visible: hovered
                                    ToolTip.text: abilityDelegate.modelData.name
                                    Accessible.name: ToolTip.text
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
