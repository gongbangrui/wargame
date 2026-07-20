import QtQuick
import QtQuick.Controls.Basic

Dialog {
    id: dlg
    property string level: "info"
    property string body: ""
    signal ackClicked()
    signal rejectClicked()

    modal: true
    anchors.centerIn: parent
    // 内容宽度与 Dialog 默认隐式宽度会相互依赖，固定后避免打开时出现绑定循环。
    implicitWidth: 440
    standardButtons: Dialog.NoButton
    contentItem: Column {
        spacing: 12
        padding: 16
        Text { text: dlg.title; font.bold: true; font.pixelSize: 18; color: "#ffffff"; renderType: Text.NativeRendering }
        Text { text: dlg.body; wrapMode: Text.WordWrap; width: 380; color: "#f3f6fb"; renderType: Text.NativeRendering }
    }
    footer: DialogButtonBox {
        Button {
            id: confirmButton
            text: "确认"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: dlg.ackClicked()
            contentItem: Text { anchors.fill: parent; text: confirmButton.text; color: "#08111e"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { color: confirmButton.hovered ? "#5aaeff" : "#4f9dff"; radius: 4 }
        }
        Button {
            id: rejectButton
            text: "忽略"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: dlg.rejectClicked()
            contentItem: Text { anchors.fill: parent; text: rejectButton.text; color: "#f3f6fb"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { color: rejectButton.hovered ? "#2a3142" : "transparent"; border.color: "#3a455a"; radius: 4 }
        }
    }
}
