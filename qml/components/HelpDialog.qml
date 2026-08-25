pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: root

    property var controller: null
    property int categoryIndex: 0
    readonly property bool narrow: width < 760
    readonly property var categories: [
        { label: "快速开始", icon: "play" },
        { label: "联网推演", icon: "network" },
        { label: "VMF 演示", icon: "command" },
        { label: "情报通信", icon: "scan" },
        { label: "房间管理", icon: "settings" },
        { label: "报错处理", icon: "warning" }
    ]

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(980, Math.max(280, parent ? parent.width - 28 : 980))
    height: Math.min(780, Math.max(360, parent ? parent.height - 28 : 780))
    padding: 0
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: AppContext.stateMotion; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: AppContext.stateMotion; easing.type: Easing.OutBack }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: AppContext.fastMotion; easing.type: Easing.InCubic }
            NumberAnimation { property: "scale"; from: 1; to: 0.98; duration: AppContext.fastMotion; easing.type: Easing.InCubic }
        }
    }
    Overlay.modal: Rectangle { color: "#05080de6" }
    background: Rectangle {
        color: AppContext.page
        border.color: AppContext.line
        border.width: 1
        radius: 7
    }

    component BodyText: Text {
        Layout.fillWidth: true
        color: AppContext.textDim
        font.pixelSize: 11
        lineHeight: 1.35
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
    }

    component HelpSection: Item {
        id: section
        property string heading: ""
        property string summary: ""
        property string iconName: "dot"
        property bool expanded: false
        default property alias sectionData: sectionBody.data

        Layout.fillWidth: true
        implicitHeight: sectionHeader.height + (expanded ? sectionBody.implicitHeight + 14 : 0) + 1

        AbstractButton {
            id: sectionHeader
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 52
            focusPolicy: Qt.StrongFocus
            onClicked: section.expanded = !section.expanded
            Accessible.name: (section.expanded ? "收起" : "展开") + section.heading
            background: Rectangle {
                color: sectionHeader.hovered || sectionHeader.activeFocus
                    ? AppContext.raised : "transparent"
                radius: 4
                Behavior on color { ColorAnimation { duration: AppContext.fastMotion } }
            }
            contentItem: RowLayout {
                spacing: 10
                Icon { name: section.iconName; iconColor: section.expanded ? AppContext.signal : AppContext.muted; iconSize: 16 }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text { text: section.heading; color: AppContext.text; font.pixelSize: 12; font.bold: true }
                    Text { Layout.fillWidth: true; text: section.summary; color: AppContext.muted; font.pixelSize: 9; elide: Text.ElideRight }
                }
                Icon {
                    name: "chevron-down"
                    iconColor: AppContext.muted
                    iconSize: 16
                    rotation: section.expanded ? 180 : 0
                    Behavior on rotation { NumberAnimation { duration: AppContext.fastMotion; easing.type: Easing.OutCubic } }
                }
            }
        }
        ColumnLayout {
            id: sectionBody
            visible: section.expanded
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: sectionHeader.bottom
            anchors.leftMargin: 26
            anchors.rightMargin: 8
            spacing: 9
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: AppContext.softLine
        }
        Behavior on implicitHeight { NumberAnimation { duration: AppContext.stateMotion; easing.type: Easing.OutCubic } }
    }

    component MapFigure: Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 170
        radius: 5
        clip: true
        color: AppContext.panel
        border.color: AppContext.line
        Image {
            anchors.fill: parent
            source: "qrc:/qt/qml/index/map/12/3406/1747.png"
            fillMode: Image.PreserveAspectCrop
            opacity: 0.72
        }
        Rectangle { anchors.fill: parent; color: "#08101666" }
        Repeater {
            model: [
                { x: 0.14, y: 0.62, color: AppContext.signal, label: "指挥" },
                { x: 0.35, y: 0.31, color: AppContext.info, label: "侦察" },
                { x: 0.53, y: 0.68, color: AppContext.warning, label: "攻击" },
                { x: 0.78, y: 0.38, color: AppContext.danger, label: "目标" }
            ]
            delegate: Rectangle {
                id: mapMarker
                required property var modelData
                required property int index
                x: parent.width * modelData.x - width / 2
                y: parent.height * modelData.y - height / 2
                width: 54; height: 24; radius: 4
                color: "#0a1118e6"; border.color: modelData.color
                Text { anchors.centerIn: parent; text: mapMarker.modelData.label; color: mapMarker.modelData.color; font.pixelSize: 10; font.bold: true }
                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    NumberAnimation { from: 0.72; to: 1; duration: 900 }
                    NumberAnimation { from: 1; to: 0.72; duration: 900 }
                }
            }
        }
        Text {
            anchors.left: parent.left; anchors.bottom: parent.bottom
            anchors.margins: 9
            text: "地图、战位与任务目标在同一画布联动"
            color: AppContext.text; font.pixelSize: 10; font.bold: true
        }
    }

    component FlowFigure: Item {
        Layout.fillWidth: true
        Layout.preferredHeight: 76
        RowLayout {
            anchors.fill: parent
            spacing: 4
            Repeater {
                model: ["侦察", "派单", "引导", "授权", "攻击", "毁伤", "返航"]
                delegate: RowLayout {
                    id: flowStep
                    required property string modelData
                    required property int index
                    Layout.fillWidth: true
                    spacing: 4
                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 36; radius: 4
                        color: flowStep.index === 0 ? "#123b35" : AppContext.raised
                        border.color: flowStep.index === 0 ? AppContext.signal : AppContext.line
                        Text { anchors.centerIn: parent; text: flowStep.modelData; color: flowStep.index === 0 ? AppContext.signal : AppContext.textDim; font.pixelSize: 9; font.bold: flowStep.index === 0 }
                    }
                    Icon { visible: flowStep.index < 6; name: "chevron-right"; iconColor: AppContext.muted; iconSize: 12 }
                }
            }
        }
    }

    header: Rectangle {
        implicitHeight: 58
        color: AppContext.panel
        border.color: AppContext.line
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 10
            spacing: 10
            Icon { name: "help"; iconColor: AppContext.signal; iconSize: 20 }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 1
                Text { text: "操作帮助"; color: AppContext.textStrong; font.pixelSize: 16; font.bold: true }
                Text { text: root.controller && root.controller.networked ? "联网模式" : "本地模式"; color: AppContext.muted; font.pixelSize: 9 }
            }
            GhostButton {
                text: ""; iconName: "close"; iconSize: 17
                implicitWidth: 36; implicitHeight: 34
                onClicked: root.close()
                Accessible.name: "关闭帮助"
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 0
        ComboBox {
            visible: root.narrow
            Layout.fillWidth: true
            Layout.margins: 10
            model: root.categories
            textRole: "label"
            currentIndex: root.categoryIndex
            onActivated: root.categoryIndex = currentIndex
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                visible: !root.narrow
                Layout.preferredWidth: 190
                Layout.fillHeight: true
                color: AppContext.panel
                border.color: AppContext.line
                ListView {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 4
                    model: root.categories
                    delegate: AbstractButton {
                        id: categoryButton
                        required property var modelData
                        required property int index
                        width: ListView.view.width
                        height: 42
                        focusPolicy: Qt.StrongFocus
                        onClicked: root.categoryIndex = index
                        background: Rectangle {
                            color: root.categoryIndex === categoryButton.index
                                ? AppContext.raised : categoryButton.hovered ? AppContext.softLine : "transparent"
                            border.color: root.categoryIndex === categoryButton.index
                                ? AppContext.signal : "transparent"
                            radius: 4
                        }
                        contentItem: RowLayout {
                            spacing: 9
                            Icon { name: categoryButton.modelData.icon; iconColor: root.categoryIndex === categoryButton.index ? AppContext.signal : AppContext.muted; iconSize: 15 }
                            Text { text: categoryButton.modelData.label; color: root.categoryIndex === categoryButton.index ? AppContext.text : AppContext.textDim; font.pixelSize: 11; font.bold: root.categoryIndex === categoryButton.index }
                        }
                    }
                }
            }

            ScrollView {
                id: helpScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                ScrollBar.vertical.policy: ScrollBar.AsNeeded
                ColumnLayout {
                    width: Math.max(0, helpScroll.availableWidth - (root.narrow ? 24 : 44))
                    x: root.narrow ? 12 : 22
                    y: 12
                    spacing: 0

                    ColumnLayout {
                        visible: root.categoryIndex === 0
                        Layout.fillWidth: true; spacing: 0
                        HelpSection { heading: "五分钟上手"; summary: "选择模式、进入视图、选择单位、执行操作"; iconName: "play"; expanded: true
                            MapFigure {}
                            BodyText { text: "1. 启动后先选本地模式或联网模式。\n2. 本地模式在顶部切换编辑、红方、蓝方或导演席；联网模式先登录，再选择房间与战位。\n3. 在地图或右侧单位列表选择单位。可执行动作只会在当前阶段和权限允许时启用。\n4. 地图上的移动、扫描、攻击、部署等操作提交后，以服务端回传状态为准。\n5. 遇到按钮不可用，先检查推演阶段、战位、单位存活状态和右上角连接状态。" }
                        }
                        HelpSection { heading: "地图操作"; summary: "选择、缩放、定位和范围显示"; iconName: "map"
                            BodyText { text: "左键选择单位；滚轮缩放；拖动平移。部署、人工情报和部分指令进入选点状态后，按界面提示在地图取点。Escape 取消当前操作。右侧单位、情报、战术标点的“定位”都会同步移动地图中心。" }
                        }
                        HelpSection { heading: "本地模式"; summary: "场景编辑、推演控制和导演席"; iconName: "local"
                            BodyText { text: "编辑视图用于增删单位、调整位置与参数；开始推演前确保红蓝双方各有且只有一个存活指挥所。指挥视图按阵营展示已知态势，导演席可查看全局。顶部运行开关、速率和单步只在本地模式提供。" }
                        }
                    }

                    ColumnLayout {
                        visible: root.categoryIndex === 1
                        Layout.fillWidth: true; spacing: 0
                        HelpSection { heading: "登录到进入战位"; summary: "账号服务、房间大厅与战位确认"; iconName: "network"; expanded: true
                            BodyText { text: "1. 在联网登录页填写账号服务地址、用户名和密码。\n2. 登录成功后从房间列表选择“进入房间”；运行中的 VMF 房间也允许参演者进入。\n3. 战位页先看阵营与状态。VMF 中标为“可接管”的红方自动战位可直接接管；普通房间的占用战位不能抢占。\n4. 战位只有在服务器回传确认后才算接管成功，界面随后进入部署或战斗页。" }
                        }
                        HelpSection { heading: "部署与就绪"; summary: "指挥官部署，本方战位依次确认"; iconName: "locate"
                            BodyText { text: "准备阶段由指挥官在部署队列选择战位，再在地图指定位置。普通战位确认部署后提交就绪；指挥官最后确认本方就绪。VMF 的蓝方为固定靶，红方自动战位已经由服务器托管，无需人工重复部署。" }
                        }
                        HelpSection { heading: "战斗页"; summary: "单位、指挥、情报三个工作区"; iconName: "unit"
                            BodyText { text: "“单位”处理本战位单位和战斗动作；“指挥”查看通信、指令、交接与部署申请；“情报”查看接触、定位、共享和人工报告。普通战位只能控制自己的单位，指挥官可通过有效指挥链控制本方单位。" }
                        }
                        HelpSection { heading: "退出与重连"; summary: "保留权威状态，避免重复提交"; iconName: "return"
                            BodyText { text: "网络中断时客户端会自动重连并请求完整快照。界面显示“同步中”期间不要连续点击操作。主动退出房间会释放普通战位；运行中的严格 VMF 战位会由服务器自动控制接续，稍后仍可由其他用户接管。" }
                        }
                    }

                    ColumnLayout {
                        visible: root.categoryIndex === 2
                        Layout.fillWidth: true; spacing: 0
                        HelpSection { heading: "VMF 演示全流程"; summary: "从接管战位到任务返航"; iconName: "command"; expanded: true
                            FlowFigure {}
                            BodyText { text: "VMF 房间只开放红方参演，蓝方由服务器作为固定目标。红方指挥、侦察、攻击、地面战位可以由人工接管；未接管的环节由服务器自动推进。全部通信范围按无限处理，不需要调整通信距离。\n\n开始推演后，指挥官在 VMF 工作区选择已观测目标并创建任务。工作区始终只给当前战位显示一个有效的下一步动作。点击后等待任务阶段和修订号更新，再继续下一步。" }
                        }
                        HelpSection { heading: "各战位怎么操作"; summary: "指挥、侦察、攻击、地面四类职责"; iconName: "unit"
                            BodyText { text: "指挥战位：创建任务、派单、命令地面引导、下达撤离。\n侦察战位：提交目标报告，在攻击后确认目标摧毁。\n攻击战位：接受派单、身份握手、接受航路、报告攻击就绪、实施攻击、报告毁伤、报告返航。\n地面战位：确认会合与身份、发送引导包、授权攻击、确认毁伤评估。\n\n如果当前环节属于未接管战位，面板显示“自动推进”，无需替它点击。用户中途接管后，从服务器当前阶段继续。" }
                        }
                        HelpSection { heading: "任务状态与回执"; summary: "阶段、操作者、ACK 和 Codec 诊断"; iconName: "check"
                            BodyText { text: "阶段标题表示权威任务状态；“当前操作者”指出下一步由哪个战位执行；按钮可用时才允许本战位提交。提交后先显示等待回执，收到新快照后阶段前进。诊断区默认折叠，其中 trace、bit 长度、Catalog 和往返一致性用于确认 VMF 编解码，不是日常操作步骤。" }
                        }
                        HelpSection { heading: "演示卡住时"; summary: "按阶段、战位、目标和链路逐项检查"; iconName: "warning"
                            BodyText { text: "先确认房间为“推演中”，再核对当前战位是否就是面板所列操作者。目标必须仍存活且已观测；任务绑定的指挥、侦察、攻击、地面单元必须存在。若显示自动推进但阶段长时间不变，查看错误弹窗中的 VMF 代码，再让网页管理员暂停并恢复；不要重复创建同一任务。" }
                        }
                    }

                    ColumnLayout {
                        visible: root.categoryIndex === 3
                        Layout.fillWidth: true; spacing: 0
                        HelpSection { heading: "精简情报页"; summary: "先看高价值记录，需要时再展开"; iconName: "scan"; expanded: true
                            BodyText { text: "情报页默认只显示少量最新记录。每条保留目标、鲜度、置信度、来源和定位；搜索、类型筛选、完整列表、传播链等放在展开区。实时表示仍被持续观测，失联表示位置可能过期，归档记录不能继续共享。" }
                        }
                        HelpSection { heading: "定位、共享与人工报告"; summary: "四个常用动作"; iconName: "send"
                            BodyText { text: "定位：选择记录并把地图移动到最后位置。\n详情：查看坐标、观测时间、备注和传播来源。\n共享：选中记录与接收战位后提交，可附短备注。\n人工报告：先在地图选点，再选报告类型并填写标题或备注。\n\n所有提交都要等待服务器确认；“已提交”不等于已经写入台账。" }
                        }
                        HelpSection { heading: "指挥通信"; summary: "实时消息、指令和通信状态"; iconName: "chat"
                            BodyText { text: "普通战位从“指挥”页向本方指挥官发送消息；指挥官在收件箱集中查看。普通联网模式运行阶段仍受通信拓扑影响，只有双向链路可发送。严格 VMF 房间通信距离无限，但身份、任务阶段和战位权限校验仍然有效。" }
                        }
                    }

                    ColumnLayout {
                        visible: root.categoryIndex === 4
                        Layout.fillWidth: true; spacing: 0
                        HelpSection { heading: "网页端与客户端分工"; summary: "生命周期与场景编辑不要混用"; iconName: "server"; expanded: true
                            BodyText { text: "网页管理端负责账号、房间开启/暂停/恢复/停止和运行状态；桌面房间管理员在准备阶段编辑场景、协议 profile、侦察范围等。运行中的场景禁止编辑。VMF profile 下战位编制和固定靶由服务端生成，通信范围固定为无限。" }
                        }
                        HelpSection { heading: "修改房间配置"; summary: "只在准备阶段保存"; iconName: "edit"
                            BodyText { text: "进入房间管理后先重载当前配置，修改名称、说明、场景或协议，再保存。保存期间不要重复点击。若其他管理员已更新配置，服务器会拒绝旧版本，应重载后重新修改。场景单位决定战位容量，不要把战位数量当成独立数据源。" }
                        }
                        HelpSection { heading: "开启一局 VMF 演示"; summary: "建议顺序"; iconName: "play"
                            BodyText { text: "1. 网页端将房间置为准备状态。\n2. 桌面房间管理选择严格 VMF profile，检查红方四类任务资源和蓝方目标。\n3. 玩家进入并接管需要的红方战位；其余红方战位保持自动控制。\n4. 红方指挥官就绪后，由网页端启动推演。\n5. 指挥官创建任务，各战位按 VMF 面板推进。\n6. 结束后由网页端停止或重置房间。" }
                        }
                    }

                    ColumnLayout {
                        visible: root.categoryIndex === 5
                        Layout.fillWidth: true; spacing: 0
                        HelpSection { heading: "登录与连接"; summary: "401、服务不可用、会话失效"; iconName: "network"; expanded: true
                            BodyText { text: "登录失败 / 401：检查用户名、密码和账号是否启用；用户名与密码只要求非空，不会因长度被客户端拒绝。\n账号服务不可用：确认地址协议、主机和端口，检查网页管理端是否能打开。\nGAME_SERVER_UNAVAILABLE：账号服务可用，但游戏 WebSocket 未启动或地址不可达。\nSESSION_REVOKED：账号被停用、令牌过期或在别处注销，重新登录。\n同步超时：保持窗口打开，等待自动重连；持续失败时重新选择会话。" }
                        }
                        HelpSection { heading: "房间与战位"; summary: "ROOM、SEAT 和 COMMANDER 错误"; iconName: "unit"
                            BodyText { text: "ROOM_NOT_FOUND / ROOM_CLOSED：房间已关闭或列表过期，返回房间页刷新。\nROOM_FINISHED：本局已结束，等待管理员重置。\nSEAT_OCCUPIED：普通战位被真人占用；VMF 自动战位应显示“可接管”，刷新后重试。\nCOMMANDER_PRIORITY：先由红方指挥官进入；普通模式还需蓝方指挥官。\nSEAT_LOCKED：当前阶段不能换位；严格 VMF 仅允许未入座用户接管红方自动战位。\nSIDE_RESERVED_FOR_FIXED_TARGET：VMF 蓝方是固定靶，不能接管。\nALREADY_SEATED：当前会话已有战位，使用退出或战位切换流程。" }
                        }
                        HelpSection { heading: "部署、命令与持久化"; summary: "权限、版本、保存和状态同步"; iconName: "save"
                            BodyText { text: "PERMISSION_DENIED：当前战位、阶段或单位不允许该动作。\nSTALE_STATE / REVISION_CONFLICT：本地状态落后，等待完整同步后重试。\nINITIAL_UNIT_MISSING：场景没有与战位匹配的初始单位，房间管理员检查单位类型和阵营。\nPERSISTENCE_FAILED：服务端未能写入检查点或事件日志，操作已回滚；检查数据目录权限和磁盘空间。\nRUNTIME_RESET_FAILED：房间权威编制与运行场景不一致，需要管理员停止并恢复房间。" }
                        }
                        HelpSection { heading: "VMF 错误"; summary: "任务顺序、角色、目标与编码"; iconName: "warning"
                            BodyText { text: "VMF_ROLE_FORBIDDEN：当前战位不是该阶段操作者。\nVMF_SEQUENCE_INVALID：动作与当前任务阶段不符，等待最新快照。\nTASK_NOT_FOUND：任务尚未同步或已清理，重新选择任务。\nTARGET_NOT_OBSERVED：指挥侧还没有该目标情报，等待侦察记录。\nTARGET_NOT_DESTROYED：权威仿真仍判定目标存活，不能提前确认。\nVMF_MESSAGE_INVALID：消息身份、任务关联或通信链校验失败。\nVMF_CODEC_FAILED / VMF_CANONICAL_MISMATCH：客户端与服务器 VMF 字典或编码结果不一致，确认三端版本相同。\nDUPLICATE_MESSAGE：同一 trace 已处理，不要重复提交。\nRESOURCE_BUSY：同一资源已绑定到另一未完成任务。" }
                        }
                    }
                    Item { Layout.fillWidth: true; Layout.preferredHeight: 20 }
                }
            }
        }
    }
}
