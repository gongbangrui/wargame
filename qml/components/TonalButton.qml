import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: btn
    property color base: "#2a4f86"
    property color textColor: "#f3f6fb"
    property int paddingH: 14
    property int paddingV: 6
    property int radius: 4
    property string iconName: ""
    property real iconSize: 14
    focusPolicy: Qt.StrongFocus

    implicitWidth: contentRow.implicitWidth + paddingH * 2
    implicitHeight: Math.max(30, label.implicitHeight + paddingV * 2)

    background: Rectangle {
        color: !btn.enabled ? "#263342" : btn.down ? Qt.darker(btn.base, 1.3) : (btn.hovered ? Qt.lighter(btn.base, 1.15) : btn.base)
        radius: btn.radius
        border.color: btn.activeFocus ? "#d8ecff" : "transparent"
        border.width: btn.activeFocus ? 1 : 0
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
                iconColor: btn.enabled ? btn.textColor : "#b2bfd0"
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: label
                text: btn.text
                color: btn.enabled ? btn.textColor : "#b2bfd0"
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                renderType: Text.NativeRendering
            }
        }
    }
}
