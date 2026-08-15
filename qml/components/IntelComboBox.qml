pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

ComboBox {
    id: control

    property color surface: AppContext.raised
    property color borderColor: AppContext.line
    property color accentColor: AppContext.signal
    property int controlHeight: 32

    implicitHeight: controlHeight
    font.pixelSize: 10
    focusPolicy: Qt.StrongFocus
    leftPadding: 9
    rightPadding: 28

    contentItem: Text {
        leftPadding: control.leftPadding
        rightPadding: control.rightPadding
        text: control.currentIndex >= 0 ? control.textAt(control.currentIndex) : ""
        color: control.enabled ? AppContext.text : AppContext.muted
        font: control.font
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    indicator: Icon {
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        name: "chevron-down"
        iconSize: 13
        iconColor: control.enabled ? AppContext.muted : AppContext.line
    }

    background: Rectangle {
        implicitHeight: control.controlHeight
        color: control.enabled ? control.surface : AppContext.page
        border.color: control.activeFocus || control.popup.visible
            ? control.accentColor : control.borderColor
        border.width: control.activeFocus || control.popup.visible ? 1.5 : 1
        radius: 4
        opacity: control.enabled ? 1 : 0.65
        Behavior on border.color { ColorAnimation { duration: 120 } }
    }

    popup: Popup {
        id: popup
        y: control.height + 3
        width: Math.max(control.width, 150)
        padding: 4
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        contentItem: ListView {
            id: optionList
            implicitHeight: Math.min(230, contentHeight)
            clip: true
            focus: true
            model: control.model
            currentIndex: control.currentIndex
            boundsBehavior: Flickable.StopAtBounds
            highlightFollowsCurrentItem: true
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: ItemDelegate {
                id: optionDelegate
                required property int index
                required property var modelData
                property string optionLabel: {
                    var item = optionDelegate.modelData
                    if (item !== null && item !== undefined
                            && control.textRole
                            && typeof item === "object"
                            && item[control.textRole] !== undefined)
                        return String(item[control.textRole])
                    if (typeof item === "string")
                        return item
                    return ""
                }
                width: optionList.width
                height: 30
                text: optionDelegate.optionLabel
                highlighted: optionList.currentIndex === optionDelegate.index
                leftPadding: 8
                rightPadding: 8
                contentItem: Text {
                    text: optionDelegate.text
                    color: optionDelegate.highlighted ? AppContext.textStrong : AppContext.text
                    font.pixelSize: 10
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: optionDelegate.down || optionDelegate.highlighted
                        ? AppContext.panel : optionDelegate.hovered ? AppContext.softLine : "transparent"
                    radius: 3
                }
                onClicked: {
                    control.currentIndex = optionDelegate.index
                    control.activated(optionDelegate.index)
                    popup.close()
                    control.forceActiveFocus()
                }
            }

            Keys.onReturnPressed: {
                if (optionList.currentIndex >= 0) {
                    control.currentIndex = optionList.currentIndex
                    control.activated(optionList.currentIndex)
                    popup.close()
                    control.forceActiveFocus()
                }
            }
            Keys.onEnterPressed: {
                if (optionList.currentIndex >= 0) {
                    control.currentIndex = optionList.currentIndex
                    control.activated(optionList.currentIndex)
                    popup.close()
                    control.forceActiveFocus()
                }
            }
        }

        onOpened: {
            optionList.currentIndex = control.currentIndex
            if (optionList.currentIndex >= 0)
                optionList.positionViewAtIndex(optionList.currentIndex, ListView.Contain)
            optionList.forceActiveFocus()
        }

        background: Rectangle {
            color: AppContext.raised
            border.color: control.accentColor
            border.width: 1
            radius: 4
        }
    }
}
