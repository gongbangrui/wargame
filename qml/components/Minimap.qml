import QtQuick

Item {
    id: root

    property var mapCenter: ({x: 20000, y: 15000})
    property var mapSize: ({w: 40000, h: 30000})
    property var viewportRect: ({x: 0, y: 0, w: 1, h: 1})
    property double mainZoom: 1.0
    property var units: []
    property string sideFilter: ""
    property var focusUnitId: ""
    property var detectedEnemies: ([])

    signal minimapClicked(var logicalPos)

    width: 160
    height: Math.round(width * mapSize.h / Math.max(1, mapSize.w))
    clip: true

    onUnitsChanged: miniCanvas.requestPaint()
    onSideFilterChanged: miniCanvas.requestPaint()
    onDetectedEnemiesChanged: miniCanvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        color: "#080b14"
        border.color: "#1e2d4a"
        border.width: 1
        radius: 6

        Canvas {
            id: miniCanvas
            anchors.fill: parent
            anchors.margins: 3

            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                var cw = width, ch = height

                ctx.fillStyle = "#0a0f1e"
                ctx.fillRect(0, 0, cw, ch)

                var mw = Math.max(1, root.mapSize.w)
                var mh = Math.max(1, root.mapSize.h)
                var scale = Math.min(cw / mw, ch / mh)

                var toMiniX = function(lx) { return (lx) * scale }
                var toMiniY = function(ly) { return ch - (ly) * scale }

                var filterSide = root.sideFilter

                var vx = toMiniX(root.viewportRect.x)
                var vy = toMiniY(root.viewportRect.y + root.viewportRect.h)
                var vw = root.viewportRect.w * scale
                var vh = root.viewportRect.h * scale

                var us = root.units
                for (var i = 0; i < us.length; i++) {
                    var u = us[i]
                    if (!u || !u.position) continue
                    if (filterSide && u.side !== filterSide) continue
                    var mx = toMiniX(u.position[0])
                    var my = toMiniY(u.position[1])
                    var color = u.side === "red" ? "#f04760" : "#4090ff"
                    if (!u.alive) color = "#4a5268"
                    ctx.fillStyle = color
                    ctx.beginPath()
                    ctx.arc(mx, my, 2.5, 0, Math.PI * 2)
                    ctx.fill()
                }

                var des = root.detectedEnemies
                for (var j = 0; j < des.length; j++) {
                    var de = des[j]
                    if (!de || de.x === undefined || de.y === undefined) continue
                    var dmx = toMiniX(de.x)
                    var dmy = toMiniY(de.y)
                    ctx.fillStyle = "rgba(255,150,50,0.8)"
                    ctx.beginPath()
                    var sz = 3.5
                    ctx.moveTo(dmx, dmy - sz)
                    ctx.lineTo(dmx + sz, dmy + sz)
                    ctx.lineTo(dmx - sz, dmy + sz)
                    ctx.closePath()
                    ctx.fill()
                }

                ctx.strokeStyle = "rgba(255,255,255,0.55)"
                ctx.lineWidth = 1.5
                ctx.setLineDash([3,2])
                ctx.strokeRect(vx, vy, vw, vh)
                ctx.setLineDash([])
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: function(mouse) {
            var sx = width / Math.max(1, root.mapSize.w)
            var sy = height / Math.max(1, root.mapSize.h)
            var scale = Math.min(sx, sy)
            var lx = mouse.x / scale
            var ly = (height - mouse.y) / scale
            root.minimapClicked({x: lx, y: ly})
        }
    }
}
