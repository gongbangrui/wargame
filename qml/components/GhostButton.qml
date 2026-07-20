import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: btn
    property color textColor: "#f3f6fb"
    property int radius: 4
    property string iconName: ""
    property real iconSize: 14
    focusPolicy: Qt.StrongFocus

    implicitWidth: contentRow.implicitWidth + 24
    implicitHeight: Math.max(28, contentRow.implicitHeight + 12)

    background: Rectangle {
        color: !btn.enabled ? "#151d29" : btn.down ? "#1e2a3d" : (btn.hovered ? "#182235" : "transparent")
        border.color: btn.activeFocus ? "#4090ff" : (btn.enabled ? "#2a3a56" : "#34445a")
        border.width: 1
        radius: btn.radius
        Behavior on color { ColorAnimation { duration: 150 } }
    }

    contentItem: Item {
        implicitWidth: contentRow.implicitWidth
        implicitHeight: contentRow.implicitHeight
        Row {
            id: contentRow
            anchors.centerIn: parent
            spacing: btn.iconName ? 6 : 0
            Icon {
                visible: btn.iconName !== ""
                name: btn.iconName
                iconSize: btn.iconSize
                iconColor: btn.enabled ? btn.textColor : "#9aa9bd"
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: label
                text: btn.text
                color: btn.enabled ? btn.textColor : "#9aa9bd"
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                renderType: Text.NativeRendering
            }
        }
    }
}
