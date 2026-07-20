import QtQuick

Rectangle {
    id: root
    color: "#0a0f19"
    border.color: "#1e2a3d"
    radius: 6

    property var messages: ([])
    property string sideFilter: ""
    property var cachedModel: []

    function filtered() {
        if (!root.sideFilter) return root.messages
        var result = []
        for (var i = 0; i < root.messages.length; i++) {
            var m = root.messages[i]
            var s = m.sender || ""
            var r = m.receiver || ""
            if (m.senderSide === root.sideFilter || m.receiverSide === root.sideFilter)
                result.push(m)
            else if (!m.senderSide && !m.receiverSide
                     && (s.indexOf(root.sideFilter) === 0 || r.indexOf(root.sideFilter) === 0))
                result.push(m)
            else if (m.type === "PositionReport" && m.payload && m.payload.side === root.sideFilter)
                result.push(m)
        }
        return result
    }

    Timer {
        id: filterThrottle
        interval: 100; repeat: false
        onTriggered: {
            root.cachedModel = root.filtered()
        }
    }

    property double _lastFilterTime: 0

    onMessagesChanged: {
        var now = Date.now()
        if (now - _lastFilterTime > 100) {
            _lastFilterTime = now
            root.cachedModel = root.filtered()
        } else if (!filterThrottle.running) {
            filterThrottle.start()
        }
    }
    onSideFilterChanged: fcs.start()
    Timer { id: fcs; interval: 10; repeat: false; onTriggered: { root.cachedModel = root.filtered() } }

    ListView {
        anchors.fill: parent
        anchors.margins: 1
        model: root.cachedModel
        clip: true
        delegate: Rectangle {
            id: logRow
            required property int index
            required property var modelData
            width: ListView.view.width
            implicitHeight: row.implicitHeight + 8
            color: logRow.index % 2 === 0 ? "#0e1217" : "#141a21"
            Row {
                id: row
                anchors.fill: parent
                anchors.leftMargin: 8; anchors.rightMargin: 8
                spacing: 8
                Rectangle {
                    width: 6; height: 16; radius: 3
                    anchors.verticalCenter: parent.verticalCenter
                    color: {
                        var t = logRow.modelData.type
                        if (t === "TargetDetect") return "#ffb24d"
                        if (t === "SharedDetect") return "#8a93a6"
                        if (t === "AttackOrder") return "#ff4d6d"
                        if (t === "FlightPlan") return "#4f9dff"
                        if (t === "Pursue") return "#ff6b4a"
                        if (t === "EngagementReport" || t === "TargetDestroyed") return "#46d29a"
                        if (t === "Ack") return "#8a93a6"
                        if (t === "Withdraw") return "#ff5566"
                        if (t === "PositionReport") return "#3a455a"
                        return "#4a5161"
                    }
                }
                Text {
                    color: "#f3f6fb"; font.family: "Consolas"; font.pixelSize: 11
                    text: {
                        var t = logRow.modelData.time || ""
                        var time = t.length >= 19 ? t.substr(11, 8) : t
                        return "[" + time + "] " + logRow.modelData.sender + "\u2192" + logRow.modelData.receiver + "  " + logRow.modelData.type
                    }
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }
}
