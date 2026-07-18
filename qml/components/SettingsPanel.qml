import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: root
    title: "设置"
    modal: true
    anchors.centerIn: parent
    width: 580
    height: 560
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

    signal connectionRequested()

    function reloadShortcutsFromSettings() {
        var result = []
        for (var i = 0; i < shortcutDefs.length; i++) {
            var def = shortcutDefs[i]
            result.push({
                action: def.action,
                label: def.label,
                seq: controller.loadSetting("shortcuts/" + def.action, def.defSeq),
                defaultSeq: def.defSeq,
                category: def.category
            })
        }
        shortcutList = result
    }

    function saveToPending() {
        controller.saveSetting("window/width", winW.value)
        controller.saveSetting("window/height", winH.value)
        controller.saveSetting("window/opacity", opacitySld.value)
        controller.saveSetting("sim/defaultView", defView.selValue)
        controller.saveSetting("sim/defaultSpeed", defaultSpeedCombo.selValue)
        controller.saveSetting("sim/showMinimap", showMinimapChk.checked)
        controller.saveSetting("sim/showGrid", showGridChk.checked)
        controller.saveSetting("sim/autoFollowFocused", autoFollowChk.checked)
    }

    function loadAll() {
        winW.value = controller.loadSetting("window/width", 1360)
        winH.value = controller.loadSetting("window/height", 860)
        opacitySld.value = controller.loadSetting("window/opacity", 1.0)
        var dv = controller.loadSetting("sim/defaultView", "commandpost-red")
        for (var i = 0; i < defView.model.count; i++) {
            if (defView.model.get(i).value === dv) { defView.currentIndex = i; break }
        }
        var ds = controller.loadSetting("sim/defaultSpeed", 1)
        for (var j = 0; j < defaultSpeedCombo.model.count; j++) {
            if (defaultSpeedCombo.model.get(j).value === ds) { defaultSpeedCombo.currentIndex = j; break }
        }
        showMinimapChk.checked    = controller.loadSetting("sim/showMinimap", true)
        showGridChk.checked       = controller.loadSetting("sim/showGrid", false)
        autoFollowChk.checked     = controller.loadSetting("sim/autoFollowFocused", true)
        reloadShortcutsFromSettings()
    }

    function applyWindowSize() {
        if (typeof window !== "undefined") {
            window.width = winW.value
            window.height = winH.value
        }
    }

    function resetAllShortcuts() {
        for (var i = 0; i < shortcutDefs.length; i++) {
            var def = shortcutDefs[i]
            controller.saveSetting("shortcuts/" + def.action, def.defSeq)
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
            controller.saveSetting("shortcuts/" + captureTargetAction, newSeq)
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
        property color bg:          "#080b14"
        property color card:        "#0c1120"
        property color border:      "#1e2d4a"
        property color borderSoft:  "#17213a"
        property color text:        "#e8edf5"
        property color textStrong:  "#ffffff"
        property color textDim:     "#bcc8de"
        property color textMuted:   "#8896b8"
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
        clip: true; contentWidth: availableWidth; contentHeight: body.implicitHeight + 70
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded; width: 6 }

        ColumnLayout {
            id: body
            anchors.left: parent.left; anchors.right: parent.right
            anchors.leftMargin: 22; anchors.rightMargin: 22; anchors.topMargin: 8
            spacing: 0

            // ── 联网 ──
            RowLayout {
                spacing: 10; Layout.topMargin: 6; Layout.bottomMargin: 6; Layout.fillWidth: true
                Rectangle { Layout.preferredWidth: 4; Layout.preferredHeight: 16; radius: 2; color: t.green; Layout.alignment: Qt.AlignVCenter }
                Text { text: "联网"; color: t.textStrong; font.pixelSize: 14; font.bold: true; renderType: Text.NativeRendering }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: t.borderSoft; Layout.alignment: Qt.AlignVCenter }
            }

            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 82
                color: t.card; radius: 8; border.color: t.borderSoft; border.width: 1
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16; spacing: 12
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 3
                        Text {
                            text: controller.sessionMode === "local" ? "本地推演"
                                  : controller.connectionState === "connected" ? "已连接服务器" : "联网模式"
                            color: controller.connectionState === "connected" ? t.green : t.textStrong
                            font.pixelSize: 13; font.bold: true; renderType: Text.NativeRendering
                        }
                        Text {
                            text: controller.sessionMode === "local" ? "本机权威推演"
                                  : (controller.connectedRole ? (controller.connectedRole + " / " + (controller.connectedSide || "无阵营"))
                                                             : controller.connectionStatus)
                            color: t.textMuted; font.pixelSize: 11; elide: Text.ElideRight
                            Layout.fillWidth: true; renderType: Text.NativeRendering
                        }
                    }
                    Button {
                        text: controller.sessionMode === "local" ? "连接服务器" : "重新连接"
                        onClicked: {
                            root.close()
                            Qt.callLater(function() { root.connectionRequested() })
                        }
                    }
                    Button {
                        visible: controller.sessionMode === "remote"
                        text: "本地模式"
                        onClicked: controller.switchToLocalMode()
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

            // 窗口尺寸
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 52
                color: t.card; radius: 8; border.color: t.borderSoft; border.width: 1
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16; spacing: 12
                    RowLayout {
                        spacing: 8; Layout.preferredWidth: 140; Layout.alignment: Qt.AlignVCenter
                        Text { text: "⎕"; color: t.accent; font.pixelSize: 13; opacity: 0.85; renderType: Text.NativeRendering }
                        Text { text: "窗口尺寸"; color: t.textDim; font.pixelSize: 13; renderType: Text.NativeRendering }
                    }
                    SpinBox {
                        id: winW; from: 800; to: 3840; stepSize: 40; editable: false
                        Layout.preferredWidth: 78; Layout.alignment: Qt.AlignVCenter
                        background: Rectangle { color: t.bg; border.color: t.borderSoft; radius: 5 }
                        contentItem: Text { text: winW.value; color: t.text; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; renderType: Text.NativeRendering }
                    }
                    Text { text: "×"; color: t.textMuted; font.pixelSize: 14; renderType: Text.NativeRendering }
                    SpinBox {
                        id: winH; from: 600; to: 2160; stepSize: 40; editable: false
                        Layout.preferredWidth: 78; Layout.alignment: Qt.AlignVCenter
                        background: Rectangle { color: t.bg; border.color: t.borderSoft; radius: 5 }
                        contentItem: Text { text: winH.value; color: t.text; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; renderType: Text.NativeRendering }
                    }
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        Layout.preferredHeight: 30; Layout.preferredWidth: Math.max(60, applyTxt.implicitWidth + 16); radius: 7
                        Layout.alignment: Qt.AlignVCenter
                        color: applyMa.pressed ? Qt.darker(t.accent, 1.25) : (applyMa.containsMouse ? Qt.lighter(t.accent, 1.08) : t.accent)
                        Behavior on color { ColorAnimation { duration: 130 } }
                        Text { id: applyTxt; anchors.centerIn: parent; text: "✓  应用"; color: "#fff"; font.pixelSize: 12; font.bold: true; renderType: Text.NativeRendering }
                        MouseArea { id: applyMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: applyWindowSize() }
                    }
                }
            }

            // 窗口透明度
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 46
                color: t.card; radius: 8; border.color: t.borderSoft; border.width: 1
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16; spacing: 12
                    RowLayout {
                        spacing: 8; Layout.preferredWidth: 140; Layout.alignment: Qt.AlignVCenter
                        Text { text: "◐"; color: t.accent; font.pixelSize: 13; opacity: 0.85; renderType: Text.NativeRendering }
                        Text { text: "窗口透明度"; color: t.textDim; font.pixelSize: 13; renderType: Text.NativeRendering }
                    }
                    Slider {
                        id: opacitySld; from: 0.3; to: 1.0; value: 1.0; stepSize: 0.05
                        Layout.fillWidth: true; Layout.alignment: Qt.AlignVCenter
                        background: Rectangle {
                            x: opacitySld.leftPadding; y: opacitySld.topPadding + opacitySld.availableHeight / 2 - 2
                            implicitWidth: 200; implicitHeight: 4; radius: 2; color: t.borderSoft
                            Rectangle { width: opacitySld.visualPosition * parent.width; height: parent.height; radius: 2; color: t.accent }
                        }
                        handle: Rectangle { width: 16; height: 16; radius: 8; color: "#fff"; border.color: t.accent; border.width: 2 }
                        onValueChanged: { if (typeof window !== "undefined") window.opacity = value }
                    }
                    Text {
                        text: Math.round(opacitySld.value * 100) + "%"
                        color: t.textDim; font.pixelSize: 13
                        Layout.preferredWidth: 38; Layout.alignment: Qt.AlignVCenter
                        horizontalAlignment: Text.AlignRight; renderType: Text.NativeRendering
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
                        width: shortcutView.width; height: 26
                        Rectangle {
                            anchors.fill: parent; anchors.leftMargin: 2; anchors.rightMargin: 2
                            color: "transparent"
                            Text {
                                anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter
                                text: {
                                    var cats = { sim: "▶ 模拟控制", unit: "⚔ 单元操作", view: "⌕ 视图导航" }
                                    return cats[section] || ""
                                }
                                color: t.textMuted; font.pixelSize: 11; font.bold: true; renderType: Text.NativeRendering
                            }
                        }
                    }
                    delegate: Item {
                        width: shortcutView.width; height: 34
                        Rectangle {
                            anchors.fill: parent; anchors.leftMargin: 2; anchors.rightMargin: 2
                            radius: 4
                            color: index % 2 === 0 ? "#151d30" : "#1a2338"
                            Behavior on color { ColorAnimation { duration: 120 } }
                        }
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 10
                            anchors.topMargin: 4; anchors.bottomMargin: 4
                            spacing: 12
                            // Key badge — 内层文本用 Layout.preferredWidth 让宽度跟随文本增长
                            Rectangle {
                                id: keyBadge
                                Layout.preferredHeight: 24
                                Layout.minimumWidth: 78
                                Layout.preferredWidth: Math.max(78, keyTxt.implicitWidth + 18)
                                radius: 6
                                color: modelData.seq !== modelData.defaultSeq ? "#3a2800" : t.accentSoft
                                border.color: modelData.seq !== modelData.defaultSeq ? "#c08020" : t.accent
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: 150 } }
                                Text {
                                    id: keyTxt; anchors.centerIn: parent
                                    text: modelData.seq || "—"
                                    color: modelData.seq !== modelData.defaultSeq ? "#ffd060" : "#e3f0ff"
                                    font.pixelSize: 11; font.family: "Consolas"; font.bold: true
                                    renderType: Text.NativeRendering
                                }
                            }
                            Text {
                                text: modelData.label || ""; color: t.textDim; font.pixelSize: 12
                                Layout.fillWidth: true; elide: Text.ElideRight
                                renderType: Text.NativeRendering
                            }
                            Text {
                                visible: modelData.seq !== modelData.defaultSeq
                                text: "已修改"
                                color: "#e0a030"; font.pixelSize: 10
                                Layout.rightMargin: 2; renderType: Text.NativeRendering
                            }
                        }
                        MouseArea {
                            anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: { root.startCapture(modelData.action) }
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

    onOpened: { loadAll(); if (typeof window !== "undefined") { winW.value = window.width; winH.value = window.height; opacitySld.value = window.opacity } }
    onClosed: saveToPending()
}
