import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: btn
    property color textColor: AppContext.text
    property int radius: 4
    property string iconName: ""
    property real iconSize: 14
    focusPolicy: Qt.StrongFocus
    scale: btn.down ? 0.98 : btn.hovered ? 1.01 : 1.0
    Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

    implicitWidth: contentRow.implicitWidth + 24
    implicitHeight: Math.max(28, contentRow.implicitHeight + 12)

    background: Rectangle {
        color: !btn.enabled ? "#151d22" : btn.down ? "#1a2a31" : (btn.hovered ? AppContext.raised : "transparent")
        border.color: btn.activeFocus ? AppContext.signal : (btn.enabled ? AppContext.line : "#34444c")
        border.width: 1
        radius: btn.radius
        opacity: btn.enabled ? 1 : 0.72
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
                iconColor: btn.enabled ? btn.textColor : AppContext.muted
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: label
                text: btn.text
                color: btn.enabled ? btn.textColor : AppContext.muted
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                renderType: Text.NativeRendering
            }
        }
    }
}
