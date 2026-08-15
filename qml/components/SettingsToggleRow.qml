pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Binary settings use a real Switch so keyboard and accessibility semantics
// remain available while the row supplies the compact visual treatment.
Item {
    id: root
    property string label: ""
    property string detail: ""
    property string iconName: "dot"
    property alias checked: toggle.checked
    property alias toggleEnabled: toggle.enabled
    signal changed(bool value)

    implicitHeight: detail.length > 0 ? 50 : 42
    Layout.fillWidth: true

    RowLayout {
        anchors.fill: parent
        spacing: 10

        Icon {
            name: root.iconName
            iconSize: 15
            iconColor: root.toggleEnabled ? AppContext.signal : AppContext.muted
            Layout.alignment: Qt.AlignVCenter
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            Text {
                text: root.label
                color: root.toggleEnabled ? AppContext.text : AppContext.muted
                font.pixelSize: 12
                elide: Text.ElideRight
                Layout.fillWidth: true
                renderType: Text.NativeRendering
            }
            Text {
                visible: root.detail.length > 0
                text: root.detail
                color: AppContext.muted
                font.pixelSize: 10
                elide: Text.ElideRight
                Layout.fillWidth: true
                renderType: Text.NativeRendering
            }
        }
        Switch {
            id: toggle
            Layout.preferredWidth: 42
            Layout.preferredHeight: 24
            indicator: Rectangle {
                x: 1
                y: (toggle.height - height) / 2
                width: 40
                height: 22
                radius: 11
                color: toggle.checked ? AppContext.signal : AppContext.softLine
                border.color: toggle.checked ? AppContext.signal : AppContext.line
                border.width: 1
                Rectangle {
                    width: 16
                    height: 16
                    radius: 8
                    anchors.verticalCenter: parent.verticalCenter
                    x: toggle.checked ? parent.width - width - 3 : 3
                    color: toggle.checked ? AppContext.page : AppContext.muted
                    Behavior on x { NumberAnimation { duration: 120 } }
                }
            }
            onClicked: root.changed(checked)
        }
    }
}
