import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: btn
    // 第6行：将颜色属性名从 text 改为 textColor，避免和父类final属性冲突
    property color textColor: "#f3f6fb"
    property int radius: 4

    implicitWidth: label.implicitWidth + 24
    implicitHeight: label.implicitHeight + 12

    background: Rectangle {
        color: btn.down ? "#2a3142" : (btn.hovered ? "#222838" : "transparent")
        border.color: "#3a455a"
        border.width: 1
        radius: btn.radius
    }

    contentItem: Text {
        id: label
        // 这一行不变：绑定父类原生的按钮文本
        text: btn.text
        // 第21行：颜色引用修改后的 textColor 属性
        color: btn.textColor
        font.pixelSize: 12
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        renderType: Text.NativeRendering
    }
}