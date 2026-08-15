pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: control

    property color surface: AppContext.raised
    property color borderColor: AppContext.line
    property color accentColor: AppContext.signal
    property int controlHeight: 32

    implicitHeight: controlHeight
    color: AppContext.text
    placeholderTextColor: AppContext.muted
    selectByMouse: true
    selectionColor: AppContext.signal
    selectedTextColor: AppContext.page
    leftPadding: 9
    rightPadding: 9
    topPadding: 0
    bottomPadding: 0
    font.pixelSize: 10
    focusPolicy: Qt.StrongFocus

    background: Rectangle {
        implicitHeight: control.controlHeight
        color: control.enabled ? control.surface : AppContext.page
        border.color: control.activeFocus ? control.accentColor : control.borderColor
        border.width: control.activeFocus ? 1.5 : 1
        radius: 4
        opacity: control.enabled ? 1 : 0.65
        Behavior on border.color { ColorAnimation { duration: 120 } }
    }
}
