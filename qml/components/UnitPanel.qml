pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    property var controller: null
    property var editor: null
    color: "transparent"

    property var snap: ({})
    property bool interactionEnabled: true
    readonly property bool detailsOpen: unitDetailsDialog.opened

    function actionEntry(action) {
        var actions = root.snap.actions || root.snap.actionCapabilities || ({})
        return actions[action]
    }

    function actionVisible(action, localFallback) {
        if (!root.interactionEnabled || !root.snap.alive) return false
        var entry = root.actionEntry(action)
        if (entry !== undefined) {
            if (typeof entry === "object" && entry !== null)
                return entry.visible !== false
            return Boolean(entry)
        }
        return root.controller && !root.controller.networked && Boolean(localFallback)
    }

    function actionAllowed(action, localFallback) {
        if (!root.interactionEnabled || !root.snap.alive) return false
        var entry = root.actionEntry(action)
        if (entry !== undefined) {
            if (typeof entry === "object" && entry !== null) {
                if (entry.enabled !== undefined) return Boolean(entry.enabled)
                if (entry.allowed !== undefined) return Boolean(entry.allowed)
                return entry.visible !== false
            }
            return Boolean(entry)
        }
        return root.controller && !root.controller.networked && Boolean(localFallback)
    }

    function ability(name) {
        var abilities = root.snap.abilities || ({})
        return abilities[name] || ({})
    }

    function speedLimit() {
        var reported = Number(root.snap.maxCommandedSpeed)
        if (isFinite(reported) && reported > 0) return reported
        if (root.snap.kind === "attackuav") return 360
        if (root.snap.kind === "reconuav") return 300
        if (root.snap.kind === "jammeruav") return 260
        if (root.snap.kind === "groundscout") return 36
        return 0
    }

    QtObject {
        id: t
        property color text: AppContext.text
        property color textStrong: AppContext.textStrong
        property color textDim: AppContext.textDim
        property color muted: AppContext.muted
        property color accent: AppContext.signal
        property color accentSoft: "#1d675e"
        property color red: AppContext.red
        property color blue: AppContext.blue
        property color danger: AppContext.danger
        property color success: AppContext.success
        property color warning: AppContext.warning
    }

    Flickable {
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: panelContent.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
        id: panelContent
        width: parent.width - (parent.contentHeight > parent.height ? 8 : 0)
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    text: root.snap.callsign || "\u2014"
                    color: root.snap.alive ? (root.snap.side === "red" ? t.red : (root.snap.side === "blue" ? t.blue : t.textStrong)) : t.muted
                    font.pixelSize: 17
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.maximumWidth: 180
                    renderType: Text.NativeRendering
                }
                Text {
                    text: {
                        var k = root.snap.kind || ""
                        var kinds = { "commandpost": "\u6307\u6325\u6240", "reconuav": "\u4fa6\u5bdf\u65e0\u4eba\u673a", "attackuav": "\u653b\u51fb\u65e0\u4eba\u673a", "groundscout": "\u5730\u9762\u5206\u961f", "jammeruav": "\u7535\u5b50\u5e72\u6270", "groundtarget": "\u5730\u9762\u9759\u6001\u76ee\u6807" }
                        var label = kinds[k] || k
                        return label + " \u00b7 " + (root.snap.side === "red" ? "\u7ea2\u65b9" : (root.snap.side === "blue" ? "\u84dd\u65b9" : root.snap.side || "-"))
                    }
                    color: t.muted
                    font.pixelSize: 11
                    renderType: Text.NativeRendering
                }
            }
            TonalButton {
                text: "详情"
                iconName: "table"
                iconSize: 15
                base: t.accentSoft
                textColor: t.textStrong
                paddingH: 10
                implicitHeight: 30
                onClicked: unitDetailsDialog.open()
                ToolTip.visible: hovered
                ToolTip.text: "单位详情与操作"
                Accessible.name: ToolTip.text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            visible: root.snap.alive && root.interactionEnabled

            AbilityActionButton {
                Layout.preferredWidth: 48
                Layout.preferredHeight: 42
                iconName: "return"
                actionLabel: "撤离"
                abilityData: ({})
                actionVisible: root.actionVisible("withdraw", root.snap.movable === true)
                actionAllowed: root.actionAllowed("withdraw", root.snap.movable === true)
                onClicked: root.controller.command("withdraw", { unitId: root.snap.id })
            }
            AbilityActionButton {
                Layout.preferredWidth: 48
                Layout.preferredHeight: 42
                iconName: "countermeasure"
                actionLabel: "释放干扰弹"
                abilityData: root.ability("countermeasure")
                actionVisible: root.actionVisible("activateCountermeasure",
                                                   root.ability("countermeasure").range > 0)
                actionAllowed: root.actionAllowed("activateCountermeasure",
                                                  root.ability("countermeasure").available === true)
                onClicked: root.controller.command("activateCountermeasure", { unitId: root.snap.id })
            }
            AbilityActionButton {
                Layout.preferredWidth: 48
                Layout.preferredHeight: 42
                iconName: "scan"
                actionLabel: "超视距扫描"
                abilityData: root.ability("scan")
                actionVisible: root.actionVisible("activateScan", root.snap.kind === "reconuav")
                actionAllowed: root.actionAllowed("activateScan",
                                                  root.snap.kind === "reconuav"
                                                  && root.ability("scan").available === true)
                onClicked: root.controller.command("activateScan", { unitId: root.snap.id })
            }
            AbilityActionButton {
                Layout.preferredWidth: 48
                Layout.preferredHeight: 42
                iconName: "repair"
                actionLabel: "战场修理"
                abilityData: root.ability("fieldRepair")
                actionVisible: root.actionVisible("attemptFieldRepair", true)
                actionAllowed: root.actionAllowed("attemptFieldRepair",
                                                  root.ability("fieldRepair").available === true)
                onClicked: root.controller.command("attemptFieldRepair", { unitId: root.snap.id })
            }
            AbilityActionButton {
                Layout.preferredWidth: 48
                Layout.preferredHeight: 42
                iconName: "service"
                actionLabel: "开始补充"
                abilityData: ({})
                actionVisible: !root.snap.serviceRequested
                               && root.actionVisible("service", root.snap.serviceEligible === true)
                actionAllowed: root.actionAllowed("service", root.snap.serviceEligible === true)
                onClicked: root.controller.command("service", { unitId: root.snap.id })
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: root.snap.serviceRequested ? 30 : 0
            visible: Boolean(root.snap.serviceRequested)
            spacing: 8
            Icon { name: "service"; iconColor: t.warning; iconSize: 19 }
            ProgressBar {
                id: serviceProgress
                Layout.fillWidth: true
                from: 0; to: 1
                value: Math.max(0, Math.min(1, Number(root.snap.serviceProgress || 0)))
                background: Rectangle { implicitHeight: 7; radius: 3; color: AppContext.raised }
                contentItem: Item {
                    implicitHeight: 7
                    Rectangle {
                        width: serviceProgress.visualPosition * parent.width
                        height: parent.height; radius: 3; color: t.warning
                    }
                }
            }
            Text {
                text: Math.round(serviceProgress.value * 100) + "%"
                color: t.textDim; font.pixelSize: 10; font.family: "Consolas"
            }
            GhostButton {
                text: ""; iconName: "close"; implicitWidth: 32; implicitHeight: 28
                enabled: root.actionAllowed("cancelService", true)
                onClicked: root.controller.command("cancelService", { unitId: root.snap.id })
                ToolTip.visible: hovered; ToolTip.text: "取消补充"
                Accessible.name: ToolTip.text
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#2a3142" }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            visible: root.snap.hp !== undefined
            Text {
                text: "\u751f\u547d\u503c"
                color: t.muted
                font.pixelSize: 11
                renderType: Text.NativeRendering
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 10
                radius: 5
                color: "#1a2030"
                Rectangle {
                    anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                    width: {
                        var ratio = Math.max(0, Math.min(1, (root.snap.hp || 0) / Math.max(1, root.snap.maxHp || 100)))
                        return parent.width * ratio
                    }
                    radius: 5
                    color: {
                        var ratio = Math.max(0, Math.min(1, (root.snap.hp || 0) / Math.max(1, root.snap.maxHp || 100)))
                        return ratio > 0.5 ? t.success : (ratio > 0.25 ? t.warning : t.danger)
                    }
                    Behavior on width { NumberAnimation { duration: 250 } }
                }
            }
            Text {
                text: Math.round(root.snap.hp || 0) + " / " + Math.round(root.snap.maxHp || 0)
                color: t.textDim
                font.pixelSize: 11
                font.family: "Consolas"
                renderType: Text.NativeRendering
            }
        }

        Rectangle {
            id: weaponStateCard
            visible: root.snap.ammoRemaining !== undefined
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 52 : 0
            radius: 4
            property real cooldown: Number(root.snap.cooldownRemaining || 0)
            color: cooldown > 0 ? "#2b251b" : "#142923"
            border.color: cooldown > 0 ? t.warning : t.success

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 5
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Text {
                        text: "弹药 " + root.snap.ammoRemaining + " / " + root.snap.ammoCapacity
                        color: t.textStrong; font.pixelSize: 11; font.bold: true
                    }
                    Row {
                        spacing: 2
                        Repeater {
                            model: Math.min(12, Math.max(0, Number(root.snap.ammoCapacity || 0)))
                            delegate: Rectangle {
                                id: ammunitionMark
                                required property int index
                                width: 4; height: 11; radius: 1
                                color: ammunitionMark.index < Number(root.snap.ammoRemaining || 0)
                                    ? (root.snap.side === "red" ? t.red : t.blue) : "transparent"
                                border.color: root.snap.side === "red" ? t.red : t.blue
                                opacity: ammunitionMark.index < Number(root.snap.ammoRemaining || 0)
                                    ? 1 : 0.45
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: weaponStateCard.cooldown > 0
                            ? "射击冷却  " + weaponStateCard.cooldown.toFixed(1) + " s"
                            : "武器就绪"
                        color: weaponStateCard.cooldown > 0 ? t.warning : t.success
                        font.pixelSize: 10; font.bold: true
                    }
                }
                ProgressBar {
                    id: cooldownProgress
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: weaponStateCard.cooldown > 0
                        ? 1 - Math.max(0, Math.min(1,
                            weaponStateCard.cooldown
                            / Math.max(0.001, Number(root.snap.cooldownSec || 0.001)))) : 1
                    Accessible.name: "攻击无人机射击冷却进度"
                    Accessible.description: weaponStateCard.cooldown > 0
                        ? weaponStateCard.cooldown.toFixed(1) + " 秒后可再次攻击"
                        : "武器已就绪"
                    background: Rectangle { implicitHeight: 6; radius: 3; color: AppContext.raised }
                    contentItem: Item {
                        implicitHeight: 6
                        Rectangle {
                            width: cooldownProgress.visualPosition * parent.width
                            height: parent.height
                            radius: 3
                            color: weaponStateCard.cooldown > 0 ? t.warning : t.success
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            visible: root.snap.speed !== undefined
            Text { text: "\u901f\u5ea6"; color: t.muted; font.pixelSize: 11; renderType: Text.NativeRendering }
            Rectangle {
                Layout.preferredWidth: root.actionAllowed("setSpeed", root.snap.movable === true) ? 104 : 66; implicitHeight: 26; radius: 4
                color: "#141b24"; border.color: "#2a3142"
                RowLayout {
                    anchors.fill: parent; spacing: 0
                    TonalButton {
                        text: "\u2212"; base: "#252d3a"; radius: 0
                        visible: root.actionAllowed("setSpeed", root.snap.movable === true)
                        implicitWidth: 26; implicitHeight: 26
                        onClicked: {
                            var v = Math.round(root.snap.speed || 0) - 5
                            root.controller.command("setSpeed", { unitId: root.snap.id, speed: Math.max(1, Math.min(root.speedLimit(), v)) })
                        }
                    }
                    TextInput {
                        id: speedInput
                        visible: root.actionAllowed("setSpeed", root.snap.movable === true)
                        Layout.fillWidth: true; Layout.alignment: Qt.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                        color: t.textDim; font.pixelSize: 12; font.family: "Consolas"
                        text: Math.round(root.snap.speed || 0)
                        onEditingFinished: {
                            var v = parseInt(text) || 1
                            root.controller.command("setSpeed", { unitId: root.snap.id, speed: Math.max(1, Math.min(root.speedLimit(), v)) })
                        }
                    }
                    Text {
                        visible: !root.actionAllowed("setSpeed", root.snap.movable === true)
                        Layout.fillWidth: true; Layout.alignment: Qt.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                        text: Math.round(root.snap.speed || 0)
                        color: t.textDim; font.pixelSize: 12; font.family: "Consolas"
                        renderType: Text.NativeRendering
                    }
                    TonalButton {
                        text: "+"; base: "#252d3a"; radius: 0
                        visible: root.actionAllowed("setSpeed", root.snap.movable === true)
                        implicitWidth: 26; implicitHeight: 26
                        onClicked: {
                            var v = Math.round(root.snap.speed || 0) + 5
                            root.controller.command("setSpeed", { unitId: root.snap.id, speed: Math.min(root.speedLimit(), v) })
                        }
                    }
                }
            }
            Text { text: "m/s"; color: t.muted; font.pixelSize: 11; renderType: Text.NativeRendering }
            Item { Layout.fillWidth: true }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#2a3142" }

        GridLayout {
            columns: 2
            columnSpacing: 10
            rowSpacing: 3
            Layout.fillWidth: true
            Repeater {
                model: {
                    var rows = []
                    if (root.snap.fuelRemaining !== undefined) {
                        rows.push({ k: "燃油", v: Math.round(root.snap.fuelRemaining) + " / " + Math.round(root.snap.fuelCapacity) + " s", c: "#5fd1c8" })
                        rows.push({ k: "耗率", v: Number(root.snap.fuelBurnRate || 0).toFixed(2) + " /s", c: "#8edbd5" })
                        if (root.snap.estimatedEnduranceSec !== undefined
                                && Number(root.snap.estimatedEnduranceSec) >= 0)
                            rows.push({ k: "续航", v: Math.round(Number(root.snap.estimatedEnduranceSec)) + " s", c: "#b8efe9" })
                    }
                    var countermeasure = root.ability("countermeasure")
                    if (countermeasure.cooldownRemaining !== undefined) {
                        var remaining = Number(countermeasure.remaining)
                        var uses = remaining < 0 ? "∞" : String(remaining)
                        rows.push({ k: "干扰弹", v: uses + " · " + Number(countermeasure.cooldownRemaining || 0).toFixed(0) + " s", c: "#ffc15a" })
                    }
                    var scan = root.ability("scan")
                    if (scan.cooldownRemaining !== undefined && root.snap.kind === "reconuav")
                        rows.push({ k: "扫描", v: Number(scan.cooldownRemaining || 0).toFixed(0) + " s", c: "#35c8ff" })
                    var repair = root.ability("fieldRepair")
                    if (repair.cooldownRemaining !== undefined)
                        rows.push({ k: "修理", v: Number(repair.cooldownRemaining || 0).toFixed(0) + " s", c: "#7bd88f" })
                    return rows
                }
                delegate: RowLayout {
                    id: statRow
                    required property var modelData
                    spacing: 6
                    Rectangle { Layout.preferredWidth: 6; Layout.preferredHeight: 6; radius: 3; color: statRow.modelData.c; Layout.alignment: Qt.AlignVCenter }
                    Text { text: statRow.modelData.k; color: t.muted; font.pixelSize: 11; Layout.preferredWidth: 28; renderType: Text.NativeRendering }
                    Text { text: statRow.modelData.v; color: t.textDim; font.pixelSize: 12; font.family: "Consolas"; renderType: Text.NativeRendering }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 30 : 0
            visible: Number(root.snap.incomingThreatCount || 0) > 0
            radius: 4
            color: "#34251b"
            border.color: t.warning
            RowLayout {
                anchors.fill: parent; anchors.margins: 7; spacing: 7
                Icon { name: "warning"; iconColor: t.warning; iconSize: 13 }
                Text {
                    Layout.fillWidth: true
                    text: "来袭 " + Number(root.snap.incomingThreatCount || 0)
                          + " · " + Number(root.snap.minimumThreatEta || 0).toFixed(1) + " s"
                          + (Number(root.snap.nearestThreatDistance || -1) >= 0
                             ? " · " + Math.round(Number(root.snap.nearestThreatDistance)) + " m" : "")
                    color: t.warning; font.pixelSize: 10; font.bold: true
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#2a3142" }

        ColumnLayout {
            id: commandBarLayout
            Layout.fillWidth: true
            spacing: 4
            visible: root.snap.alive && root.controller !== null
                     && !root.controller.isObserver
                     && root.controller.currentSeatType !== "commander"
                     && root.controller.chatMessages !== undefined
            property var latestCommandMessage: {
                var msgs = root.controller ? root.controller.chatMessages : []
                var currentSeatId = root.controller ? root.controller.currentSeatId : ""
                for (var i = msgs.length - 1; i >= 0; i--) {
                    var m = msgs[i]
                    if (!m || !m.seatId) continue
                    var parts = m.seatId.split("_")
                    if (parts.length >= 2 && parts[1] === "commander") {
                        var recipients = m.recipientSeatIds || []
                        if (recipients.indexOf(currentSeatId) >= 0) return m
                    }
                }
                return null
            }
            Text { text: "\u6307\u6325\u547d\u4ee4"; color: t.muted; font.pixelSize: 11; renderType: Text.NativeRendering; visible: commandBarLayout.latestCommandMessage !== null }
            Rectangle {
                visible: commandBarLayout.latestCommandMessage !== null
                Layout.fillWidth: true
                implicitHeight: cmdText.implicitHeight + 14
                radius: 4
                color: "#1a1e2c"
                border.color: AppContext.signal
                Text {
                    id: cmdText
                    anchors.fill: parent; anchors.margins: 7
                    text: commandBarLayout.latestCommandMessage ? (commandBarLayout.latestCommandMessage.text || "") : ""
                    color: AppContext.text; font.pixelSize: 11; wrapMode: Text.WordWrap
                    renderType: Text.NativeRendering
                }
            }
        }

        ColumnLayout {
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 32
                radius: 4
                color: {
                    var s = root.snap.status || ""
                    if (!root.snap.alive) return "#3a1520"
                    if (s.indexOf("攻击") >= 0 || s.indexOf("追击") >= 0) return "#3a2015"
                    if (s.indexOf("摧毁") >= 0) return "#3a2015"
                    if (s.indexOf("引导") >= 0 || s.indexOf("机动") >= 0) return "#1a2a3a"
                    if (s.indexOf("撤离") >= 0) return "#2a1a20"
                    return "#1a2030"
                }
                border.color: {
                    var s = root.snap.status || ""
                    if (!root.snap.alive) return "#ff4d6d"
                    if (s.indexOf("攻击") >= 0 || s.indexOf("追击") >= 0) return "#ff6b4a"
                    if (s.indexOf("摧毁") >= 0) return "#ff4d6d"
                    if (s.indexOf("引导") >= 0 || s.indexOf("机动") >= 0) return "#4f9dff"
                    if (s.indexOf("撤离") >= 0) return "#ff5566"
                    return "#3a455a"
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 6
                    Rectangle {
                        Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                        Layout.alignment: Qt.AlignVCenter
                        color: {
                            var s = root.snap.status || ""
                            if (!root.snap.alive) return t.danger
                            if (s.indexOf("已毁") >= 0 || s.indexOf("摧毁") >= 0) return t.danger
                            if (s.indexOf("攻击") >= 0 || s.indexOf("追击") >= 0) return t.warning
                            if (s.indexOf("引导") >= 0 || s.indexOf("机动") >= 0 || s.indexOf("巡航") >= 0) return t.accent
                            if (s.indexOf("撤离") >= 0) return t.red
                            if (s.indexOf("到达") >= 0 || s.indexOf("待命") >= 0) return t.success
                            return t.muted
                        }
                    }
                    Text {
                        text: root.snap.status || "-"
                        color: {
                            if (!root.snap.alive) return t.danger
                            var s = root.snap.status || ""
                            if (s.indexOf("已毁") >= 0) return t.danger
                            if (s.indexOf("攻击") >= 0 || s.indexOf("追击") >= 0) return t.warning
                            if (s.indexOf("引导") >= 0 || s.indexOf("机动") >= 0 || s.indexOf("巡航") >= 0) return t.accent
                            if (s.indexOf("撤离") >= 0) return t.red
                            return t.textDim
                        }
                        font.pixelSize: 12
                        font.bold: true
                        Layout.fillWidth: true
                        renderType: Text.NativeRendering
                    }
                }
            }
        }
        }
    }

    UnitDetailsDialog {
        id: unitDetailsDialog
        parent: Overlay.overlay
        controller: root.controller
        snap: root.snap
        interactionEnabled: root.interactionEnabled
    }
}
