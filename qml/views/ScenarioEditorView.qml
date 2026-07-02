import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"

Item {
    id: root
    property var units: controller.unitsJson().units

    QtObject {
        id: t
        property color panel: "#1a1f2b"; property color panelAlt: "#222838"
        property color border: "#3a455a"; property color text: "#f3f6fb"
        property color textDim: "#d4dbe6"; property color muted: "#b0b8c4"
        property color accent: "#4f9dff"; property color danger: "#ff4d6d"
        property color red: "#ff5566"; property color blue: "#4d9bff"
        property color success: "#46d29a"
    }

    function reload() { root.units = controller.unitsJson().units }
    Connections {
        target: controller
        function onCommandExecuted() { reload() }
    }

    RowLayout {
        anchors.fill: parent; spacing: 0

        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: 360
            color: t.panel
            Rectangle { anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 1; color: t.border }

            ColumnLayout {
                anchors.fill: parent; anchors.margins: 16; spacing: 12

                ColumnLayout {
                    spacing: 4
                    Text { text: "场景编辑器"; color: t.text; font.pixelSize: 18; font.bold: true; renderType: Text.NativeRendering }
                    Text { text: "双击地图放置新单元；右键单元删除；选中后点击编辑修改参数或规划路径"; color: t.muted; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true; renderType: Text.NativeRendering }
                }

                SectionTitle { text: "场景单元" }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: t.panelAlt; border.color: t.border; radius: 6
                    ListView {
                        id: list
                        anchors.fill: parent; anchors.margins: 4
                        clip: true; model: root.units
                        delegate: Rectangle {
                            width: list.width; implicitHeight: 40
                            color: ListView.isCurrentItem ? "#2a4f86" : (index % 2 === 0 ? "#171c27" : "#1d2330")
                            radius: 3
                            Row {
                                anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10
                                spacing: 10
                                Rectangle { width: 10; height: 10; radius: 5; anchors.verticalCenter: parent.verticalCenter
                                    color: modelData.side === "red" ? t.red : t.blue }
                                Text { text: modelData.callsign; color: t.text; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter; renderType: Text.NativeRendering }
                                Text { text: modelData.kind === "commandpost" ? "指挥所" : (modelData.kind === "reconuav" ? "侦察" : (modelData.kind === "attackuav" ? "攻击" : "地面")); color: t.muted; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter; renderType: Text.NativeRendering }
                                Item { width: 6 }
                                Rectangle {
                                    visible: modelData.kind !== "commandpost"
                                    width: 40; height: 18; radius: 4
                                    color: "#2a4f86"
                                    anchors.verticalCenter: parent.verticalCenter
                                    Text {
                                        anchors.centerIn: parent
                                        text: (modelData.schedule ? modelData.schedule.length : 0) + " 点"
                                        color: "#f3f6fb"; font.pixelSize: 10
                                        renderType: Text.NativeRendering
                                    }
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    list.currentIndex = index
                                    if (modelData.x !== undefined && modelData.y !== undefined)
                                        mapCanvas.centerOn(modelData.x, modelData.y)
                                }
                                onDoubleClicked: { list.currentIndex = index; editDialog.openWith(modelData) }
                            }
                        }
                    }
                }

                Flow {
                    Layout.fillWidth: true; spacing: 8
                    TonalButton { text: "新增"; base: t.accent; onClicked: addNew() }
                    TonalButton { text: "编辑选中"; base: "#2a4f86"; onClicked: editSelected() }
                    TonalButton {
                        text: "规划路径"
                        base: t.success
                        enabled: list.currentIndex >= 0 && root.units[list.currentIndex] && root.units[list.currentIndex].kind !== "commandpost"
                        onClicked: planRouteSelected()
                    }
                    TonalButton { text: "删除"; base: t.danger; enabled: list.currentIndex >= 0; onClicked: removeSelected() }
                    GhostButton { text: "保存"; onClicked: saveToFile() }
                    GhostButton { text: "读取"; onClicked: loadFromFile() }
                }
            }
        }

        Item {
            Layout.fillHeight: true; Layout.fillWidth: true
            MapCanvas {
                id: mapCanvas
                anchors.fill: parent; anchors.margins: 12
                sideFilter: controller.focusedSide
                showAllSides: false
                focusUnitId: list.currentIndex >= 0 && root.units[list.currentIndex] ? root.units[list.currentIndex].id : ""
                routes: visibleRoutes()
                onUnitClicked: function(uid, btn) {
                    for (var i = 0; i < root.units.length; i++) {
                        if (root.units[i].id === uid) {
                            list.currentIndex = i
                            mapCanvas.centerOn(root.units[i].x, root.units[i].y)
                            break
                        }
                    }
                    if (btn === "right") { confirmDelete.uid = uid; confirmDelete.open() }
                }
                onRightClickedMap: function(lp) { editDialog.openNew(lp.x, lp.y, controller.focusedSide) }
            }

            MouseArea {
                anchors.fill: mapCanvas
                z: 5
                acceptedButtons: Qt.LeftButton
                propagateComposedEvents: true
                onDoubleClicked: function(mouse) {
                    var lp = mapCanvas.logicalFromPixel(mouse.x, mouse.y)
                    editDialog.openNew(lp.x, lp.y, controller.focusedSide)
                }
            }

            Rectangle {
                anchors.left: parent.left; anchors.bottom: parent.bottom
                anchors.leftMargin: 24; anchors.bottomMargin: 24
                color: "#1a1f2b"; border.color: "#3a455a"; radius: 6
                implicitHeight: 36; implicitWidth: hintText.implicitWidth + 28
                Text {
                    id: hintText
                    anchors.centerIn: parent
                    text: "提示：双击地图 = 新增  ·  右键空白 = 新增  ·  右键单元 = 删除  ·  选中单元后 \"规划路径\"  ·  选中自动居中"
                    color: "#c2cad8"; font.pixelSize: 11
                    renderType: Text.NativeRendering
                }
            }
        }
    }

    function visibleRoutes() {
        var rs = []
        for (var i = 0; i < root.units.length; i++) {
            var u = root.units[i]
            if (u.kind === "commandpost") continue
            if (u.side !== controller.focusedSide) continue
            if (!u.schedule || u.schedule.length < 1) continue
            rs.push({ color: u.side === "red" ? "#ff5566" : "#4d9bff", pendingColor: "#8a93a6", points: u.schedule })
        }
        return rs
    }

    function addNew() { editDialog.openNew(mapCanvas.mapSize.w / 2, mapCanvas.mapSize.h / 2, "red") }
    function editSelected() {
        if (list.currentIndex < 0) return
        editDialog.openWith(root.units[list.currentIndex])
    }
    function planRouteSelected() {
        if (list.currentIndex < 0) return
        var u = root.units[list.currentIndex]
        if (u.kind === "commandpost") return
        var snap = controller.unitAt(u.id)
        routeDialog.openFor(snap)
    }
    function removeSelected() {
        if (list.currentIndex < 0) return
        confirmDelete.uid = root.units[list.currentIndex].id
        confirmDelete.open()
    }
    function saveToFile() {
        editor.saveJsonText("./scenarios/editing.json", JSON.stringify(controller.unitsJson(), null, 2))
        reload()
    }
    function loadFromFile() {
        var txt = editor.loadJsonText("./scenarios/editing.json")
        if (txt) {
            try {
                var obj = JSON.parse(txt)
                if (obj && obj.units) for (var i = 0; i < obj.units.length; i++) controller.upsertUnit(obj.units[i])
            } catch (e) { console.warn("parse err", e) }
        }
        reload()
    }

    UnitEditDialog {
        id: editDialog
        onFormAccepted: function(data) { controller.upsertUnit(data); reload() }
    }

    RoutePlannerDialog {
        id: routeDialog
        onRouteAccepted: function(points) {
            var cur = root.units[list.currentIndex]
            if (!cur) return
            var data = JSON.parse(JSON.stringify(cur))
            data.schedule = points
            controller.upsertUnit(data)
            controller.setUnitSchedule(cur.id, points)
            reload()
        }
    }

    Dialog {
        id: confirmDelete
        property string uid: ""
        title: "删除确认"
        modal: true; anchors.centerIn: parent
        standardButtons: Dialog.NoButton
        background: Rectangle { color: t.panel; border.color: t.border; radius: 6 }
        Column {
            anchors.margins: 16
            Text { text: "确定要删除该单元？"; color: t.text; font.pixelSize: 14; renderType: Text.NativeRendering }
        }
        footer: DialogButtonBox {
            TonalButton { text: "删除"; base: t.danger; onClicked: { if (confirmDelete.uid) { controller.removeUnit(confirmDelete.uid); reload(); confirmDelete.uid = "" } confirmDelete.close() } }
            GhostButton { text: "取消"; onClicked: confirmDelete.close() }
        }
    }
}
