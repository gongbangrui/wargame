pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root

    property var controller: null
    property string selectedUnitId: ""
    property string recipientSeatId: ""
    property string currentSide: ""

    property color page
    property color ink
    property color dim
    property color panel
    property color panelAlt
    property color line
    property color cyan
    property color orange
    property color danger

    property string draftTargetId: ""
    property bool awaitingAttackTarget: false
    property string activeKind: ""
    property string textDraft: ""
    property var draftPoint: null
    property var selectedUnit: {
        if (!root.controller || !root.selectedUnitId) return ({})
        if (root.controller.unitStateRevision >= 0)
            return root.controller.unitAt(root.selectedUnitId) || ({})
        return ({})
    }
    property var targetUnit: {
        if (!root.controller || !root.draftTargetId) return ({})
        if (root.controller.unitStateRevision >= 0)
            return root.controller.unitAt(root.draftTargetId) || ({})
        return ({})
    }

    signal pointSelectionRequested()
    signal pointSelectionCancelled()

    implicitHeight: panelColumn.implicitHeight

    onSelectedUnitIdChanged: {
        root.pointSelectionCancelled()
        root._reset()
    }

    function kindColor(kind) {
        if (kind === "attack") return root.orange
        if (kind === "withdrawal") return root.danger
        if (kind === "text") return AppContext.info
        return root.cyan
    }

    function kindEnabled(kind) {
        if (!root.selectedUnitId || !root.selectedUnit.alive) return false
        if (kind === "text") return root.recipientSeatId.length > 0
        if (kind === "attack") return root.selectedUnit.kind === "attackuav"
        return root.selectedUnit.movable === true
    }

    function activateKind(kind) {
        if (!root.kindEnabled(kind)) return
        if (root.activeKind === kind) {
            if (kind === "maneuver") root.pointSelectionCancelled()
            root._reset()
            return
        }
        if (root.activeKind === "maneuver") root.pointSelectionCancelled()
        root._reset()
        root.activeKind = kind
        if (kind === "maneuver") root.pointSelectionRequested()
        if (kind === "attack") root.awaitingAttackTarget = true
    }

    function mapPointSelected(point) {
        if (root.activeKind !== "maneuver" || !point) return
        root.draftPoint = { x: Number(point.x), y: Number(point.y) }
    }

    function mapTargetSelected(unitId) {
        if (!root.awaitingAttackTarget || !unitId) return
        root.awaitingAttackTarget = false
        root.draftTargetId = unitId
    }

    function mapSelectionCanceled() {
        root._reset()
    }

    function _reset() {
        root.activeKind = ""
        root.awaitingAttackTarget = false
        root.draftTargetId = ""
        root.draftPoint = null
        root.textDraft = ""
    }

    ColumnLayout {
        id: panelColumn
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 9

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    Layout.fillWidth: true
                    text: "指挥命令"
                    color: root.ink
                    font.pixelSize: 13
                    font.bold: true
                }
                Text {
                    Layout.fillWidth: true
                    text: root.selectedUnitId
                        ? (root.selectedUnit.callsign || root.selectedUnit.id || root.selectedUnitId)
                        : "尚未选择受令单位"
                    color: root.selectedUnitId ? root.cyan : root.dim
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                Layout.preferredWidth: 8
                Layout.preferredHeight: 8
                radius: 4
                color: root.selectedUnit.alive ? AppContext.success : root.dim
                Accessible.name: root.selectedUnit.alive ? "受令单位在线" : "没有可用受令单位"
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 6
            rowSpacing: 6

            Repeater {
                model: [
                    { kind: "maneuver", label: "机动", detail: "地图选点", icon: "locate" },
                    { kind: "text", label: "命令", detail: "文字下达", icon: "send" },
                    { kind: "attack", label: "攻击", detail: "指定目标", icon: "warning" },
                    { kind: "withdrawal", label: "撤离", detail: "返回驻地", icon: "chevron-left" }
                ]

                delegate: Button {
                    id: commandTypeButton
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    enabled: root.kindEnabled(commandTypeButton.modelData.kind)
                    onClicked: root.activateKind(commandTypeButton.modelData.kind)
                    Accessible.name: commandTypeButton.modelData.label + "命令"

                    contentItem: RowLayout {
                        spacing: 8
                        Icon {
                            name: commandTypeButton.modelData.icon
                            iconSize: 19
                            iconColor: commandTypeButton.enabled
                                ? root.kindColor(commandTypeButton.modelData.kind) : root.dim
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text {
                                Layout.fillWidth: true
                                text: commandTypeButton.modelData.label
                                color: commandTypeButton.enabled ? root.ink : root.dim
                                font.pixelSize: 11
                                font.bold: true
                            }
                            Text {
                                Layout.fillWidth: true
                                text: commandTypeButton.modelData.detail
                                color: root.dim
                                font.pixelSize: 9
                            }
                        }
                        Text {
                            text: root.activeKind === commandTypeButton.modelData.kind ? "●" : ""
                            color: root.kindColor(commandTypeButton.modelData.kind)
                            font.pixelSize: 8
                        }
                    }

                    background: Rectangle {
                        radius: 4
                        color: root.activeKind === commandTypeButton.modelData.kind
                            ? Qt.rgba(root.kindColor(commandTypeButton.modelData.kind).r,
                                      root.kindColor(commandTypeButton.modelData.kind).g,
                                      root.kindColor(commandTypeButton.modelData.kind).b, 0.12)
                            : commandTypeButton.hovered && commandTypeButton.enabled ? root.panelAlt : root.page
                        border.color: root.activeKind === commandTypeButton.modelData.kind
                            ? root.kindColor(commandTypeButton.modelData.kind) : root.line
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }
                }
            }
        }

        Rectangle {
            visible: root.activeKind.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? draftColumn.implicitHeight + 18 : 0
            color: root.page
            border.color: root.activeKind ? root.kindColor(root.activeKind) : root.line
            radius: 4

            ColumnLayout {
                id: draftColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 9
                spacing: 8

                RowLayout {
                    visible: root.activeKind === "maneuver"
                    Layout.fillWidth: true
                    spacing: 8
                    Text {
                        Layout.fillWidth: true
                        text: root.draftPoint
                            ? "目标坐标  " + Math.round(root.draftPoint.x) + ", " + Math.round(root.draftPoint.y)
                            : "请在地图上选择机动目标点"
                        color: root.draftPoint ? root.ink : root.cyan
                        font.pixelSize: 10
                        font.family: root.draftPoint ? "Consolas" : ""
                        elide: Text.ElideRight
                    }
                    Button {
                        id: reselectPointButton
                        visible: root.draftPoint !== null
                        text: "重选"
                        onClicked: {
                            root.draftPoint = null
                            root.pointSelectionRequested()
                        }
                        contentItem: Text { text: reselectPointButton.text; color: root.cyan; font.pixelSize: 9; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { color: reselectPointButton.hovered ? root.panelAlt : "transparent"; border.color: root.line; radius: 4 }
                    }
                }

                Button {
                    id: confirmMoveButton
                    visible: root.activeKind === "maneuver" && root.draftPoint !== null
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    text: "确认下达机动命令"
                    onClicked: {
                        root.controller.command("moveTo", {
                            unitId: root.selectedUnitId,
                            pos: root.draftPoint
                        })
                        root._reset()
                    }
                    contentItem: Text { text: confirmMoveButton.text; color: root.page; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { color: confirmMoveButton.hovered ? Qt.lighter(root.cyan, 1.08) : root.cyan; radius: 4 }
                }

                ColumnLayout {
                    visible: root.activeKind === "attack"
                    Layout.fillWidth: true
                    spacing: 7
                    Text {
                        Layout.fillWidth: true
                        text: root.draftTargetId
                            ? "攻击目标  " + (root.targetUnit.callsign || root.targetUnit.id || root.draftTargetId)
                            : "请在地图上选择已发现的敌方单位"
                        color: root.draftTargetId ? root.ink : root.orange
                        font.pixelSize: 10
                        elide: Text.ElideRight
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Button {
                            id: reselectTargetButton
                            visible: root.draftTargetId.length > 0
                            text: "重选目标"
                            onClicked: {
                                root.draftTargetId = ""
                                root.awaitingAttackTarget = true
                            }
                            contentItem: Text { text: reselectTargetButton.text; color: root.orange; font.pixelSize: 9; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: reselectTargetButton.hovered ? root.panelAlt : "transparent"; border.color: root.line; radius: 4 }
                        }
                        Button {
                            id: confirmAttackButton
                            visible: root.draftTargetId.length > 0
                            Layout.fillWidth: true
                            Layout.preferredHeight: 32
                            text: "确认攻击"
                            onClicked: {
                                root.controller.command("assignTarget", {
                                    attackerId: root.selectedUnitId,
                                    targetId: root.draftTargetId
                                })
                                root._reset()
                            }
                            contentItem: Text { text: confirmAttackButton.text; color: root.page; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: confirmAttackButton.hovered ? Qt.lighter(root.orange, 1.08) : root.orange; radius: 4 }
                        }
                    }
                }

                ColumnLayout {
                    visible: root.activeKind === "withdrawal"
                    Layout.fillWidth: true
                    spacing: 7
                    Text {
                        Layout.fillWidth: true
                        text: "撤离将中止当前任务并返回本方驻地。"
                        color: root.danger
                        font.pixelSize: 10
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Button {
                            id: cancelWithdrawalButton
                            text: "取消"
                            onClicked: root._reset()
                            contentItem: Text { text: cancelWithdrawalButton.text; color: root.dim; font.pixelSize: 10; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: cancelWithdrawalButton.hovered ? root.panelAlt : "transparent"; border.color: root.line; radius: 4 }
                        }
                        Button {
                            id: confirmWithdrawalButton
                            Layout.fillWidth: true
                            Layout.preferredHeight: 32
                            text: "确认撤离"
                            onClicked: {
                                root.controller.command("withdraw", { unitId: root.selectedUnitId })
                                root._reset()
                            }
                            contentItem: Text { text: confirmWithdrawalButton.text; color: root.page; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: confirmWithdrawalButton.hovered ? Qt.lighter(root.danger, 1.08) : root.danger; radius: 4 }
                        }
                    }
                }

                ColumnLayout {
                    visible: root.activeKind === "text"
                    Layout.fillWidth: true
                    spacing: 7
                    TextArea {
                        id: orderInput
                        Layout.fillWidth: true
                        Layout.preferredHeight: 66
                        text: root.textDraft
                        placeholderText: "输入任务、时限或协同要求"
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        color: root.ink
                        placeholderTextColor: root.dim
                        font.pixelSize: 10
                        onTextChanged: root.textDraft = text
                        background: Rectangle { color: root.panelAlt; border.color: orderInput.activeFocus ? AppContext.info : root.line; radius: 4 }
                    }
                    Button {
                        id: sendOrderButton
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        text: "下达文字命令"
                        enabled: root.textDraft.trim().length > 0
                        onClicked: {
                            root.controller.sendUnitOrder(root.selectedUnitId, root.textDraft.trim())
                            root._reset()
                        }
                        contentItem: Text { text: sendOrderButton.text; color: sendOrderButton.enabled ? root.page : root.dim; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { color: sendOrderButton.enabled ? AppContext.info : root.line; radius: 4 }
                    }
                }
            }
        }

        Rectangle {
            visible: root.controller !== null && root.controller.lastCommandStatus.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? commandStatusRow.implicitHeight + 12 : 0
            radius: 4
            color: root.panelAlt
            border.color: root.controller && root.controller.lastCommandStatus === "rejected"
                ? root.danger : root.line

            RowLayout {
                id: commandStatusRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 6
                spacing: 7
                Rectangle {
                    Layout.preferredWidth: 7
                    Layout.preferredHeight: 7
                    radius: 4
                    color: root.controller && root.controller.lastCommandStatus === "accepted"
                        ? AppContext.success
                        : root.controller && root.controller.lastCommandStatus === "rejected"
                        ? root.danger : root.orange
                }
                Text {
                    Layout.fillWidth: true
                    text: root.controller
                        ? (root.controller.lastCommandMessage || root.controller.lastCommandStatus) : ""
                    color: root.ink
                    font.pixelSize: 9
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
