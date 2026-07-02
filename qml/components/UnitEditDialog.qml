import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: dlg
    title: editingExisting ? "编辑单元参数" : "新增单元"
    modal: true
    anchors.centerIn: parent
    width: 480
    standardButtons: Dialog.NoButton

    property bool editingExisting: false
    property var data: ({})

    function openWith(d) {
        dlg.editingExisting = true
        dlg.data = JSON.parse(JSON.stringify(d))
        callSignField.text = dlg.data.callsign || ""
        kindCombo.currentIndex = kindCombo.find(dlg.data.kind || "commandpost")
        sideCombo.currentIndex = sideCombo.find(dlg.data.side || "red")
        xSpin.value = dlg.data.x || 0
        ySpin.value = dlg.data.y || 0
        altSpin.value = dlg.data.alt || 0
        detectSpin.value = dlg.data.detectRange || 5000
        attackSpin.value = dlg.data.attackRange || 1500
        commSpin.value = dlg.data.commRange || 20000
        speedSpin.value = dlg.data.speed || 50
        hpSpin.value = dlg.data.maxHp || 100
        idField.text = dlg.data.id || ""
        open()
    }

    function openNew(x, y, side) {
        dlg.editingExisting = false
        dlg.data = ({
            id: "", callsign: "新单元", kind: "groundscout", side: side || "red",
            x: x || 0, y: y || 0, alt: 0,
            detectRange: 5000, attackRange: 1500, commRange: 20000, speed: 50, maxHp: 100
        })
        callSignField.text = dlg.data.callsign
        kindCombo.currentIndex = kindCombo.find(dlg.data.kind)
        sideCombo.currentIndex = sideCombo.find(dlg.data.side)
        xSpin.value = dlg.data.x
        ySpin.value = dlg.data.y
        altSpin.value = dlg.data.alt
        detectSpin.value = dlg.data.detectRange
        attackSpin.value = dlg.data.attackRange
        commSpin.value = dlg.data.commRange
        speedSpin.value = dlg.data.speed
        hpSpin.value = dlg.data.maxHp
        idField.text = ""
        open()
    }

    function collect() {
        return ({
            id: idField.text, callsign: callSignField.text,
            kind: kindCombo.valueAt(kindCombo.currentIndex),
            side: sideCombo.valueAt(sideCombo.currentIndex),
            x: xSpin.value, y: ySpin.value, alt: altSpin.value,
            detectRange: detectSpin.value, attackRange: attackSpin.value,
            commRange: commSpin.value, speed: speedSpin.value, maxHp: hpSpin.value
        })
    }

    background: Rectangle { color: "#1a1f2b"; border.color: "#3a455a"; radius: 6 }

    contentItem: ColumnLayout {
        spacing: 10

        GridLayout {
            columns: 4; columnSpacing: 14; rowSpacing: 8
            Layout.fillWidth: true

            Text { text: "ID"; color: "#8a93a6"; font.pixelSize: 11 }
            TextField {
                id: idField
                Layout.fillWidth: true; Layout.columnSpan: 3
                placeholderText: "留空自动生成"
                color: "#f3f6fb"
                background: Rectangle { color: "#0e1217"; radius: 4; border.color: "#2a3142" }
            }

            Text { text: "呼号"; color: "#8a93a6"; font.pixelSize: 11 }
            TextField {
                id: callSignField
                Layout.fillWidth: true; Layout.columnSpan: 3
                color: "#f3f6fb"
                background: Rectangle { color: "#0e1217"; radius: 4; border.color: "#2a3142" }
            }

            Text { text: "类型"; color: "#8a93a6"; font.pixelSize: 11 }
            ComboBox {
                id: kindCombo
                Layout.fillWidth: true
                model: [
                    { text: "指挥所", value: "commandpost" },
                    { text: "侦察无人机", value: "reconuav" },
                    { text: "攻击无人机", value: "attackuav" },
                    { text: "地面侦察分队", value: "groundscout" }
                ]
                textRole: "text"; valueRole: "value"
                function find(v) { for (var i = 0; i < model.length; i++) if (model[i].value === v) return i; return 0 }
            }
            Text { text: "阵营"; color: "#8a93a6"; font.pixelSize: 11 }
            ComboBox {
                id: sideCombo
                Layout.fillWidth: true
                model: [ { text: "红方", value: "red" }, { text: "蓝方", value: "blue" } ]
                textRole: "text"; valueRole: "value"
                function find(v) { for (var i = 0; i < model.length; i++) if (model[i].value === v) return i; return 0 }
            }

            Text { text: "X (米)"; color: "#8a93a6"; font.pixelSize: 11 }
            SpinBox { id: xSpin; Layout.fillWidth: true; from: -100000; to: 100000; stepSize: 100; editable: true }
            Text { text: "Y (米)"; color: "#8a93a6"; font.pixelSize: 11 }
            SpinBox { id: ySpin; Layout.fillWidth: true; from: -100000; to: 100000; stepSize: 100; editable: true }

            Text { text: "海拔"; color: "#8a93a6"; font.pixelSize: 11 }
            SpinBox { id: altSpin; Layout.fillWidth: true; from: 0; to: 20000; stepSize: 50; editable: true }
            Text { text: "探测 m"; color: "#8a93a6"; font.pixelSize: 11 }
            SpinBox { id: detectSpin; Layout.fillWidth: true; from: 0; to: 100000; stepSize: 100; editable: true }

            Text { text: "攻击 m"; color: "#8a93a6"; font.pixelSize: 11 }
            SpinBox { id: attackSpin; Layout.fillWidth: true; from: 0; to: 100000; stepSize: 100; editable: true }
            Text { text: "通信 m"; color: "#8a93a6"; font.pixelSize: 11 }
            SpinBox { id: commSpin; Layout.fillWidth: true; from: 0; to: 100000; stepSize: 100; editable: true }

            Text { text: "速度 m/s"; color: "#8a93a6"; font.pixelSize: 11 }
            SpinBox { id: speedSpin; Layout.fillWidth: true; from: 0; to: 1000; stepSize: 1; editable: true }
            Text { text: "HP 上限"; color: "#8a93a6"; font.pixelSize: 11 }
            SpinBox { id: hpSpin; Layout.fillWidth: true; from: 1; to: 10000; stepSize: 10; editable: true }
        }
    }

    footer: DialogButtonBox {
        TonalButton {
            text: "保存"
            base: "#4f9dff"
            onClicked: {
                var d = dlg.collect()
                if (!d.callsign) return
                if (dlg.editingExisting && idField.text) d.id = idField.text
                else d.id = ""
                dlg.formAccepted(d)
                dlg.close()
            }
        }
        GhostButton {
            text: "取消"
            onClicked: dlg.close()
        }
    }

    signal formAccepted(var data)
}
