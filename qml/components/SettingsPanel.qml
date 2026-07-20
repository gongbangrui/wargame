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
    title: "设置"
    modal: true
    anchors.centerIn: parent
    width: Math.max(320, Math.min(620, parent ? parent.width - 32 : 620))
    height: Math.max(420, Math.min(700, parent ? parent.height - 32 : 700))
    standardButtons: Dialog.NoButton

    // 统一快捷键元数据，绑定到 ListView 的 model（数组 → 渲染；数组替换 → 列表重建）
    property var shortcutDefs: [
        { action: "toggleRun",   label: "暂停 / 继续",      defSeq: "Space",     category: "sim" },
        { action: "speed0",      label: "速率 暂停",        defSeq: "1",         category: "sim" },
        { action: "speed1",      label: "速率 1x",          defSeq: "2",         category: "sim" },
        { action: "speed2",      label: "速率 2x",          defSeq: "3",         category: "sim" },
        { action: "speed4",      label: "速率 4x",          defSeq: "4",         category: "sim" },
        { action: "speed8",      label: "速率 8x",          defSeq: "5",         category: "sim" },
        { action: "step",        label: "单步",             defSeq: ".",         category: "sim" },
        { action: "speedUp",     label: "加速选中单元",     defSeq: "W",         category: "unit" },
        { action: "speedDown",   label: "减速选中单元",     defSeq: "S",         category: "unit" },
        { action: "prevUnit",    label: "前一个单元",       defSeq: "A,Left",    category: "unit" },
        { action: "nextUnit",    label: "后一个单元",       defSeq: "D,Right",   category: "unit" },
        { action: "nextUnitTab", label: "下一个单元",       defSeq: "Tab",       category: "unit" },
        { action: "prevUnitSh",  label: "上一个单元",       defSeq: "Shift+Tab", category: "unit" },
        { action: "autoFit",     label: "自适应缩放",       defSeq: "Ctrl+F",    category: "view" },
        { action: "cancelTrack", label: "取消追踪/停止",    defSeq: "P",         category: "unit" }
    ]
    property var shortcutList: []
    property string captureTargetAction: ""

    function reloadShortcutsFromSettings() {
        var result = []
        for (var i = 0; i < shortcutDefs.length; i++) {
            var def = shortcutDefs[i]
            result.push({
                action: def.action,
                label: def.label,
                seq: root.controller.loadSetting("shortcuts/" + def.action, def.defSeq),
                defaultSeq: def.defSeq,
                category: def.category
            })
        }
        shortcutList = result
    }

    function saveToPending() {
        root.controller.saveSetting("window/width", winW.value)
        root.controller.saveSetting("window/height", winH.value)
        root.controller.saveSetting("window/opacity", opacitySld.value)
        root.controller.saveSetting("sim/defaultView", defView.selValue)
        root.controller.saveSetting("sim/defaultSpeed", defaultSpeedCombo.selValue)
        root.controller.saveSetting("sim/showMinimap", showMinimapChk.checked)
        root.controller.saveSetting("sim/showGrid", showGridChk.checked)
        root.controller.saveSetting("sim/autoFollowFocused", autoFollowChk.checked)
    }

    function loadAll() {
        winW.value = root.controller.loadSetting("window/width", 1360)
        winH.value = root.controller.loadSetting("window/height", 860)
        opacitySld.value = root.controller.loadSetting("window/opacity", 1.0)
        var dv = root.controller.loadSetting("sim/defaultView", "commandpost-red")
        for (var i = 0; i < defView.model.count; i++) {
            if (defView.model.get(i).value === dv) { defView.currentIndex = i; break }
        }
        var ds = root.controller.loadSetting("sim/defaultSpeed", 1)
        for (var j = 0; j < defaultSpeedCombo.model.count; j++) {
            if (defaultSpeedCombo.model.get(j).value === ds) { defaultSpeedCombo.currentIndex = j; break }
        }
        showMinimapChk.checked    = root.controller.loadSetting("sim/showMinimap", true)
        showGridChk.checked       = root.controller.loadSetting("sim/showGrid", false)
        autoFollowChk.checked     = root.controller.loadSetting("sim/autoFollowFocused", true)
        reloadShortcutsFromSettings()
    }

    function applyWindowSize() {
        if (root.appWindow) {
            root.appWindow.width = winW.value
            root.appWindow.height = winH.value
        }
    }

    function resetAllShortcuts() {
        for (var i = 0; i < shortcutDefs.length; i++) {
            var def = shortcutDefs[i]
            root.controller.saveSetting("shortcuts/" + def.action, def.defSeq)
        }
        reloadShortcutsFromSettings()
    }

    function startCapture(action) {
        captureTargetAction = action
        capDialog.ready = false
        capDialog.captured = ""
        capDialog.hasMainKey = false
        capDialog.open()
        Qt.callLater(function() { captureKeyScope.forceActiveFocus() })
    }

    function finishCapture(newSeq) {
        if (captureTargetAction) {
            root.controller.saveSetting("shortcuts/" + captureTargetAction, newSeq)
            // 直接从存储重新读取并重建列表，确保 ListView 立即反映新值
            reloadShortcutsFromSettings()
        }
        captureTargetAction = ""
        capDialog.ready = false
        capDialog.captured = ""
        capDialog.hasMainKey = false
    }

    QtObject {
        id: t
        property color bg:          "#091423"
        property color card:        "#122239"
        property color border:      "#355675"
        property color borderSoft:  "#294664"
        property color text:        "#f3f7fc"
        property color textStrong:  "#ffffff"
        property color textDim:     "#d0dced"
        property color textMuted:   "#aebfd4"
        property color accent:      "#4090ff"
        property color accentSoft:  "#1e4080"
        property color green:       "#36c98a"
        property color red:         "#f04760"
    }

    background: Rectangle { color: t.bg; radius: 12; border.color: t.border; border.width: 1 }

    header: Rectangle {
        height: 52; color: "transparent"
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 22; anchors.rightMargin: 14; spacing: 10
            Rectangle { Layout.preferredWidth: 4; Layout.preferredHeight: 20; radius: 2; color: t.accent; Layout.alignment: Qt.AlignVCenter }
            Text {
                text: "⚙  设置"; color: t.textStrong; font.pixelSize: 17; font.bold: true
                Layout.fillWidth: true; renderType: Text.NativeRendering
            }
            Rectangle {
                Layout.preferredWidth: 32; Layout.preferredHeight: 32; radius: 16
                color: closeMa.containsMouse ? "#324060" : "transparent"
                Behavior on color { ColorAnimation { duration: 140 } }
                Text { anchors.centerIn: parent; text: "✕"; color: t.textDim; font.pixelSize: 15 }
                MouseArea { id: closeMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.close() }
            }
        }
    }

    contentItem: Flickable {
        id: settingsFlickable
        clip: true; contentWidth: settingsFlickable.width; contentHeight: body.implicitHeight + 70
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded; width: 6 }

        ColumnLayout {
            id: body
            anchors.left: parent.left; anchors.right: parent.right
            anchors.leftMargin: 22; anchors.rightMargin: 22; anchors.topMargin: 8
            spacing: 0

            RowLayout {
                spacing: 10; Layout.topMargin: 6; Layout.bottomMargin: 6; Layout.fillWidth: true
                Rectangle { Layout.preferredWidth: 4; Layout.preferredHeight: 16; radius: 2; color: t.green; Layout.alignment: Qt.AlignVCenter }
                Text { text: "运行模式"; color: t.textStrong; font.pixelSize: 14; font.bold: true; renderType: Text.NativeRendering }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: t.borderSoft; Layout.alignment: Qt.AlignVCenter }
            }

            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 62
                color: t.card; radius: 8; border.color: root.controller.networked ? t.green : t.borderSoft; border.width: 1
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 12; spacing: 12
                    Rectangle { Layout.preferredWidth: 9; Layout.preferredHeight: 9; radius: 5; color: root.controller.networked && root.controller.networkState === "connected" ? t.green : root.controller.networked ? "#e1a94c" : t.textMuted }
                    ColumnLayout {
                        spacing: 2; Layout.fillWidth: true
                        Text { text: root.controller.networked ? "联网模式 · " + (root.controller.displayName || root.controller.username) : "本地模式"; color: t.text; font.pixelSize: 13; font.bold: true }
                        Text { text: root.controller.networked ? root.controller.serverAddress + " · " + root.controller.networkStatus : "推演状态仅保存在当前客户端"; color: t.textMuted; font.pixelSize: 10; elide: Text.ElideRight; Layout.fillWidth: true }
                        Text { visible: root.controller.networked; text: root.controller.gameLatencyMs >= 0 ? "延迟  " + root.controller.gameLatencyMs + " ms" : "延迟  --"; color: t.textMuted; font.pixelSize: 10; font.family: "Consolas" }
                    }
                    Rectangle {
                        Layout.preferredWidth: 96; Layout.preferredHeight: 30; radius: 6
                        color: switchModeMa.containsMouse ? "#26364a" : "#1d2939"; border.color: t.borderSoft
                        Text { anchors.centerIn: parent; text: "切换模式"; color: t.textDim; font.pixelSize: 12 }
                        MouseArea { id: switchModeMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { root.close(); root.sessionChangeRequested() } }
                    }
                }
            }

            Item { Layout.fillWidth: true; Layout.preferredHeight: 10 }

            // ── 外观 ──
            RowLayout {
                spacing: 10; Layout.topMargin: 6; Layout.bottomMargin: 6; Layout.fillWidth: true
                Rectangle { Layout.preferredWidth: 4; Layout.preferredHeight: 16; radius: 2; color: t.accent; Layout.alignment: Qt.AlignVCenter }
                Text { text: "外观"; color: t.textStrong; font.pixelSize: 14; font.bold: true; renderType: Text.NativeRendering }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: t.borderSoft; Layout.alignment: Qt.AlignVCenter }
            }

            // 窗口尺寸：窄窗口时将标题和数值分为两行，避免内容重叠。
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: root.width < 500 ? 112 : 70
                color: t.card; radius: 8; border.color: t.borderSoft; border.width: 1
                ColumnLayout {
                    anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16; spacing: 4
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        Text { text: "⎕"; color: t.accent; font.pixelSize: 13; opacity: 0.85; renderType: Text.NativeRendering }
                        Text { text: "窗口尺寸"; color: t.textStrong; font.pixelSize: 13; font.bold: true; renderType: Text.NativeRendering }
                        Item { Layout.fillWidth: true }
                        Text { text: "下次启动将保留此尺寸"; color: t.textMuted; font.pixelSize: 10; visible: root.width >= 500; renderType: Text.NativeRendering }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        SpinBox {
                            id: winW; from: 800; to: 3840; stepSize: 40; editable: false
                            Layout.preferredWidth: 96; Layout.preferredHeight: 30
                            background: Rectangle { color: t.bg; border.color: winW.activeFocus ? t.accent : t.borderSoft; radius: 5 }
                            contentItem: Text {
                                anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 26
                                text: winW.value; color: t.textStrong; font.pixelSize: 12
                                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; renderType: Text.NativeRendering
                            }
                        }
                        Text { text: "×"; color: t.textMuted; font.pixelSize: 15; renderType: Text.NativeRendering }
                        SpinBox {
                            id: winH; from: 600; to: 2160; stepSize: 40; editable: false
                            Layout.preferredWidth: 96; Layout.preferredHeight: 30
                            background: Rectangle { color: t.bg; border.color: winH.activeFocus ? t.accent : t.borderSoft; radius: 5 }
                            contentItem: Text {
                                anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 26
                                text: winH.value; color: t.textStrong; font.pixelSize: 12
                                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; renderType: Text.NativeRendering
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            id: applySizeButton
                            Layout.preferredHeight: 30; Layout.preferredWidth: 68
                            text: "应用"; font.pixelSize: 12; font.bold: true
                            onClicked: root.applyWindowSize()
                            contentItem: Text { text: applySizeButton.text; color: "#ffffff"; font: applySizeButton.font; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; renderType: Text.NativeRendering }
                            background: Rectangle { radius: 6; color: applySizeButton.down ? Qt.darker(t.accent, 1.25) : (applySizeButton.hovered ? Qt.lighter(t.accent, 1.08) : t.accent) }
                        }
                    }
                }
            }

            // 窗口透明度
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: root.width < 500 ? 80 : 62
                color: t.card; radius: 8; border.color: t.borderSoft; border.width: 1
                ColumnLayout {
                    anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16; spacing: 2
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        Text { text: "◐"; color: t.accent; font.pixelSize: 13; opacity: 0.85; renderType: Text.NativeRendering }
                        Text { text: "窗口透明度"; color: t.textStrong; font.pixelSize: 13; font.bold: true; renderType: Text.NativeRendering }
                        Item { Layout.fillWidth: true }
                        Text { text: Math.round(opacitySld.value * 100) + "%"; color: t.textStrong; font.pixelSize: 12; font.bold: true; renderType: Text.NativeRendering }
                    }
                    Slider {
                        id: opacitySld; from: 0.3; to: 1.0; value: 1.0; stepSize: 0.05
                        Layout.fillWidth: true; Layout.preferredHeight: 24
                        background: Rectangle {
                            x: opacitySld.leftPadding; y: opacitySld.topPadding + opacitySld.availableHeight / 2 - 2
                            implicitWidth: 200; implicitHeight: 4; radius: 2; color: t.borderSoft
                            Rectangle { width: opacitySld.visualPosition * parent.width; height: parent.height; radius: 2; color: t.accent }
                        }
                        handle: Rectangle { width: 16; height: 16; radius: 8; color: "#fff"; border.color: t.accent; border.width: 2 }
                        onValueChanged: { if (root.appWindow) root.appWindow.opacity = value }
                    }
                }
            }

            Item { Layout.fillWidth: true; Layout.preferredHeight: 10 }

            // ── 推演 ──
            RowLayout {
                spacing: 10; Layout.topMargin: 6; Layout.bottomMargin: 6; Layout.fillWidth: true
                Rectangle { Layout.preferredWidth: 4; Layout.preferredHeight: 16; radius: 2; color: t.accent; Layout.alignment: Qt.AlignVCenter }
                Text { text: "推演"; color: t.textStrong; font.pixelSize: 14; font.bold: true; renderType: Text.NativeRendering }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: t.borderSoft; Layout.alignment: Qt.AlignVCenter }
            }

            // 默认视角
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 46
                color: t.card; radius: 8; border.color: t.borderSoft; border.width: 1
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16; spacing: 12
                    RowLayout {
                        spacing: 8; Layout.preferredWidth: 140; Layout.alignment: Qt.AlignVCenter
                        Text { text: "⌖"; color: t.accent; font.pixelSize: 13; opacity: 0.85; renderType: Text.NativeRendering }
                        Text { text: "默认视角"; color: t.textDim; font.pixelSize: 13; renderType: Text.NativeRendering }
                    }
                    ComboBox {
                        id: defView; Layout.preferredWidth: 130; Layout.alignment: Qt.AlignVCenter; textRole: "label"
                        property var selValue: currentIndex >= 0 && model && currentIndex < model.count
                                               ? model.get(currentIndex).value : "commandpost-red"
                        model: ListModel {
                            ListElement { label: "✏  编辑";   value: "editor" }
                            ListElement { label: "⬤  红方";   value: "commandpost-red" }
                            ListElement { label: "⬤  蓝方";   value: "commandpost-blue" }
                            ListElement { label: "⌖  导演席"; value: "director" }
                        }
                        currentIndex: 1
                    }
                    Item { Layout.fillWidth: true }
                    Text { text: "启动时默认视角"; color: t.textMuted; font.pixelSize: 11; renderType: Text.NativeRendering }
                }
            }

            // 默认速率
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 46
                color: t.card; radius: 8; border.color: t.borderSoft; border.width: 1
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16; spacing: 12
                    RowLayout {
                        spacing: 8; Layout.preferredWidth: 140; Layout.alignment: Qt.AlignVCenter
                        Text { text: "▶"; color: t.accent; font.pixelSize: 13; opacity: 0.85; renderType: Text.NativeRendering }
                        Text { text: "默认速率"; color: t.textDim; font.pixelSize: 13; renderType: Text.NativeRendering }
                    }
                    ComboBox {
                        id: defaultSpeedCombo; Layout.preferredWidth: 110; Layout.alignment: Qt.AlignVCenter; textRole: "label"
                        property var selValue: currentIndex >= 0 && model && currentIndex < model.count
                                               ? model.get(currentIndex).value : 1
                        model: ListModel {
                            ListElement { label: "暂停"; value: 0 }
                            ListElement { label: "1x";   value: 1 }
                            ListElement { label: "2x";   value: 2 }
                            ListElement { label: "4x";   value: 4 }
                            ListElement { label: "8x";   value: 8 }
                        }
                        currentIndex: 1
                    }
                    Item { Layout.fillWidth: true }
                    Text { text: "启动时推演速率"; color: t.textMuted; font.pixelSize: 11; renderType: Text.NativeRendering }
                }
            }

            // 显示选项
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 46
                color: t.card; radius: 8; border.color: t.borderSoft; border.width: 1
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16; spacing: 18
                    RowLayout { spacing: 6; Layout.alignment: Qt.AlignVCenter
                        CheckBox { id: showMinimapChk; checked: true
                            indicator: Rectangle { implicitWidth: 36; implicitHeight: 20; radius: 10; color: showMinimapChk.checked ? t.accent : "#1c2848"; border.color: showMinimapChk.checked ? t.accent : t.borderSoft
                                Rectangle { x: showMinimapChk.checked ? 18 : 2; width: 16; height: 16; radius: 8; color: "#fff"; anchors.verticalCenter: parent.verticalCenter; Behavior on x { NumberAnimation { duration: 120 } } } } }
                        Text { text: "▣  小地图"; color: t.textDim; font.pixelSize: 13; renderType: Text.NativeRendering }
                    }
                    RowLayout { spacing: 6; Layout.alignment: Qt.AlignVCenter
                        CheckBox { id: showGridChk; checked: false
                            indicator: Rectangle { implicitWidth: 36; implicitHeight: 20; radius: 10; color: showGridChk.checked ? t.accent : "#1c2848"; border.color: showGridChk.checked ? t.accent : t.borderSoft
                                Rectangle { x: showGridChk.checked ? 18 : 2; width: 16; height: 16; radius: 8; color: "#fff"; anchors.verticalCenter: parent.verticalCenter; Behavior on x { NumberAnimation { duration: 120 } } } } }
                        Text { text: "⌗  坐标网格"; color: t.textDim; font.pixelSize: 13; renderType: Text.NativeRendering }
                    }
                    RowLayout { spacing: 6; Layout.alignment: Qt.AlignVCenter
                        CheckBox { id: autoFollowChk; checked: true
                            indicator: Rectangle { implicitWidth: 36; implicitHeight: 20; radius: 10; color: autoFollowChk.checked ? t.accent : "#1c2848"; border.color: autoFollowChk.checked ? t.accent : t.borderSoft
                                Rectangle { x: autoFollowChk.checked ? 18 : 2; width: 16; height: 16; radius: 8; color: "#fff"; anchors.verticalCenter: parent.verticalCenter; Behavior on x { NumberAnimation { duration: 120 } } } } }
                        Text { text: "⌕  自动跟随"; color: t.textDim; font.pixelSize: 13; renderType: Text.NativeRendering }
                    }
                }
            }

            Item { Layout.fillWidth: true; Layout.preferredHeight: 10 }

            // ── 键盘快捷键 ──
            RowLayout {
                spacing: 10; Layout.topMargin: 6; Layout.bottomMargin: 4; Layout.fillWidth: true
                Rectangle { Layout.preferredWidth: 4; Layout.preferredHeight: 16; radius: 2; color: t.accent; Layout.alignment: Qt.AlignVCenter }
                Text { text: "键盘快捷键（点击修改）"; color: t.textStrong; font.pixelSize: 14; font.bold: true; Layout.fillWidth: true; renderType: Text.NativeRendering }
            }

            // 快捷键列表
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 260
                color: t.card; radius: 8; border.color: t.borderSoft; border.width: 1
                ListView {
                    id: shortcutView
                    anchors.fill: parent; anchors.margins: 4; clip: true; spacing: 1
                    model: root.shortcutList
                    // section 顶部额外留白（紧贴上一个区段时区分明显）
                    section.criteria: ViewSection.FullString
                    section.delegate: Item {
                        id: shortcutSection
                        required property string section
                        width: shortcutView.width; height: 26
                        Rectangle {
                            anchors.fill: parent; anchors.leftMargin: 2; anchors.rightMargin: 2
                            color: "transparent"
                            Text {
                                anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter
                                text: {
                                    var cats = { sim: "▶ 模拟控制", unit: "⚔ 单元操作", view: "⌕ 视图导航" }
                                    return cats[shortcutSection.section] || ""
                                }
                                color: t.textMuted; font.pixelSize: 11; font.bold: true; renderType: Text.NativeRendering
                            }
                        }
                    }
                    delegate: Button {
                        id: shortcutButton
                        required property int index
                        required property var modelData
                        width: shortcutView.width; height: 34
                        hoverEnabled: true
                        focusPolicy: Qt.StrongFocus
                        onClicked: root.startCapture(shortcutButton.modelData.action)

                        background: Rectangle {
                            anchors.leftMargin: 2; anchors.rightMargin: 2; radius: 4
                            color: shortcutButton.down ? "#263b5f"
                                                      : (shortcutButton.hovered ? "#203653"
                                                                                : (shortcutButton.index % 2 === 0 ? "#151d30" : "#1a2338"))
                            border.color: shortcutButton.activeFocus ? t.accent : "transparent"
                            border.width: shortcutButton.activeFocus ? 1 : 0
                            Behavior on color { ColorAnimation { duration: 100 } }
                        }
                        contentItem: RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 10
                            anchors.topMargin: 4; anchors.bottomMargin: 4; spacing: 12
                            Rectangle {
                                id: keyBadge
                                Layout.preferredHeight: 24; Layout.minimumWidth: 78
                                Layout.preferredWidth: Math.max(78, keyTxt.implicitWidth + 18); radius: 6
                                color: shortcutButton.modelData.seq !== shortcutButton.modelData.defaultSeq ? "#3a2800" : t.accentSoft
                                border.color: shortcutButton.modelData.seq !== shortcutButton.modelData.defaultSeq ? "#d8a12c" : t.accent
                                border.width: 1
                                Text {
                                    id: keyTxt; anchors.centerIn: parent; text: shortcutButton.modelData.seq || "—"
                                    color: shortcutButton.modelData.seq !== shortcutButton.modelData.defaultSeq ? "#ffe09a" : "#eaf4ff"
                                    font.pixelSize: 11; font.family: "Consolas"; font.bold: true; renderType: Text.NativeRendering
                                }
                            }
                            Text {
                                text: shortcutButton.modelData.label || ""; color: t.textStrong; font.pixelSize: 12
                                Layout.fillWidth: true; elide: Text.ElideRight; renderType: Text.NativeRendering
                            }
                            Text {
                                visible: shortcutButton.modelData.seq !== shortcutButton.modelData.defaultSeq; text: "已修改"
                                color: "#ffd060"; font.pixelSize: 10; Layout.rightMargin: 2; renderType: Text.NativeRendering
                            }
                        }
                    }
                }
            }

            // 重置
            RowLayout { Layout.fillWidth: true; Layout.topMargin: 4
                Item { Layout.fillWidth: true }
                Rectangle {
                    Layout.preferredHeight: 24; Layout.preferredWidth: Math.max(100, resetTxt.implicitWidth + 18); radius: 6
                    color: resetMa.containsMouse ? "#1e3050" : "transparent"
                    Behavior on color { ColorAnimation { duration: 100 } }
                    Text { id: resetTxt; anchors.centerIn: parent; text: "↺  全部重置为默认"; color: t.textMuted; font.pixelSize: 11; renderType: Text.NativeRendering }
                    MouseArea { id: resetMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: root.resetAllShortcuts()
                    }
                }
            }

            Item { Layout.fillWidth: true; Layout.preferredHeight: 8 }

            // ── 确认/取消 ──
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 46; color: "transparent"
                RowLayout {
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; spacing: 10
                    Rectangle {
                        Layout.preferredHeight: 34; Layout.preferredWidth: Math.max(80, cancelTxt.implicitWidth + 24); radius: 7
                        color: cancelMa.pressed ? "#344060" : (cancelMa.containsMouse ? "#283452" : "#222d42")
                        Behavior on color { ColorAnimation { duration: 130 } }
                        Text { id: cancelTxt; anchors.centerIn: parent; text: "✕  取消"; color: t.textDim; font.pixelSize: 13; renderType: Text.NativeRendering }
                        MouseArea { id: cancelMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.close() }
                    }
                    Rectangle {
                        Layout.preferredHeight: 34; Layout.preferredWidth: Math.max(80, confirmTxt.implicitWidth + 24); radius: 7
                        color: confirmMa.pressed ? Qt.darker(t.accent, 1.25) : (confirmMa.containsMouse ? Qt.lighter(t.accent, 1.08) : t.accent)
                        Behavior on color { ColorAnimation { duration: 130 } }
                        Text { id: confirmTxt; anchors.centerIn: parent; text: "✓  确认"; color: "#fff"; font.pixelSize: 13; font.bold: true; renderType: Text.NativeRendering }
                        MouseArea { id: confirmMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.close() }
                    }
                }
            }

            // ── 关于 ──
            RowLayout {
                Layout.fillWidth: true; Layout.preferredHeight: 36
                spacing: 8
                Rectangle { Layout.preferredWidth: 4; Layout.preferredHeight: 16; radius: 2; color: t.textMuted; Layout.alignment: Qt.AlignVCenter }
                Text { text: "⚔  兵器推演"; color: t.textStrong; font.pixelSize: 14; font.bold: true; renderType: Text.NativeRendering }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 14; color: t.borderSoft; Layout.alignment: Qt.AlignVCenter }
                Text { text: "v0.2"; color: t.accent; font.pixelSize: 12; font.bold: true; renderType: Text.NativeRendering }
                Item { Layout.fillWidth: true }
                Text { text: "Qt 6 · Quick + QML"; color: t.textMuted; font.pixelSize: 11; renderType: Text.NativeRendering }
            }

            Item { Layout.fillWidth: true; Layout.preferredHeight: 8 }
        }
    }

    // ── 按键捕获对话框 ──
    Dialog {
        id: capDialog
        width: 300; height: 170; anchors.centerIn: parent; modal: true
        standardButtons: Dialog.NoButton
        title: "请按下新快捷键..."
        background: Rectangle { color: t.bg; radius: 10; border.color: t.accent; border.width: 1.5 }
        property bool ready: false
        property string captured: ""
        // True only when the captured sequence has a non-modifier "main" key —
        // otherwise binding "Ctrl+" alone would persist a useless sequence.
        property bool hasMainKey: false
        onOpened: captureKeyScope.forceActiveFocus()
        function keyDisplayName(key) {
            var map = {}
            map[Qt.Key_Space] = "Space"; map[Qt.Key_Tab] = "Tab"
            map[Qt.Key_Left] = "Left"; map[Qt.Key_Right] = "Right"; map[Qt.Key_Up] = "Up"; map[Qt.Key_Down] = "Down"
            map[Qt.Key_Escape] = "Escape"; map[Qt.Key_Return] = "Enter"; map[Qt.Key_Enter] = "Enter"
            map[Qt.Key_Backspace] = "Backspace"; map[Qt.Key_Delete] = "Delete"
            map[Qt.Key_PageUp] = "PageUp"; map[Qt.Key_PageDown] = "PageDown"
            map[Qt.Key_Home] = "Home"; map[Qt.Key_End] = "End"
            map[Qt.Key_F1]="F1"; map[Qt.Key_F2]="F2"; map[Qt.Key_F3]="F3"; map[Qt.Key_F4]="F4"
            map[Qt.Key_F5]="F5"; map[Qt.Key_F6]="F6"; map[Qt.Key_F7]="F7"; map[Qt.Key_F8]="F8"
            map[Qt.Key_F9]="F9"; map[Qt.Key_F10]="F10"; map[Qt.Key_F11]="F11"; map[Qt.Key_F12]="F12"
            map[Qt.Key_Comma] = ","; map[Qt.Key_Period] = "."
            map[Qt.Key_Semicolon] = ";"; map[Qt.Key_Apostrophe] = "'"
            map[Qt.Key_BracketLeft] = "["; map[Qt.Key_BracketRight] = "]"
            map[Qt.Key_Minus] = "-"; map[Qt.Key_Equal] = "="
            map[Qt.Key_Slash] = "/"; map[Qt.Key_Backslash] = "\\"
            map[Qt.Key_QuoteLeft] = "`"
            if (map[key]) return map[key]
            if (key >= Qt.Key_A && key <= Qt.Key_Z) return String.fromCharCode(65 + key - Qt.Key_A)
            if (key >= Qt.Key_0 && key <= Qt.Key_9) return String.fromCharCode(48 + key - Qt.Key_0)
            if (key >= Qt.Key_F1 && key <= Qt.Key_F35) return "F" + (key - Qt.Key_F1 + 1)
            return ""
        }
        contentItem: FocusScope {
            id: captureKeyScope
            focus: capDialog.opened
            Keys.onPressed: function(event) {
                event.accepted = true
                // Esc 在捕获框内 = 取消
                if (event.key === Qt.Key_Escape) { capDialog.close(); return }
                capDialog.ready = true
                capDialog.hasMainKey = false
                var parts = []
                if (event.modifiers & Qt.ControlModifier) parts.push("Ctrl")
                if (event.modifiers & Qt.ShiftModifier) parts.push("Shift")
                if (event.modifiers & Qt.AltModifier) parts.push("Alt")
                if (event.modifiers & Qt.MetaModifier) parts.push("Meta")
                var keyName = capDialog.keyDisplayName(event.key)
                if (keyName && keyName.length > 0) {
                    parts.push(keyName)
                    capDialog.hasMainKey = true
                }
                capDialog.captured = parts.join("+")
            }

            ColumnLayout {
                spacing: 10; anchors.centerIn: parent
                Text {
                    text: "按下组合键...\n(如 Ctrl+Shift+K, Esc 取消)"
                    color: t.textDim; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; renderType: Text.NativeRendering
                }
                Text {
                    text: capDialog.captured || "等待按键..."
                    color: t.textStrong; font.pixelSize: 18; font.bold: true
                    font.family: "Consolas"; horizontalAlignment: Text.AlignHCenter; renderType: Text.NativeRendering
                }
                RowLayout {
                    spacing: 10
                    Rectangle {
                        Layout.preferredHeight: 28; Layout.preferredWidth: 70; radius: 6
                        color: clearMa.containsMouse ? Qt.darker(t.red, 0.8) : t.red
                        Text { anchors.centerIn: parent; text: "清除"; color: "#fff"; font.pixelSize: 11; renderType: Text.NativeRendering }
                        MouseArea { id: clearMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { root.finishCapture(""); capDialog.close() } }
                    }
                    Rectangle {
                        Layout.preferredHeight: 28; Layout.preferredWidth: 70; radius: 6
                        color: (capDialog.ready && capDialog.hasMainKey && capDialog.captured) ? (confirmCap.pressed ? Qt.darker(t.accent, 1.2) : t.accent) : "#3a455a"
                        Text { anchors.centerIn: parent; text: "确认"; color: (capDialog.ready && capDialog.hasMainKey && capDialog.captured) ? "#fff" : t.textMuted; font.pixelSize: 11; renderType: Text.NativeRendering }
                        MouseArea { id: confirmCap; anchors.fill: parent; hoverEnabled: true; enabled: capDialog.ready && capDialog.hasMainKey && capDialog.captured.length > 0; cursorShape: capDialog.ready && capDialog.hasMainKey ? Qt.PointingHandCursor : Qt.ArrowCursor; onClicked: { if (capDialog.ready && capDialog.hasMainKey && capDialog.captured) root.finishCapture(capDialog.captured); capDialog.close() } }
                    }
                    Rectangle {
                        Layout.preferredHeight: 28; Layout.preferredWidth: 70; radius: 6
                        color: cancelCap.containsMouse ? "#324060" : "transparent"
                        Text { anchors.centerIn: parent; text: "取消"; color: t.textDim; font.pixelSize: 11; renderType: Text.NativeRendering }
                        MouseArea { id: cancelCap; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: capDialog.close() }
                    }
                }
            }
        }
    }

    onOpened: { loadAll(); if (root.appWindow) { winW.value = root.appWindow.width; winH.value = root.appWindow.height; opacitySld.value = root.appWindow.opacity } }
    onClosed: saveToPending()
}
