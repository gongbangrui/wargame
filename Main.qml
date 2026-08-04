import QtQuick
import QtQuick.Controls.Basic
import "qml" as App

ApplicationWindow {
    id: window
    required property var simulationController
    required property var scenarioEditor
    width: 1360
    height: 860
    minimumWidth: 900
    minimumHeight: 620
    property real uiScale: Math.max(0.9, Math.min(1.25, Math.min(width / 1360, height / 860)))
    visible: true
    title: qsTr("兵棋推演")
    color: "#080b12"
    App.SimulationRoot {
        anchors.fill: parent
        appWindow: window
        controller: window.simulationController
        editor: window.scenarioEditor
    }
}
