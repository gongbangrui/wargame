import QtQuick
import QtQuick.Controls.Basic

ApplicationWindow {
    id: window
    width: 1360
    height: 860
    minimumWidth: 1100
    minimumHeight: 720
    visible: true
    title: qsTr("beta")
    color: "#0e1116"

    SimulationRoot {
        anchors.fill: parent
    }
}
