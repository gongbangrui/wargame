import QtQuick

Text {
    id: root
    property string name: "dot"
    property color iconColor: "#bcc8de"
    property real iconSize: 14

    width: iconSize
    height: iconSize
    color: root.iconColor
    font.pixelSize: root.iconSize
    font.family: "Noto Sans Symbols 2"
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    renderType: Text.NativeRendering
    text: {
        var glyphs = {
            close: "×", settings: "⚙", shortcut: "⌨", chat: "▤",
            refresh: "↻", check: "✓", warning: "!", network: "⌁",
            server: "▣", local: "⌂", plus: "+", minus: "−",
            play: "▶", pause: "Ⅱ", stop: "■", edit: "✎",
            delete: "⌫", save: "▣", load: "↥", send: "➤", dot: "•",
            history: "◷", download: "⇩", table: "▦", locate: "⌖",
            missile: "➤", countermeasure: "✦", scan: "◎", repair: "✚",
            service: "▰", fuel: "◒", "return": "↩", menu: "☰",
            unit: "▦", command: "⌁",
            "chevron-left": "‹", "chevron-right": "›", "chevron-down": "⌄"
        }
        return glyphs[root.name] || glyphs.dot
    }
}
