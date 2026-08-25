import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: root
    property var controller: null
    property var editor: null
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(800, parent ? parent.width - 32 : 800)
    height: Math.min(700, parent ? parent.height - 32 : 700)
    title: "运行模式"
    standardButtons: Dialog.NoButton
    padding: 0
    closePolicy: root.controller.sessionMode === "unselected" ? Popup.NoAutoClose
                                                        : Popup.CloseOnEscape | Popup.CloseOnPressOutside

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: AppContext.stateMotion; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: AppContext.stateMotion; easing.type: Easing.OutBack }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: AppContext.fastMotion; easing.type: Easing.InCubic }
            NumberAnimation { property: "scale"; from: 1; to: 0.98; duration: AppContext.fastMotion; easing.type: Easing.InCubic }
        }
    }
    Overlay.modal: Rectangle { color: "#05080dcc" }

    property string selectedMode: "online"
    property bool compactLayout: width < 680
    property bool shortLayout: height < 640

    function submitOnlineLogin() {
        var remember = rememberPasswordBox.checked
        root.controller.saveSetting("network/rememberPassword", remember)
        root.controller.saveSetting("network/username", usernameField.text.trim())
        root.controller.saveRememberedPassword(serverField.text, usernameField.text, passwordField.text, remember)
        root.controller.loginOnline(serverField.text, usernameField.text, passwordField.text)
    }

    QtObject {
        id: t
        property color bg: AppContext.page
        property color panel: AppContext.panel
        property color panelAlt: AppContext.raised
        property color border: AppContext.line
        property color text: AppContext.text
        property color dim: AppContext.textDim
        property color muted: AppContext.muted
        property color accent: AppContext.signal
        property color info: AppContext.info
        property color warning: AppContext.warning
        property color red: AppContext.danger
    }

    function prepare() {
        serverField.text = root.controller.loadSetting("network/server", "http://localhost:8080")
        usernameField.text = root.controller.loadSetting("network/username", "")
        rememberPasswordBox.checked = root.controller.loadSetting("network/rememberPassword", false)
        passwordField.text = ""
        if (rememberPasswordBox.checked)
            root.controller.loadRememberedPassword(serverField.text, usernameField.text)
    }

    onOpened: prepare()
    Connections {
        target: root.controller
        function onRememberedPasswordLoaded(password) {
            if (rememberPasswordBox.checked)
                passwordField.text = password
        }
    }
    background: Rectangle {
        color: t.bg; border.color: t.border; radius: 7
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: t.bg }
            GradientStop { position: 0.62; color: "#0e1820" }
            GradientStop { position: 1.0; color: "#0a141b" }
        }
    }

    header: Rectangle {
        height: root.shortLayout ? 64 : 70; color: t.panel
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: t.panel }
            GradientStop { position: 1.0; color: "#14242d" }
        }
        Rectangle { anchors.left: parent.left; anchors.bottom: parent.bottom; width: parent.width; height: 1; color: t.border }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 24; anchors.rightMargin: 18; spacing: 14
            Rectangle {
                Layout.preferredWidth: 40; Layout.preferredHeight: 40; radius: 6
                color: t.panelAlt; border.color: t.accent
                Text { anchors.centerIn: parent; text: "棋"; color: t.text; font.bold: true; font.pixelSize: 18 }
            }
            ColumnLayout {
                spacing: 1; Layout.fillWidth: true
                Text { text: "兵棋推演"; color: t.text; font.bold: true; font.pixelSize: 18; renderType: Text.NativeRendering }
            }
            Rectangle {
                Layout.preferredWidth: 9; Layout.preferredHeight: 9; radius: 5
                color: root.controller.networkState === "connected" ? t.accent
                      : root.controller.networkState === "error" ? t.red : t.warning
            }
            GhostButton { visible: root.controller.sessionMode !== "unselected"; text: "关闭"; iconName: "close"; onClicked: root.close() }
        }
    }

    contentItem: GridLayout {
        columns: root.compactLayout ? 1 : 2
        columnSpacing: 0; rowSpacing: 0
        Rectangle {
            visible: !root.compactLayout
            Layout.preferredWidth: 208; Layout.fillHeight: true; color: t.panel
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 16; spacing: 10
                Button {
                    id: onlineModeButton
                    Layout.fillWidth: true; Layout.preferredHeight: 58
                    text: "联网"; onClicked: root.selectedMode = "online"
                    contentItem: RowLayout { spacing: 10
                        Icon { name: "network"; iconColor: root.selectedMode === "online" ? t.accent : t.muted; iconSize: 17 }
                        Text { Layout.fillWidth: true; text: onlineModeButton.text; color: root.selectedMode === "online" ? t.text : t.dim; font.pixelSize: 12; font.bold: root.selectedMode === "online" }
                    }
                    background: Rectangle { color: root.selectedMode === "online" ? t.panelAlt : "transparent"; radius: 5; border.color: root.selectedMode === "online" ? t.accent : "transparent" }
                }
                Button {
                    id: localModeButton
                    Layout.fillWidth: true; Layout.preferredHeight: 58
                    text: "本地"; onClicked: root.selectedMode = "local"
                    contentItem: RowLayout { spacing: 10
                        Icon { name: "local"; iconColor: root.selectedMode === "local" ? t.info : t.muted; iconSize: 17 }
                        Text { Layout.fillWidth: true; text: localModeButton.text; color: root.selectedMode === "local" ? t.text : t.dim; font.pixelSize: 12; font.bold: root.selectedMode === "local" }
                    }
                    background: Rectangle { color: root.selectedMode === "local" ? t.panelAlt : "transparent"; radius: 5; border.color: root.selectedMode === "local" ? t.info : "transparent" }
                }
                Item { Layout.fillHeight: true }
            }
        }

        RowLayout {
            visible: root.compactLayout
            Layout.fillWidth: true; Layout.preferredHeight: 54
            Layout.leftMargin: 16; Layout.rightMargin: 16; spacing: 8
            Button { id: compactOnlineModeButton; Layout.fillWidth: true; text: "联网"; onClicked: root.selectedMode = "online"; contentItem: Text { text: compactOnlineModeButton.text; color: root.selectedMode === "online" ? t.accent : t.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.bold: root.selectedMode === "online" } background: Rectangle { color: root.selectedMode === "online" ? t.panelAlt : t.panel; border.color: root.selectedMode === "online" ? t.accent : t.border; radius: 5 } }
            Button { id: compactLocalModeButton; Layout.fillWidth: true; text: "本地"; onClicked: root.selectedMode = "local"; contentItem: Text { text: compactLocalModeButton.text; color: root.selectedMode === "local" ? t.info : t.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.bold: root.selectedMode === "local" } background: Rectangle { color: root.selectedMode === "local" ? t.panelAlt : t.panel; border.color: root.selectedMode === "local" ? t.info : t.border; radius: 5 } }
        }

        ScrollView {
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ScrollBar.vertical.policy: ScrollBar.AsNeeded
            ColumnLayout {
                width: Math.max(0, parent.width - 48)
                x: 24
                y: root.shortLayout ? 8 : 20
                spacing: root.shortLayout ? 6 : 14
                ColumnLayout {
                    visible: root.selectedMode === "online"; spacing: 12; Layout.fillWidth: true
                    Text { text: "登录"; color: t.text; font.pixelSize: 19; font.bold: true }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: t.border }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
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
                            background: Rectangle { color: t.panel; border.color: serverHistoryBox.activeFocus ? t.accent : t.border; radius: 5 }
                        }
                    }
                    ColumnLayout { Layout.fillWidth: true; spacing: 5
                        Text { text: "账号服务器"; color: t.dim; font.pixelSize: 10; font.bold: true }
                        RowLayout { Layout.fillWidth: true; spacing: 8
                            TextField {
                                id: serverField; Layout.fillWidth: true; placeholderText: "http://localhost:8080"
                                Accessible.name: "账号服务器地址"
                                color: t.text; selectByMouse: true
                                onTextEdited: serverCheckTimer.restart()
                                onEditingFinished: root.controller.diagnoseServer(text)
                                background: Rectangle { implicitHeight: root.shortLayout ? 36 : 42; color: t.panelAlt; border.color: serverField.activeFocus ? t.accent : t.border; radius: 5 }
                            }
                            GhostButton { text: "检测"; iconName: "refresh"; enabled: root.controller.networkDiagnosticState !== "checking"; onClicked: root.controller.diagnoseServer(serverField.text) }
                        }
                    }
                    Timer {
                        id: serverCheckTimer; interval: 500; repeat: false
                        onTriggered: { if (serverField.text.trim().length > 0) root.controller.diagnoseServer(serverField.text) }
                    }
                    ColumnLayout { Layout.fillWidth: true; spacing: 5
                        Text { text: "用户名"; color: t.dim; font.pixelSize: 10; font.bold: true }
                        TextField {
                            id: usernameField; Layout.fillWidth: true; placeholderText: "输入用户名"; Accessible.name: "用户名"; color: t.text; selectByMouse: true
                            background: Rectangle { implicitHeight: root.shortLayout ? 36 : 42; color: t.panelAlt; border.color: usernameField.activeFocus ? t.accent : t.border; radius: 5 }
                        }
                    }
                    ColumnLayout { Layout.fillWidth: true; spacing: 5
                        Text { text: "密码"; color: t.dim; font.pixelSize: 10; font.bold: true }
                        TextField {
                            id: passwordField; Layout.fillWidth: true; placeholderText: "输入密码"; Accessible.name: "密码"; echoMode: TextInput.Password; color: t.text; selectByMouse: true
                            onAccepted: {
                                if (loginButton.enabled)
                                    root.submitOnlineLogin()
                            }
                            background: Rectangle { implicitHeight: root.shortLayout ? 36 : 42; color: t.panelAlt; border.color: passwordField.activeFocus ? t.accent : t.border; radius: 5 }
                        }
                    }
                    CheckBox {
                        id: rememberPasswordBox
                        text: "记住密码"
                        Layout.fillWidth: true
                        contentItem: Text {
                            text: rememberPasswordBox.text
                            color: t.dim
                            font.pixelSize: 12
                            leftPadding: rememberPasswordBox.indicator.width + 8
                            verticalAlignment: Text.AlignVCenter
                        }
                        indicator: Rectangle {
                            implicitWidth: 16
                            implicitHeight: 16
                            x: rememberPasswordBox.leftPadding
                            y: parent.height / 2 - height / 2
                            radius: 3
                            color: rememberPasswordBox.checked ? t.accent : t.panelAlt
                            border.color: rememberPasswordBox.activeFocus ? t.accent : t.border
                            Text {
                                anchors.centerIn: parent
                                visible: rememberPasswordBox.checked
                                text: "✓"
                                color: t.bg
                                font.bold: true
                                font.pixelSize: 12
                            }
                        }
                    }
                    Text {
                        id: statusText; Layout.fillWidth: true; wrapMode: Text.WordWrap
                        text: root.controller.networkState === "disconnected" ? "" : root.controller.networkStatus
                        color: root.controller.networkState === "error" ? t.red : t.dim; font.pixelSize: 11
                    }
                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 52
                        color: t.panelAlt
                        border.color: root.controller.networkDiagnosticState === "healthy" ? t.accent : root.controller.networkDiagnosticState === "error" ? t.red : t.border
                        radius: 6
                        RowLayout {
                            anchors.fill: parent; anchors.margins: 10; spacing: 9
                            Icon { name: root.controller.networkDiagnosticState === "healthy" ? "check" : root.controller.networkDiagnosticState === "error" ? "warning" : "network"; iconColor: root.controller.networkDiagnosticState === "healthy" ? t.accent : root.controller.networkDiagnosticState === "error" ? t.red : t.dim; iconSize: 19 }
                            ColumnLayout {
                                Layout.fillWidth: true; spacing: 2
                                Text { text: root.controller.networkDiagnosticMessage; color: t.dim; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            ColumnLayout {
                                spacing: 1
                                Text { text: "账号 " + (root.controller.accountLatencyMs >= 0 ? root.controller.accountLatencyMs + " ms" : "--"); color: t.text; font.family: "Consolas"; font.pixelSize: 10 }
                                Text { text: "推演 " + (root.controller.gameLatencyMs >= 0 ? root.controller.gameLatencyMs + " ms" : "--"); color: t.dim; font.family: "Consolas"; font.pixelSize: 10 }
                            }
                        }
                    }
                    Button {
                        id: loginButton; Layout.fillWidth: true; Layout.preferredHeight: root.shortLayout ? 36 : 44
                        enabled: serverField.text.trim().length > 0 && usernameField.text.trim().length > 0
                                 && passwordField.text.length > 0
                                 && root.controller.networkState !== "loggingIn" && root.controller.networkState !== "connecting"
                                 && root.controller.networkState !== "authenticating"
                                 && root.controller.networkState !== "synchronizing"
                        text: root.controller.networkState === "loggingIn" || root.controller.networkState === "connecting" || root.controller.networkState === "authenticating" || root.controller.networkState === "synchronizing" ? "连接中..." : "登录"
                        onClicked: root.submitOnlineLogin()
                        contentItem: RowLayout { anchors.centerIn: parent; spacing: 8
                            Icon { name: "network"; iconColor: loginButton.enabled ? t.bg : t.muted; iconSize: 14 }
                            Text { text: loginButton.text; color: loginButton.enabled ? t.bg : t.muted; font.bold: true }
                        }
                        background: Rectangle {
                            color: loginButton.enabled ? t.accent : t.border
                            radius: 5
                            border.color: loginButton.activeFocus ? t.text : "transparent"
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0.0; color: loginButton.enabled ? t.accent : t.border }
                                GradientStop { position: 1.0; color: loginButton.enabled ? t.info : t.border }
                            }
                            Behavior on color { ColorAnimation { duration: 150 } }
                        }
                    }
                }

                ColumnLayout {
                    visible: root.selectedMode === "local"; spacing: 16; Layout.fillWidth: true
                    Text { text: "本地"; color: t.text; font.pixelSize: 20; font.bold: true }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: t.border }
                    Button {
                        id: localEnterButton
                        Layout.fillWidth: true; Layout.preferredHeight: 44; text: "进入"
                        onClicked: { root.controller.useLocalMode(); root.close() }
                        contentItem: Text { anchors.fill: parent; text: localEnterButton.text; color: t.bg; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.bold: true }
                        background: Rectangle {
                            color: t.accent; radius: 5; border.color: localEnterButton.activeFocus ? t.text : "transparent"
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0.0; color: t.accent }
                                GradientStop { position: 1.0; color: t.info }
                            }
                        }
                    }
                }
                Item { Layout.preferredHeight: root.shortLayout ? 0 : 20 }
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
