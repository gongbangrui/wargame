pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    property var controller: null
    property var mapCanvas: null
    property var seats: []
    property var reportPosition: null
    property bool reportPicking: false
    property color accent: AppContext.signal
    property color line: AppContext.line
    property color panel: AppContext.panel
    property color page: AppContext.page
    property color text: AppContext.text
    property color muted: AppContext.muted
    property string filterText: ""
    property string sourceFilter: ""
    property string freshnessFilter: "all"
    property string typeFilter: "all"
    property var selectedRecipients: []
    property var selectedRecord: null
    property string shareState: ""
    property string reportState: ""
    property string historyState: ""
    property string shareRequestId: ""
    property string reportRequestId: ""
    property string historyRequestId: ""
    signal reportPickRequested()
    signal reportPickCancelled()

    implicitHeight: body.implicitHeight
    Layout.fillWidth: true

    function filteredRecords() {
        var source = root.controller ? root.controller.onlineIntelRecords : []
        var result = []
        var needle = root.filterText.trim().toLowerCase()
        for (var i = 0; i < source.length; ++i) {
            var item = source[i] || ({})
            var attrs = item.knownAttributes || ({})
            if (root.freshnessFilter !== "all"
                    && String(item.freshness || "") !== root.freshnessFilter) continue
            if (root.typeFilter !== "all"
                    && String(item.type || "") !== root.typeFilter) continue
            if (root.sourceFilter.length > 0
                    && String(item.sourceSeatId || "").toLowerCase()
                        .indexOf(root.sourceFilter.toLowerCase()) < 0) continue
            var haystack = String(item.targetId || "") + " " + String(item.intelId || "")
                + " " + String(attrs.title || "") + " " + String(attrs.callsign || "")
                + " " + String(item.sourceSeatId || "") + " " + String(item.note || "")
            if (needle.length > 0 && haystack.toLowerCase().indexOf(needle) < 0) continue
            result.push(item)
        }
        return result
    }

    function seatLabel(seatId) {
        for (var i = 0; i < root.seats.length; ++i) {
            var seat = root.seats[i] || ({})
            if (String(seat.seatId || "") !== String(seatId || "")) continue
            var labels = { commander: "指挥官", attack: "攻击机", recon: "侦察机",
                           ground: "地面单位", jammer: "干扰机" }
            var slot = seat.slot && seat.seatType !== "commander" ? " #" + seat.slot : ""
            return (seat.side === "red" ? "红方 · " : "蓝方 · ")
                + (labels[seat.seatType] || seat.seatId) + slot
        }
        return String(seatId || "")
    }

    function toggleRecipient(id) {
        var copy = root.selectedRecipients.slice(0)
        var index = copy.indexOf(id)
        if (index >= 0) copy.splice(index, 1)
        else copy.push(id)
        root.selectedRecipients = copy
    }

    function locate(record) {
        root.selectedRecord = record || null
        var target = String((record || {}).targetId || "")
        if (target.length > 0 && root.controller) root.controller.setFocusedUnitId(target)
        var position = (record || {}).lastPosition || (record || {}).position || ({})
        if (root.mapCanvas && position.x !== undefined && position.y !== undefined)
            root.mapCanvas.centerOn(Number(position.x), Number(position.y))
    }

    function historyQuery(cursor) {
        var query = { pageSize: 50 }
        var target = historySearch.text.trim()
        var source = historySource.text.trim()
        var from = historyFrom.text.trim()
        var to = historyTo.text.trim()
        if (cursor) query.cursor = cursor
        if (target) query.target = target
        if (historyType.currentIndex > 0) query.type = historyType.currentValue
        if (historyFreshness.currentIndex > 0)
            query.freshness = historyFreshness.currentValue
        if (source) query.sourceSeatId = source
        if (from) query.from = from
        if (to) query.to = to
        return query
    }

    function requestHistory(cursor) {
        if (!root.controller || root.historyRequestId.length > 0) return
        root.historyState = "已提交"
        root.historyRequestId = root.controller.requestOnlineIntelHistory(
            root.historyQuery(cursor || ""))
        if (!root.historyRequestId) root.historyState = "未提交"
    }

    function statusText(status, message) {
        if (status === "accepted") return "已确认"
        if (status === "rejected" || status === "unknown")
            return message ? "失败 · " + message : "失败"
        return "已提交"
    }

    function loadHistoryDefault() {
        if (!root.controller) return
        var value = String(root.controller.loadSetting(
            "online/intel/defaultHistoryFreshness", ""))
        historyFreshness.currentIndex = value === "live" ? 1
            : value === "stale" ? 2 : value === "archived" ? 3 : 0
    }

    Connections {
        target: root.controller
        function onOnlineIntelCommandStatus(action, requestId, status, code, message) {
            if (action === "shareIntel" && requestId === root.shareRequestId) {
                root.shareState = root.statusText(status, message)
                if (status === "accepted" || status === "rejected" || status === "unknown")
                    root.shareRequestId = ""
            } else if (action === "createIntelReport"
                       && requestId === root.reportRequestId) {
                root.reportState = root.statusText(status, message)
                if (status === "accepted" || status === "rejected" || status === "unknown")
                    root.reportRequestId = ""
            } else if (action === "requestIntelHistory"
                       && requestId === root.historyRequestId) {
                root.historyState = root.statusText(status, message)
                if (status === "accepted" || status === "rejected" || status === "unknown")
                    root.historyRequestId = ""
            }
        }
        function onSettingChanged(key) {
            if (key === "online/intel/defaultHistoryFreshness")
                root.loadHistoryDefault()
        }
        function onOnlineIntelChanged() {
            if (!root.selectedRecord || !root.controller) return
            var records = root.controller.onlineIntelRecords || []
            for (var i = 0; i < records.length; ++i) {
                if (records[i] && records[i].intelId === root.selectedRecord.intelId) return
            }
            root.selectedRecord = null
        }
    }

    Component.onCompleted: root.loadHistoryDefault()

    ColumnLayout {
        id: body
        width: parent.width
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 5
            Text { text: "情报"; color: root.text; font.bold: true; font.pixelSize: 12 }
            Text {
                text: root.controller ? (root.controller.onlineIntelRevision + " rev") : ""
                color: root.muted
                font.pixelSize: 9
            }
            Item { Layout.fillWidth: true }
            ComboBox {
                id: viewMode
                model: ["当前", "历史"]
                Layout.preferredWidth: 78
                Layout.preferredHeight: 26
                onActivated: if (currentIndex === 1
                                    && root.controller.onlineIntelHistory.length === 0)
                                 root.requestHistory("")
            }
        }

        RowLayout {
            visible: viewMode.currentIndex === 0
            Layout.fillWidth: true
            spacing: 4
            TextField {
                Layout.fillWidth: true
                placeholderText: "目标、来源或备注"
                maximumLength: 128
                onTextChanged: root.filterText = text
            }
            ComboBox {
                id: freshness
                model: ["全部", "实时", "失联", "归档"]
                Layout.preferredWidth: 76
                onActivated: root.freshnessFilter = currentIndex === 1 ? "live"
                    : currentIndex === 2 ? "stale" : currentIndex === 3 ? "archived" : "all"
            }
            ComboBox {
                id: kind
                model: ["全部", "接触", "人工"]
                Layout.preferredWidth: 76
                onActivated: root.typeFilter = currentIndex === 1 ? "sensorContact"
                    : currentIndex === 2 ? "manualReport" : "all"
            }
        }
        TextField {
            visible: viewMode.currentIndex === 0
            Layout.fillWidth: true
            placeholderText: "来源战位"
            maximumLength: 128
            onTextChanged: root.sourceFilter = text.trim()
        }

        ListView {
            visible: viewMode.currentIndex === 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(190, Math.max(42, contentHeight))
            clip: true
            spacing: 3
            model: root.filteredRecords()
            delegate: Rectangle {
                id: recordDelegate
                required property var modelData
                required property int index
                width: ListView.view.width
                height: 44
                color: root.selectedRecord
                       && root.selectedRecord.intelId === recordDelegate.modelData.intelId
                    ? root.page : recordDelegate.index % 2 ? root.page : root.panel
                border.color: recordDelegate.modelData.freshness === "live"
                    ? root.accent : root.line
                radius: 4
                MouseArea {
                    anchors.fill: parent
                    z: 0
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.selectedRecord = recordDelegate.modelData
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 6
                    Icon {
                        name: recordDelegate.modelData.type === "manualReport" ? "plus"
                            : recordDelegate.modelData.freshness === "live" ? "scan" : "warning"
                        iconSize: 14
                        iconColor: recordDelegate.modelData.freshness === "live"
                            ? root.accent : AppContext.warning
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            text: recordDelegate.modelData.targetId
                                || (recordDelegate.modelData.knownAttributes || ({})).title
                                || recordDelegate.modelData.intelId
                            color: root.text
                            font.pixelSize: 10
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Text {
                            text: String(recordDelegate.modelData.freshness || "") + " · "
                                + Number(recordDelegate.modelData.confidence || 0).toFixed(0)
                                + "% · " + root.seatLabel(recordDelegate.modelData.sourceSeatId)
                            color: root.muted
                            font.pixelSize: 9
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }
                    Button {
                        z: 1
                        text: "定位"
                        onClicked: root.locate(recordDelegate.modelData)
                        contentItem: Text {
                            text: "定位"
                            color: root.accent
                            font.pixelSize: 9
                            horizontalAlignment: Text.AlignHCenter
                        }
                        background: Rectangle { color: "transparent" }
                    }
                }
            }
        }

        Rectangle {
            visible: viewMode.currentIndex === 0 && root.selectedRecord !== null
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? detailColumn.implicitHeight + 12 : 0
            color: root.page
            border.color: root.line
            radius: 4
            ColumnLayout {
                id: detailColumn
                anchors.fill: parent
                anchors.margins: 6
                spacing: 2
                Text {
                    text: root.selectedRecord ? (root.selectedRecord.targetId
                        || ((root.selectedRecord.knownAttributes || ({})).title
                            || root.selectedRecord.intelId)) : ""
                    color: root.text
                    font.bold: true
                    font.pixelSize: 10
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                Text {
                    text: root.selectedRecord ? ("位置 "
                        + Number((root.selectedRecord.lastPosition || {}).x || 0).toFixed(0)
                        + ", " + Number((root.selectedRecord.lastPosition || {}).y || 0).toFixed(0)
                        + " · 最后观测 " + String(root.selectedRecord.lastObservedAt || "")) : ""
                    color: root.muted
                    font.pixelSize: 9
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                Text {
                    text: root.selectedRecord ? ("备注 "
                        + String(root.selectedRecord.note || "无")) : ""
                    color: root.muted
                    font.pixelSize: 9
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                }
                Text {
                    text: root.selectedRecord ? ("传播 "
                        + ((root.selectedRecord.propagationSources || []).length > 0
                           ? (root.selectedRecord.propagationSources || []).map(function(item) {
                                 return root.seatLabel(item.sourceSeatId)
                             }).join(" > ") : "本战位")) : ""
                    color: root.muted
                    font.pixelSize: 9
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }
        }

        Flow {
            visible: viewMode.currentIndex === 0
            Layout.fillWidth: true
            spacing: 4
            Repeater {
                model: root.controller ? root.controller.onlineIntelShareTargets : []
                delegate: CheckBox {
                    id: recipientCheckBox
                    required property string modelData
                    text: root.seatLabel(recipientCheckBox.modelData)
                    checked: root.selectedRecipients.indexOf(recipientCheckBox.modelData) >= 0
                    onToggled: root.toggleRecipient(recipientCheckBox.modelData)
                    contentItem: Text {
                        text: recipientCheckBox.text
                        color: recipientCheckBox.checked ? root.accent : root.muted
                        font.pixelSize: 9
                        leftPadding: 18
                    }
                }
            }
        }

        RowLayout {
            visible: viewMode.currentIndex === 0
            Layout.fillWidth: true
            spacing: 5
            TextField {
                id: shareNote
                Layout.fillWidth: true
                placeholderText: "共享备注"
                maximumLength: 500
            }
            Button {
                id: shareButton
                text: "共享"
                enabled: root.shareRequestId.length === 0 && root.selectedRecord !== null
                    && root.selectedRecord.freshness !== "archived"
                    && root.selectedRecipients.length > 0 && root.controller
                onClicked: {
                    root.shareState = "已提交"
                    root.shareRequestId = root.controller.shareOnlineIntel(
                        root.selectedRecord.intelId, root.selectedRecipients,
                        shareNote.text.trim())
                    if (!root.shareRequestId) root.shareState = "未提交"
                }
                contentItem: Text {
                    text: shareButton.text
                    color: shareButton.enabled ? root.accent : root.muted
                    font.pixelSize: 10
                }
                background: Rectangle { color: root.panel; border.color: root.line; radius: 4 }
            }
        }
        Text {
            visible: viewMode.currentIndex === 0 && root.shareState.length > 0
            text: "共享 · " + root.shareState
            color: root.shareState.indexOf("失败") === 0 ? AppContext.danger : root.muted
            font.pixelSize: 9
            Layout.fillWidth: true
            elide: Text.ElideRight
        }

        ColumnLayout {
            visible: viewMode.currentIndex === 0
            Layout.fillWidth: true
            spacing: 4
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Button {
                    id: reportPickButton
                    text: root.reportPicking ? "取消选点" : "地图选点"
                    onClicked: root.reportPicking ? root.reportPickCancelled()
                                                   : root.reportPickRequested()
                    contentItem: Text {
                        text: reportPickButton.text
                        color: root.reportPicking ? AppContext.warning : root.accent
                        font.pixelSize: 9
                    }
                    background: Rectangle { color: root.panel; border.color: root.line; radius: 4 }
                }
                Text {
                    Layout.fillWidth: true
                    text: root.reportPosition
                        ? Math.round(Number(root.reportPosition.x)) + ", "
                            + Math.round(Number(root.reportPosition.y))
                        : "未选择位置"
                    color: root.reportPosition ? root.text : root.muted
                    font.pixelSize: 9
                    elide: Text.ElideRight
                }
                ComboBox {
                    id: reportType
                    model: [
                        { label: "位置报告", value: "location" },
                        { label: "疑似接触", value: "suspectedContact" },
                        { label: "障碍", value: "obstacle" }
                    ]
                    textRole: "label"
                    valueRole: "value"
                    Layout.preferredWidth: 112
                }
            }
            TextField {
                id: reportTitle
                Layout.fillWidth: true
                placeholderText: "报告标题"
                maximumLength: 64
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                TextField {
                    id: reportNote
                    Layout.fillWidth: true
                    placeholderText: "报告备注"
                    maximumLength: 500
                }
                Button {
                    id: reportButton
                    text: "报告"
                    enabled: root.reportRequestId.length === 0 && root.controller
                        && root.reportPosition !== null
                        && (reportTitle.text.trim().length > 0
                            || reportNote.text.trim().length > 0)
                    onClicked: {
                        root.reportState = "已提交"
                        root.reportRequestId = root.controller.createOnlineIntelReport(
                            root.reportPosition, reportType.currentValue,
                            reportTitle.text.trim(), reportNote.text.trim())
                        if (!root.reportRequestId) root.reportState = "未提交"
                    }
                    contentItem: Text {
                        text: reportButton.text
                        color: reportButton.enabled ? root.accent : root.muted
                        font.pixelSize: 10
                    }
                    background: Rectangle { color: root.panel; border.color: root.line; radius: 4 }
                }
            }
            Text {
                visible: root.reportState.length > 0
                text: "报告 · " + root.reportState
                color: root.reportState.indexOf("失败") === 0 ? AppContext.danger : root.muted
                font.pixelSize: 9
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
        }

        ColumnLayout {
            visible: viewMode.currentIndex === 1
            Layout.fillWidth: true
            spacing: 4
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                TextField {
                    id: historySearch
                    Layout.fillWidth: true
                    placeholderText: "目标名称或标识"
                    maximumLength: 128
                }
                ComboBox {
                    id: historyType
                    model: [
                        { label: "全部类型", value: "" },
                        { label: "接触", value: "sensorContact" },
                        { label: "人工", value: "manualReport" }
                    ]
                    textRole: "label"
                    valueRole: "value"
                    Layout.preferredWidth: 92
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                ComboBox {
                    id: historyFreshness
                    model: [
                        { label: "全部鲜度", value: "" },
                        { label: "实时", value: "live" },
                        { label: "失联", value: "stale" },
                        { label: "归档", value: "archived" }
                    ]
                    textRole: "label"
                    valueRole: "value"
                    Layout.preferredWidth: 92
                }
                TextField {
                    id: historySource
                    Layout.fillWidth: true
                    placeholderText: "来源战位"
                    maximumLength: 128
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                TextField {
                    id: historyFrom
                    Layout.fillWidth: true
                    placeholderText: "开始时间 ISO 8601"
                    maximumLength: 64
                }
                TextField {
                    id: historyTo
                    Layout.fillWidth: true
                    placeholderText: "结束时间 ISO 8601"
                    maximumLength: 64
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Button {
                    text: "首页"
                    enabled: root.historyRequestId.length === 0
                    onClicked: root.requestHistory("")
                    contentItem: Text { text: "首页"; color: root.accent; font.pixelSize: 9 }
                    background: Rectangle { color: "transparent" }
                }
                Button {
                    text: "下一页"
                    enabled: root.historyRequestId.length === 0 && root.controller
                        && root.controller.onlineIntelHistoryHasMore
                        && root.controller.onlineIntelHistoryCursor.length > 0
                    onClicked: root.requestHistory(root.controller.onlineIntelHistoryCursor)
                    contentItem: Text {
                        text: "下一页"
                        color: parent.enabled ? root.accent : root.muted
                        font.pixelSize: 9
                    }
                    background: Rectangle { color: "transparent" }
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: root.historyState
                    color: root.historyState.indexOf("失败") === 0
                        ? AppContext.danger : root.muted
                    font.pixelSize: 9
                    elide: Text.ElideRight
                }
            }
            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(180, Math.max(38, contentHeight))
                clip: true
                model: root.controller ? root.controller.onlineIntelHistory : []
                delegate: Button {
                    id: historyDelegate
                    required property var modelData
                    width: ListView.view.width
                    height: 34
                    onClicked: root.locate(historyDelegate.modelData)
                    contentItem: Text {
                        text: (historyDelegate.modelData.occurredAt || "") + " · "
                            + (historyDelegate.modelData.eventType || "") + " · "
                            + (historyDelegate.modelData.targetId
                               || historyDelegate.modelData.intelId || "")
                        color: root.muted
                        font.pixelSize: 9
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle { color: historyDelegate.hovered ? root.page : "transparent" }
                }
            }
        }
    }
}
