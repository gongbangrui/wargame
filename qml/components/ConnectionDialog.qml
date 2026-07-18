import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: root
    title: "连接服务器"
    modal: true
    anchors.centerIn: parent
    width: 460
    standardButtons: Dialog.NoButton
    closePolicy: Popup.NoAutoClose

    property bool connectInProgress: controller.connectionState === "connecting"
                                     || controller.connectionState === "authenticating"
                                     || controller.connectionState === "syncing"
                                     || controller.connectionState === "reconnecting"
    property bool passwordMode: false

    function openForConnection() {
        serverField.text = controller.connectionDefaultUrl || controller.serverUrl
                || controller.loadSetting("network/serverUrl", "ws://127.0.0.1:8080/ws")
        tokenField.text = ""
        usernameField.text = ""
        passwordField.text = ""
        rememberAddress.checked = controller.loadSetting("network/rememberServer", true)
        open()
        Qt.callLater(function() { serverField.forceActiveFocus() })
    }

    function statusText() {
        if (controller.connectionError) return controller.connectionError
        return controller.connectionStatus || "等待连接"
    }

    background: Rectangle {
        color: "#0e1322"
        border.color: "#1e2d4a"
        border.width: 1
        radius: 6
    }

    header: Rectangle {
        height: 54
        color: "transparent"
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 14
            spacing: 10
            Text {
                text: "连接服务器"
                color: "#ffffff"
                font.pixelSize: 17
                font.bold: true
                Layout.fillWidth: true
                renderType: Text.NativeRendering
            }
            ToolButton {
                text: "X"
                enabled: !root.connectInProgress
                onClicked: root.reject()
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 10

        Text {
            text: "服务器地址"
            color: "#bcc8de"
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        TextField {
            id: serverField
            Layout.fillWidth: true
            selectByMouse: true
            placeholderText: "wss://game.example.com/ws"
            color: "#e8edf5"
            font.pixelSize: 13
            background: Rectangle {
                color: "#080b14"
                border.color: serverField.activeFocus ? "#4090ff" : "#1e2d4a"
                border.width: 1
                radius: 4
            }
        }

        Text {
            text: root.passwordMode ? "账号" : "访问令牌"
            color: "#bcc8de"
            font.pixelSize: 12
            Layout.topMargin: 4
            renderType: Text.NativeRendering
        }
        TextField {
            id: tokenField
            visible: !root.passwordMode
            Layout.fillWidth: true
            selectByMouse: true
            echoMode: TextInput.Password
            placeholderText: controller.hasStartupToken ? "已从启动环境读取令牌" : "输入服务器提供的 token"
            color: "#e8edf5"
            font.pixelSize: 13
            background: Rectangle {
                color: "#080b14"
                border.color: tokenField.activeFocus ? "#4090ff" : "#1e2d4a"
                border.width: 1
                radius: 4
            }
        }

        TextField {
            id: usernameField
            visible: root.passwordMode
            Layout.fillWidth: true
            selectByMouse: true
            placeholderText: "输入账号"
            color: "#e8edf5"
            background: Rectangle { color: "#080b14"; border.color: usernameField.activeFocus ? "#4090ff" : "#1e2d4a"; border.width: 1; radius: 4 }
        }
        Text {
            visible: root.passwordMode
            text: "密码"
            color: "#bcc8de"
            font.pixelSize: 12
        }
        TextField {
            id: passwordField
            visible: root.passwordMode
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: "输入密码"
            color: "#e8edf5"
            background: Rectangle { color: "#080b14"; border.color: passwordField.activeFocus ? "#4090ff" : "#1e2d4a"; border.width: 1; radius: 4 }
        }

        CheckBox {
            id: rememberAddress
            text: "记住服务器地址"
            checked: true
            contentItem: Text {
                text: rememberAddress.text
                color: "#bcc8de"
                font.pixelSize: 12
                leftPadding: rememberAddress.indicator.width + rememberAddress.spacing
                verticalAlignment: Text.AlignVCenter
                renderType: Text.NativeRendering
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: "#17213a"
        }
        Text {
            Layout.fillWidth: true
            text: root.statusText()
            color: controller.connectionError ? "#f04760"
                 : controller.connectionState === "connected" ? "#36c98a" : "#8896b8"
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            renderType: Text.NativeRendering
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            Item { Layout.fillWidth: true }
            Button {
                text: root.passwordMode ? "使用 Token" : "使用账号密码"
                onClicked: root.passwordMode = !root.passwordMode
            }
            Button {
                text: root.connectInProgress ? "取消" : "返回本地"
                onClicked: {
                    if (root.connectInProgress || controller.sessionMode === "remote")
                        controller.switchToLocalMode()
                    root.close()
                }
            }
            Button {
                text: root.connectInProgress ? "连接中" : "连接"
                enabled: !root.connectInProgress
                onClicked: {
                    if (rememberAddress.checked) {
                        controller.saveSetting("network/serverUrl", serverField.text.trim())
                        controller.saveSetting("network/rememberServer", true)
                    } else {
                        controller.saveSetting("network/serverUrl", "")
                        controller.saveSetting("network/rememberServer", false)
                    }
                    if (root.passwordMode)
                        controller.connectToServerWithPassword(serverField.text, usernameField.text, passwordField.text)
                    else
                        controller.connectToServer(serverField.text, tokenField.text)
                    if (controller.connectionState !== "error") root.close()
                }
            }
        }
    }

    onRejected: {
        if (controller.sessionMode === "remote" && controller.connectionState !== "connected")
            controller.switchToLocalMode()
    }

    Connections {
        target: controller
        function onConnectionStateChanged() {
            if (controller.connectionState === "connected") root.close()
        }
    }
}
