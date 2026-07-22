import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: dlg
    title: editingExisting ? "编辑单元参数" : "新增单元"
    modal: true
    anchors.centerIn: parent
    width: parent ? Math.max(0, Math.min(760, parent.width - 24)) : 760
    height: parent ? Math.max(0, Math.min(720, parent.height - 24)) : 720
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
    property string formError: ""
    property string forcedSide: ""

    property string currentKind: "commandpost"
    readonly property bool isCp:        currentKind === "commandpost"
    readonly property bool isAttack:    currentKind === "attackuav"
    readonly property bool isRecon:     currentKind === "reconuav"
    readonly property bool isGround:    currentKind === "groundscout"
    readonly property bool isJammer:    currentKind === "jammeruav"
    readonly property var _kindDefaults: ({
        "commandpost": { detectRange: 5000, attackRange: 0,    commRange: 20000, speed: 0,    maxHp: 200, attackPower: 0,   alt: 50, armor: 0.2, repairRate: 3, subsystemRepairRate: 0.03 },
        "reconuav":    { detectRange: 8000, attackRange: 0,    commRange: 20000, speed: 80,   maxHp: 100, attackPower: 0,   alt: 3000, armor: 0.05, repairRate: 2, subsystemRepairRate: 0.02 },
        "attackuav":   { detectRange: 4000, attackRange: 2500, commRange: 15000, speed: 100,  maxHp: 120, attackPower: 100, alt: 2000,
                          ammoCapacity: 4, initialAmmo: 4, hitProbability: 1.0, optimalRange: 1800,
                          minAttackRange: 0, cooldownSec: 1, damageMin: 80, damageMax: 120, rangeFalloff: 0.35,
                          armor: 0.1, repairRate: 2, subsystemRepairRate: 0.02,
                          fuelCapacitySec: 1800, initialFuelSec: 1800, rearmDurationSec: 12 },
        "groundscout": { detectRange: 3000, attackRange: 0,    commRange: 10000, speed: 6,    maxHp: 80,  attackPower: 0,   alt: 0, armor: 0.15, repairRate: 2, subsystemRepairRate: 0.02 },
        "jammeruav":   { detectRange: 6000, attackRange: 0,    commRange: 20000, speed: 60,   maxHp: 80,  attackPower: 0,   alt: 4000, armor: 0.05, repairRate: 2, subsystemRepairRate: 0.02 }
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
        armorSpin.value = Math.round(d.armor * 100)
        repairRateSpin.value = d.repairRate
        subsystemRepairSpin.value = Math.round(d.subsystemRepairRate * 100)
        if (kind === "attackuav") {
            ammoCapacitySpin.value = d.ammoCapacity
            initialAmmoSpin.value = d.initialAmmo
            hitProbabilitySpin.value = Math.round(d.hitProbability * 100)
            optimalRangeSpin.value = d.optimalRange
            minAttackRangeSpin.value = d.minAttackRange
            cooldownSpin.value = d.cooldownSec
            damageMinSpin.value = d.damageMin
            damageMaxSpin.value = d.damageMax
            falloffSpin.value = Math.round(d.rangeFalloff * 100)
            fuelCapacitySpin.value = d.fuelCapacitySec
            initialFuelSpin.value = d.initialFuelSec
            rearmDurationSpin.value = d.rearmDurationSec
        }
        dlg.formError = ""
    }

    function openWith(d) {
        dlg.editingExisting = true
        dlg.data = JSON.parse(JSON.stringify(d))
        idField.text = dlg.data.id || ""
        callSignField.text = dlg.data.callsign || ""
        kindCombo.currentIndex = kindCombo.find(dlg.data.kind || "commandpost")
        sideCombo.currentIndex = sideCombo.find(dlg.forcedSide || dlg.data.side || "red")
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
        armorSpin.value = Math.round(valueOrDefault(dlg.data.armor, dlg._kindDefaults[dlg.currentKind].armor) * 100)
        repairRateSpin.value = valueOrDefault(dlg.data.repairRate, dlg._kindDefaults[dlg.currentKind].repairRate)
        subsystemRepairSpin.value = Math.round(valueOrDefault(dlg.data.subsystemRepairRate, dlg._kindDefaults[dlg.currentKind].subsystemRepairRate) * 100)
        var wd = dlg._kindDefaults.attackuav
        ammoCapacitySpin.value = valueOrDefault(dlg.data.ammoCapacity, wd.ammoCapacity)
        initialAmmoSpin.value = valueOrDefault(dlg.data.initialAmmo, ammoCapacitySpin.value)
        hitProbabilitySpin.value = Math.round(valueOrDefault(dlg.data.hitProbability, wd.hitProbability) * 100)
        optimalRangeSpin.value = valueOrDefault(dlg.data.optimalRange, attackSpin.value)
        minAttackRangeSpin.value = valueOrDefault(dlg.data.minAttackRange, wd.minAttackRange)
        cooldownSpin.value = valueOrDefault(dlg.data.cooldownSec, wd.cooldownSec)
        damageMinSpin.value = valueOrDefault(dlg.data.damageMin, atkPowerSpin.value)
        damageMaxSpin.value = valueOrDefault(dlg.data.damageMax, atkPowerSpin.value)
        falloffSpin.value = Math.round(valueOrDefault(dlg.data.rangeFalloff, wd.rangeFalloff) * 100)
        fuelCapacitySpin.value = valueOrDefault(dlg.data.fuelCapacitySec, wd.fuelCapacitySec)
        initialFuelSpin.value = valueOrDefault(dlg.data.initialFuelSec, fuelCapacitySpin.value)
        rearmDurationSpin.value = valueOrDefault(dlg.data.rearmDurationSec, wd.rearmDurationSec)
        dlg.formError = ""
        open()
    }

    function openNew(x, y, side) {
        dlg.editingExisting = false
        var k = "groundscout"
        var def = dlg._kindDefaults[k]
        dlg.data = ({
            id: "", callsign: "新单元", kind: k, side: dlg.forcedSide || side || "red",
            x: x || 0, y: y || 0, alt: def.alt,
            detectRange: def.detectRange, attackRange: def.attackRange,
            commRange: def.commRange, speed: def.speed,
            maxHp: def.maxHp, attackPower: def.attackPower
        })
        callSignField.text = dlg.data.callsign
        kindCombo.currentIndex = kindCombo.find(dlg.data.kind)
        sideCombo.currentIndex = sideCombo.find(dlg.forcedSide || dlg.data.side)
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
        armorSpin.value = Math.round(def.armor * 100)
        repairRateSpin.value = def.repairRate
        subsystemRepairSpin.value = Math.round(def.subsystemRepairRate * 100)
        var wd = dlg._kindDefaults.attackuav
        ammoCapacitySpin.value = wd.ammoCapacity
        initialAmmoSpin.value = wd.initialAmmo
        hitProbabilitySpin.value = Math.round(wd.hitProbability * 100)
        optimalRangeSpin.value = wd.optimalRange
        minAttackRangeSpin.value = wd.minAttackRange
        cooldownSpin.value = wd.cooldownSec
        damageMinSpin.value = wd.damageMin
        damageMaxSpin.value = wd.damageMax
        falloffSpin.value = Math.round(wd.rangeFalloff * 100)
        fuelCapacitySpin.value = wd.fuelCapacitySec
        initialFuelSpin.value = wd.initialFuelSec
        rearmDurationSpin.value = wd.rearmDurationSec
        idField.text = ""
        dlg.formError = ""
        open()
    }

    function collect() {
        var d = ({
            id: idField.text, callsign: callSignField.text,
            kind: kindCombo.valueAt(kindCombo.currentIndex),
            side: dlg.forcedSide || sideCombo.valueAt(sideCombo.currentIndex),
            x: xSpin.value, y: ySpin.value, alt: altSpin.value,
            detectRange: detectSpin.value, attackRange: attackSpin.value,
            commRange: commSpin.value, speed: speedSpin.value, maxHp: hpSpin.value,
            armor: armorSpin.value / 100, repairRate: repairRateSpin.value,
            subsystemRepairRate: subsystemRepairSpin.value / 100,
            schedule: dlg.data.schedule || []
        })
        d.attackPower = dlg.isAttack
            ? Math.round((damageMinSpin.value + damageMaxSpin.value) / 2) : 0
        // 攻击范围仅攻击单位可调；其他类型固定为 0
        d.attackRange = dlg.isAttack ? attackSpin.value : 0
        if (dlg.isAttack) {
            d.ammoCapacity = ammoCapacitySpin.value
            d.initialAmmo = Math.min(initialAmmoSpin.value, ammoCapacitySpin.value)
            d.hitProbability = hitProbabilitySpin.value / 100
            d.optimalRange = Math.max(minAttackRangeSpin.value,
                                      Math.min(optimalRangeSpin.value, attackSpin.value))
            d.minAttackRange = Math.min(minAttackRangeSpin.value, d.optimalRange)
            d.cooldownSec = cooldownSpin.value
            d.damageMin = Math.min(damageMinSpin.value, damageMaxSpin.value)
            d.damageMax = Math.max(damageMinSpin.value, damageMaxSpin.value)
            d.rangeFalloff = falloffSpin.value / 100
            d.fuelCapacitySec = fuelCapacitySpin.value
            d.initialFuelSec = Math.min(initialFuelSpin.value, fuelCapacitySpin.value)
            d.rearmDurationSec = rearmDurationSpin.value
        }
        // 速度仅移动单位可调；CP 固定为 0
        d.speed = dlg.isCp ? 0 : speedSpin.value
        return d
    }

    background: Rectangle { color: "#0f1827"; border.color: "#3a5675"; radius: 6 }

    contentItem: ScrollView {
        id: formScroll
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            width: formScroll.availableWidth
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                Text { text: "身份与归属"; color: "#f3f6fb"; font.pixelSize: 13; font.bold: true }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#2b405a" }
                GhostButton {
                    text: "恢复默认"; iconName: "refresh"; implicitHeight: 26
                    onClicked: dlg._applyKindDefaults(dlg.currentKind)
                }
            }

            GridLayout {
                id: identityGrid
                Layout.fillWidth: true
                columns: width >= 620 ? 4 : 2
                columnSpacing: 12; rowSpacing: 7

                Text { text: "呼号"; color: "#aebed1"; font.pixelSize: 11 }
                TextField {
                    id: callSignField
                    Layout.fillWidth: true; implicitHeight: 34
                    color: "#f3f6fb"; selectByMouse: true
                    onTextEdited: dlg.formError = ""
                    background: Rectangle { color: "#17263b"; radius: 4; border.color: callSignField.activeFocus ? "#4f9dff" : "#3a5675" }
                }
                Text { text: "ID"; color: "#aebed1"; font.pixelSize: 11 }
                TextField {
                    id: idField
                    Layout.fillWidth: true; implicitHeight: 34
                    readOnly: dlg.editingExisting; placeholderText: "自动生成"
                    color: readOnly ? "#91a0b5" : "#f3f6fb"; selectByMouse: true
                    background: Rectangle { color: "#17263b"; radius: 4; border.color: idField.activeFocus ? "#4f9dff" : "#3a5675" }
                }
                Text { text: "类型"; color: "#aebed1"; font.pixelSize: 11 }
                ComboBox {
                    id: kindCombo
                    Layout.fillWidth: true; implicitHeight: 34
                    model: [
                        { text: "指挥所", value: "commandpost" },
                        { text: "侦察无人机", value: "reconuav" },
                        { text: "攻击无人机", value: "attackuav" },
                        { text: "地面侦察分队", value: "groundscout" },
                        { text: "电子干扰机", value: "jammeruav" }
                    ]
                    textRole: "text"; valueRole: "value"
                    function find(v) { for (var i = 0; i < model.length; i++) if (model[i].value === v) return i; return 0 }
                    function valueAt(idx) { return idx >= 0 && idx < model.length ? model[idx].value : "commandpost" }
                    onActivated: function(idx) {
                        dlg.currentKind = model[idx].value
                        dlg._applyKindDefaults(dlg.currentKind)
                    }
                }
                Text { text: "阵营"; visible: !dlg.forcedSide; color: "#aebed1"; font.pixelSize: 11 }
                ComboBox {
                    id: sideCombo
                    Layout.fillWidth: true; implicitHeight: 34
                    visible: !dlg.forcedSide
                    enabled: !dlg.forcedSide
                    model: [ { text: "红方", value: "red" }, { text: "蓝方", value: "blue" } ]
                    textRole: "text"; valueRole: "value"
                    function find(v) { for (var i = 0; i < model.length; i++) if (model[i].value === v) return i; return 0 }
                }
            }

            RowLayout {
                Layout.fillWidth: true; Layout.topMargin: 2
                Text { text: "平台参数"; color: "#f3f6fb"; font.pixelSize: 13; font.bold: true }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#2b405a" }
            }

            GridLayout {
                id: platformGrid
                Layout.fillWidth: true
                columns: width >= 620 ? 4 : 2
                columnSpacing: 12; rowSpacing: 7

                Text { text: "X 坐标 (m)"; color: "#aebed1"; font.pixelSize: 11 }
                SpinBox { id: xSpin; Layout.fillWidth: true; implicitHeight: 34; from: -100000; to: 100000; stepSize: 100; editable: true }
                Text { text: "Y 坐标 (m)"; color: "#aebed1"; font.pixelSize: 11 }
                SpinBox { id: ySpin; Layout.fillWidth: true; implicitHeight: 34; from: -100000; to: 100000; stepSize: 100; editable: true }
                Text { text: "海拔 (m)"; color: "#aebed1"; font.pixelSize: 11 }
                SpinBox { id: altSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 20000; stepSize: 50; editable: true }
                Text { text: "速度 (m/s)"; color: dlg.isCp ? "#6f7f94" : "#aebed1"; font.pixelSize: 11 }
                SpinBox { id: speedSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 1000; editable: true; enabled: !dlg.isCp }
                Text { text: "探测半径 (m)"; color: "#aebed1"; font.pixelSize: 11 }
                SpinBox { id: detectSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 100000; stepSize: 100; editable: true }
                Text { text: "通信半径 (m)"; color: "#aebed1"; font.pixelSize: 11 }
                SpinBox { id: commSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 100000; stepSize: 100; editable: true }
                Text { text: "生命上限"; color: "#aebed1"; font.pixelSize: 11 }
                SpinBox { id: hpSpin; Layout.fillWidth: true; implicitHeight: 34; from: 1; to: 10000; stepSize: 10; editable: true }
                Text { text: "装甲减伤 (%)"; color: "#aebed1"; font.pixelSize: 11 }
                SpinBox { id: armorSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 90; editable: true }
                Text { text: "机体维修 (HP/s)"; color: "#aebed1"; font.pixelSize: 11 }
                SpinBox { id: repairRateSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 100; editable: true }
                Text { text: "系统维修 (%/s)"; color: "#aebed1"; font.pixelSize: 11 }
                SpinBox { id: subsystemRepairSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 100; editable: true }
            }

            RowLayout {
                visible: dlg.isAttack
                Layout.fillWidth: true; Layout.topMargin: 2
                Text { text: "攻击配置"; color: "#f3f6fb"; font.pixelSize: 13; font.bold: true }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#734239" }
                Text { text: "弹药 " + initialAmmoSpin.value + " / " + ammoCapacitySpin.value; color: "#efb07d"; font.pixelSize: 11 }
            }

            GridLayout {
                id: weaponGrid
                visible: dlg.isAttack
                Layout.fillWidth: true
                columns: width >= 620 ? 4 : 2
                columnSpacing: 12; rowSpacing: 7

                Text { text: "攻击射程 (m)"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: attackSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 100000; stepSize: 100; editable: true }
                Text { text: "最佳射程 (m)"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: optimalRangeSpin; Layout.fillWidth: true; implicitHeight: 34; from: minAttackRangeSpin.value; to: attackSpin.value; stepSize: 100; editable: true }
                Text { text: "最小射程 (m)"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: minAttackRangeSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: attackSpin.value; stepSize: 100; editable: true }
                Text { text: "距离衰减 (%)"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: falloffSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 100; editable: true }
                Text { text: "命中率 (%)"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: hitProbabilitySpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 100; editable: true }
                Text { text: "射击间隔 (s)"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: cooldownSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 300; editable: true }
                Text { text: "最小伤害"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: damageMinSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 10000; stepSize: 10; editable: true }
                Text { text: "最大伤害"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: damageMaxSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 10000; stepSize: 10; editable: true }
                Text { text: "弹药容量"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: ammoCapacitySpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 1000; editable: true }
                Text { text: "初始弹药"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: initialAmmoSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: ammoCapacitySpin.value; editable: true }
                Text { text: "燃油航时 (s)"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: fuelCapacitySpin; Layout.fillWidth: true; implicitHeight: 34; from: 1; to: 86400; stepSize: 60; editable: true }
                Text { text: "初始燃油 (s)"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: initialFuelSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: fuelCapacitySpin.value; stepSize: 60; editable: true }
                Text { text: "补给耗时 (s)"; color: "#d8b2aa"; font.pixelSize: 11 }
                SpinBox { id: rearmDurationSpin; Layout.fillWidth: true; implicitHeight: 34; from: 0; to: 3600; editable: true }
            }

            Text {
                visible: dlg.formError.length > 0
                Layout.fillWidth: true
                text: dlg.formError; color: "#ff7187"; font.pixelSize: 11
            }

            SpinBox { id: atkPowerSpin; visible: false; from: 0; to: 10000 }
            Item { Layout.fillWidth: true; Layout.preferredHeight: 2 }
        }
    }

    footer: DialogButtonBox {
        TonalButton {
            text: "保存"
            iconName: "check"
            base: "#4f9dff"
            onClicked: {
                var d = dlg.collect()
                if (!d.callsign || !d.callsign.trim()) {
                    dlg.formError = "呼号不能为空"
                    callSignField.forceActiveFocus()
                    return
                }
                if (dlg.editingExisting) {
                    d.id = idField.text || dlg.data.id
                }
                dlg.formAccepted(d)
                dlg.close()
            }
        }
        GhostButton {
            text: "取消"
            iconName: "close"
            onClicked: dlg.close()
        }
    }

    signal formAccepted(var data)
}
