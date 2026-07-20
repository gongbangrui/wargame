pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: dlg
    property var controller: null
    property var editor: null
    property var unitData: ({})
    property var schedule: []
    modal: true
    title: "路径规划 - " + (unitData.callsign || "")
    anchors.centerIn: parent
    width: 880; height: 540
    standardButtons: Dialog.NoButton
    palette.text: "#f3f6fb"
    palette.window: "#101a2a"
    palette.base: "#17263b"
    palette.alternateBase: "#1e3049"
    palette.button: "#2a4263"
    palette.buttonText: "#f3f6fb"
    palette.highlight: "#3279c4"
    palette.highlightedText: "#ffffff"
    palette.placeholderText: "#9dafc4"

    background: Rectangle { color: "#101a2a"; border.color: "#49627f"; radius: 6 }

    function openFor(unitJson) {
        dlg.unitData = unitJson || {}
        var arr = []
        if (unitJson && unitJson.schedule) {
            for (var i = 0; i < unitJson.schedule.length; i++) {
                var s = unitJson.schedule[i]
                arr.push({ time: s.time, x: s.x, y: s.y })
            }
        }
        if (arr.length === 0 && unitJson && unitJson.position) {
            arr.push({ time: 0, x: unitJson.position[0], y: unitJson.position[1] })
        }
        dlg.schedule = arr
        scheduleModel.clear()
        for (var j = 0; j < arr.length; j++) scheduleModel.append(arr[j])
        // 居中到当前单元位置
        if (unitJson && unitJson.position) {
            planMap.centerOn(unitJson.position[0], unitJson.position[1])
            planMap.zoom = 1.5
        }
        refreshCanvas()
        open()
    }

    ListModel { id: scheduleModel }

    contentItem: ColumnLayout {
        spacing: 10
        anchors.margins: 16

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: "单元: " + (dlg.unitData.callsign || "")
                color: "#ffffff"; font.bold: true; font.pixelSize: 14
                renderType: Text.NativeRendering
            }
            Text {
                text: "kind=" + (dlg.unitData.kind || "-") + " · side=" + (dlg.unitData.side || "-")
                color: "#b9c9dc"; font.pixelSize: 11
                renderType: Text.NativeRendering
            }
            Item { Layout.fillWidth: true }
            Text {
                text: "在右侧地图上单击设置航路点的 (x, y)，左侧表格编辑 time (秒)"
                color: "#b9c9dc"; font.pixelSize: 11
                renderType: Text.NativeRendering
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            Rectangle {
                Layout.preferredWidth: 380
                Layout.fillHeight: true
                color: "#101a2a"; border.color: "#49627f"; radius: 6

                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 8; spacing: 6
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: "时间 (秒)"; color: "#c2cad8"; font.pixelSize: 11; Layout.preferredWidth: 60; renderType: Text.NativeRendering }
                        Text { text: "X (米)"; color: "#c2cad8"; font.pixelSize: 11; Layout.preferredWidth: 90; renderType: Text.NativeRendering }
                        Text { text: "Y (米)"; color: "#c2cad8"; font.pixelSize: 11; Layout.preferredWidth: 90; renderType: Text.NativeRendering }
                        Item { Layout.fillWidth: true }
                    }

                    ListView {
                        id: scheduleList
                        Layout.fillWidth: true; Layout.fillHeight: true
                        clip: true
                        model: scheduleModel
                        delegate: Rectangle {
                            id: scheduleRow
                            required property int index
                            required property var time
                            width: scheduleList.width; implicitHeight: 36
                            color: scheduleRow.index % 2 === 0 ? "#1b2940" : "#22314a"
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 4; spacing: 4
                                SpinBox {
                                    id: tEdit
                                    Layout.preferredWidth: 70
                                    from: 0; to: 999999; stepSize: 1; editable: true
                                    value: scheduleRow.time
                                    onValueChanged: if (activeFocus) { scheduleModel.setProperty(scheduleRow.index, "time", value); dlg.refreshCanvas() }
                                }
                                SpinBox {
                                    id: xEdit
                                    Layout.preferredWidth: 110
                                    from: -100000; to: 100000; stepSize: 100; editable: true
                                    value: Math.round(dlg.pointAt(scheduleRow.index).x)
                                    onValueChanged: if (activeFocus) { scheduleModel.setProperty(scheduleRow.index, "x", value); dlg.refreshCanvas() }
                                }
                                SpinBox {
                                    id: yEdit
                                    Layout.preferredWidth: 110
                                    from: -100000; to: 100000; stepSize: 100; editable: true
                                    value: Math.round(dlg.pointAt(scheduleRow.index).y)
                                    onValueChanged: if (activeFocus) { scheduleModel.setProperty(scheduleRow.index, "y", value); dlg.refreshCanvas() }
                                }
                                Item { Layout.fillWidth: true }
                                GhostButton {
                                    text: "删除"
                                    onClicked: { scheduleModel.remove(scheduleRow.index); dlg.refreshCanvas() }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        TonalButton {
                            text: "添加点"
                            base: "#4f9dff"
                            onClicked: {
                                var last = scheduleModel.count > 0 ? scheduleModel.get(scheduleModel.count - 1) : { time: 0, x: dlg.unitData.position ? dlg.unitData.position[0] : 0, y: dlg.unitData.position ? dlg.unitData.position[1] : 0 }
                                scheduleModel.append({ time: last.time + 10, x: last.x, y: last.y })
                                dlg.refreshCanvas()
                            }
                        }
                        GhostButton {
                            text: "清空"
                            onClicked: { scheduleModel.clear(); dlg.refreshCanvas() }
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: "已配置 " + scheduleModel.count + " 个点"
                            color: "#b9c9dc"; font.pixelSize: 11
                            renderType: Text.NativeRendering
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#0a1428"; border.color: "#3a455a"; radius: 6

                MapCanvas { controller: dlg.controller; editor: dlg.editor;
                    id: planMap
                    anchors.fill: parent; anchors.margins: 2
                    sideFilter: dlg.unitData.side || "red"
                    showAllSides: false
                    routes: [{ points: dlg.collectPoints(), color: "#46d29a" }]
                    onClickedMap: function(lp) {
                        var last = scheduleModel.count > 0 ? scheduleModel.get(scheduleModel.count - 1) : { time: 0 }
                        scheduleModel.append({ time: last.time + 10, x: lp.x, y: lp.y })
                        dlg.refreshCanvas()
                    }
                }

                Rectangle {
                    anchors.left: parent.left; anchors.bottom: parent.bottom
                    anchors.leftMargin: 12; anchors.bottomMargin: 12
                    color: "#17263b"; border.color: "#49627f"; radius: 4
                    implicitWidth: hintText.implicitWidth + 18
                    implicitHeight: 26
                    Text {
                        id: hintText
                        anchors.centerIn: parent
                        text: "在地图上单击 = 新增航路点"
                        color: "#c2cad8"; font.pixelSize: 11
                        renderType: Text.NativeRendering
                    }
                }
            }
        }
    }

    function collectPoints() {
        var pts = []
        for (var i = 0; i < scheduleModel.count; i++) {
            var it = scheduleModel.get(i)
            pts.push({ time: it.time, x: it.x, y: it.y })
        }
        return pts
    }

    function pointAt(index) {
        return scheduleModel.get(index)
    }

    function refreshCanvas() {
        planMap.routes = [{ points: dlg.collectPoints(), color: "#46d29a" }]
    }

    footer: DialogButtonBox {
        TonalButton {
            text: "保存路径"
            base: "#46d29a"
            onClicked: {
                dlg.routeAccepted(dlg.collectPoints())
                dlg.close()
            }
        }
        GhostButton { text: "取消"; onClicked: dlg.close() }
    }

    signal routeAccepted(var points)
}
