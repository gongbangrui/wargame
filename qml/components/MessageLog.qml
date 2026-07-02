import QtQuick
import QtQuick.Controls.Basic

Rectangle {
    id: root
    color: "#0e1217"
    border.color: "#2a3142"
    radius: 6

    property var messages: ([])
    property string sideFilter: ""

    function filtered() {
        if (!root.sideFilter) return root.messages
        var result = []
        for (var i = 0; i < root.messages.length; i++) {
            var m = root.messages[i]
            var s = m.sender || ""
            var r = m.receiver || ""
            if (s.indexOf(root.sideFilter) === 0 || r.indexOf(root.sideFilter) === 0)
                result.push(m)
            else if (m.type === "PositionReport" && m.payload && m.payload.side === root.sideFilter)
                result.push(m)
        }
        return result
    }

    ListView {
        anchors.fill: parent
        anchors.margins: 1
        model: root.filtered()
        clip: true
        delegate: Rectangle {
            width: ListView.view.width
            implicitHeight: row.implicitHeight + 8
            color: index % 2 === 0 ? "#0e1217" : "#141a21"
            Row {
                id: row
                anchors.fill: parent
                anchors.leftMargin: 8; anchors.rightMargin: 8
                spacing: 8
                Rectangle {
                    width: 6; height: 16; radius: 3
                    anchors.verticalCenter: parent.verticalCenter
                    color: {
                        var t = modelData.type
                        if (t === "TargetDetect") return "#ffb24d"
                        if (t === "AttackOrder") return "#ff4d6d"
                        if (t === "FlightPlan") return "#4f9dff"
                        if (t === "EngagementReport" || t === "TargetDestroyed") return "#46d29a"
                        if (t === "Ack") return "#8a93a6"
                        if (t === "Withdraw") return "#ff5566"
                        if (t === "PositionReport") return "#3a455a"
                        return "#4a5161"
                    }
                }
                Text {
                    color: "#f3f6fb"; font.family: "Consolas"; font.pixelSize: 11
                    text: "[" + modelData.time.substr(11,8) + "] " + modelData.sender + "\u2192" + modelData.receiver + "  " + modelData.type
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }
}