import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"

Item {
    id: root
    property string side: controller.focusedSide
    property var snap: controller.unitAt(controller.focusedUnitId)

    QtObject {
        id: t
        property color panel: "#1a1f2b"; property color panelAlt: "#222838"
        property color border: "#3a455a"; property color text: "#f3f6fb"
        property color textDim: "#d4dbe6"; property color muted: "#b0b8c4"
        property color accent: "#4f9dff"
    }

    RowLayout {
        anchors.fill: parent; spacing: 0
        Item {
            Layout.fillHeight: true; Layout.fillWidth: true
            MapCanvas {
                anchors.fill: parent; anchors.margins: 12
                sideFilter: "all"; focusUnitId: controller.focusedUnitId
                showAllSides: false
                onClickedMap: function(lp) {
                    controller.command("moveTo", { unitId: controller.focusedUnitId, pos: lp })
                }
            }
        }
        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: 420
            color: t.panel
            Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 1; color: t.border }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 16; spacing: 14
                Text { text: "侦察无人机"; color: t.text; font.pixelSize: 18; font.bold: true; renderType: Text.NativeRendering }
                Text { text: "仅显示该单元的探测范围与目标"; color: t.muted; font.pixelSize: 11; renderType: Text.NativeRendering }
                SectionTitle { text: "切换单元" }
                ComboBox {
                    Layout.fillWidth: true
                    model: controller.unitOptions("reconuav", root.side)
                    textRole: "callsign"; valueRole: "id"
                    onActivated: function(idx) { controller.setFocusedUnitId(model[idx].id) }
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: t.panelAlt; border.color: t.border; radius: 6
                    UnitPanel { anchors.fill: parent; anchors.margins: 12 }
                }
                SectionTitle { text: "实时消息" }
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 180
                    color: t.panelAlt; border.color: t.border; radius: 6
                    MessageLog { anchors.fill: parent; anchors.margins: 1 }
                }
            }
        }
    }
}

