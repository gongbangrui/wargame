import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: dlg
    property var unitData: ({})
    property var schedule: []
    modal: true
    title: "路径规划 - " + (unitData.callsign || "")
    anchors.centerIn: parent
    width: 880; height: 540
    standardButtons: Dialog.NoButton

    background: Rectangle { color: "#1a1f2b"; border.color: "#3a455a"; radius: 6 }

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
                color: "#8a93a6"; font.pixelSize: 11
                renderType: Text.NativeRendering
            }
            Item { Layout.fillWidth: true }
            Text {
                text: "在右侧地图上单击设置航路点的 (x, y)，左侧表格编辑 time (秒)"
                color: "#8a93a6"; font.pixelSize: 11
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
                color: "#0e1217"; border.color: "#3a455a"; radius: 6

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
                            width: scheduleList.width; implicitHeight: 36
                            color: index % 2 === 0 ? "#171c27" : "#1d2330"
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 4; spacing: 4
                                SpinBox {
                                    id: tEdit
                                    Layout.preferredWidth: 70
                                    from: 0; to: 999999; stepSize: 1; editable: true
                                    value: model.time
                                    onValueChanged: if (activeFocus) { model.time = value; refreshCanvas() }
                                }
                                SpinBox {
                                    id: xEdit
                                    Layout.preferredWidth: 110
                                    from: -100000; to: 100000; stepSize: 100; editable: true
                                    value: Math.round(model.x)
                                    onValueModified: { model.x = value; refreshCanvas() }
                                }
                                SpinBox {
                                    id: yEdit
                                    Layout.preferredWidth: 110
                                    from: -100000; to: 100000; stepSize: 100; editable: true
                                    value: Math.round(model.y)
                                    onValueModified: { model.y = value; refreshCanvas() }
                                }
                                Item { Layout.fillWidth: true }
                                GhostButton {
                                    text: "删除"
                                    onClicked: { scheduleModel.remove(index); refreshCanvas() }
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
                                refreshCanvas()
                            }
                        }
                        GhostButton {
                            text: "清空"
                            onClicked: { scheduleModel.clear(); refreshCanvas() }
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: "已配置 " + scheduleModel.count + " 个点"
                            color: "#8a93a6"; font.pixelSize: 11
                            renderType: Text.NativeRendering
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#0a1428"; border.color: "#3a455a"; radius: 6

                MapCanvas {
                    id: planMap
                    anchors.fill: parent; anchors.margins: 2
                    sideFilter: dlg.unitData.side || "red"
                    showAllSides: false
                    routes: [{ points: collectPoints(), color: "#46d29a" }]
                    onClickedMap: function(lp) {
                        var last = scheduleModel.count > 0 ? scheduleModel.get(scheduleModel.count - 1) : { time: 0 }
                        scheduleModel.append({ time: last.time + 10, x: lp.x, y: lp.y })
                        refreshCanvas()
                    }
                }

                Rectangle {
                    anchors.left: parent.left; anchors.bottom: parent.bottom
                    anchors.leftMargin: 12; anchors.bottomMargin: 12
                    color: "#1a1f2b"; border.color: "#3a455a"; radius: 4
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

    function refreshCanvas() {
        planMap.routes = [{ points: collectPoints(), color: "#46d29a" }]
    }

    footer: DialogButtonBox {
        TonalButton {
            text: "保存路径"
            base: "#46d29a"
            onClicked: {
                dlg.routeAccepted(collectPoints())
                dlg.close()
            }
        }
        GhostButton { text: "取消"; onClicked: dlg.close() }
    }

    signal routeAccepted(var points)
}
