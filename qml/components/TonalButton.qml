import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: btn
    property color base: AppContext.signal
    property color textColor: AppContext.text
    property int paddingH: 14
    property int paddingV: 6
    property int radius: 4
    property string iconName: ""
    property real iconSize: 14
    focusPolicy: Qt.StrongFocus
    scale: btn.down ? 0.98 : btn.hovered ? 1.01 : 1.0
    Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

    implicitWidth: contentRow.implicitWidth + paddingH * 2
    implicitHeight: Math.max(30, label.implicitHeight + paddingV * 2)

    background: Rectangle {
        color: !btn.enabled ? "#26343a" : btn.base
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: !btn.enabled ? "#26343a" : btn.down ? Qt.darker(btn.base, 1.22) : btn.base }
            GradientStop { position: 1.0; color: !btn.enabled ? "#26343a" : btn.down ? Qt.darker(AppContext.info, 1.22) : btn.hovered ? Qt.lighter(AppContext.info, 1.08) : AppContext.info }
        }
        radius: btn.radius
        border.color: btn.activeFocus ? AppContext.text : "transparent"
        border.width: btn.activeFocus ? 1 : 0
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
