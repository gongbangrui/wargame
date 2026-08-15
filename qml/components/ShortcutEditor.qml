pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Reusable key editor.  The storage prefix is explicit so local and online
// bindings can never accidentally write each other's namespace.
Item {
    id: root
    property var controller: null
    property string storagePrefix: "shortcuts/local/"
    property bool migrateLegacy: false
    property var definitions: []
    property var entries: []
    property string captureAction: ""
    property bool loading: false
    signal shortcutChanged()

    implicitHeight: editorColumn.implicitHeight
    Layout.fillWidth: true

    function readValue(def) {
        if (!root.controller) return def.defSeq
        var value = root.controller.loadSetting(root.storagePrefix + def.action, undefined)
        if ((value === undefined || value === null) && root.migrateLegacy) {
            value = root.controller.loadSetting("shortcuts/" + def.action, def.defSeq)
            root.controller.saveSetting(root.storagePrefix + def.action, value)
        }
        return value === undefined || value === null ? def.defSeq : String(value)
    }

    function reload() {
        var result = []
        for (var i = 0; i < root.definitions.length; ++i) {
            var def = root.definitions[i]
            result.push({ action: def.action, label: def.label,
                          seq: root.readValue(def), defaultSeq: def.defSeq,
                          category: def.category || "general" })
        }
        root.entries = result
    }

    function save(action, sequence) {
        if (!root.controller) return
        root.controller.saveSetting(root.storagePrefix + action, sequence)
        root.reload()
        root.shortcutChanged()
    }

    function resetAll() {
        if (!root.controller) return
        for (var i = 0; i < root.definitions.length; ++i) {
            var def = root.definitions[i]
            root.controller.saveSetting(root.storagePrefix + def.action, def.defSeq)
        }
        root.reload()
        root.shortcutChanged()
    }

    function beginCapture(action) {
        root.captureAction = action
        captureDialog.captured = ""
        captureDialog.ready = false
        captureDialog.hasMainKey = false
        captureDialog.open()
        Qt.callLater(function() { captureScope.forceActiveFocus() })
    }

    function keyName(key) {
        var names = {}
        names[Qt.Key_Space] = "Space"; names[Qt.Key_Tab] = "Tab"
        names[Qt.Key_Left] = "Left"; names[Qt.Key_Right] = "Right"
        names[Qt.Key_Up] = "Up"; names[Qt.Key_Down] = "Down"
        names[Qt.Key_Escape] = "Escape"; names[Qt.Key_Return] = "Enter"
        names[Qt.Key_Enter] = "Enter"; names[Qt.Key_Backspace] = "Backspace"
        names[Qt.Key_Delete] = "Delete"; names[Qt.Key_PageUp] = "PageUp"
        names[Qt.Key_PageDown] = "PageDown"; names[Qt.Key_Home] = "Home"
        names[Qt.Key_End] = "End"; names[Qt.Key_Comma] = ","
        names[Qt.Key_Period] = "."; names[Qt.Key_Semicolon] = ";"
        names[Qt.Key_Apostrophe] = "'"; names[Qt.Key_BracketLeft] = "["
        names[Qt.Key_BracketRight] = "]"; names[Qt.Key_Minus] = "-"
        names[Qt.Key_Equal] = "="; names[Qt.Key_Slash] = "/"
        names[Qt.Key_Backslash] = "\\"; names[Qt.Key_QuoteLeft] = "`"
        if (names[key]) return names[key]
        if (key >= Qt.Key_A && key <= Qt.Key_Z) return String.fromCharCode(65 + key - Qt.Key_A)
        if (key >= Qt.Key_0 && key <= Qt.Key_9) return String.fromCharCode(48 + key - Qt.Key_0)
        if (key >= Qt.Key_F1 && key <= Qt.Key_F35) return "F" + (key - Qt.Key_F1 + 1)
        return ""
    }

    Component.onCompleted: root.reload()

    ColumnLayout {
        id: editorColumn
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 2

        ListView {
            id: shortcutList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(390, Math.max(42, root.entries.length * 35))
            clip: true
            model: root.entries
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded; width: 6 }
            delegate: Button {
                id: shortcutButton
                required property var modelData
                required property int index
                width: shortcutList.width
                height: 34
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                onClicked: root.beginCapture(modelData.action)
                background: Rectangle {
                    color: shortcutButton.down ? AppContext.softLine
                          : shortcutButton.hovered ? AppContext.raised
                          : (shortcutButton.index % 2 ? "#111a20" : "#0d151a")
                    border.color: shortcutButton.activeFocus ? AppContext.signal : "transparent"
                    border.width: shortcutButton.activeFocus ? 1 : 0
                    radius: 4
                }
                contentItem: RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 9
                    Rectangle {
                        Layout.preferredWidth: Math.max(66, keyLabel.implicitWidth + 16)
                        Layout.preferredHeight: 24
                        radius: 4
                        color: shortcutButton.modelData.seq !== shortcutButton.modelData.defaultSeq ? "#3a2a0c" : "#16463e"
                        border.color: shortcutButton.modelData.seq !== shortcutButton.modelData.defaultSeq ? AppContext.warning : AppContext.signal
                        Text {
                            id: keyLabel
                            anchors.centerIn: parent
                            text: shortcutButton.modelData.seq || "--"
                            color: shortcutButton.modelData.seq !== shortcutButton.modelData.defaultSeq ? "#ffe09a" : AppContext.text
                            font.pixelSize: 11
                            font.family: "Consolas"
                            renderType: Text.NativeRendering
                        }
                    }
                    Text {
                        text: shortcutButton.modelData.label
                        color: AppContext.text
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        renderType: Text.NativeRendering
                    }
                    Icon { name: "edit"; iconSize: 13; iconColor: AppContext.muted }
                }
                ToolTip.visible: shortcutButton.hovered
                ToolTip.text: "点击重新绑定"
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            Item { Layout.fillWidth: true }
            GhostButton {
                text: "恢复默认"
                iconName: "refresh"
                onClicked: root.resetAll()
                ToolTip.visible: hovered
                ToolTip.text: "恢复此模式的默认快捷键"
            }
        }
    }

    Dialog {
        id: captureDialog
        property bool ready: false
        property bool hasMainKey: false
        property string captured: ""
        width: Math.min(340, root.width - 24)
        height: 190
        anchors.centerIn: parent
        modal: true
        standardButtons: Dialog.NoButton
        title: "绑定快捷键"
        background: Rectangle { color: AppContext.page; radius: 6; border.color: AppContext.signal; border.width: 1 }

        contentItem: FocusScope {
            id: captureScope
            focus: captureDialog.opened
            Keys.onPressed: function(event) {
                event.accepted = true
                if (event.key === Qt.Key_Escape) { captureDialog.close(); return }
                var parts = []
                if (event.modifiers & Qt.ControlModifier) parts.push("Ctrl")
                if (event.modifiers & Qt.ShiftModifier) parts.push("Shift")
                if (event.modifiers & Qt.AltModifier) parts.push("Alt")
                if (event.modifiers & Qt.MetaModifier) parts.push("Meta")
                var key = root.keyName(event.key)
                captureDialog.hasMainKey = key.length > 0
                if (captureDialog.hasMainKey) parts.push(key)
                captureDialog.captured = parts.join("+")
                captureDialog.ready = captureDialog.hasMainKey
            }
            ColumnLayout {
                anchors.centerIn: parent
                spacing: 10
                Text {
                    text: "按下组合键，Escape 取消"
                    color: AppContext.textDim
                    font.pixelSize: 12
                    Layout.alignment: Qt.AlignHCenter
                }
                Text {
                    text: captureDialog.captured || "等待按键"
                    color: AppContext.textStrong
                    font.pixelSize: 18
                    font.bold: true
                    font.family: "Consolas"
                    Layout.alignment: Qt.AlignHCenter
                }
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8
                    TonalButton {
                        text: "清除"
                        iconName: "delete"
                        base: AppContext.danger
                        onClicked: { root.save(root.captureAction, ""); captureDialog.close() }
                    }
                    TonalButton {
                        text: "确认"
                        iconName: "check"
                        enabled: captureDialog.ready
                        onClicked: { root.save(root.captureAction, captureDialog.captured); captureDialog.close() }
                    }
                    GhostButton { text: "取消"; iconName: "close"; onClicked: captureDialog.close() }
                }
            }
        }
    }
}
