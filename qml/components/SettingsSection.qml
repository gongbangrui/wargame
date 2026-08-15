pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A compact section heading plus a stable content column.  The section keeps
// spacing and hierarchy consistent between local and online settings.
Item {
    id: root
    property string title: ""
    property string iconName: "dot"
    property color accent: AppContext.signal
    default property alias content: contentColumn.data

    implicitHeight: contentColumn.implicitHeight
    Layout.fillWidth: true

    ColumnLayout {
        id: contentColumn
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 7

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            Layout.bottomMargin: 2
            spacing: 8

            Icon {
                name: root.iconName
                iconSize: 15
                iconColor: root.accent
                Layout.alignment: Qt.AlignVCenter
            }
            Text {
                text: root.title
                color: AppContext.textStrong
                font.pixelSize: 13
                font.bold: true
                renderType: Text.NativeRendering
                Layout.alignment: Qt.AlignVCenter
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: AppContext.softLine
                Layout.alignment: Qt.AlignVCenter
            }
        }
    }
}
