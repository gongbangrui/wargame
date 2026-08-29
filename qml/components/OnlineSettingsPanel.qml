pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: root
    property var controller: null
    property var editor: null
    property var appWindow: null
    signal sessionChangeRequested()
    signal helpRequested()
    modal: true
    anchors.centerIn: Overlay.overlay
    title: "联网设置"
    standardButtons: Dialog.NoButton
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
    width: Math.max(360, Math.min(620, parent ? parent.width - 32 : 620))
    height: Math.max(420, Math.min(700, parent ? parent.height - 32 : 700))
    property var onlineDefs: [
        { action: "nextUnit", label: "下一个单元", defSeq: "Tab" },
        { action: "prevUnit", label: "上一个单元", defSeq: "Shift+Tab" },
        { action: "locate", label: "定位焦点", defSeq: "F" },
        { action: "fitMap", label: "适配地图", defSeq: "Ctrl+F" },
        { action: "sidebar", label: "战术侧栏", defSeq: "B" },
        { action: "cancel", label: "取消操作", defSeq: "Escape" },
        { action: "scan", label: "扫描", defSeq: "S" },
        { action: "engage", label: "交战", defSeq: "Return" }
    ]
    function read(key, fallback) { return root.controller ? root.controller.loadSetting(key, fallback) : fallback }
    function save(key, value) { if (root.controller) root.controller.saveSetting(key, value) }
    function currentSeatLabel() {
        var id = root.controller ? String(root.controller.currentSeatId || "") : ""
        var seats = root.controller ? root.controller.onlineSeats || [] : []
        for (var i = 0; i < seats.length; ++i) {
            if (String(seats[i].seatId || "") === id) {
                var seat = seats[i]
                var labels = { commander: "指挥席", attack: "攻击席", recon: "侦察席",
                               ground: "地面引导席", jammer: "干扰席" }
                var side = seat.side === "red" ? "红方" : seat.side === "blue" ? "蓝方" : ""
                var slot = seat.slot && seat.seatType !== "commander" ? " #" + seat.slot : ""
                return (side ? side + " · " : "") + (labels[seat.seatType] || "战位") + slot
            }
        }
        return id ? "当前战位" : "未选战位"
    }
    function load() {
        live.checked = read("online/intel/showLive", true)
        stale.checked = read("online/intel/showStale", true)
        manual.checked = read("online/intel/showManual", true)
        uncertainty.checked = read("online/intel/showUncertainty", true)
        newNotice.checked = read("online/notifications/newIntel", true)
        shareNotice.checked = read("online/notifications/intelShare", true)
        commRange.checked = read("online/map/showCommunicationRange", false)
        detectRange.checked = read("online/map/showDetectionRange", true)
        attackRange.checked = read("online/map/showAttackRange", true)
        defaultView.currentIndex = Math.max(0, Math.min(2,
            Number(read("online/sidebar/defaultView", 0))))
        var historyFreshness = String(read("online/intel/defaultHistoryFreshness", ""))
        defaultHistory.currentIndex = historyFreshness === "live" ? 1
            : historyFreshness === "stale" ? 2 : historyFreshness === "archived" ? 3 : 0
        shortcutEditor.reload()
    }
    background: Rectangle { color: AppContext.page; border.color: AppContext.line; radius: 6; border.width: 1 }
    header: Rectangle {
        implicitHeight: 52
        color: AppContext.panel
        border.color: AppContext.line
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 10
            spacing: 10
            Icon { name: "settings"; iconColor: AppContext.signal; iconSize: 18 }
            Text {
                Layout.fillWidth: true
                text: "联网设置"
                color: AppContext.textStrong
                font.pixelSize: 15
                font.bold: true
            }
            GhostButton {
                text: ""
                iconName: "close"
                iconSize: 17
                implicitWidth: 36
                implicitHeight: 34
                onClicked: root.close()
                Accessible.name: "关闭联网设置"
            }
        }
    }
    contentItem: Flickable {
        clip: true; contentWidth: width; contentHeight: body.implicitHeight + 24
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        ColumnLayout {
            id: body; width: parent.width - 28; x: 14; y: 12; spacing: 8
            SettingsSection { title: "会话"; iconName: "network"
                GridLayout { columns: 2; Layout.fillWidth: true
                    Label { text: "账号"; color: AppContext.muted }
                    Label { text: root.controller ? (root.controller.username || root.controller.displayName) : ""; color: AppContext.text; elide: Text.ElideRight; Layout.fillWidth: true }
                    Label { text: "服务器"; color: AppContext.muted }
                    Label { text: root.controller ? root.controller.serverAddress : ""; color: AppContext.text; elide: Text.ElideRight; Layout.fillWidth: true }
                    Label { text: "房间 / 战位"; color: AppContext.muted }
                    Label { text: root.controller ? (root.controller.currentRoomId + " / " + root.currentSeatLabel()) : ""; color: AppContext.text; elide: Text.ElideRight; Layout.fillWidth: true }
                    Label { text: "连接"; color: AppContext.muted }
                    Label { text: root.controller ? root.controller.networkStatus : ""; color: AppContext.text; elide: Text.ElideRight; Layout.fillWidth: true }
                    Label { text: "延迟"; color: AppContext.muted }
                    Label { text: root.controller && root.controller.gameLatencyMs >= 0 ? root.controller.gameLatencyMs + " ms" : "--"; color: AppContext.text; Layout.fillWidth: true }
                }
                RowLayout { Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    GhostButton { text: "更换会话"; iconName: "network"; onClicked: { root.close(); root.sessionChangeRequested() } }
                }
            }
            SettingsSection { title: "联网显示"; iconName: "settings"
                SettingsToggleRow { id: commRange; label: "通信范围"; iconName: "network"; onChanged: value => root.save("online/map/showCommunicationRange", value) }
                SettingsToggleRow { id: detectRange; label: "探测范围"; iconName: "scan"; onChanged: value => root.save("online/map/showDetectionRange", value) }
                SettingsToggleRow { id: attackRange; label: "攻击范围"; iconName: "warning"; onChanged: value => root.save("online/map/showAttackRange", value) }
                RowLayout { Layout.fillWidth: true; spacing: 8
                    Label { text: "默认侧栏"; color: AppContext.text; Layout.fillWidth: true }
                    ComboBox { id: defaultView; model: ["单位", "指挥", "情报"]; Layout.preferredWidth: 120
                        onActivated: root.save("online/sidebar/defaultView", currentIndex)
                    }
                }
            }
            SettingsSection { title: "情报地图"; iconName: "map"
                SettingsToggleRow { id: live; label: "显示实时接触"; iconName: "locate"; onChanged: value => root.save("online/intel/showLive", value) }
                SettingsToggleRow { id: stale; label: "显示失联接触"; iconName: "warning"; onChanged: value => root.save("online/intel/showStale", value) }
                SettingsToggleRow { id: manual; label: "显示人工报告"; iconName: "plus"; onChanged: value => root.save("online/intel/showManual", value) }
                SettingsToggleRow { id: uncertainty; label: "显示不确定范围"; iconName: "scan"; onChanged: value => root.save("online/intel/showUncertainty", value) }
                RowLayout { Layout.fillWidth: true; spacing: 8
                    Label { text: "默认历史过滤"; color: AppContext.text; Layout.fillWidth: true }
                    ComboBox { id: defaultHistory; model: ["全部", "实时", "失联", "归档"]; Layout.preferredWidth: 120
                        onActivated: root.save("online/intel/defaultHistoryFreshness",
                            currentIndex === 1 ? "live" : currentIndex === 2 ? "stale"
                            : currentIndex === 3 ? "archived" : "")
                    }
                }
            }
            SettingsSection { title: "通知"; iconName: "chat"
                SettingsToggleRow { id: newNotice; label: "新情报"; iconName: "dot"; onChanged: value => root.save("online/notifications/newIntel", value) }
                SettingsToggleRow { id: shareNotice; label: "收到共享"; iconName: "send"; onChanged: value => root.save("online/notifications/intelShare", value) }
            }
            SettingsSection { title: "联网快捷键"; iconName: "shortcut"
                ShortcutEditor { id: shortcutEditor; controller: root.controller; definitions: root.onlineDefs; storagePrefix: "shortcuts/online/"; onShortcutChanged: root.load() }
            }
            RowLayout {
                Layout.fillWidth: true
                GhostButton { text: "帮助"; iconName: "help"; onClicked: root.helpRequested() }
                Item { Layout.fillWidth: true }
            }
        }
    }
    onOpened: root.load()
}
