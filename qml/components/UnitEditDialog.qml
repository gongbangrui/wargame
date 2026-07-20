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
    palette.text: "#f3f6fb"
    palette.window: "#101a2a"
    palette.base: "#17263b"
    palette.alternateBase: "#1e3049"
    palette.button: "#2a4263"
    palette.buttonText: "#f3f6fb"
    palette.highlight: "#3279c4"
    palette.highlightedText: "#ffffff"
    palette.placeholderText: "#9dafc4"

    property bool editingExisting: false
    property var data: ({})

    // 当前选中的类型值（kind），用于驱动字段的可见性与约束
    property string currentKind: "commandpost"
    readonly property bool isCp:        currentKind === "commandpost"
    readonly property bool isAttack:    currentKind === "attackuav"
    readonly property bool isRecon:     currentKind === "reconuav"
    readonly property bool isGround:    currentKind === "groundscout"
    readonly property bool isJammer:    currentKind === "jammeruav"
    // 各单元类型的默认参数；切换类型时一并回填，避免用户改了一个类型却用了另一类的数值
    readonly property var _kindDefaults: ({
        "commandpost": { detectRange: 5000, attackRange: 0,    commRange: 20000, speed: 0,    maxHp: 200, attackPower: 0,   alt: 50 },
        "reconuav":    { detectRange: 8000, attackRange: 0,    commRange: 20000, speed: 80,   maxHp: 100, attackPower: 0,   alt: 3000 },
        "attackuav":   { detectRange: 4000, attackRange: 2500, commRange: 15000, speed: 100,  maxHp: 120, attackPower: 100, alt: 2000 },
        "groundscout": { detectRange: 3000, attackRange: 0,    commRange: 10000, speed: 6,    maxHp: 80,  attackPower: 0,   alt: 0 },
        "jammeruav":   { detectRange: 6000, attackRange: 0,    commRange: 20000, speed: 60,   maxHp: 80,  attackPower: 0,   alt: 4000 }
    })

    function valueOrDefault(value, fallback) {
        return value === undefined || value === null ? fallback : value
    }

    function _applyKindDefaults(kind) {
        var d = _kindDefaults[kind]
        if (!d) return
        detectSpin.value   = d.detectRange
        attackSpin.value   = d.attackRange
        commSpin.value     = d.commRange
        speedSpin.value    = d.speed
        hpSpin.value       = d.maxHp
        atkPowerSpin.value = d.attackPower
        altSpin.value      = d.alt
    }

    function openWith(d) {
        dlg.editingExisting = true
        dlg.data = JSON.parse(JSON.stringify(d))
        idField.text = dlg.data.id || ""
        callSignField.text = dlg.data.callsign || ""
        kindCombo.currentIndex = kindCombo.find(dlg.data.kind || "commandpost")
        sideCombo.currentIndex = sideCombo.find(dlg.data.side || "red")
        dlg.currentKind = kindCombo.valueAt(kindCombo.currentIndex)
        xSpin.value = valueOrDefault(dlg.data.x, 0)
        ySpin.value = valueOrDefault(dlg.data.y, 0)
        altSpin.value = valueOrDefault(dlg.data.alt, dlg._kindDefaults[dlg.currentKind].alt)
        detectSpin.value = valueOrDefault(dlg.data.detectRange, dlg._kindDefaults[dlg.currentKind].detectRange)
        attackSpin.value = valueOrDefault(dlg.data.attackRange, dlg._kindDefaults[dlg.currentKind].attackRange)
        commSpin.value = valueOrDefault(dlg.data.commRange, dlg._kindDefaults[dlg.currentKind].commRange)
        speedSpin.value = valueOrDefault(dlg.data.speed, dlg._kindDefaults[dlg.currentKind].speed)
        hpSpin.value = valueOrDefault(dlg.data.maxHp, dlg._kindDefaults[dlg.currentKind].maxHp)
        atkPowerSpin.value = valueOrDefault(dlg.data.attackPower, dlg._kindDefaults[dlg.currentKind].attackPower)
        open()
    }

    function openNew(x, y, side) {
        dlg.editingExisting = false
        var k = "groundscout"
        var def = dlg._kindDefaults[k]
        dlg.data = ({
            id: "", callsign: "新单元", kind: k, side: side || "red",
            x: x || 0, y: y || 0, alt: def.alt,
            detectRange: def.detectRange, attackRange: def.attackRange,
            commRange: def.commRange, speed: def.speed,
            maxHp: def.maxHp, attackPower: def.attackPower
        })
        callSignField.text = dlg.data.callsign
        kindCombo.currentIndex = kindCombo.find(dlg.data.kind)
        sideCombo.currentIndex = sideCombo.find(dlg.data.side)
        dlg.currentKind = kindCombo.valueAt(kindCombo.currentIndex)
        xSpin.value = dlg.data.x
        ySpin.value = dlg.data.y
        altSpin.value = dlg.data.alt
        detectSpin.value = dlg.data.detectRange
        attackSpin.value = dlg.data.attackRange
        commSpin.value = dlg.data.commRange
        speedSpin.value = dlg.data.speed
        hpSpin.value = dlg.data.maxHp
        atkPowerSpin.value = dlg.data.attackPower
        idField.text = ""
        open()
    }

    function collect() {
        var d = ({
            id: idField.text, callsign: callSignField.text,
            kind: kindCombo.valueAt(kindCombo.currentIndex),
            side: sideCombo.valueAt(sideCombo.currentIndex),
            x: xSpin.value, y: ySpin.value, alt: altSpin.value,
            detectRange: detectSpin.value, attackRange: attackSpin.value,
            commRange: commSpin.value, speed: speedSpin.value, maxHp: hpSpin.value,
            schedule: dlg.data.schedule || []
        })
        // 攻击力仅攻击单位有意义；其他类型固定为 0，避免误填
        d.attackPower = dlg.isAttack ? atkPowerSpin.value : 0
        // 攻击范围仅攻击单位可调；其他类型固定为 0
        d.attackRange = dlg.isAttack ? attackSpin.value : 0
        // 速度仅移动单位可调；CP 固定为 0
        d.speed = dlg.isCp ? 0 : speedSpin.value
        return d
    }

    background: Rectangle { color: "#101a2a"; border.color: "#49627f"; radius: 6 }

    contentItem: ColumnLayout {
        spacing: 10

        GridLayout {
            columns: 4; columnSpacing: 14; rowSpacing: 8
            Layout.fillWidth: true

            Text { text: "ID"; color: "#c3d0e1"; font.pixelSize: 11 }
            TextField {
                id: idField
                Layout.fillWidth: true; Layout.columnSpan: 3
                readOnly: dlg.editingExisting
                placeholderText: "留空自动生成"
                color: "#f3f6fb"
                background: Rectangle { color: "#17263b"; radius: 4; border.color: "#49627f" }
            }

            Text { text: "呼号"; color: "#c3d0e1"; font.pixelSize: 11 }
            TextField {
                id: callSignField
                Layout.fillWidth: true; Layout.columnSpan: 3
                color: "#f3f6fb"
                background: Rectangle { color: "#17263b"; radius: 4; border.color: "#49627f" }
            }

            Text { text: "类型"; color: "#c3d0e1"; font.pixelSize: 11 }
            ComboBox {
                id: kindCombo
                Layout.fillWidth: true
                model: [
                    { text: "指挥所",       value: "commandpost" },
                    { text: "侦察无人机",   value: "reconuav" },
                    { text: "攻击无人机",   value: "attackuav" },
                    { text: "地面侦察分队", value: "groundscout" },
                    { text: "电子干扰机",   value: "jammeruav" }
                ]
                textRole: "text"; valueRole: "value"
                function find(v) { for (var i = 0; i < model.length; i++) if (model[i].value === v) return i; return 0 }
                function valueAt(idx) { return idx >= 0 && idx < model.length ? model[idx].value : "commandpost" }
                onActivated: function(idx) {
                    dlg.currentKind = model[idx].value
                    // 切换类型时，自动回填该类型的默认参数（用户可继续微调）
                    dlg._applyKindDefaults(dlg.currentKind)
                }
            }
            Text { text: "阵营"; color: "#c3d0e1"; font.pixelSize: 11 }
            ComboBox {
                id: sideCombo
                Layout.fillWidth: true
                model: [ { text: "红方", value: "red" }, { text: "蓝方", value: "blue" } ]
                textRole: "text"; valueRole: "value"
                function find(v) { for (var i = 0; i < model.length; i++) if (model[i].value === v) return i; return 0 }
            }

            Text { text: "X (米)"; color: "#c3d0e1"; font.pixelSize: 11 }
            SpinBox { id: xSpin; Layout.fillWidth: true; from: -100000; to: 100000; stepSize: 100; editable: true }
            Text { text: "Y (米)"; color: "#c3d0e1"; font.pixelSize: 11 }
            SpinBox { id: ySpin; Layout.fillWidth: true; from: -100000; to: 100000; stepSize: 100; editable: true }

            Text { text: "海拔"; color: "#c3d0e1"; font.pixelSize: 11 }
            SpinBox { id: altSpin; Layout.fillWidth: true; from: 0; to: 20000; stepSize: 50; editable: true }
            Text { text: "探测 m"; color: "#c3d0e1"; font.pixelSize: 11 }
            SpinBox { id: detectSpin; Layout.fillWidth: true; from: 0; to: 100000; stepSize: 100; editable: true }

            // 攻击范围与攻击力仅攻击单位显示；其他类型在此格隐藏
            Text {
                text: "攻击 m"
                color: dlg.isAttack ? "#c3d0e1" : "#71839a"
                font.pixelSize: 11
                visible: dlg.isAttack
            }
            SpinBox {
                id: attackSpin; Layout.fillWidth: true
                from: 0; to: 100000; stepSize: 100; editable: true
                visible: dlg.isAttack
            }
            Text {
                text: dlg.isAttack ? "通信 m" : (dlg.isCp ? "通信 m" : "")
                color: "#c3d0e1"; font.pixelSize: 11
                visible: !dlg.isAttack
            }
            // 当攻击范围被隐藏时，让通信范围占据第 3 列
            SpinBox {
                id: commSpin; Layout.fillWidth: true
                from: 0; to: 100000; stepSize: 100; editable: true
            }

            // 速度：CP 不可移动 → 显示只读 0；其他类型可调
            Text { text: "速度 m/s"; color: dlg.isCp ? "#71839a" : "#c3d0e1"; font.pixelSize: 11 }
            SpinBox {
                id: speedSpin; Layout.fillWidth: true
                from: 0; to: 1000; stepSize: 1; editable: true
                enabled: !dlg.isCp
            }
            Text { text: "HP 上限"; color: "#c3d0e1"; font.pixelSize: 11 }
            SpinBox { id: hpSpin; Layout.fillWidth: true; from: 1; to: 10000; stepSize: 10; editable: true }

            // 攻击力：仅攻击无人机可见；其他类型固定为 0 且不显示
            Text {
                text: "攻击力 (单次伤害)"
                color: dlg.isAttack ? "#c3d0e1" : "#71839a"
                font.pixelSize: 11
                visible: dlg.isAttack
            }
            SpinBox {
                id: atkPowerSpin; Layout.fillWidth: true
                from: 0; to: 10000; stepSize: 10; editable: true
                visible: dlg.isAttack
            }
            // 占位：当攻击力隐藏时仍保持 4 列对齐
            Text { visible: !dlg.isAttack; text: "" }
            Item { visible: !dlg.isAttack }
        }
    }

    footer: DialogButtonBox {
        TonalButton {
            text: "保存"
            base: "#4f9dff"
            onClicked: {
                var d = dlg.collect()
                if (!d.callsign) return
                if (dlg.editingExisting) {
                    d.id = idField.text || dlg.data.id
                }
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
