import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: btn
    property color base: "#2a4f86"
    // 第7行：将属性名从 text 改为 textColor，避免和父类final属性冲突
    property color textColor: "#f3f6fb"
    property int paddingH: 14
    property int paddingV: 6
    property int radius: 4

    implicitWidth: label.implicitWidth + paddingH * 2
    implicitHeight: label.implicitHeight + paddingV * 2

    background: Rectangle {
        color: btn.down ? Qt.darker(btn.base, 1.3) : (btn.hovered ? Qt.lighter(btn.base, 1.15) : btn.base)
        radius: btn.radius
    }

    contentItem: Text {
        id: label
        // 这里的 text 绑定父类原生的按钮文本，保持不变
        text: btn.text
        // 颜色引用修改后的 textColor 属性
        color: btn.textColor
        font.pixelSize: 12
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        renderType: Text.NativeRendering
    }
}