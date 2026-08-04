pragma Singleton

import QtQuick
QtObject {
    property var controller: null
    property var editor: null

    readonly property color page: "#080b12"
    readonly property color panel: "#101820"
    readonly property color raised: "#17232d"
    readonly property color line: "#2a3b48"
    readonly property color softLine: "#1d2a33"
    readonly property color text: "#edf4f5"
    readonly property color textStrong: "#ffffff"
    readonly property color textDim: "#b9c7cd"
    readonly property color muted: "#82919c"
    readonly property color signal: "#38d2b4"
    readonly property color info: "#4c9dff"
    readonly property color red: "#ff7180"
    readonly property color blue: "#4c9dff"
    readonly property color success: "#38d2b4"
    readonly property color warning: "#f0a040"
    readonly property color danger: "#ff7180"
    readonly property color rangeCommunication: text
    readonly property color rangeDetection: info
    readonly property color rangeAttack: danger
    readonly property color markSelf: signal
    readonly property color markCommander: warning

    readonly property int radius: 6
    readonly property int fastMotion: 150
    readonly property int stateMotion: 220
}
