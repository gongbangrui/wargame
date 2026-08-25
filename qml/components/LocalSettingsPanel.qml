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
    title: "本地设置"
    standardButtons: Dialog.NoButton
    width: Math.max(360, Math.min(620, parent ? parent.width - 32 : 620))
    height: Math.max(420, Math.min(700, parent ? parent.height - 32 : 700))

    property var localDefs: [
        { action: "toggleRun", label: "暂停 / 继续", defSeq: "Space" },
        { action: "speed0", label: "速率暂停", defSeq: "1" },
        { action: "speed1", label: "速率 1x", defSeq: "2" },
        { action: "speed2", label: "速率 2x", defSeq: "3" },
        { action: "speed4", label: "速率 4x", defSeq: "4" },
        { action: "speed8", label: "速率 8x", defSeq: "5" },
        { action: "step", label: "单步", defSeq: "." },
        { action: "speedUp", label: "加速选中单元", defSeq: "W" },
        { action: "speedDown", label: "减速选中单元", defSeq: "S" },
        { action: "nextUnitTab", label: "下一个单元", defSeq: "Tab" },
        { action: "prevUnitSh", label: "上一个单元", defSeq: "Shift+Tab" },
        { action: "autoFit", label: "自适应缩放", defSeq: "Ctrl+F" },
        { action: "cancelTrack", label: "取消追踪", defSeq: "P" }
    ]

    function read(key, fallback) { return root.controller ? root.controller.loadSetting(key, fallback) : fallback }
    function save(key, value) { if (root.controller) root.controller.saveSetting(key, value) }
    function load() {
        widthBox.value = read("window/width", root.appWindow ? root.appWindow.width : 1360)
        heightBox.value = read("window/height", root.appWindow ? root.appWindow.height : 860)
        opacityBox.value = read("window/opacity", root.appWindow ? root.appWindow.opacity : 1.0)
        speedBox.currentIndex = [0, 1, 2, 4, 8].indexOf(read("sim/defaultSpeed", 1))
        if (speedBox.currentIndex < 0) speedBox.currentIndex = 1
        miniMap.checked = read("sim/showMinimap", true)
        grid.checked = read("sim/showGrid", false)
        follow.checked = read("sim/autoFollowFocused", true)
        shortcutEditor.reload()
    }
    function commit() {
        save("window/width", widthBox.value); save("window/height", heightBox.value)
        save("window/opacity", opacityBox.value)
        save("sim/defaultSpeed", [0, 1, 2, 4, 8][speedBox.currentIndex])
        save("sim/showMinimap", miniMap.checked); save("sim/showGrid", grid.checked)
        save("sim/autoFollowFocused", follow.checked)
        if (root.appWindow) { root.appWindow.width = widthBox.value; root.appWindow.height = heightBox.value; root.appWindow.opacity = opacityBox.value }
    }

    background: Rectangle { color: AppContext.page; border.color: AppContext.line; radius: 6; border.width: 1 }
    contentItem: Flickable {
        clip: true; contentWidth: width; contentHeight: body.implicitHeight + 24
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        ColumnLayout {
            id: body; width: parent.width - 28; x: 14; y: 12; spacing: 8
            SettingsSection { title: "显示"; iconName: "settings"
                RowLayout { Layout.fillWidth: true; spacing: 8
                    Label { text: "窗口宽度"; color: AppContext.text; Layout.fillWidth: true }
                    SpinBox { id: widthBox; from: 900; to: 3840; editable: true; Layout.preferredWidth: 120 }
                    Label { text: "高度"; color: AppContext.text }
                    SpinBox { id: heightBox; from: 620; to: 2160; editable: true; Layout.preferredWidth: 120 }
                }
                RowLayout { Layout.fillWidth: true; spacing: 8
                    Label { text: "透明度"; color: AppContext.text; Layout.fillWidth: true }
                    Slider { id: opacityBox; from: 0.65; to: 1.0; stepSize: 0.05; Layout.fillWidth: true }
                }
                ComboBox { id: speedBox; model: ["暂停", "1x", "2x", "4x", "8x"]; Layout.fillWidth: true }
                SettingsToggleRow { id: miniMap; label: "迷你地图"; iconName: "map" }
                SettingsToggleRow { id: grid; label: "坐标网格"; iconName: "table" }
                SettingsToggleRow { id: follow; label: "自动跟随焦点"; iconName: "locate" }
            }
            SettingsSection { title: "快捷键"; iconName: "shortcut"
                ShortcutEditor { id: shortcutEditor; controller: root.controller; definitions: root.localDefs; storagePrefix: "shortcuts/local/"; migrateLegacy: true; onShortcutChanged: root.load() }
            }
            RowLayout { Layout.fillWidth: true; spacing: 8
                GhostButton { text: "帮助"; iconName: "help"; onClicked: root.helpRequested() }
                Item { Layout.fillWidth: true }
                GhostButton { text: "关闭"; iconName: "close"; onClicked: { root.commit(); root.close() } }
            }
        }
    }
    onOpened: root.load()
    onRejected: root.commit()
    onClosed: root.commit()
}
