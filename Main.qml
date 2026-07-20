import QtQuick
import QtQuick.Controls.Basic
import "qml" as App

ApplicationWindow {
    id: window
    width: 1360
    height: 860
    minimumWidth: 900
    minimumHeight: 620
    property real uiScale: Math.max(0.9, Math.min(1.25, Math.min(width / 1360, height / 860)))
    visible: true
    title: qsTr("兵器推演 beta")
    color: "#090d16"

    // 这两个对象由 C++ 根上下文注入，无法由静态 QML 类型系统解析。
    // qmllint disable unqualified
    App.SimulationRoot { anchors.fill: parent; appWindow: window; controller: appController; editor: scenarioEditor }
    // qmllint enable unqualified
}
