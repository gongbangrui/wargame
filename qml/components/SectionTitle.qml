import QtQuick
import QtQuick.Layouts

RowLayout {
    id: root
    spacing: 8
    Layout.fillWidth: true
    property string text: ""

    Rectangle {
        Layout.preferredWidth: 3; Layout.preferredHeight: 14; radius: 1.5
        color: "#4f9dff"
        Layout.alignment: Qt.AlignVCenter
    }

    Text {
        text: root.text
        color: "#c2cad8"
        font.pixelSize: 11
        font.bold: true
        font.letterSpacing: 1
        renderType: Text.NativeRendering
        Layout.fillWidth: true
    }
}
