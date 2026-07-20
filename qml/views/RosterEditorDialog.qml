pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"

Dialog {
    id: root
    property var controller: null
    property var editor: null
    modal: true
    anchors.centerIn: parent
    width: parent ? Math.min(1100, Math.max(320, parent.width - 32)) : 1100
    height: parent ? Math.min(720, Math.max(480, parent.height - 32)) : 720
    title: root.controller.userRole === "red" ? "红方初始阵容" : "蓝方初始阵容"
    standardButtons: Dialog.NoButton
    palette.text: "#f3f6fb"
    palette.window: "#090e16"
    palette.base: "#162238"
    palette.alternateBase: "#1d2d45"
    palette.button: "#263b59"
    palette.buttonText: "#f3f6fb"
    palette.highlight: "#2d72bd"
    palette.highlightedText: "#ffffff"
    palette.placeholderText: "#91a4ba"
    background: Rectangle { color: "#090e16"; border.color: "#2a3748"; radius: 7 }
    header: Rectangle {
        height: 52; color: "#111923"
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: "#2a3748" }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 18; anchors.rightMargin: 12
            Text { text: root.title; color: "#f1f4f8"; font.pixelSize: 16; font.bold: true; Layout.fillWidth: true }
            Text { text: "推演开始后自动锁定"; color: "#aab9cc"; font.pixelSize: 11 }
            GhostButton { text: "完成"; onClicked: root.close() }
        }
    }
    contentItem: ScenarioEditorView { controller: root.controller; editor: root.editor;
        anchors.fill: parent
        restrictedSide: root.controller.userRole
        rosterMode: true
    }
}
