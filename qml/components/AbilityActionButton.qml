pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

Item {
    id: root

    property var abilityData: ({})
    property string iconName: "countermeasure"
    property string actionLabel: "技能"
    property color accent: AppContext.signal
    property bool actionVisible: true
    property bool actionAllowed: false

    readonly property real cooldownRemaining: Math.max(0,
        Number(root.abilityData.cooldownRemaining || 0))
    readonly property real cooldownSeconds: Math.max(0,
        Number(root.abilityData.cooldownSec || 0))
    readonly property real cooldownProgress: root.cooldownSeconds > 0
        ? Math.max(0, Math.min(1, root.cooldownRemaining / root.cooldownSeconds)) : 0
    readonly property bool cooldownActive: root.cooldownRemaining > 0.05
    readonly property string cooldownText: root.cooldownRemaining >= 10
        ? Math.ceil(root.cooldownRemaining).toFixed(0)
        : root.cooldownRemaining.toFixed(1)

    signal clicked()

    implicitWidth: 48
    implicitHeight: 42
    visible: root.actionVisible
    Accessible.name: root.actionLabel

    GhostButton {
        id: actionButton
        anchors.fill: parent
        text: ""
        iconName: root.iconName
        iconSize: 23
        textColor: root.accent
        enabled: root.actionAllowed
        onClicked: root.clicked()
        ToolTip.visible: hovered
        ToolTip.text: root.actionAllowed
            ? root.actionLabel
            : root.cooldownActive
                ? root.actionLabel + " · 冷却 " + root.cooldownText + " 秒"
                : root.actionLabel + " · 当前不可用"
        Accessible.name: root.actionLabel
    }

    Rectangle {
        anchors.fill: parent
        z: 1
        visible: root.cooldownActive
        color: "#0a1018b8"
        border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.45)
        radius: 5
    }

    Text {
        anchors.centerIn: parent
        z: 2
        visible: root.cooldownActive
        text: root.cooldownText
        color: AppContext.textStrong
        font.pixelSize: root.cooldownRemaining >= 10 ? 12 : 11
        font.bold: true
        font.family: "Consolas"
        style: Text.Outline
        styleColor: "#070b10"
    }

    Canvas {
        id: cooldownRing
        anchors.fill: parent
        z: 3

        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            if (!root.cooldownActive) return
            ctx.save()
            ctx.strokeStyle = root.accent
            ctx.globalAlpha = 0.95
            ctx.lineWidth = 2.2
            ctx.lineCap = "round"
            ctx.beginPath()
            ctx.arc(width / 2, height / 2, Math.min(width, height) / 2 - 3,
                    -Math.PI / 2,
                    -Math.PI / 2 + Math.PI * 2 * root.cooldownProgress)
            ctx.stroke()
            ctx.restore()
        }

        Component.onCompleted: requestPaint()
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 3
        z: 4
        height: 2
        radius: 1
        color: root.cooldownActive ? root.accent : "transparent"
        opacity: 0.85
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: parent.width * (1 - root.cooldownProgress)
            radius: 1
            color: AppContext.success
        }
    }

    onCooldownProgressChanged: cooldownRing.requestPaint()
    onCooldownActiveChanged: cooldownRing.requestPaint()
    onWidthChanged: cooldownRing.requestPaint()
    onHeightChanged: cooldownRing.requestPaint()
}
