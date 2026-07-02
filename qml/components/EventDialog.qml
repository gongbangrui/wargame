import QtQuick
import QtQuick.Controls.Basic

Dialog {
    id: dlg
    property string level: "info"
    property string title: ""
    property string body: ""
    signal ackClicked()
    signal rejectClicked()

    modal: true
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    contentItem: Column {
        spacing: 12
        padding: 16
        Text { text: dlg.title; font.bold: true; font.pixelSize: 18; color: "#ffffff"; renderType: Text.NativeRendering }
        Text { text: dlg.body; wrapMode: Text.WordWrap; width: 380; color: "#f3f6fb"; renderType: Text.NativeRendering }
    }
    footer: DialogButtonBox {
        Button {
            text: "确认"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: { dlg.ackClicked(); dlg.close() }
            contentItem: Text { text: parent.text; color: "#0e1116"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { color: parent.hovered ? "#5aaeff" : "#4f9dff"; radius: 4 }
        }
        Button {
            text: "忽略"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: { dlg.rejectClicked(); dlg.close() }
            contentItem: Text { text: parent.text; color: "#f3f6fb"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { color: parent.hovered ? "#2a3142" : "transparent"; border.color: "#3a455a"; radius: 4 }
        }
    }
}
