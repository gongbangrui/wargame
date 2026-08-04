import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: dlg
    property string level: "info"
    property string body: ""
    readonly property color accent: level === "error" ? AppContext.danger
                                          : level === "warn" ? AppContext.warning
                                          : AppContext.info
    signal ackClicked()
    signal rejectClicked()

    modal: true
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape
    // 内容宽度与 Dialog 默认隐式宽度会相互依赖，固定后避免打开时出现绑定循环。
    implicitWidth: Math.min(440, parent ? Math.max(300, parent.width - 32) : 440)
    standardButtons: Dialog.NoButton
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: AppContext.stateMotion }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: AppContext.stateMotion; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: AppContext.fastMotion }
        NumberAnimation { property: "scale"; from: 1; to: 0.98; duration: AppContext.fastMotion }
    }
    background: Rectangle {
        color: AppContext.raised
        border.color: dlg.accent
        border.width: 1
        radius: AppContext.radius
    }
    contentItem: ColumnLayout {
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Rectangle {
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                radius: AppContext.radius
                color: Qt.rgba(dlg.accent.r, dlg.accent.g, dlg.accent.b, 0.14)
                Icon {
                    anchors.centerIn: parent
                    name: dlg.level === "info" ? "network" : "warning"
                    iconColor: dlg.accent
                    iconSize: 16
                }
            }
            Text {
                Layout.fillWidth: true
                text: dlg.title
                font.bold: true
                font.pixelSize: 16
                color: AppContext.textStrong
                elide: Text.ElideRight
                renderType: Text.NativeRendering
            }
        }
        Text {
            Layout.fillWidth: true
            text: dlg.body
            wrapMode: Text.WordWrap
            color: AppContext.text
            lineHeight: 1.25
            renderType: Text.NativeRendering
        }
    }
    footer: DialogButtonBox {
        spacing: 8
        padding: 12
        background: Rectangle { color: AppContext.panel; radius: AppContext.radius }
        Button {
            id: confirmButton
            text: "确认"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: dlg.ackClicked()
            Accessible.name: dlg.title + "：确认"
            contentItem: Text { anchors.fill: parent; text: confirmButton.text; color: AppContext.page; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { color: confirmButton.hovered ? Qt.lighter(dlg.accent, 1.08) : dlg.accent; radius: 4 }
        }
        Button {
            id: rejectButton
            text: "忽略"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: dlg.rejectClicked()
            Accessible.name: dlg.title + "：忽略"
            contentItem: Text { anchors.fill: parent; text: rejectButton.text; color: AppContext.text; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { color: rejectButton.hovered ? AppContext.raised : "transparent"; border.color: AppContext.line; radius: 4 }
        }
    }
}
