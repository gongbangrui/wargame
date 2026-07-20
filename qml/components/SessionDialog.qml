import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: root
    property var controller: null
    property var editor: null
    modal: true
    anchors.centerIn: parent
    width: Math.min(720, parent ? parent.width - 32 : 720)
    height: Math.min(620, parent ? parent.height - 32 : 620)
    title: "选择运行模式"
    standardButtons: Dialog.NoButton
    closePolicy: root.controller.sessionMode === "unselected" ? Popup.NoAutoClose
                                                        : Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property string selectedMode: "online"

    QtObject {
        id: t
        property color bg: "#0b1018"
        property color panel: "#121923"
        property color panelAlt: "#171f2b"
        property color border: "#2a3748"
        property color text: "#edf2f8"
        property color dim: "#9ca8b8"
        property color muted: "#6f7d90"
        property color accent: "#2aa897"
        property color red: "#e14d5c"
    }

    function prepare() {
        serverField.text = root.controller.loadSetting("network/server", "http://localhost:8080")
        passwordField.text = ""
        statusText.text = ""
    }

    onOpened: prepare()
    background: Rectangle { color: t.bg; border.color: t.border; radius: 8 }

    header: Rectangle {
        height: 66; color: t.panel
        Rectangle { anchors.left: parent.left; anchors.bottom: parent.bottom; width: parent.width; height: 1; color: t.border }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 22; anchors.rightMargin: 18; spacing: 12
            Rectangle {
                Layout.preferredWidth: 38; Layout.preferredHeight: 38; radius: 5; color: t.red
                Text { anchors.centerIn: parent; text: "兵"; color: "white"; font.bold: true; font.pixelSize: 18 }
            }
            ColumnLayout {
                spacing: 1; Layout.fillWidth: true
                Text { text: "兵器推演"; color: t.text; font.bold: true; font.pixelSize: 17; renderType: Text.NativeRendering }
                Text { text: "选择本地推演或连接联网服务器"; color: t.muted; font.pixelSize: 11; renderType: Text.NativeRendering }
            }
            GhostButton { visible: root.controller.sessionMode !== "unselected"; text: "关闭"; onClicked: root.close() }
        }
    }

    contentItem: RowLayout {
        spacing: 0
        Rectangle {
            Layout.preferredWidth: 190; Layout.fillHeight: true; color: t.panel
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 14; spacing: 8
                Text { text: "运行方式"; color: t.muted; font.pixelSize: 11; Layout.leftMargin: 8; Layout.bottomMargin: 4 }
                Button {
                    id: onlineModeButton
                    Layout.fillWidth: true; Layout.preferredHeight: 48
                    text: "联网模式"; onClicked: root.selectedMode = "online"
                    contentItem: Text { text: onlineModeButton.text; color: root.selectedMode === "online" ? "#7de0d1" : t.dim; verticalAlignment: Text.AlignVCenter; leftPadding: 12; font.bold: root.selectedMode === "online" }
                    background: Rectangle { color: root.selectedMode === "online" ? "#18312f" : "transparent"; radius: 5; border.color: root.selectedMode === "online" ? "#2f756c" : "transparent" }
                }
                Button {
                    id: localModeButton
                    Layout.fillWidth: true; Layout.preferredHeight: 48
                    text: "本地模式"; onClicked: root.selectedMode = "local"
                    contentItem: Text { text: localModeButton.text; color: root.selectedMode === "local" ? "#f1b2b8" : t.dim; verticalAlignment: Text.AlignVCenter; leftPadding: 12; font.bold: root.selectedMode === "local" }
                    background: Rectangle { color: root.selectedMode === "local" ? "#342127" : "transparent"; radius: 5; border.color: root.selectedMode === "local" ? "#78404a" : "transparent" }
                }
                Item { Layout.fillHeight: true }
                Text { text: root.controller.sessionMode === "online" ? "当前：联网模式" : root.controller.sessionMode === "local" ? "当前：本地模式" : "尚未选择"; color: t.muted; font.pixelSize: 11; Layout.leftMargin: 8 }
            }
        }

        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 28; spacing: 14
                ColumnLayout {
                    visible: root.selectedMode === "online"; spacing: 14; Layout.fillWidth: true
                    Text { text: "连接联网服务器"; color: t.text; font.pixelSize: 20; font.bold: true }
                    Text { text: "使用管理员在账号平台创建的兵棋账号登录"; color: t.dim; font.pixelSize: 12 }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        Text { text: "常用服务器"; color: t.muted; font.pixelSize: 11 }
                        ComboBox {
                            id: serverHistoryBox
                            Layout.fillWidth: true; Layout.preferredHeight: 30
                            visible: root.controller.serverHistory.length > 0
                            model: root.controller.serverHistory
                            onActivated: function(index) {
                                serverField.text = currentText
                                root.controller.diagnoseServer(serverField.text)
                            }
                            contentItem: Text {
                                leftPadding: 10; rightPadding: 28; verticalAlignment: Text.AlignVCenter
                                text: serverHistoryBox.displayText; color: t.text; font.pixelSize: 11; elide: Text.ElideRight
                            }
                            background: Rectangle { color: "#101923"; border.color: serverHistoryBox.activeFocus ? t.accent : t.border; radius: 5 }
                        }
                    }
                    TextField {
                        id: serverField; Layout.fillWidth: true; placeholderText: "账号服务器，例如 http://localhost:8080"
                        color: t.text; selectByMouse: true
                        onTextEdited: serverCheckTimer.restart()
                        onEditingFinished: root.controller.diagnoseServer(text)
                        background: Rectangle { implicitHeight: 42; color: t.panelAlt; border.color: serverField.activeFocus ? t.accent : t.border; radius: 5 }
                    }
                    Timer {
                        id: serverCheckTimer; interval: 500; repeat: false
                        onTriggered: { if (serverField.text.trim().length > 0) root.controller.diagnoseServer(serverField.text) }
                    }
                    TextField {
                        id: usernameField; Layout.fillWidth: true; placeholderText: "用户名"; color: t.text; selectByMouse: true
                        background: Rectangle { implicitHeight: 42; color: t.panelAlt; border.color: usernameField.activeFocus ? t.accent : t.border; radius: 5 }
                    }
                    TextField {
                        id: passwordField; Layout.fillWidth: true; placeholderText: "密码"; echoMode: TextInput.Password; color: t.text; selectByMouse: true
                        onAccepted: {
                            if (loginButton.enabled)
                                root.controller.loginOnline(serverField.text, usernameField.text, passwordField.text)
                        }
                        background: Rectangle { implicitHeight: 42; color: t.panelAlt; border.color: passwordField.activeFocus ? t.accent : t.border; radius: 5 }
                    }
                    Text {
                        id: statusText; Layout.fillWidth: true; wrapMode: Text.WordWrap
                        text: root.controller.networkState === "disconnected" ? "" : root.controller.networkStatus
                        color: root.controller.networkState === "error" ? "#ff7886" : t.dim; font.pixelSize: 12
                    }
                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 76
                        color: root.controller.networkDiagnosticState === "healthy" ? "#102d2a" : root.controller.networkDiagnosticState === "error" ? "#321b24" : t.panelAlt
                        border.color: root.controller.networkDiagnosticState === "healthy" ? t.accent : root.controller.networkDiagnosticState === "error" ? "#8d3d4c" : t.border
                        radius: 6
                        RowLayout {
                            anchors.fill: parent; anchors.margins: 10; spacing: 9
                            Text { text: root.controller.networkDiagnosticState === "healthy" ? "✓" : root.controller.networkDiagnosticState === "error" ? "!" : "⌁"; color: root.controller.networkDiagnosticState === "healthy" ? "#6ee2c7" : root.controller.networkDiagnosticState === "error" ? "#ff7d8b" : t.dim; font.pixelSize: 19; font.bold: true }
                            ColumnLayout {
                                Layout.fillWidth: true; spacing: 2
                                Text { text: "服务器连接诊断"; color: t.text; font.pixelSize: 11; font.bold: true }
                                Text { text: root.controller.networkDiagnosticMessage; color: t.dim; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            ColumnLayout {
                                spacing: 1
                                Text { text: "账号 " + (root.controller.accountLatencyMs >= 0 ? root.controller.accountLatencyMs + " ms" : "--"); color: t.text; font.family: "Consolas"; font.pixelSize: 10 }
                                Text { text: "推演 " + (root.controller.gameLatencyMs >= 0 ? root.controller.gameLatencyMs + " ms" : "--"); color: t.dim; font.family: "Consolas"; font.pixelSize: 10 }
                            }
                            GhostButton { text: "检测"; iconName: "refresh"; enabled: root.controller.networkDiagnosticState !== "checking"; onClicked: root.controller.diagnoseServer(serverField.text) }
                        }
                    }
                    Button {
                        id: loginButton; Layout.fillWidth: true; Layout.preferredHeight: 42
                        enabled: serverField.text.trim().length > 0 && usernameField.text.trim().length > 0
                                 && passwordField.text.length > 0
                                 && root.controller.networkState !== "loggingIn" && root.controller.networkState !== "connecting"
                                 && root.controller.networkState !== "authenticating"
                                 && root.controller.networkState !== "synchronizing"
                        text: root.controller.networkState === "loggingIn" || root.controller.networkState === "connecting" || root.controller.networkState === "authenticating" || root.controller.networkState === "synchronizing" ? "正在连接..." : "登录并进入推演室"
                        onClicked: root.controller.loginOnline(serverField.text, usernameField.text, passwordField.text)
                        contentItem: Text { anchors.fill: parent; text: loginButton.text; color: "#ffffff"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.bold: true }
                        background: Rectangle { color: loginButton.enabled ? t.accent : "#38504f"; radius: 5 }
                    }
                }

                ColumnLayout {
                    visible: root.selectedMode === "local"; spacing: 14; Layout.fillWidth: true
                    Text { text: "使用本地模式"; color: t.text; font.pixelSize: 20; font.bold: true }
                    Text { text: "单机推演保留完整编辑、红蓝双方和导演视角，所有状态仅在当前进程中运行。"; color: t.dim; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: t.border; Layout.topMargin: 8; Layout.bottomMargin: 8 }
                    Button {
                        id: localEnterButton
                        Layout.fillWidth: true; Layout.preferredHeight: 42; text: "进入本地推演"
                        onClicked: { root.controller.useLocalMode(); root.close() }
                        contentItem: Text { anchors.fill: parent; text: localEnterButton.text; color: "#ffffff"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.bold: true }
                        background: Rectangle { color: t.red; radius: 5 }
                    }
                }
                Item { Layout.fillHeight: true }
            }
        }
    }

    Connections {
        target: root.controller
        function onSessionChanged() {
            if (root.controller.sessionMode === "online") {
                passwordField.text = ""
                root.close()
            }
            else if (root.controller.sessionMode === "unselected" && !root.opened) root.open()
        }
    }
}
