pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: dlg
    property var controller: null
    property string exportStatus: ""
    title: "回放与战报"
    modal: true
    anchors.centerIn: parent
    width: parent ? Math.max(0, Math.min(820, parent.width - 32)) : 820
    height: parent ? Math.max(0, Math.min(680, parent.height - 32)) : 680
    standardButtons: Dialog.NoButton
    background: Rectangle { color: "#0f1827"; border.color: "#3a5675"; radius: 6 }

    function seek(value) {
        if (controller && controller.seekReplay(value)) timeline.positionViewAtBeginning()
    }

    contentItem: ColumnLayout {
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Text { text: "时间轴"; color: "#f3f6fb"; font.pixelSize: 13; font.bold: true }
            Slider {
                id: replaySlider
                Layout.fillWidth: true
                from: 0
                to: Math.max(0.001, dlg.controller ? dlg.controller.replayDuration : 0.001)
                onMoved: dlg.seek(value)
                Binding on value {
                    when: !replaySlider.pressed
                    value: dlg.controller ? Math.min(replaySlider.to,
                                                     dlg.controller.simTime) : 0
                    restoreMode: Binding.RestoreBindingOrValue
                }
            }
            Text {
                text: (dlg.controller ? dlg.controller.simTime : 0).toFixed(1)
                      + " / " + (dlg.controller ? dlg.controller.replayDuration : 0).toFixed(1) + " s"
                color: "#b8c9dc"; font.pixelSize: 11; font.family: "Consolas"
            }
        }

        RowLayout {
            Layout.fillWidth: true
            GhostButton { iconName: "chevron-left"; text: "上一事件"; onClicked: dlg.controller.stepReplayEvent(-1) }
            GhostButton { iconName: "chevron-right"; text: "下一事件"; onClicked: dlg.controller.stepReplayEvent(1) }
            Item { Layout.fillWidth: true }
            TonalButton {
                text: "导出 JSON"; iconName: "download"; base: "#3279c4"
                onClicked: dlg.exportStatus = dlg.controller.exportBattleReport("", "json")
            }
            GhostButton {
                text: "导出 CSV"; iconName: "table"
                onClicked: dlg.exportStatus = dlg.controller.exportBattleReport("", "csv")
            }
        }

        Text {
            visible: dlg.exportStatus.length > 0
            Layout.fillWidth: true
            text: "已导出：" + dlg.exportStatus
            color: "#46d29a"; font.pixelSize: 11; elide: Text.ElideMiddle
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#2b405a" }

        ListView {
            id: timeline
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; spacing: 4
            model: dlg.controller ? dlg.controller.timeline : []
            delegate: Rectangle {
                id: eventRow
                required property int index
                required property var modelData
                width: timeline.width; implicitHeight: 54; radius: 4
                color: index % 2 === 0 ? "#142238" : "#172941"
                border.color: modelData.level === "warn" ? "#78464d" : "#29445f"
                RowLayout {
                    anchors.fill: parent; anchors.margins: 8; spacing: 10
                    Text {
                        text: Number(eventRow.modelData.simTime || 0).toFixed(1) + "s"
                        color: "#7fb8ff"; font.pixelSize: 11; font.family: "Consolas"
                        Layout.preferredWidth: 58
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 2
                        Text { text: eventRow.modelData.title || "-"; color: "#f3f6fb"; font.pixelSize: 12; font.bold: true }
                        Text {
                            text: eventRow.modelData.category + "  "
                                  + JSON.stringify(eventRow.modelData.details || {})
                            color: "#9dafc4"; font.pixelSize: 10
                            elide: Text.ElideRight; Layout.fillWidth: true
                        }
                    }
                    GhostButton {
                        iconName: "locate"; implicitWidth: 30; implicitHeight: 28
                        onClicked: dlg.seek(Number(eventRow.modelData.simTime || 0))
                    }
                }
            }
        }
    }

    footer: DialogButtonBox {
        GhostButton { text: "关闭"; iconName: "close"; onClicked: dlg.close() }
    }
}
