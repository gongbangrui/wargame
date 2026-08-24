pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"

Item {
    id: root
    property var controller: null
    property var editor: null
    property var canvas: onlineCanvas
    property bool autoFollow: true
    property string deployUnitId: ""
    property string deploymentTargetSeatId: ""
    property string deploymentState: "idle"
    property string selectedUnitId: ""
    property string lastCenteredUnitId: ""
    property string attackTargetId: ""
    property bool commanderPointSelectionActive: false
    property bool showCommunicationRange: false
    property bool showDetectionRange: true
    property bool showAttackRange: true
    property string subordinateMessageDraft: ""
    property string deploymentNotice: ""
    property string unitNameDraft: ""
    property string unitNameDraftSeatId: ""
    property bool unitNameDirty: false
    property int unitSpeedDraft: 1
    property string unitSpeedDraftUnitId: ""
    property bool unitSpeedDirty: false
    property string selectedCommandMarkerId: ""
    property var intelReportPosition: null
    property bool intelReportPicking: false
    property var dismissedCommandMarkerIds: []
    property var selectedRecipientIds: []
    property bool switchSeatSelection: false
    property bool switchRequestPending: false
    property bool exitRequestPending: false
    property string switchSourceSeatId: ""
    property string switchTargetSeatId: ""
    property string observedMatchPhase: ""
    property bool showIntelLive: true
    property bool showIntelStale: true
    property bool showIntelManual: true
    property bool showIntelUncertainty: true
    property bool notifyNewIntel: true
    property bool notifyIntelShare: true
    property string intelNotificationScope: ""
    property bool intelNotificationBaselineReady: false
    property var knownIntelIds: ({})
    // The compact and desktop layouts share this one panel; only its active
    // tactical view changes.  Keeping the state here also preserves the tab
    // when the panel is moved into the narrow-screen drawer.
    property int tacticalViewIndex: 0
    property bool compactLayout: width < 840
    onCompactLayoutChanged: if (!compactLayout) tacticalDrawer.close()
    property color page: AppContext.page
    property color ink: AppContext.text
    property color dim: AppContext.muted
    property color panel: AppContext.panel
    property color panelAlt: AppContext.raised
    property color line: AppContext.line
    property color cyan: AppContext.signal
    property color orange: AppContext.warning
    property color info: AppContext.info
    property color danger: AppContext.danger
    readonly property bool strictVmf: root.controller
        && root.controller.protocolProfile === "vmf-guided-strike-v1"

    onAttackTargetIdChanged: root.syncAttackTargetBox()
    onSelectedUnitIdChanged: root.queueSelectedUnitCenter()
    onDeployUnitIdChanged: root.queueSelectedUnitCenter()

    signal openChatRequested()

    function reloadOnlineSettings() {
        if (!root.controller) return
        root.showIntelLive = Boolean(root.controller.loadSetting("online/intel/showLive", true))
        root.showIntelStale = Boolean(root.controller.loadSetting("online/intel/showStale", true))
        root.showIntelManual = Boolean(root.controller.loadSetting("online/intel/showManual", true))
        root.showIntelUncertainty = Boolean(root.controller.loadSetting("online/intel/showUncertainty", true))
        root.notifyNewIntel = Boolean(root.controller.loadSetting("online/notifications/newIntel", true))
        root.notifyIntelShare = Boolean(root.controller.loadSetting("online/notifications/intelShare", true))
        root.showCommunicationRange = Boolean(root.controller.loadSetting("online/map/showCommunicationRange", false))
        root.showDetectionRange = Boolean(root.controller.loadSetting("online/map/showDetectionRange", true))
        root.showAttackRange = Boolean(root.controller.loadSetting("online/map/showAttackRange", true))
        root.tacticalViewIndex = Math.max(0, Math.min(2,
            Number(root.controller.loadSetting("online/sidebar/defaultView", 0))))
    }

    function currentIntelNotificationScope() {
        if (!root.controller) return ""
        return String(root.controller.currentRoomId || "") + "|"
            + String(root.controller.currentSeatId || "")
    }

    function resetIntelNotificationBaseline() {
        root.intelNotificationScope = root.currentIntelNotificationScope()
        root.intelNotificationBaselineReady = false
        root.knownIntelIds = ({})
    }

    function rememberIntelId(intelId) {
        var id = String(intelId || "")
        if (!id) return
        var known = root.knownIntelIds
        known[id] = true
        root.knownIntelIds = known
    }

    function syncIntelNotifications() {
        var scope = root.currentIntelNotificationScope()
        if (scope !== root.intelNotificationScope)
            root.resetIntelNotificationBaseline()
        var records = root.controller ? root.controller.onlineIntelRecords : []
        var known = root.knownIntelIds
        var newRecords = []
        for (var i = 0; i < records.length; ++i) {
            var record = records[i] || ({})
            var intelId = String(record.intelId || "")
            if (!intelId) continue
            if (root.intelNotificationBaselineReady && !known[intelId])
                newRecords.push(record)
            known[intelId] = true
        }
        root.knownIntelIds = known
        root.intelNotificationBaselineReady = true
        if (!root.notifyNewIntel || newRecords.length === 0) return
        if (newRecords.length > 1) {
            root.deploymentNotice = "收到 " + newRecords.length + " 条新情报"
            return
        }
        var item = newRecords[0]
        var attributes = item.knownAttributes || ({})
        root.deploymentNotice = "新情报 · "
            + String(item.targetId || attributes.title || item.intelId)
    }

    property bool isHumanControlledSeat: root.controller
        && (!root.strictVmf || root.controller.currentSeatSide === "red")
        && (root.controller.roomMode !== "pve" || root.controller.currentSeatSide === "red")
    property bool isCommander: root.isHumanControlledSeat && root.controller.currentSeatType === "commander"
    property bool canDeploy: root.isCommander && root.controller.matchPhase === "preparing"
                            && root.deploymentTargetSeatId.length > 0 && root.deployUnitId.length > 0
    property var selectedUnitSnapshot: {
        if (!root.controller || !root.selectedUnitId) return ({})
        if (root.controller.unitStateRevision >= 0)
            return root.controller.unitAt(root.selectedUnitId) || ({})
        return ({})
    }

    function unitSpeedLimit(unit) {
        var reported = Number((unit || ({})).maxCommandedSpeed)
        if (isFinite(reported) && reported > 0) return reported
        var kind = (unit || ({})).kind || ""
        if (kind === "attackuav") return 360
        if (kind === "reconuav") return 300
        if (kind === "jammeruav") return 260
        if (kind === "groundscout") return 36
        if (kind === "groundtarget") return 0
        return 0
    }

    function onlineMapVisibleUnitIds() {
        if (!root.controller || root.controller.isObserver) return null
        var ids = root.deployedUnitIds()
        var selectedId = root.selectedUnitId || root.deployUnitId
        if (selectedId && ids.indexOf(selectedId) < 0) ids.push(selectedId)
        var projectedUnits = root.controller.units || []
        for (var i = 0; i < projectedUnits.length; ++i) {
            var unit = projectedUnits[i] || ({})
            if (unit.kind === "groundtarget" && unit.id && ids.indexOf(unit.id) < 0)
                ids.push(unit.id)
        }
        return ids
    }

    function editableUnitSpeedLimit(unit) {
        return Math.max(1, root.unitSpeedLimit(unit))
    }

    function ownSeatUnitSnapshot() {
        var seat = root.seatById(root.controller.currentSeatId) || ({})
        return root.controller.unitAt(seat.unitId) || ({})
    }

    function queueSelectedUnitCenter() {
        Qt.callLater(function() {
            var unitId = root.selectedUnitId || root.deployUnitId
            if (!unitId || unitId === root.lastCenteredUnitId || !onlineCanvas) return
            if (onlineCanvas.focusOnUnit(unitId)) root.lastCenteredUnitId = unitId
        })
    }

    function resetRoundViewState() {
        root.selectedUnitId = ""
        root.lastCenteredUnitId = ""
        root.attackTargetId = ""
        root.deployUnitId = ""
        root.deploymentTargetSeatId = ""
        root.deploymentState = "idle"
        root.selectedCommandMarkerId = ""
        root.intelReportPosition = null
        root.intelReportPicking = false
        root.dismissedCommandMarkerIds = []
        root.selectedRecipientIds = []
        root.subordinateMessageDraft = ""
        root.unitNameDraft = ""
        root.unitNameDraftSeatId = ""
        root.unitNameDirty = false
        root.unitSpeedDraft = 1
        root.unitSpeedDraftUnitId = ""
        root.unitSpeedDirty = false
        root.switchSeatSelection = false
        root.switchRequestPending = false
        root.switchSourceSeatId = ""
        root.switchTargetSeatId = ""
        if (root.commanderPointSelectionActive) root.cancelCommanderPointSelection()
        commanderCommandPanel.mapSelectionCanceled()
    }

    function syncAttackTargetBox() {
        if (!targetBox) return
        var wanted = root.attackTargetId || ""
        var options = targetBox.model || []
        var next = -1
        for (var i = 0; i < options.length; i++) {
            var option = options[i] || ({})
            if (String(option.id || "") === wanted) {
                next = i
                break
            }
        }
        targetBox.currentIndex = next
    }

    function selectedActionEnabled(action) {
        var actions = root.selectedUnitSnapshot.actions
            || root.selectedUnitSnapshot.actionCapabilities || ({})
        var entry = actions[action]
        if (entry === undefined) return false
        return typeof entry === "object" && entry !== null
            ? Boolean(entry.enabled) : Boolean(entry)
    }

    function adjustShortcutUnitSpeed(delta) {
        if (!root.controller || root.controller.isObserver
                || root.controller.matchPhase !== "running" || !root.selectedUnitId)
            return
        if (!root.selectedActionEnabled("setSpeed")) return
        var limit = root.unitSpeedLimit(root.selectedUnitSnapshot)
        if (limit <= 0) return
        var current = Number(root.selectedUnitSnapshot.speed)
        if (!isFinite(current)) current = Number(root.unitSpeedDraft || 1)
        var next = Math.max(1, Math.min(limit, Math.round((current + delta) / 5) * 5))
        root.unitSpeedDraft = next
        root.unitSpeedDirty = true
        root.controller.command("setSpeed", { unitId: root.selectedUnitId, speed: next })
        root.deploymentNotice = "移动速度已提交 · " + next + " m/s"
    }

    function shortcutsBlocked() {
        return Boolean((switchSeatConfirmation && switchSeatConfirmation.opened)
                       || (exitRoomConfirmation && exitRoomConfirmation.opened)
                       || (tacticalDrawer && tacticalDrawer.opened)
                       || (selectedUnitPanel && selectedUnitPanel.detailsOpen))
    }

    function normalizedCommunicationState() {
        return root.controller.communicationState === "twoWay"
            ? "bilateral" : root.controller.communicationState
    }
    function communicationLabel() {
        if (root.isCommander) return "指挥链路已接入"
        var state = root.normalizedCommunicationState()
        if (state === "bilateral") return "双向通信"
        if (state === "receiveOnly") return "仅可接收"
        return "通信中断"
    }
    function communicationColor() {
        if (root.isCommander) return root.cyan
        var state = root.normalizedCommunicationState()
        return state === "bilateral" ? root.cyan
             : state === "receiveOnly" ? root.orange : root.danger
    }
    function priorityIntelSummary() {
        var records = root.controller ? (root.controller.onlineIntelRecords || []) : []
        var live = 0
        var stale = 0
        var actionable = 0
        for (var i = 0; i < records.length; ++i) {
            var record = records[i] || ({})
            if (record.freshness === "live") live++
            else if (record.freshness === "stale") stale++
            if (record.actionable === true) actionable++
        }
        if (actionable > 0) return "待处置 " + actionable
        if (stale > 0) return "失联 " + stale + " · 实时 " + live
        return live > 0 ? "实时 " + live : "无待处置情报"
    }
    function commanderSeatId() {
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) {
            if (seats[i].side === root.controller.currentSeatSide
                    && seats[i].seatType === "commander" && seats[i].occupied)
                return seats[i].seatId
        }
        return ""
    }
    function subordinateCanSend() {
        if (!root.commanderSeatId()) return false
        if (root.controller.matchPhase === "preparing") return true
        return root.controller.matchPhase === "running"
            && root.normalizedCommunicationState() === "bilateral"
    }
    function rangeDisplayUnitIds() {
        if (root.selectedUnitId) return [root.selectedUnitId]
        var ownSeat = root.seatById(root.controller.currentSeatId)
        return ownSeat && ownSeat.unitId ? [ownSeat.unitId] : []
    }
    function selectedFriendlySeat() {
        return root.seatForUnit(root.selectedUnitId)
    }

    function roomStatusLabel(status) {
        var labels = {
            stopped: "停止",
            preparing: "准备",
            paused: "暂停",
            running: "推演",
            finished: "已结束",
            ready: "已就绪",
            stale: "状态过期",
            unknown: "状态未知"
        }
        return labels[status] || "状态未知"
    }
    function roomStatusColor(status) {
        return status === "preparing" ? root.cyan
             : status === "running" || status === "ready" ? root.info
             : status === "paused" || status === "stale" ? root.orange
             : status === "stopped" || status === "finished" ? root.danger
             : root.dim
    }
    function roomCanJoin(room) {
        return room && room.enabled !== false && room.hostedByGameServer === true
                && room.status === "preparing"
    }
    function roomCanObserve(room) {
        return room && room.enabled !== false && room.hostedByGameServer === true
                && ["preparing", "running", "paused", "finished"].indexOf(room.status) >= 0
    }
    function roomModeLabel(room) {
        if (room && room.protocolProfile === "vmf-guided-strike-v1")
            return "VMF"
        if (room && room.mode === "pve") {
            var labels = { easy: "简单", normal: "普通", hard: "困难" }
            return "人机对抗 · " + (labels[room.aiDifficulty] || "普通")
        }
        return "人人对抗"
    }
    function roomConfigurationLabel() {
        if (root.strictVmf)
            return "VMF · 红方"
        if (root.controller.roomMode === "pve") {
            var labels = { easy: "简单", normal: "普通", hard: "困难" }
            return "人机对抗 · " + (labels[root.controller.aiDifficulty] || "普通")
        }
        return "人人对抗"
    }
    function orderedRooms() {
        var rooms = (root.controller.onlineRooms || []).filter(function(room) {
            return room && room.roomId
        })
        rooms.sort(function(a, b) {
            var aJoinable = root.roomCanJoin(a) ? 0 : root.roomCanObserve(a) ? 1 : 2
            var bJoinable = root.roomCanJoin(b) ? 0 : root.roomCanObserve(b) ? 1 : 2
            if (aJoinable !== bJoinable) return aJoinable - bJoinable
            return String(a.name || a.roomId).localeCompare(String(b.name || b.roomId))
        })
        return rooms
    }
    function aiEngineLabel() {
        var engine = root.controller && root.controller.aiEffectiveEngine
        if (engine === "ollama") return "Ollama"
        if (engine === "rules") return "规则 AI"
        return "同步中"
    }
    function aiEngineColor() {
        return root.controller && root.controller.aiEffectiveEngine === "ollama"
             ? root.cyan : root.orange
    }
    function aiDifficultyLabel() {
        var difficulty = root.controller && root.controller.aiDifficulty
        if (difficulty === "hard") return "困难 · 战略情报增强"
        if (difficulty === "easy") return "简单 · 反应延迟"
        return "普通 · 协同规划"
    }
    function isRoomEmpty() {
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) if (seats[i].occupied) return false
        return true
    }
    function seatById(seatId) {
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) if (seats[i].seatId === seatId) return seats[i]
        return null
    }
    function canClaimSeat(seat) {
        if (!seat || seat.controllerType === "ai") return false
        if (root.strictVmf) {
            if (seat.side !== "red" || seat.claimable !== true
                    || root.controller.currentSeatId) return false
            var strictPhase = root.controller.matchPhase
            if (["preparing", "running", "paused"].indexOf(strictPhase) < 0)
                return false
            var strictCommander = root.seatById("red_commander")
            var humanCommander = strictCommander && strictCommander.occupied
                    && strictCommander.controlMode === "human"
            if (!humanCommander)
                return strictPhase === "preparing" && seat.seatId === "red_commander"
            return strictPhase === "preparing" || seat.controlMode === "vmf-auto"
        }
        if (seat.controllerType === "placeholder") {
            return false
        }
        if (seat.occupied || root.controller.matchPhase !== "preparing") return false
        if (root.controller.roomMode === "pve" && seat.side !== "red") return false
        if (root.switchSeatSelection) {
            return !root.switchRequestPending && seat.side === root.controller.currentSeatSide
                    && seat.seatType !== "commander"
        }
        var redCommander = root.seatById("red_commander")
        var blueCommander = root.seatById("blue_commander")
        if (root.isRoomEmpty()) return seat.seatId === "red_commander"
        if (!redCommander || !redCommander.occupied) return seat.seatId === "red_commander"
        if (!blueCommander || !blueCommander.occupied) return seat.seatId === "blue_commander"
        return true
    }
    function seatHint(seat) {
        if (seat.controlMode === "fixed-target") return "保留场景参数，不执行任何操作"
        if (seat.controlMode === "vmf-auto") return "缺失环节由服务器自动分发传递"
        if (seat.controllerType === "ai") return "AI 控制"
        if (seat.controllerType === "placeholder") return "服务器控制"
        if (seat.occupied) return seat.displayName || "已占用"
        if (root.switchSeatSelection && seat.side !== root.controller.currentSeatSide)
            return "只能申请本方战位"
        if (root.switchSeatSelection && seat.seatType === "commander")
            return "指挥官战位不可在此切换"
        if (root.switchRequestPending && seat.seatId === root.switchTargetSeatId)
            return "等待本方指挥官确认"
        if (root.canClaimSeat(seat)) return "可选择"
        if (root.strictVmf) return "请先选择红方指挥官"
        if (root.isRoomEmpty()) return "请先选择红方指挥官"
        return "等待双方指挥官就位"
    }
    function seatStatusLabel(seat) {
        if (seat.controlMode === "fixed-target") return "固定靶"
        if (seat.controlMode === "vmf-auto") return "服务器自动控制"
        if (seat.controllerType === "placeholder") return "服务器控制"
        if (!seat.occupied) return "空缺"
        if (seat.controllerType === "ai") return seat.deployed ? "AI 执行中" : "AI 已就位"
        if (seat.pendingTransfer) return "交接中"
        if (!seat.connected) return "失联"
        if (!seat.deployed) return seat.seatType === "commander" ? "待选指挥所" : "等待指挥官部署"
        if (seat.ready) return "已就绪"
        return "已部署"
    }
    function seatStatusColor(seat) {
        if (seat.controlMode === "fixed-target") return root.info
        if (seat.controlMode === "vmf-auto") return root.cyan
        if (seat.controllerType === "placeholder") return root.orange
        if (!seat.occupied) return root.dim
        if (seat.controllerType === "ai") return seat.deployed ? root.cyan : root.info
        if (seat.pendingTransfer || !seat.deployed) return root.orange
        if (!seat.connected) return root.danger
        if (seat.ready) return root.cyan
        return root.info
    }
    function pendingDeploymentSeats() {
        var result = []
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) {
            var seat = seats[i]
            if (seat.occupied && seat.side === root.controller.currentSeatSide && !seat.deployed)
                result.push(seat)
        }
        return result
    }
    function pendingRedeploySeats() {
        var result = []
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) {
            var seat = seats[i]
            if (seat.occupied && seat.side === root.controller.currentSeatSide
                    && seat.redeployRequested) result.push(seat)
        }
        return result
    }
    function friendlySeatsReady() {
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) {
            var seat = seats[i]
            if (seat.occupied && seat.side === root.controller.currentSeatSide
                    && seat.seatType !== "commander" && (!seat.deployed || !seat.ready)) return false
        }
        return true
    }
    function hasFriendlyDeployment() {
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) {
            if (seats[i].occupied && seats[i].side === root.controller.currentSeatSide
                    && seats[i].deployed) return true
        }
        return false
    }
    function deployedUnitIds() {
        var ids = []
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) {
            if (seats[i].occupied && seats[i].deployed && seats[i].unitId)
                ids.push(seats[i].unitId)
        }
        return ids
    }
    function deployedFriendlyUnits() {
        if (root.controller.isObserver) {
            return (root.controller.units || []).filter(function(unit) {
                return unit && unit.alive
            })
        }
        var deployed = root.deployedUnitIds()
        return (root.controller.units || []).filter(function(unit) {
            return unit.alive && unit.side === root.controller.currentSeatSide
                    && deployed.indexOf(unit.id) >= 0
        })
    }
    function seatsForSide(side) {
        var seats = (root.controller.onlineSeats || []).filter(function(seat) {
            return seat.side === side
        })
        seats.sort(function(a, b) {
            var aCommander = a.seatType === "commander" ? 0 : 1
            var bCommander = b.seatType === "commander" ? 0 : 1
            if (aCommander !== bCommander) return aCommander - bCommander
            if (a.seatType !== b.seatType) return String(a.seatType).localeCompare(String(b.seatType))
            return Number(a.slot || 0) - Number(b.slot || 0)
        })
        return seats
    }
    function refreshUnitNameDraft() {
        var seat = root.seatById(root.controller.currentSeatId) || {}
        var unit = root.controller.unitAt(seat.unitId) || {}
        var seatId = seat.seatId || ""
        var authoritativeName = seat.unitName || unit.callsign || ""
        if (root.unitNameDraftSeatId !== seatId) {
            root.unitNameDraftSeatId = seatId
            root.unitNameDirty = false
            root.unitNameDraft = authoritativeName
            return
        }
        if (root.unitNameDirty) {
            if (authoritativeName !== root.unitNameDraft.trim()) return
            root.unitNameDirty = false
        }
        root.unitNameDraft = authoritativeName
    }
    function refreshUnitSpeedDraft() {
        var seat = root.seatById(root.controller.currentSeatId) || {}
        var unit = root.controller.unitAt(seat.unitId) || {}
        var speed = Math.round(Number(unit.speed || 1))
        var unitId = seat.unitId || ""
        if (root.unitSpeedDraftUnitId !== unitId) {
            root.unitSpeedDraftUnitId = unitId
            root.unitSpeedDirty = false
            root.unitSpeedDraft = Math.max(1, Math.min(root.editableUnitSpeedLimit(unit), speed))
            return
        }
        if (root.unitSpeedDirty) {
            if (speed !== root.unitSpeedDraft) return
            root.unitSpeedDirty = false
        }
        root.unitSpeedDraft = Math.max(1, Math.min(root.editableUnitSpeedLimit(unit), speed))
    }
    function dismissSelectedCommandMarker() {
        if (!root.selectedCommandMarkerId) return
        var dismissed = root.dismissedCommandMarkerIds.slice()
        if (dismissed.indexOf(root.selectedCommandMarkerId) < 0)
            dismissed.push(root.selectedCommandMarkerId)
        root.dismissedCommandMarkerIds = dismissed
        root.selectedCommandMarkerId = ""
    }
    function selectDeploymentSeat(seat) {
        if (!root.isCommander || !seat || seat.side !== root.controller.currentSeatSide
                || !seat.occupied || seat.deployed || !seat.unitId) return
        root.deploymentTargetSeatId = seat.seatId
        root.deployUnitId = seat.unitId
        root.deploymentState = "selected"
        root.deploymentNotice = "已选择 " + root.seatLabel(seat) + "，请在地图上点击指挥所位置"
    }
    function reconcileDeploymentState() {
        if (!root.isCommander) {
            var ownSeat = root.seatById(root.controller.currentSeatId)
            if (ownSeat && ownSeat.deployed) {
                root.deploymentState = "deployed"
                root.deployUnitId = ""
                root.selectedUnitId = ownSeat.unitId || root.selectedUnitId
            } else if (ownSeat && ownSeat.occupied) {
                root.deploymentState = "waiting"
                root.selectedUnitId = root.strictVmf ? (ownSeat.unitId || "") : ""
            }
            return
        }
        if (root.deploymentTargetSeatId) {
            var target = root.seatById(root.deploymentTargetSeatId)
            if (target && target.deployed) {
                root.deploymentState = "deployed"
                root.deploymentNotice = root.seatLabel(target) + " 已完成部署"
                root.deploymentTargetSeatId = ""
                root.deployUnitId = ""
            }
        }
        if (!root.deploymentTargetSeatId && root.controller.matchPhase === "preparing") {
            var pending = root.pendingDeploymentSeats()
            if (pending.length > 0) root.selectDeploymentSeat(pending[0])
        }
    }
    function selectUnit(unit) {
        if (!unit) return
        if (root.controller.isObserver) {
            root.selectedUnitId = unit.id
            root.attackTargetId = ""
            return
        }
        if (unit.side !== root.controller.currentSeatSide) return
        var ownSeat = root.seatById(root.controller.currentSeatId)
        if (!root.isCommander && ownSeat && ownSeat.unitId && ownSeat.unitId !== unit.id) return
        root.selectedUnitId = unit.id
        if (root.isCommander && root.controller.matchPhase !== "running") return
        root.attackTargetId = ""
    }
    function observerTrajectoryIds() {
        var projected = root.controller.observerTrajectories || ({})
        return projected.selectedUnitIds ? projected.selectedUnitIds.slice() : []
    }
    function observerTrajectorySelected(unitId) {
        return root.observerTrajectoryIds().indexOf(unitId) >= 0
    }
    function toggleObserverTrajectory(unitId) {
        if (!root.controller.isObserver || !unitId) return
        var selected = root.observerTrajectoryIds()
        var index = selected.indexOf(unitId)
        if (index >= 0) selected.splice(index, 1)
        else {
            if (selected.length >= 8) {
                root.deploymentNotice = "最多同时显示 8 个单位轨迹"
                return
            }
            selected.push(unitId)
        }
        root.controller.setObserverTrajectories(selected)
    }
    function switchUnit(direction) {
        var candidates = root.controller.isObserver
            ? (root.controller.units || []).filter(function(unit) { return unit && unit.alive })
            : root.deployedFriendlyUnits()
        if (!candidates || candidates.length === 0) return
        var current = -1
        for (var i = 0; i < candidates.length; i++) {
            if (candidates[i].id === root.selectedUnitId) { current = i; break }
        }
        var next = (current + direction + candidates.length) % candidates.length
        root.selectUnit(candidates[next])
    }
    function autoFitZoom() {
        if (onlineCanvas) onlineCanvas.focusAt(onlineCanvas.mapSize.w / 2,
                                               onlineCanvas.mapSize.h / 2)
    }
    function openTacticalDrawer() {
        if (root.compactLayout) tacticalDrawer.open()
    }
    function cancelOnlineShortcut() {
        if (root.commanderPointSelectionActive) root.cancelCommanderPointSelection()
        if (onlineCanvas) onlineCanvas.stopGuideMode()
    }
    function shortcutUnitId() {
        return root.selectedUnitId || root.controller.focusedUnitId || ""
    }
    function engageFocusedTarget() {
        if (!root.controller.isObserver && root.controller.currentSeatType === "attack"
                && root.controller.matchPhase === "running"
                && root.selectedUnitId && root.attackTargetId
                && root.selectedActionEnabled("engageTarget")) {
            root.controller.command("engageTarget", {
                attackerId: root.selectedUnitId, targetId: root.attackTargetId
            })
        }
    }

    function engageAvailabilityLabel() {
        if (!root.selectedUnitId) return "请选择攻击机"
        if (!root.attackTargetId) return "请选择已掌握的敌方目标"
        if (!root.selectedActionEnabled("engageTarget")) {
            var cooldown = Number(root.selectedUnitSnapshot.cooldownRemaining || 0)
            if (cooldown > 0) return "武器冷却中 · " + cooldown.toFixed(1) + " s"
            if (Number(root.selectedUnitSnapshot.ammoRemaining || 0) <= 0) return "弹药已耗尽"
            if (root.selectedUnitSnapshot.serviceRequested) return "补给进行中"
            return "服务器暂不允许攻击"
        }
        return "执行攻击"
    }
    function seatForUnit(unitId) {
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) {
            if (seats[i].occupied && seats[i].unitId === unitId) return seats[i]
        }
        return null
    }
    function cancelCommanderPointSelection() {
        root.commanderPointSelectionActive = false
        onlineCanvas.stopGuideMode()
    }
    function selectAttackTarget(unitId) {
        root.attackTargetId = root.attackTargetId === unitId ? "" : unitId
    }
    function startSwitchSeatSelection() {
        root.switchSourceSeatId = root.controller.currentSeatId
        root.switchTargetSeatId = ""
        root.switchRequestPending = false
        root.switchSeatSelection = true
        root.deploymentNotice = "当前战位保持不变。选择目标战位后将提交给本方指挥官确认。"
    }
    function cancelSwitchSeatSelection() {
        if (root.switchRequestPending) return
        root.switchSeatSelection = false
        root.switchTargetSeatId = ""
        root.deploymentNotice = "已取消战位切换，当前战位未变更。"
    }
    function requestSeat(seat) {
        if (!root.canClaimSeat(seat)) return
        root.switchTargetSeatId = seat.seatId
        root.controller.claimOnlineSeat(seat.seatId)
        if (root.switchSeatSelection) {
            root.switchRequestPending = true
            root.deploymentNotice = "切换请求已提交，当前战位将在服务器确认后变更。"
        }
    }

    function seatLabel(seat) {
        var labels = { commander: "指挥官", attack: "攻击机", recon: "侦察机", ground: "地面单位", jammer: "干扰机" }
        var slot = seat.slot && seat.seatType !== "commander" ? " #" + seat.slot : ""
        return (seat.side === "red" ? "红方 · " : "蓝方 · ") + (labels[seat.seatType] || seat.seatId) + slot
    }

    function recipientSeats() {
        var result = []
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) {
            var seat = seats[i]
            if (seat.occupied && seat.side === root.controller.currentSeatSide
                    && seat.seatId !== root.controller.currentSeatId
                    && root.selectedRecipientIds.indexOf(seat.seatId) >= 0)
                result.push(seat.seatId)
        }
        return result
    }

    function mapMarkRecipientSeats() {
        var selected = root.recipientSeats()
        if (selected.length > 0 || !root.isCommander) return selected
        var result = []
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) {
            var seat = seats[i]
            if (seat.occupied && seat.deployed && seat.side === root.controller.currentSeatSide
                    && seat.seatId !== root.controller.currentSeatId)
                result.push(seat.seatId)
        }
        return result
    }
    function toggleRecipient(seatId, checked) {
        var next = root.selectedRecipientIds.slice()
        var index = next.indexOf(seatId)
        if (checked && index < 0) next.push(seatId)
        if (!checked && index >= 0) next.splice(index, 1)
        root.selectedRecipientIds = next
    }
    function selectedRecipient(seatId) { return root.selectedRecipientIds.indexOf(seatId) >= 0 }

    function seatForUnitId(unitId) {
        var seats = root.controller.onlineSeats || []
        for (var i = 0; i < seats.length; i++) {
            if (seats[i].unitId === unitId) return seats[i]
        }
        return null
    }
    function commandKind(message) {
        if (!message) return ""
        if (message.type === "UnitOrder") return "text"
        if (message.type === "AttackOrder") return "attack"
        if (message.type === "Withdraw") return "withdrawal"
        if (message.type === "Guidance" && message.payload && message.payload.kind === "moveTo")
            return "maneuver"
        return ""
    }
    function commandKindLabel(kind) {
        var labels = { text: "文字命令", attack: "攻击", maneuver: "机动", withdrawal: "撤离" }
        return labels[kind] || "命令"
    }
    function commandSourceLabel(unitId, seatId) {
        var seat = seatId ? root.seatById(seatId) : root.seatForUnitId(unitId)
        if (seat) return root.seatLabel(seat)
        var unit = unitId ? root.controller.unitAt(unitId) : null
        return (unit && (unit.callsign || unit.id)) || "指挥席"
    }
    function commandResultFor(message, messages) {
        var payload = message.payload || {}
        if (payload.result === "rejected" || payload.status === "rejected") return "已拒绝"
        if (payload.result === "completed" || payload.status === "completed") return "已完成"
        for (var i = 0; i < messages.length; i++) {
            var acknowledgement = messages[i]
            if (acknowledgement.type === "Ack" && acknowledgement.payload
                    && acknowledgement.payload.inReplyTo === message.id) return "已确认"
        }
        return message.requiresAck ? "待回执" : ""
    }
    function targetLabel(targetId) {
        var target = targetId ? root.controller.unitAt(targetId) : null
        return target ? (target.callsign || target.id) : "已授权目标"
    }
    function participantCommandFeed() {
        if (root.controller.unitStateRevision < 0) return []
        var ownSeat = root.seatById(root.controller.currentSeatId)
        var ownUnitId = ownSeat ? ownSeat.unitId : ""
        if (!root.isCommander && !ownUnitId) return []
        var feed = []
        var messages = root.controller.messages || []
        for (var i = 0; i < messages.length; i++) {
            var message = messages[i]
            var kind = root.commandKind(message)
            if (!kind) continue
            var receiverSeat = root.seatForUnitId(message.receiver)
            if (root.isCommander) {
                if (!receiverSeat || receiverSeat.side !== root.controller.currentSeatSide) continue
            } else if (message.receiver !== ownUnitId) continue
            var payload = message.payload || {}
            var commanderOrder = payload.notificationOnly === true
            feed.push({
                id: message.id || (message.type + "_" + i),
                kind: kind,
                commanderOrder: commanderOrder,
                source: root.commandSourceLabel(
                    commanderOrder ? (message.sender || "") : (message.receiver || ""), ""),
                time: message.time || "",
                text: payload.text || "",
                targetId: payload.targetId || "",
                point: payload.x !== undefined && payload.y !== undefined
                    ? { x: Number(payload.x), y: Number(payload.y) }
                    : kind === "withdrawal" && payload.homeX !== undefined && payload.homeY !== undefined
                    ? { x: Number(payload.homeX), y: Number(payload.homeY) } : null,
                status: root.commandResultFor(message, messages)
            })
        }
        feed.sort(function(a, b) { return String(b.time).localeCompare(String(a.time)) })
        return feed.slice(0, 30)
    }
    function participantMapMarkers() {
        if (root.controller.unitStateRevision < 0) return []
        var markers = (root.controller.onlineMapMarks || []).slice()
        var feed = root.participantCommandFeed()
        var hasParticipantMarker = false
        for (var i = 0; i < feed.length; i++) {
            var command = feed[i]
            if (root.dismissedCommandMarkerIds.indexOf(command.id) >= 0) continue
            if (!command.commanderOrder) {
                // A subordinate's movement guidance is personal intent, not a
                // commander order. Keep only its newest point and never
                // synthesize it on the commander's canvas.
                if (root.isCommander || hasParticipantMarker) continue
            }
            var point = command.point
            if (!point && command.kind === "attack") {
                var target = root.controller.unitAt(command.targetId)
                if (target && target.position && target.position.length >= 2)
                    point = { x: target.position[0], y: target.position[1] }
            }
            if (!point || !isFinite(point.x) || !isFinite(point.y)) continue
            if (!command.commanderOrder) hasParticipantMarker = true
            markers.push({ id: command.id, position: point, side: root.controller.currentSeatSide,
                           seatId: command.commanderOrder ? "" : root.controller.currentSeatId,
                           category: command.commanderOrder ? "command" : "selfMove",
                           markType: command.commanderOrder ? "commander" : "self",
                           commandKind: command.kind, label: root.commandKindLabel(command.kind),
                           source: command.source, status: command.status, time: command.time })
        }
        return markers
    }
    function visibleMapMarkFeed() {
        var marks = (root.controller.onlineMapMarks || []).slice()
        marks.sort(function(a, b) { return String(b.time || "").localeCompare(String(a.time || "")) })
        return marks.slice(0, 8)
    }
    function mapMarkRoleLabel(mark) {
        if (!mark) return "战术标点"
        if (mark.markType === "commander") return "指挥标点"
        if (mark.seatId === root.controller.currentSeatId) return "我的标点"
        var seat = root.seatById(mark.seatId)
        return root.isCommander && seat ? "下属标点 · " + root.seatLabel(seat) : "友方标点"
    }
    function mapMarkCoordinate(mark) {
        var position = mark && mark.position
        if (!position) return ""
        var x = position.x !== undefined ? Number(position.x) : Number(position[0])
        var y = position.y !== undefined ? Number(position.y) : Number(position[1])
        if (!isFinite(x) || !isFinite(y)) return ""
        return "X " + Math.round(x) + "  /  Y " + Math.round(y) + " m"
    }
    function attackTargetLabel(fallback) {
        if (!root.attackTargetId) return fallback || "选择已掌握的敌方目标"
        var target = root.controller.unitAt(root.attackTargetId)
        return target && target.callsign ? target.callsign : root.attackTargetId
    }

    Rectangle { anchors.fill: parent; color: root.page }
    Rectangle { visible: root.deploymentNotice.length > 0; anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; z: 20; width: Math.min(parent.width - 40, 620); height: Math.max(38, noticeText.implicitHeight + 16); color: root.panelAlt; border.color: root.orange; radius: 5
        Text { id: noticeText; anchors.fill: parent; anchors.margins: 8; text: root.deploymentNotice; color: root.orange; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight }
        Timer { interval: 6500; running: root.deploymentNotice.length > 0; onTriggered: root.deploymentNotice = "" }
    }

    Connections {
        target: root.controller
        function onDeploymentPrompt(prompt) {
            if (root.isCommander && prompt.targetSeatId) {
                root.deploymentTargetSeatId = prompt.targetSeatId
                root.deployUnitId = prompt.unitId || ""
                root.deploymentState = "awaiting"
                root.deploymentNotice = prompt.message || "请为待部署战位选择位置"
            } else {
                root.deploymentTargetSeatId = ""
                root.deployUnitId = ""
                root.deploymentState = "waiting"
                root.deploymentNotice = prompt.message || "等待本方指挥官部署"
            }
        }
        function onOnlineSeatsChanged() {
            root.reconcileDeploymentState()
            root.queueSelectedUnitCenter()
            root.refreshUnitNameDraft()
            root.refreshUnitSpeedDraft()
            if (root.commanderPointSelectionActive && !root.seatForUnit(root.selectedUnitId)) {
                commanderCommandPanel.mapSelectionCanceled()
                root.cancelCommanderPointSelection()
            }
        }
        function onOnlineStateChanged() {
            root.reconcileDeploymentState()
            root.queueSelectedUnitCenter()
            if (root.currentIntelNotificationScope() !== root.intelNotificationScope)
                root.resetIntelNotificationBaseline()
            if (root.controller.onlineStage === "login") root.controller.requestOnlineRooms()
            if (root.controller.onlineStage === "seatSelect"
                    && root.controller.currentSeatId === ""
                    && (root.selectedUnitId !== "" || root.deployUnitId !== "")) {
                root.resetRoundViewState()
            }
            root.refreshUnitNameDraft()
            root.refreshUnitSpeedDraft()
            if (root.exitRequestPending && root.controller.onlineStage === "roomSelect") {
                root.exitRequestPending = false
                root.switchSeatSelection = false
                root.switchRequestPending = false
                root.deploymentNotice = "已退出房间，正在刷新房间目录。"
            }
            if (root.switchSeatSelection && root.controller.onlineStage === "roomSelect") {
                root.switchSeatSelection = false
                root.switchRequestPending = false
                root.switchTargetSeatId = ""
            }
        }
        function onUnitsForward() {
            if (root.attackTargetId && !root.controller.unitAt(root.attackTargetId)) {
                root.attackTargetId = ""
            }
            root.refreshUnitNameDraft()
            root.refreshUnitSpeedDraft()
            root.queueSelectedUnitCenter()
        }
        function onRoomStateChanged() {
            var phase = root.controller.matchPhase
            if (phase === "preparing" && root.observedMatchPhase !== ""
                    && root.observedMatchPhase !== "preparing") {
                root.resetRoundViewState()
                root.deploymentNotice = "新一局已准备，请重新选择战位并部署单位。"
            }
            root.observedMatchPhase = phase
        }
        function onErrorForward(message) {
            if (root.exitRequestPending) {
                root.exitRequestPending = false
                root.deploymentNotice = "退出请求被服务器拒绝，当前战位保持不变。"
            }
            if (root.switchRequestPending) {
                root.switchRequestPending = false
                root.switchSeatSelection = false
                root.switchTargetSeatId = ""
                root.deploymentNotice = "切换请求被服务器拒绝，当前战位保持不变。"
            }
        }
        function onEventForward(title, body) {
            if (!title && !body) return
            if (title === "联网房间" && body) {
                root.deploymentNotice = body
            } else if (title && body) {
                root.deploymentNotice = title + " · " + body
            } else {
                root.deploymentNotice = title || body
            }
        }
        function onTransferEventReceived(event) {
            if (!root.switchSeatSelection || !event
                    || event.sourceSeatId !== root.switchSourceSeatId
                    || event.targetSeatId !== root.switchTargetSeatId) return
            if (event.kind === "transferRequested") {
                root.switchRequestPending = true
                root.deploymentNotice = "切换请求已送达，等待本方指挥官确认。"
            } else if (event.kind === "transferCompleted") {
                root.switchSeatSelection = false
                root.switchRequestPending = false
                root.deploymentNotice = "服务器已确认战位切换。"
            } else if (event.kind === "transferRejected") {
                root.switchSeatSelection = false
                root.switchRequestPending = false
                root.switchTargetSeatId = ""
                root.deploymentNotice = "服务器未批准战位切换，当前战位保持不变。"
            }
        }
        function onIntelShareReceived(share) {
            if (!share) return
            root.rememberIntelId(share.intelId)
            if (!root.notifyIntelShare) return
            var source = root.seatById(share.senderSeatId)
            var target = root.controller.unitAt(share.targetId)
            var sourceLabel = source ? root.seatLabel(source) : (share.senderSeatId || "友方战位")
            var targetLabel = target && target.callsign ? target.callsign
                : (share.targetId || share.intelId || "情报")
            root.deploymentNotice = sourceLabel + "共享情报 · " + targetLabel
        }
        function onOnlineIntelChanged() { root.syncIntelNotifications() }
        function onSettingChanged(key) {
            if (String(key).indexOf("online/") === 0)
                root.reloadOnlineSettings()
        }
    }

    Component.onCompleted: {
        root.reloadOnlineSettings()
        root.resetIntelNotificationBaseline()
        root.syncIntelNotifications()
        root.observedMatchPhase = root.controller.matchPhase
        if (root.controller.onlineStage === "login") root.controller.requestOnlineRooms()
        root.queueSelectedUnitCenter()
        root.refreshUnitNameDraft()
        root.refreshUnitSpeedDraft()
    }

    Dialog {
        id: switchSeatConfirmation
        anchors.centerIn: parent
        modal: true
        title: "切换战位"
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Cancel | Dialog.Ok
        background: Rectangle { color: root.panelAlt; border.color: root.line; radius: AppContext.radius }
        contentItem: Item {
            Accessible.name: "确认切换战位"
            implicitWidth: 280
            implicitHeight: switchSeatMessage.implicitHeight

            Text {
                id: switchSeatMessage
                anchors.left: parent.left
                anchors.right: parent.right
                text: "选择目标战位后，当前战位会保持不变，直到本方指挥官确认。"
                color: root.ink
                wrapMode: Text.WordWrap
            }
        }
        onAccepted: root.startSwitchSeatSelection()
    }

    Dialog {
        id: exitRoomConfirmation
        anchors.centerIn: parent
        modal: true
        title: "退出房间"
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.NoButton
        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: AppContext.stateMotion }
            NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: AppContext.stateMotion; easing.type: Easing.OutCubic }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: AppContext.fastMotion }
        }
        background: Rectangle { color: root.panelAlt; border.color: root.orange; radius: AppContext.radius }
        contentItem: Item {
            Accessible.name: "确认退出当前房间"
            implicitWidth: 280
            implicitHeight: exitRoomMessage.implicitHeight

            Text {
                id: exitRoomMessage
                anchors.left: parent.left
                anchors.right: parent.right
                text: "退出后将释放当前战位并返回房间目录。当前登录身份会保留。"
                color: root.ink
                wrapMode: Text.WordWrap
            }
        }
        footer: DialogButtonBox {
            spacing: 8
            padding: 10
            background: Rectangle { color: root.panel; radius: AppContext.radius }
            GhostButton {
                text: "留在房间"
                Accessible.name: "取消退出并留在当前房间"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                onClicked: exitRoomConfirmation.reject()
            }
            TonalButton {
                text: "确认退出"
                base: root.orange
                Accessible.name: "确认退出当前房间"
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                onClicked: exitRoomConfirmation.accept()
            }
        }
        onAccepted: {
            root.exitRequestPending = true
            root.deploymentNotice = "退出请求已提交，等待服务器确认。"
            root.controller.leaveOnlineRoom()
        }
    }

    Drawer {
        id: tacticalDrawer
        edge: Qt.RightEdge
        width: Math.min(392, root.width * 0.94)
        height: root.height
        modal: true
        interactive: root.compactLayout && enabled
        enabled: root.compactLayout && stages.currentIndex === 3
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onEnabledChanged: if (!enabled) close()
        background: Rectangle {
            color: root.panel
            border.color: root.line
        }
    }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 18; spacing: 14
            RowLayout {
                Layout.fillWidth: true; Layout.preferredHeight: 54; spacing: 14
                ColumnLayout { Layout.fillWidth: true; spacing: 2
                Text { Layout.fillWidth: true; text: root.controller.onlineStage === "roomSelect" ? "选择推演房间" : root.controller.onlineStage === "roomAdmin" ? "房间管理" : root.controller.isObserver ? "只读观战" : root.switchSeatSelection || root.controller.onlineStage === "seatSelect" ? "选择战位" : "联网实战席"; color: root.ink; font.pixelSize: root.compactLayout ? 20 : 23; font.bold: true; elide: Text.ElideRight }
                Text { Layout.fillWidth: true; text: root.controller.currentRoomId ? root.controller.currentRoomId + " · " + root.roomConfigurationLabel() : "选择准备中的房间"; color: root.dim; font.pixelSize: 12; elide: Text.ElideRight }
            }
            Rectangle { visible: root.controller.onlineStage !== "roomSelect" && root.controller.onlineStage !== "roomAdmin"; Layout.preferredWidth: root.compactLayout ? 148 : 180; Layout.preferredHeight: visible ? 34 : 0; color: root.panelAlt; border.color: root.line; radius: 5
                RowLayout { anchors.fill: parent; anchors.margins: 8; spacing: 8
                    Rectangle { Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4; color: root.controller.networkState === "connected" ? root.cyan : root.orange }
                    Text { Layout.fillWidth: true; text: root.controller.networkState === "connected" ? ("数据面 · " + root.controller.dataPlaneName) : root.controller.networkStatus; color: root.dim; font.pixelSize: 10; elide: Text.ElideRight }
                }
            }
        }

        Rectangle {
            visible: root.controller.onlineStage === "deployment" || root.controller.onlineStage === "battle" || root.controller.onlineStage === "observer"
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 42 : 0
            color: root.panel
            border.color: root.line
            radius: 6
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 12
                Rectangle {
                    Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                    color: root.controller.matchPhase === "running" ? root.cyan : root.orange
                    Behavior on color { ColorAnimation { duration: 220 } }
                }
                Text {
                    text: root.controller.isObserver ? (root.controller.matchPhase === "running" ? "观战 · 推演执行中" : root.controller.matchPhase === "finished" ? "观战 · 推演已结束" : root.controller.matchPhase === "paused" ? "观战 · 推演暂停" : "观战 · 准备阶段") : root.controller.matchPhase === "running" ? "推演执行中" : root.controller.matchPhase === "finished" ? "推演已结束" : "部署准备阶段"
                    Layout.fillWidth: true
                    color: root.ink; font.pixelSize: 11; font.bold: true; elide: Text.ElideRight
                }
                Rectangle {
                    visible: root.controller.roomMode === "pve"
                             && root.controller.matchPhase === "running"
                    Layout.preferredWidth: aiEngineText.implicitWidth + 16
                    Layout.preferredHeight: 22
                    color: root.panelAlt
                    border.color: root.aiEngineColor()
                    radius: 4
                    Text {
                        id: aiEngineText
                        anchors.centerIn: parent
                        text: "AI · " + root.aiEngineLabel() + " · " + root.aiDifficultyLabel()
                        color: root.aiEngineColor()
                        font.pixelSize: 10
                        font.bold: true
                    }
                }
                Text {
                    visible: !root.compactLayout && !root.controller.isObserver
                             && root.isCommander && root.controller.matchPhase === "preparing"
                    text: "待部署 " + root.pendingDeploymentSeats().length
                    color: root.pendingDeploymentSeats().length > 0 ? root.orange : root.cyan
                    font.pixelSize: 10; font.bold: true
                }
                Text {
                    visible: root.controller.isObserver || !root.compactLayout
                    text: root.controller.isObserver ? "只读观察" : "战位 " + root.controller.currentSeatId
                    color: root.dim; font.pixelSize: 10; font.family: "Consolas"
                    elide: Text.ElideRight
                }
                Text {
                    visible: !root.controller.isObserver && !root.compactLayout
                    text: root.priorityIntelSummary()
                    color: root.controller.onlineIntelRecords.length > 0 ? root.orange : root.dim
                    font.pixelSize: 10
                    font.bold: root.controller.onlineIntelRecords.length > 0
                    elide: Text.ElideRight
                }
            }
            Behavior on opacity { NumberAnimation { duration: 220 } }
        }

        StackLayout { id: stages; Layout.fillWidth: true; Layout.fillHeight: true; currentIndex: root.controller.onlineStage === "roomSelect" ? 0 : root.controller.onlineStage === "roomAdmin" ? 2 : root.switchSeatSelection || root.controller.onlineStage === "seatSelect" ? 1 : 3
            Item {
                ColumnLayout { anchors.centerIn: parent; width: Math.min(parent.width - 40, 760); spacing: 14
                    Text { text: "可用推演室"; color: root.ink; font.pixelSize: 16; font.bold: true }
                    ListView { id: roomList; Layout.fillWidth: true; Layout.preferredHeight: Math.min(500, Math.max(130, contentHeight)); spacing: 8; model: root.orderedRooms(); clip: true
                    delegate: Rectangle { id: roomDelegate; required property var modelData; required property int index; property bool compactActions: width < 620; property bool narrowActions: width < 410; property int actionWidth: compactActions ? (narrowActions ? 70 : 82) : 96; property int availableActionCount: (root.roomCanJoin(modelData) ? 1 : 0) + (root.roomCanObserve(modelData) ? 1 : 0); width: roomList.width; height: compactActions ? 112 : 78; color: roomHover.hovered ? root.panelAlt : root.panel; border.color: roomHover.hovered ? root.cyan : root.line; radius: 6
                            HoverHandler { id: roomHover }
                            ColumnLayout { anchors.fill: parent; anchors.margins: 12; spacing: roomDelegate.compactActions ? 8 : 4
                                RowLayout { Layout.fillWidth: true; spacing: 12
                                    Rectangle { Layout.preferredWidth: 36; Layout.preferredHeight: 36; radius: 18; color: roomDelegate.modelData.status === "running" ? root.panelAlt : root.panel
                                        Text { anchors.centerIn: parent; text: roomDelegate.modelData.status === "running" ? "战" : "室"; color: root.cyan; font.bold: true }
                                    }
                                    ColumnLayout { Layout.fillWidth: true; spacing: 2
                                        Text { Layout.fillWidth: true; text: roomDelegate.modelData.name; color: root.ink; font.bold: true; font.pixelSize: 14; elide: Text.ElideRight }
                                        Text { Layout.fillWidth: true; text: roomDelegate.modelData.roomId + " · " + root.roomModeLabel(roomDelegate.modelData); color: root.dim; font.family: "Consolas"; font.pixelSize: 10; elide: Text.ElideRight }
                                    }
                                    ColumnLayout { visible: !roomDelegate.compactActions; Layout.preferredWidth: 112; spacing: 2
                                        Text { text: root.roomStatusLabel(roomDelegate.modelData.status); color: root.roomStatusColor(roomDelegate.modelData.status); font.pixelSize: 11; font.bold: true }
                                        Text { text: roomDelegate.modelData.hostedByGameServer === true ? (root.roomCanObserve(roomDelegate.modelData) ? "可进入或旁观" : "不可用") : "未托管"; color: root.dim; font.pixelSize: 9 }
                                    }
                                    Flow { visible: !roomDelegate.compactActions && roomDelegate.availableActionCount > 0; Layout.preferredWidth: roomDelegate.availableActionCount * roomDelegate.actionWidth + (roomDelegate.availableActionCount > 1 ? 6 : 0); spacing: 6
                                        Button { id: joinRoomButton; visible: root.roomCanJoin(roomDelegate.modelData); enabled: visible; width: roomDelegate.actionWidth; height: 30; text: "进入房间"; Accessible.name: "进入房间 " + roomDelegate.modelData.name; onClicked: root.controller.joinOnlineRoom(roomDelegate.modelData.roomId)
                                            contentItem: Text { text: joinRoomButton.text; color: joinRoomButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10; font.bold: true; elide: Text.ElideRight }
                                            background: Rectangle { color: joinRoomButton.enabled ? root.cyan : root.line; radius: 4 }
                                        }
                                        Button { id: observeRoomButton; visible: root.roomCanObserve(roomDelegate.modelData); enabled: visible; width: roomDelegate.actionWidth; height: 30; text: "旁观"; Accessible.name: "旁观 " + roomDelegate.modelData.name; onClicked: root.controller.observeOnlineRoom(roomDelegate.modelData.roomId)
                                            contentItem: Text { text: observeRoomButton.text; color: observeRoomButton.enabled ? root.cyan : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10; elide: Text.ElideRight }
                                            background: Rectangle { color: observeRoomButton.hovered ? root.panelAlt : "transparent"; border.color: observeRoomButton.enabled ? root.cyan : root.line; radius: 4 }
                                        }
                                    }
                                }
                                RowLayout { visible: roomDelegate.compactActions; Layout.fillWidth: true; spacing: 8
                                    ColumnLayout { Layout.fillWidth: true; spacing: 1
                                        Text { text: root.roomStatusLabel(roomDelegate.modelData.status); color: root.roomStatusColor(roomDelegate.modelData.status); font.pixelSize: 11; font.bold: true }
                                        Text { Layout.fillWidth: true; text: roomDelegate.modelData.hostedByGameServer === true ? (root.roomCanObserve(roomDelegate.modelData) ? "可进入或旁观" : "不可用") : "未托管"; color: root.dim; font.pixelSize: 9; elide: Text.ElideRight }
                                    }
                                    Flow { visible: roomDelegate.availableActionCount > 0; Layout.preferredWidth: roomDelegate.availableActionCount * roomDelegate.actionWidth + (roomDelegate.availableActionCount > 1 ? 6 : 0); spacing: 6
                                        Button { id: compactJoinRoomButton; visible: root.roomCanJoin(roomDelegate.modelData); enabled: visible; width: roomDelegate.actionWidth; height: 30; text: roomDelegate.narrowActions ? "进入" : "进入房间"; Accessible.name: "进入房间 " + roomDelegate.modelData.name; onClicked: root.controller.joinOnlineRoom(roomDelegate.modelData.roomId)
                                            contentItem: Text { text: compactJoinRoomButton.text; color: compactJoinRoomButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10; font.bold: true; elide: Text.ElideRight }
                                            background: Rectangle { color: compactJoinRoomButton.enabled ? root.cyan : root.line; radius: 4 }
                                        }
                                        Button { id: compactObserveRoomButton; visible: root.roomCanObserve(roomDelegate.modelData); enabled: visible; width: roomDelegate.actionWidth; height: 30; text: "旁观"; Accessible.name: "旁观 " + roomDelegate.modelData.name; onClicked: root.controller.observeOnlineRoom(roomDelegate.modelData.roomId)
                                            contentItem: Text { text: compactObserveRoomButton.text; color: compactObserveRoomButton.enabled ? root.cyan : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10; elide: Text.ElideRight }
                                            background: Rectangle { color: compactObserveRoomButton.hovered ? root.panelAlt : "transparent"; border.color: compactObserveRoomButton.enabled ? root.cyan : root.line; radius: 4 }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Text { visible: roomList.count === 0; text: "暂无房间"; color: root.dim; font.pixelSize: 11; Layout.alignment: Qt.AlignHCenter }
                    Button { id: refreshRoomsButton; Layout.alignment: Qt.AlignHCenter; text: "刷新"
                        onClicked: root.controller.requestOnlineRooms()
                        contentItem: Text { text: refreshRoomsButton.text; color: root.cyan; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { color: "transparent"; border.color: root.line; radius: 4 }
                    }
                }
            }
            Item {
                ColumnLayout { anchors.fill: parent; anchors.margins: 12; spacing: 12
                    RowLayout { Layout.fillWidth: true
                        Text { text: root.switchSeatSelection ? "申请切换战位" : "房间战位"; color: root.ink; font.pixelSize: 16; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Button { id: backToRoomsButton; text: root.switchSeatSelection ? "取消切换" : root.controller.leaveRoomPending ? "正在退出…" : "退出房间"; enabled: !root.switchRequestPending && !root.controller.leaveRoomPending; Accessible.name: text; onClicked: root.switchSeatSelection ? root.cancelSwitchSeatSelection() : exitRoomConfirmation.open()
                            contentItem: Text { text: backToRoomsButton.text; color: root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: "transparent"; border.color: root.line; radius: 4 }
                        }
                    }
                    Text { text: root.switchSeatSelection ? (root.switchRequestPending ? "等待指挥官确认" : "选择本方非指挥官战位") : root.strictVmf ? "VMF：红方参演" : root.isRoomEmpty() ? "先选择红方指挥官" : "红方指挥官优先"; color: root.strictVmf || root.isRoomEmpty() || root.switchSeatSelection ? root.orange : root.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    GridLayout { Layout.fillWidth: true; Layout.fillHeight: true; columns: root.strictVmf ? 1 : width > 900 ? 2 : 1; columnSpacing: 14; rowSpacing: 14
                        Repeater { model: root.strictVmf ? ["red"] : ["red", "blue"]
                            delegate: Rectangle { id: sidePanel; required property string modelData; Layout.fillWidth: true; Layout.fillHeight: true; color: root.panel; border.color: sidePanel.modelData === "red" ? root.danger : root.info; radius: 6
                            ColumnLayout { anchors.fill: parent; anchors.margins: 14; spacing: 8
                                Text { text: sidePanel.modelData === "red" ? "红方战位" : "蓝方战位"; color: sidePanel.modelData === "red" ? root.danger : root.info; font.bold: true; font.pixelSize: 14 }
                                ListView { id: seatList; Layout.fillWidth: true; Layout.fillHeight: true; spacing: 6; clip: true; model: root.seatsForSide(sidePanel.modelData)
                                    delegate: Rectangle { id: seatDelegate
                                        required property var modelData
                                        width: seatList.width; height: 58
                                        color: seatDelegate.modelData.occupied ? root.panelAlt : root.page
                                        border.color: seatDelegate.modelData.occupied ? root.line : root.canClaimSeat(seatDelegate.modelData) ? root.cyan : root.line
                                        radius: 5
                                        RowLayout { anchors.fill: parent; anchors.margins: 10; spacing: 10
                                            Rectangle { Layout.preferredWidth: 3; Layout.preferredHeight: 28; radius: 2; color: root.seatStatusColor(seatDelegate.modelData) }
                                            ColumnLayout { Layout.fillWidth: true; spacing: 2
                                                Text { Layout.fillWidth: true; text: root.seatLabel(seatDelegate.modelData); color: root.ink; font.pixelSize: 11; font.bold: true; elide: Text.ElideRight }
                                                Text { Layout.fillWidth: true; text: seatDelegate.modelData.occupied && seatDelegate.modelData.selectedTemplate ? (root.seatHint(seatDelegate.modelData) + " · " + seatDelegate.modelData.selectedTemplate) : root.seatHint(seatDelegate.modelData); color: root.dim; font.pixelSize: 9; elide: Text.ElideRight }
                                            }
                                            Text { text: root.seatStatusLabel(seatDelegate.modelData); color: root.seatStatusColor(seatDelegate.modelData); font.pixelSize: 9; font.bold: true }
                                            Button { id: claimSeatButton; visible: seatDelegate.modelData.claimable === true; enabled: root.canClaimSeat(seatDelegate.modelData); text: seatDelegate.modelData.controlMode === "vmf-auto" ? "接管" : root.switchSeatSelection ? "申请" : "进入"; onClicked: root.requestSeat(seatDelegate.modelData)
                                                Component.onCompleted: if (claimSeatButton.enabled) Qt.callLater(claimSeatButton.forceActiveFocus)
                                                onEnabledChanged: if (enabled) Qt.callLater(claimSeatButton.forceActiveFocus)
                                                contentItem: Text { text: claimSeatButton.text; color: claimSeatButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10; font.bold: true }
                                                background: Rectangle { color: claimSeatButton.enabled ? root.cyan : root.panelAlt; radius: 4; Behavior on color { ColorAnimation { duration: 150 } } }
                                            }
                                        }
                                        Behavior on border.color { ColorAnimation { duration: 180 } }
                                    }
                                }
                            }
                            }
                        }
                    }
                }
            }
            Item {
                RoomAdminView {
                    anchors.fill: parent
                    controller: root.controller
                    editor: root.editor
                }
            }
            Item {
                    GridLayout { id: battleLayout; anchors.fill: parent; columns: root.compactLayout ? 1 : 2; columnSpacing: 12; rowSpacing: 12
                    Rectangle { Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumWidth: root.compactLayout ? 0 : 420; Layout.preferredWidth: root.compactLayout ? -1 : Math.max(420, battleLayout.width - 368); Layout.minimumHeight: root.compactLayout ? 250 : 0; Layout.preferredHeight: root.compactLayout ? battleLayout.height : -1; color: root.panel; border.color: root.line; radius: 6
                            MapCanvas { id: onlineCanvas; anchors.fill: parent; zoom: 0.05; controller: root.controller; editor: root.editor; sideFilter: root.controller.isObserver ? "" : root.controller.currentSeatSide || "red"; showAllSides: root.controller.isObserver; showRecentPaths: root.controller.isObserver; visibleUnitIds: root.onlineMapVisibleUnitIds(); detectedEnemyIds: root.controller.isObserver ? [] : root.controller.units.filter(function(unit){ return unit.side !== root.controller.currentSeatSide }).map(function(unit){ return unit.id }); allowRightClickActions: !root.controller.isObserver; showCommRange: !root.controller.isObserver && root.showCommunicationRange; showDetectRange: !root.controller.isObserver && root.showDetectionRange; showAttackRange: !root.controller.isObserver && root.showAttackRange; rangeUnitIds: root.controller.isObserver ? [] : root.rangeDisplayUnitIds(); showCoordinateReadout: true; simTime: root.controller.simTime; focusUnitId: root.selectedUnitId || root.deployUnitId; actionTargetId: commanderCommandPanel.draftTargetId || root.attackTargetId; mapMarkers: root.controller.isObserver ? [] : root.participantMapMarkers(); intelRecords: root.controller.isObserver ? [] : root.controller.onlineIntelRecords; showIntelLive: root.showIntelLive; showIntelStale: root.showIntelStale; showIntelManual: root.showIntelManual; showIntelUncertainty: root.showIntelUncertainty; selectedMapMarkerId: root.controller.isObserver ? "" : root.selectedCommandMarkerId
                            onClickedMap: function(point) {
                                if (root.controller.isObserver) return
                                if (root.canDeploy) {
                                    root.controller.deployOnlineUnit(root.deployUnitId, point)
                                    root.deploymentState = "submitting"
                                    root.deploymentNotice = "部署位置已提交，等待服务器确认"
                                    onlineCanvas.pulseActionAt(point, root.cyan)
                                } else if (root.isCommander && root.controller.matchPhase === "running" && root.selectedUnitId) {
                                    root.deploymentNotice = "请在右侧指挥面板选择命令并确认下达。"
                                } else if (!root.isCommander && root.controller.matchPhase === "running" && root.selectedUnitId) {
                                    root.controller.command("moveTo", {"unitId": root.selectedUnitId, "pos": point})
                                    onlineCanvas.pulseActionAt(point, root.cyan)
                                }
                            }
                            onRightClickedMap: function(point) {
                                if (root.controller.isObserver) return
                                if (root.intelReportPicking) {
                                    root.intelReportPosition = point
                                    root.intelReportPicking = false
                                    onlineCanvas.pulseActionAt(point, root.cyan)
                                    root.deploymentNotice = "已选情报位置 "
                                        + Number(point.x).toFixed(0) + ", "
                                        + Number(point.y).toFixed(0)
                                    return
                                }
                                if (root.controller.matchPhase === "running") {
                                    var recipients = root.mapMarkRecipientSeats()
                                    root.controller.markOnlineMap(point,
                                        root.isCommander ? "指挥标点" : "我的标点",
                                        recipients)
                                    onlineCanvas.pulseActionAt(point,
                                        root.isCommander ? AppContext.markCommander : AppContext.markSelf)
                                    root.deploymentNotice = root.isCommander
                                        ? "指挥标点已提交 · 接收战位 " + recipients.length
                                        : "我的标点已更新 · 旧标点已替换"
                                }
                            }
                            onUnitClicked: function(unitId, button) {
                                var unit = root.controller.unitAt(unitId)
                                if (!unit) return
                                if (root.intelReportPicking && button === "right"
                                        && unit.position && unit.position.length >= 2) {
                                    root.intelReportPosition = {
                                        x: Number(unit.position[0]), y: Number(unit.position[1])
                                    }
                                    root.intelReportPicking = false
                                    root.deploymentNotice = "已选情报位置 "
                                        + Number(unit.position[0]).toFixed(0) + ", "
                                        + Number(unit.position[1]).toFixed(0)
                                    return
                                }
                                if (root.controller.isObserver) {
                                    root.selectUnit(unit)
                                } else if (unit.side === root.controller.currentSeatSide) {
                                    root.selectUnit(unit)
                                } else if (root.isCommander && commanderCommandPanel.awaitingAttackTarget) {
                                    commanderCommandPanel.mapTargetSelected(unitId)
                                    root.deploymentNotice = "已选中攻击目标 · " + (unit.callsign || unit.id)
                                } else if (root.controller.currentSeatType === "attack") {
                                    root.selectAttackTarget(unitId)
                                }
                            }
                            onGuideSourceChanged: function(unitId) {
                                if (root.controller.isObserver) return
                                var unit = root.controller.unitAt(unitId)
                                if (unit) root.selectUnit(unit)
                            }
                            onGuidePointPicked: function(point) {
                                if (root.controller.isObserver) return
                                if (!root.commanderPointSelectionActive) return
                                root.commanderPointSelectionActive = false
                                onlineCanvas.stopGuideMode()
                                commanderCommandPanel.mapPointSelected(point)
                                onlineCanvas.pulseActionAt(point, root.cyan)
                            }
                            onGuideCancelled: {
                                if (root.controller.isObserver) return
                                if (!root.commanderPointSelectionActive) return
                                root.commanderPointSelectionActive = false
                                commanderCommandPanel.mapSelectionCanceled()
                                root.deploymentNotice = "已取消地图选点，未发送命令。"
                            }
                            onMapMarkerClicked: function(marker) {
                                if (root.controller.isObserver) return
                                if (!root.isCommander && marker && marker.category === "command")
                                    root.selectedCommandMarkerId = marker.id || ""
                            }
                            Rectangle {
                                visible: root.selectedCommandMarkerId.length > 0
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 14
                                z: 70
                                width: Math.min(parent.width - 24, markerActionRow.implicitWidth + 20)
                                height: markerActionRow.implicitHeight + 14
                                color: root.panelAlt
                                border.color: root.cyan
                                radius: 5
                                RowLayout {
                                    id: markerActionRow
                                    anchors.centerIn: parent
                                    spacing: 8
                                    Text { text: "已选中命令标点"; color: root.ink; font.pixelSize: 10 }
                                    Button {
                                        id: dismissMarkerButton
                                        text: "隐藏标点"
                                        onClicked: root.dismissSelectedCommandMarker()
                                        contentItem: Text { text: dismissMarkerButton.text; color: root.page; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10; font.bold: true }
                                        background: Rectangle { color: root.cyan; radius: 4 }
                                    }
                                    Button {
                                        id: closeMarkerSelectionButton
                                        text: "关闭"
                                        onClicked: root.selectedCommandMarkerId = ""
                                        contentItem: Text { text: closeMarkerSelectionButton.text; color: root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10 }
                                        background: Rectangle { color: root.panel; border.color: root.line; radius: 4 }
                                    }
                                }
                            }
                        }
                        Button {
                            id: openTacticalDrawerButton
                            visible: root.compactLayout
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: 12
                            z: 90
                            width: 86
                            height: 34
                            Accessible.name: "打开战术侧栏"
                            onClicked: tacticalDrawer.open()
                            contentItem: Row {
                                anchors.centerIn: parent
                                spacing: 6
                                Icon { name: "menu"; iconSize: 15; iconColor: root.page }
                                Text { text: "侧栏"; color: root.page; font.pixelSize: 11; font.bold: true }
                            }
                            background: Rectangle { color: root.cyan; radius: 5 }
                        }
                    }
                    Item {
                        id: tacticalPanelHost
                        visible: !root.compactLayout
                        Layout.fillWidth: false
                        Layout.fillHeight: !root.compactLayout
                        Layout.minimumWidth: root.compactLayout ? 0 : 332
                        Layout.minimumHeight: root.compactLayout ? 0 : 300
                        Layout.preferredWidth: root.compactLayout ? 0 : 356
                        Layout.preferredHeight: root.compactLayout ? 0 : -1
                        Layout.maximumWidth: root.compactLayout ? 0 : 356
                        Layout.maximumHeight: root.compactLayout ? 0 : -1
                        Rectangle {
                            id: tacticalPanel
                            parent: root.compactLayout ? tacticalDrawer.contentItem : tacticalPanelHost
                            visible: !root.compactLayout || tacticalDrawer.opened
                            anchors.fill: parent
                            anchors.margins: root.compactLayout ? 10 : 0
                            color: root.panel
                            border.color: root.line
                            radius: 6
                        ScrollView { id: commandScroll; anchors.fill: parent; anchors.margins: 14; clip: true; contentWidth: availableWidth; contentHeight: commandColumn.implicitHeight
                        ColumnLayout { id: commandColumn; width: commandScroll.availableWidth; spacing: 10
                            RowLayout { Layout.fillWidth: true; spacing: 8
                                Text { Layout.fillWidth: true; text: (root.controller.currentSeatSide === "red" ? "红方" : "蓝方") + " · " + (root.controller.currentSeatType === "commander" ? "指挥官" : root.controller.currentSeatType === "attack" ? "攻击机" : root.controller.currentSeatType === "recon" ? "侦察机" : root.controller.currentSeatType === "ground" ? "地面单位" : "干扰机"); color: root.ink; font.bold: true; font.pixelSize: 15; elide: Text.ElideRight }
                                Text { text: root.controller.matchPhase === "running" ? "推演中" : root.controller.matchPhase === "finished" ? "已结束" : "准备阶段"; color: root.controller.matchPhase === "running" ? root.cyan : root.orange; font.pixelSize: 10; font.bold: true }
                            }
                            RowLayout { Layout.fillWidth: true; spacing: 8
                                Item { Layout.fillWidth: true }
                                Button { id: switchSeatButton; text: "切换战位"; visible: !root.controller.isObserver && root.controller.currentSeatType !== "commander" && root.controller.matchPhase === "preparing"; enabled: !root.switchRequestPending; onClicked: switchSeatConfirmation.open()
                                    contentItem: Text { text: switchSeatButton.text; color: root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10 }
                                    background: Rectangle { color: switchSeatButton.hovered ? root.panelAlt : "transparent"; border.color: root.line; radius: 4; Behavior on color { ColorAnimation { duration: 150 } } }
                                }
                                Button { id: exitRoomButton; text: root.controller.leaveRoomPending ? "正在退出…" : "退出房间"; enabled: !root.controller.leaveRoomPending; Accessible.name: text; onClicked: exitRoomConfirmation.open()
                                    contentItem: Text { text: exitRoomButton.text; color: root.orange; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10 }
                                    background: Rectangle { color: exitRoomButton.hovered ? root.panelAlt : "transparent"; border.color: root.orange; radius: 4; Behavior on color { ColorAnimation { duration: 150 } } }
                                }
                            }
                            Item {
                                id: tacticalTabs
                                Layout.fillWidth: true
                                Layout.preferredHeight: 40
                                property int currentIndex: root.tacticalViewIndex

                                Rectangle {
                                    anchors.fill: parent
                                    color: root.page
                                    border.color: root.line
                                    radius: 4
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 2
                                    spacing: 2

                                    Repeater {
                                        model: [
                                            { label: "单位", icon: "unit", accessible: "单位视图" },
                                            { label: "指挥", icon: "command", accessible: "指挥视图" },
                                            { label: "情报", icon: "scan", accessible: "情报视图" }
                                        ]
                                        delegate: Button {
                                            id: tacticalTabButton
                                            required property var modelData
                                            required property int index
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            Layout.minimumWidth: 0
                                            padding: 0
                                            leftPadding: 0
                                            rightPadding: 0
                                            topPadding: 0
                                            bottomPadding: 0
                                            property bool active: root.tacticalViewIndex === tacticalTabButton.index
                                            Accessible.name: tacticalTabButton.modelData.accessible
                                            onClicked: {
                                                root.tacticalViewIndex = tacticalTabButton.index
                                                root.controller.saveSetting("online/sidebar/defaultView",
                                                                            tacticalTabButton.index)
                                            }

                                            contentItem: Item {
                                                anchors.fill: parent
                                                Row {
                                                    id: tacticalTabContent
                                                    anchors.centerIn: parent
                                                    spacing: 6
                                                    Icon {
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        name: tacticalTabButton.modelData.icon
                                                        iconSize: 16
                                                        iconColor: tacticalTabButton.active ? root.cyan : root.dim
                                                    }
                                                    Text {
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        text: tacticalTabButton.modelData.label
                                                        color: tacticalTabButton.active ? root.cyan : root.dim
                                                        font.pixelSize: 11
                                                        font.bold: tacticalTabButton.active
                                                    }
                                                }
                                            }
                                            background: Rectangle {
                                                color: tacticalTabButton.active ? root.panelAlt
                                                                                  : (tacticalTabButton.hovered ? root.panel : "transparent")
                                                radius: 3
                                                Rectangle {
                                                    anchors.left: parent.left
                                                    anchors.right: parent.right
                                                    anchors.bottom: parent.bottom
                                                    height: 2
                                                    color: tacticalTabButton.active ? root.cyan : "transparent"
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.line }
                            RowLayout {
                                visible: root.tacticalViewIndex === 1
                                Layout.fillWidth: true
                                spacing: 8
                                Rectangle {
                                    visible: !root.isCommander
                                    Layout.preferredWidth: 10; Layout.preferredHeight: 10; radius: 5
                                    color: root.communicationColor()
                                }
                                Text { text: root.isCommander ? "指挥官通信" : "指挥链路"; color: root.dim; font.pixelSize: 10 }
                                Text {
                                    visible: !root.isCommander
                                    Layout.fillWidth: true
                                    text: root.communicationLabel()
                                    color: root.communicationColor(); font.pixelSize: 10; font.bold: true
                                    Accessible.name: "指挥链路状态：" + text
                                }
                                Item { visible: root.isCommander; Layout.fillWidth: true }
                                Button {
                                    id: openInboxButton
                                    visible: !root.controller.isObserver && root.isCommander
                                    text: "实时收件箱 · " + root.controller.chatMessages.length
                                    Accessible.name: "打开指挥官实时通信收件箱"
                                    onClicked: root.openChatRequested()
                                    contentItem: Text { text: openInboxButton.text; color: root.cyan; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 9 }
                                    background: Rectangle { color: openInboxButton.hovered ? root.panelAlt : "transparent"; border.color: root.line; radius: 4 }
                                }
                            }
                            ColumnLayout {
                                visible: !root.controller.isObserver && root.controller.matchPhase === "running"
                                         && root.visibleMapMarkFeed().length > 0
                                         && root.tacticalViewIndex === 2
                                Layout.fillWidth: true
                                spacing: 6
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { Layout.fillWidth: true; text: "战术标点"; color: root.ink; font.pixelSize: 11; font.bold: true }
                                    Text { text: root.visibleMapMarkFeed().length + " 个"; color: root.dim; font.pixelSize: 9 }
                                }
                                RowLayout {
                                    Layout.fillWidth: true; spacing: 14
                                    RowLayout { spacing: 5
                                        Rectangle { Layout.preferredWidth: 42; Layout.preferredHeight: 18; radius: 3; color: "transparent"; border.color: AppContext.markSelf
                                            Text { anchors.centerIn: parent; text: root.isCommander ? "下属点" : "我的点"; color: AppContext.markSelf; font.pixelSize: 9; font.bold: true }
                                        }
                                    }
                                    RowLayout { spacing: 5
                                        Rectangle { Layout.preferredWidth: 52; Layout.preferredHeight: 18; radius: 3; color: AppContext.markCommander; border.color: AppContext.markCommander
                                            Text { anchors.centerIn: parent; text: "指挥令"; color: root.page; font.pixelSize: 9; font.bold: true }
                                        }
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                                ListView {
                                    id: mapMarkList
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: Math.min(152, Math.max(38, contentHeight))
                                    model: root.visibleMapMarkFeed()
                                    clip: true; spacing: 0
                                    delegate: Item {
                                        id: mapMarkDelegate
                                        required property var modelData
                                        width: mapMarkList.width; height: 38
                                        property bool commanderMark: modelData.markType === "commander"
                                        RowLayout {
                                            anchors.fill: parent; anchors.leftMargin: 2; anchors.rightMargin: 2
                                            spacing: 8
                                            Rectangle {
                                                Layout.preferredWidth: mapMarkDelegate.commanderMark ? 52 : 42
                                                Layout.preferredHeight: 20
                                                radius: 3
                                                color: mapMarkDelegate.commanderMark ? AppContext.markCommander : "transparent"
                                                border.color: mapMarkDelegate.commanderMark ? AppContext.markCommander : AppContext.markSelf
                                                Text { anchors.centerIn: parent; text: mapMarkDelegate.commanderMark ? "指挥令" : (root.isCommander ? "下属点" : "我的点"); color: mapMarkDelegate.commanderMark ? root.page : AppContext.markSelf; font.pixelSize: 9; font.bold: true }
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true; spacing: 1
                                                Text { Layout.fillWidth: true; text: root.mapMarkRoleLabel(mapMarkDelegate.modelData); color: root.ink; font.pixelSize: 9; font.bold: true; elide: Text.ElideRight }
                                                Text { Layout.fillWidth: true; text: root.mapMarkCoordinate(mapMarkDelegate.modelData); color: root.dim; font.pixelSize: 9; font.family: "Consolas"; elide: Text.ElideRight }
                                            }
                                            Text { text: mapMarkDelegate.modelData.time ? new Date(mapMarkDelegate.modelData.time).toLocaleTimeString(Qt.locale(), "HH:mm") : ""; color: root.dim; font.pixelSize: 8 }
                                        }
                                        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: root.line; opacity: 0.7 }
                                    }
                                }
                            }
                            Rectangle {
                                visible: !root.controller.isObserver && !root.isCommander
                                         && root.tacticalViewIndex === 1
                                Layout.fillWidth: true
                                Layout.preferredHeight: visible ? subordinateComposer.implicitHeight + 16 : 0
                                color: root.panelAlt; border.color: root.subordinateCanSend() ? root.line : root.orange; radius: 5
                                ColumnLayout {
                                    id: subordinateComposer
                                    anchors.fill: parent; anchors.margins: 8; spacing: 6
                                    RowLayout {
                                        Layout.fillWidth: true; spacing: 6
                                        TextField {
                                            id: subordinateMessageInput
                                            Layout.fillWidth: true
                                            text: root.subordinateMessageDraft
                                            onTextEdited: root.subordinateMessageDraft = text
                                            placeholderText: root.subordinateCanSend() ? "发送给本方指挥官" : "当前无法发送"
                                            maximumLength: 500; selectByMouse: true
                                            enabled: root.subordinateCanSend()
                                            color: root.ink
                                            Accessible.name: "发送给本方指挥官的消息"
                                            background: Rectangle { color: root.page; border.color: subordinateMessageInput.activeFocus ? root.cyan : root.line; radius: 4 }
                                            onAccepted: sendCommanderMessageButton.clicked()
                                        }
                                        Button {
                                            id: sendCommanderMessageButton
                                            text: "发送"
                                            enabled: root.subordinateCanSend() && root.subordinateMessageDraft.trim().length > 0
                                            Accessible.name: "发送给本方指挥官"
                                            onClicked: {
                                                root.controller.sendChat(root.subordinateMessageDraft.trim(), [root.commanderSeatId()])
                                                root.subordinateMessageDraft = ""
                                            }
                                            contentItem: Text { text: sendCommanderMessageButton.text; color: sendCommanderMessageButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10; font.bold: true }
                                            background: Rectangle { color: sendCommanderMessageButton.enabled ? root.cyan : root.line; radius: 4 }
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.controller.matchPhase === "preparing" ? "准备阶段可直接联系指挥官"
                                            : root.subordinateCanSend() ? "双向通信可用"
                                            : "运行阶段仅在双向通信时可发送"
                                        color: root.subordinateCanSend() ? root.dim : root.orange
                                        font.pixelSize: 9; elide: Text.ElideRight
                                    }
                                }
                            }
                            Rectangle {
                                visible: !root.controller.isObserver && root.isCommander && root.controller.matchPhase === "preparing"
                                         && (root.controller.pendingSeatTransfers.length > 0
                                             || root.pendingRedeploySeats().length > 0)
                                         && root.tacticalViewIndex === 1
                                Layout.fillWidth: true
                                Layout.preferredHeight: visible ? transferColumn.implicitHeight + 18 : 0
                                color: root.panelAlt; border.color: root.orange; radius: 5
                                ColumnLayout {
                                    id: transferColumn
                                    anchors.fill: parent; anchors.margins: 9; spacing: 8
                                    Text { text: "待处理申请"; color: root.ink; font.pixelSize: 12; font.bold: true }
                                    Repeater {
                                        model: root.controller.pendingSeatTransfers
                                        delegate: ColumnLayout {
                                            id: transferDelegate
                                            required property var modelData
                                            Layout.fillWidth: true; spacing: 6
                                            property var sourceSeat: root.seatById(modelData.sourceSeatId) || ({})
                                            Text { Layout.fillWidth: true; text: (parent.sourceSeat.displayName || "用户 " + parent.modelData.userId) + " 申请切换"; color: root.ink; font.pixelSize: 10; elide: Text.ElideRight }
                                            Text { Layout.fillWidth: true; text: parent.modelData.sourceSeatId + "  ->  " + parent.modelData.targetSeatId; color: root.dim; font.pixelSize: 9; elide: Text.ElideMiddle }
                                            RowLayout {
                                                Layout.fillWidth: true; spacing: 8
                                                Button { id: rejectTransferButton; Layout.fillWidth: true; text: "拒绝"; onClicked: root.controller.rejectSeatTransfer(transferDelegate.modelData.userId, transferDelegate.modelData.revision)
                                                    contentItem: Text { text: rejectTransferButton.text; color: root.danger; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10 }
                                                    background: Rectangle { color: rejectTransferButton.hovered ? root.panel : "transparent"; border.color: root.danger; radius: 4 }
                                                }
                                                Button { id: approveTransferButton; Layout.fillWidth: true; text: "批准"; onClicked: root.controller.approveSeatTransfer(transferDelegate.modelData.userId, transferDelegate.modelData.revision)
                                                    contentItem: Text { text: approveTransferButton.text; color: root.page; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10; font.bold: true }
                                                    background: Rectangle { color: approveTransferButton.hovered ? Qt.lighter(root.cyan, 1.08) : root.cyan; radius: 4 }
                                                }
                                            }
                                        }
                                    }
                                    Repeater {
                                        model: root.pendingRedeploySeats()
                                        delegate: RowLayout {
                                            id: redeployRequestDelegate
                                            required property var modelData
                                            Layout.fillWidth: true; spacing: 8
                                            Text { Layout.fillWidth: true; text: root.seatLabel(redeployRequestDelegate.modelData) + " 申请重新部署"; color: root.ink; font.pixelSize: 10; elide: Text.ElideRight }
                                            Button { id: approveRedeployButton; text: "重新部署"; Accessible.name: "批准" + root.seatLabel(redeployRequestDelegate.modelData) + "重新部署"; onClicked: root.controller.redeployOnlineUnit(redeployRequestDelegate.modelData.seatId)
                                                contentItem: Text { text: approveRedeployButton.text; color: root.page; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10; font.bold: true }
                                                background: Rectangle { color: approveRedeployButton.hovered ? Qt.lighter(root.cyan, 1.08) : root.cyan; radius: 4 }
                                            }
                                        }
                                    }
                                }
                            }
                            Rectangle {
                                visible: root.controller.matchPhase === "finished"
                                Layout.fillWidth: true
                                Layout.preferredHeight: visible ? 58 : 0
                                color: root.panelAlt; border.color: root.danger; radius: 5
                                Column {
                                    anchors.centerIn: parent; spacing: 4
                                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: "推演已结束"; color: root.danger; font.pixelSize: 13; font.bold: true }
                                }
                            }
                            CommandPanel {
                                id: commanderCommandPanel
                                visible: !root.controller.isObserver && root.isCommander && root.controller.matchPhase === "running"
                                         && root.tacticalViewIndex === 1
                                Layout.fillWidth: true
                                controller: root.controller
                                selectedUnitId: root.selectedUnitId
                                recipientSeatId: (root.seatForUnit(root.selectedUnitId) || {}).seatId || ""
                                currentSide: root.controller.currentSeatSide
                                page: root.page; ink: root.ink; dim: root.dim; panel: root.panel; panelAlt: root.panelAlt
                                line: root.line; cyan: root.cyan; orange: root.orange; danger: root.danger
                                onPointSelectionRequested: {
                                    root.commanderPointSelectionActive = true
                                    onlineCanvas.startGuideMode(root.selectedUnitId)
                                    root.deploymentNotice = "地图选点中。按 Escape 或在右侧取消，不会发送命令。"
                                }
                                onPointSelectionCancelled: root.cancelCommanderPointSelection()
                            }
                            GuidedStrikeWorkflowPanel {
                                id: onlineGuidedStrikePanel
                                visible: !root.controller.isObserver && root.tacticalViewIndex === 1
                                         && onlineGuidedStrikePanel.vmfAvailable
                                controller: root.controller
                                side: root.controller.currentSeatSide
                                Layout.fillWidth: true
                                Layout.preferredHeight: visible ? Math.min(370, implicitHeight) : 0
                                Layout.maximumHeight: 370
                            }
                            Rectangle {
                                visible: !root.controller.isObserver && !root.isCommander && root.controller.matchPhase === "running"
                                         && root.tacticalViewIndex === 1
                                Layout.fillWidth: true
                                Layout.preferredHeight: visible ? participantFeedColumn.implicitHeight + 18 : 0
                                color: root.panelAlt; border.color: root.line; radius: 5
                                ColumnLayout {
                                    id: participantFeedColumn
                                    anchors.fill: parent; anchors.margins: 9; spacing: 7
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text { Layout.fillWidth: true; text: "指挥命令"; color: root.ink; font.pixelSize: 12; font.bold: true }
                                        Text { text: root.participantCommandFeed().length + " 条"; color: root.dim; font.pixelSize: 9 }
                                    }
                                    Text { visible: participantCommandList.count === 0; text: "暂无命令"; color: root.dim; font.pixelSize: 10 }
                                    ListView {
                                        id: participantCommandList
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.min(260, Math.max(0, contentHeight))
                                        model: root.participantCommandFeed(); clip: true; spacing: 5
                                        delegate: Rectangle {
                                            id: participantCommandDelegate
                                            required property var modelData
                                            width: participantCommandList.width
                                            height: commandDetails.implicitHeight + 14
                                            color: root.page
                                            border.color: participantCommandDelegate.modelData.status === "已拒绝" ? root.danger
                                                : participantCommandDelegate.modelData.status === "待回执" ? root.orange : root.line
                                            radius: 4
                                            ColumnLayout {
                                                id: commandDetails
                                                anchors.fill: parent; anchors.margins: 7; spacing: 3
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    Text { text: root.commandKindLabel(participantCommandDelegate.modelData.kind); color: root.cyan; font.pixelSize: 10; font.bold: true }
                                                    Text { Layout.fillWidth: true; text: participantCommandDelegate.modelData.source; color: root.dim; font.pixelSize: 9; elide: Text.ElideRight }
                                                    Text { text: participantCommandDelegate.modelData.time ? new Date(participantCommandDelegate.modelData.time).toLocaleTimeString(Qt.locale(), "HH:mm:ss") : ""; color: root.dim; font.pixelSize: 9 }
                                                }
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: participantCommandDelegate.modelData.kind === "text" ? participantCommandDelegate.modelData.text
                                                        : participantCommandDelegate.modelData.kind === "attack" && !participantCommandDelegate.modelData.point ? ("目标 · " + root.targetLabel(participantCommandDelegate.modelData.targetId))
                                                        : participantCommandDelegate.modelData.kind === "withdrawal" ? "已收到撤离通知"
                                                        : participantCommandDelegate.modelData.point ? ("坐标 · X " + Math.round(participantCommandDelegate.modelData.point.x) + " / Y " + Math.round(participantCommandDelegate.modelData.point.y) + " m") : ""
                                                    color: root.ink; font.pixelSize: 10; wrapMode: Text.WordWrap
                                                }
                                                Text { visible: participantCommandDelegate.modelData.status.length > 0; text: participantCommandDelegate.modelData.status; color: participantCommandDelegate.modelData.status === "已拒绝" ? root.danger : participantCommandDelegate.modelData.status === "待回执" ? root.orange : root.cyan; font.pixelSize: 9; font.bold: true }
                                            }
                                        }
                                    }
                                }
                            }
                            Rectangle {
                                visible: !root.controller.isObserver && root.controller.matchPhase === "preparing"
                                         && root.tacticalViewIndex === 1
                                Layout.fillWidth: true
                                Layout.preferredHeight: visible ? deploymentPanel.implicitHeight + 18 : 0
                                color: "#111f2a"; border.color: root.deploymentState === "waiting" ? root.orange : root.line; radius: 5
                                ColumnLayout { id: deploymentPanel; anchors.fill: parent; anchors.margins: 9; spacing: 7
                                    RowLayout { Layout.fillWidth: true
                                        Text { Layout.fillWidth: true; text: root.isCommander ? "部署队列" : "本战位部署"; color: root.ink; font.pixelSize: 11; font.bold: true }
                                        Text { text: root.isCommander ? ("待处理 " + root.pendingDeploymentSeats().length) : root.seatStatusLabel(root.seatById(root.controller.currentSeatId) || ({})); color: root.isCommander ? root.orange : root.seatStatusColor(root.seatById(root.controller.currentSeatId) || ({})); font.pixelSize: 9; font.bold: true }
                                    }
                                    Text {
                                        visible: !root.isCommander
                                        Layout.fillWidth: true
                                        text: root.deploymentState === "deployed" ? "部署位置已确认，可提交战位就绪。" : "等待本方指挥官在地图上分配位置。"
                                        color: root.deploymentState === "deployed" ? root.cyan : root.orange
                                        font.pixelSize: 10; wrapMode: Text.WordWrap
                                    }
                                    ListView {
                                        id: deploymentQueue
                                        visible: root.isCommander
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: visible ? Math.min(112, Math.max(34, contentHeight)) : 0
                                        spacing: 5; clip: true
                                        model: root.pendingDeploymentSeats()
                                        delegate: Rectangle { id: deploymentDelegate
                                            required property var modelData
                                            width: deploymentQueue.width; height: 34; radius: 4
                                            color: root.deploymentTargetSeatId === deploymentDelegate.modelData.seatId ? root.panelAlt : root.page
                                            border.color: root.deploymentTargetSeatId === deploymentDelegate.modelData.seatId ? root.cyan : root.line
                                            RowLayout { anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 7
                                                Text { Layout.fillWidth: true; text: root.seatLabel(deploymentDelegate.modelData); color: root.ink; font.pixelSize: 9; elide: Text.ElideRight }
                                                Text { text: root.deploymentTargetSeatId === deploymentDelegate.modelData.seatId ? "选点中" : "选择"; color: root.deploymentTargetSeatId === deploymentDelegate.modelData.seatId ? root.cyan : root.dim; font.pixelSize: 9 }
                                            }
                                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.selectDeploymentSeat(deploymentDelegate.modelData) }
                                            Behavior on color { ColorAnimation { duration: 150 } }
                                        }
                                    }
                                    Text { visible: root.isCommander && root.pendingDeploymentSeats().length === 0; text: "本方已登录战位均已完成部署"; color: root.cyan; font.pixelSize: 10 }
                                    Text { visible: root.isCommander; Layout.fillWidth: true; text: root.strictVmf ? ("红方 " + (root.controller.redReady ? "已就绪" : "未就绪") + "  ·  蓝方固定靶已托管") : ("红方 " + (root.controller.redReady ? "已就绪" : "未就绪") + "  ·  蓝方 " + (root.controller.blueReady ? "已就绪" : "未就绪")); color: root.dim; font.pixelSize: 9 }
                                    Button {
                                        id: redeployButton
                                        visible: root.isCommander
                                        Layout.fillWidth: true
                                        property var targetSeat: root.selectedFriendlySeat() || ({})
                                        enabled: root.controller.matchPhase === "preparing"
                                                 && targetSeat.deployed === true
                                        text: enabled ? "重新部署选中单位" : "请选择已部署单位"
                                        Accessible.name: enabled ? "重新部署" + root.seatLabel(targetSeat) : text
                                        onClicked: root.controller.redeployOnlineUnit(targetSeat.seatId)
                                        contentItem: Text { text: redeployButton.text; color: redeployButton.enabled ? root.orange : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10 }
                                        background: Rectangle { color: root.panel; border.color: redeployButton.enabled ? root.orange : root.line; radius: 4 }
                                    }
                                }
                                Behavior on border.color { ColorAnimation { duration: 180 } }
                            }
                            RowLayout {
                                visible: !root.controller.isObserver && (root.controller.currentSeatType === "recon" || root.controller.currentSeatType === "commander")
                                         && root.tacticalViewIndex === 2
                                Layout.fillWidth: true
                                Text { Layout.fillWidth: true; text: root.isCommander ? "标点 / 情报接收战位" : "情报接收战位"; color: root.dim; font.pixelSize: 10 }
                                Text { text: root.selectedRecipientIds.length + " 已选"; color: root.selectedRecipientIds.length > 0 ? root.cyan : root.orange; font.pixelSize: 9 }
                            }
                            Flow {
                                visible: !root.controller.isObserver && root.tacticalViewIndex === 2
                                Layout.fillWidth: true
                                spacing: 6
                                CheckBox { id: communicationRangeToggle; text: "通信范围"; checked: root.showCommunicationRange; Accessible.name: "显示选中单位的通信范围"; onToggled: root.controller.saveSetting("online/map/showCommunicationRange", checked)
                                    contentItem: Text { text: communicationRangeToggle.text; color: communicationRangeToggle.checked ? AppContext.rangeCommunication : root.dim; font.pixelSize: 10; leftPadding: 18; verticalAlignment: Text.AlignVCenter }
                                    indicator: Rectangle { x: 1; anchors.verticalCenter: parent.verticalCenter; width: 13; height: 13; radius: 2; color: communicationRangeToggle.checked ? AppContext.rangeCommunication : root.page; border.color: root.line; Text { anchors.centerIn: parent; text: communicationRangeToggle.checked ? "✓" : ""; color: root.page; font.pixelSize: 9 } }
                                }
                                CheckBox { id: detectionRangeToggle; text: "探测范围"; checked: root.showDetectionRange; Accessible.name: "显示选中单位的探测范围"; onToggled: root.controller.saveSetting("online/map/showDetectionRange", checked)
                                    contentItem: Text { text: detectionRangeToggle.text; color: detectionRangeToggle.checked ? AppContext.rangeDetection : root.dim; font.pixelSize: 10; leftPadding: 18; verticalAlignment: Text.AlignVCenter }
                                    indicator: Rectangle { x: 1; anchors.verticalCenter: parent.verticalCenter; width: 13; height: 13; radius: 2; color: detectionRangeToggle.checked ? AppContext.rangeDetection : root.page; border.color: root.line; Text { anchors.centerIn: parent; text: detectionRangeToggle.checked ? "✓" : ""; color: root.page; font.pixelSize: 9 } }
                                }
                                CheckBox { id: attackRangeToggle; text: "攻击范围"; checked: root.showAttackRange; Accessible.name: "显示选中单位的攻击范围"; onToggled: root.controller.saveSetting("online/map/showAttackRange", checked)
                                    contentItem: Text { text: attackRangeToggle.text; color: attackRangeToggle.checked ? AppContext.rangeAttack : root.dim; font.pixelSize: 10; leftPadding: 18; verticalAlignment: Text.AlignVCenter }
                                    indicator: Rectangle { x: 1; anchors.verticalCenter: parent.verticalCenter; width: 13; height: 13; radius: 2; color: attackRangeToggle.checked ? AppContext.rangeAttack : root.page; border.color: root.line; Text { anchors.centerIn: parent; text: attackRangeToggle.checked ? "✓" : ""; color: root.page; font.pixelSize: 9 } }
                                }
                            }
                            Flow {
                                visible: !root.controller.isObserver && (root.controller.currentSeatType === "recon" || root.controller.currentSeatType === "commander")
                                         && root.tacticalViewIndex === 2
                                Layout.fillWidth: true; spacing: 5
                                Repeater {
                                    model: root.controller.onlineSeats || []
                                        delegate: CheckBox { id: recipientCheckBox
                                            required property var modelData
                                        visible: recipientCheckBox.modelData.occupied && recipientCheckBox.modelData.side === root.controller.currentSeatSide && recipientCheckBox.modelData.seatId !== root.controller.currentSeatId
                                        text: root.seatLabel(recipientCheckBox.modelData)
                                        checked: root.selectedRecipient(recipientCheckBox.modelData.seatId)
                                        onToggled: root.toggleRecipient(recipientCheckBox.modelData.seatId, checked)
                                        contentItem: Text { text: recipientCheckBox.text; color: recipientCheckBox.checked ? root.cyan : root.dim; font.pixelSize: 9; leftPadding: 18; verticalAlignment: Text.AlignVCenter }
                                        indicator: Rectangle { x: 1; anchors.verticalCenter: parent.verticalCenter; width: 13; height: 13; radius: 2; color: recipientCheckBox.checked ? root.cyan : root.page; border.color: root.line; Text { anchors.centerIn: parent; text: recipientCheckBox.checked ? "✓" : ""; color: root.page; font.pixelSize: 9 } }
                                    }
                                }
                            }
                            Text { visible: root.tacticalViewIndex === 0; text: root.controller.isObserver ? "战场单位" : "已部署友方单位"; color: root.dim; font.pixelSize: 10 }
                            Text { visible: root.tacticalViewIndex === 0 && root.deployedFriendlyUnits().length === 0; text: "当前没有已部署单位"; color: root.dim; font.pixelSize: 10 }
                            ListView { id: friendlyUnitList; visible: root.tacticalViewIndex === 0 && count > 0; Layout.fillWidth: true; Layout.preferredHeight: visible ? Math.min(180, Math.max(38, contentHeight)) : 0; model: root.deployedFriendlyUnits(); clip: true; spacing: 4
                                delegate: Rectangle { id: friendlyDelegate; required property var modelData; width: friendlyUnitList.width; height: 38; color: root.selectedUnitId === friendlyDelegate.modelData.id ? root.panelAlt : root.panel; border.color: root.selectedUnitId === friendlyDelegate.modelData.id ? root.cyan : "transparent"; radius: 4; visible: friendlyDelegate.modelData.alive
                                    RowLayout { anchors.fill: parent; anchors.margins: 8; z: 1
                                        Text { Layout.fillWidth: true; text: friendlyDelegate.modelData.callsign || friendlyDelegate.modelData.id; color: root.ink; font.pixelSize: 10; elide: Text.ElideRight }
                                        Text { text: friendlyDelegate.modelData.status || "在线"; color: root.selectedUnitId === friendlyDelegate.modelData.id ? root.cyan : root.dim; font.pixelSize: 9 }
                                        Button {
                                            id: observerTrailButton
                                            visible: root.controller.isObserver
                                            Layout.preferredWidth: 70
                                            Layout.preferredHeight: 24
                                            text: root.observerTrajectorySelected(friendlyDelegate.modelData.id)
                                                  ? "隐藏轨迹" : "显示轨迹"
                                            Accessible.name: text + " "
                                                             + (friendlyDelegate.modelData.callsign
                                                                || friendlyDelegate.modelData.id)
                                            onClicked: root.toggleObserverTrajectory(
                                                friendlyDelegate.modelData.id)
                                            contentItem: Text {
                                                text: observerTrailButton.text
                                                color: root.observerTrajectorySelected(
                                                           friendlyDelegate.modelData.id)
                                                       ? root.orange : root.cyan
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                                font.pixelSize: 9
                                            }
                                            background: Rectangle {
                                                color: root.panelAlt
                                                border.color: root.observerTrajectorySelected(
                                                                  friendlyDelegate.modelData.id)
                                                              ? root.orange : root.line
                                                radius: 4
                                            }
                                        }
                                    }
                                    MouseArea { anchors.fill: parent; z: 0; cursorShape: Qt.PointingHandCursor; onClicked: root.selectUnit(friendlyDelegate.modelData) }
                                    Behavior on color { ColorAnimation { duration: 150 } }
                                }
                            }
                            UnitPanel {
                                id: selectedUnitPanel
                                visible: root.tacticalViewIndex === 0 && root.selectedUnitId.length > 0
                                Layout.fillWidth: true
                                Layout.preferredHeight: visible ? 300 : 0
                                clip: true
                                controller: root.controller
                                editor: root.editor
                                snap: root.selectedUnitSnapshot
                                interactionEnabled: !root.controller.isObserver
                                                    && root.controller.matchPhase === "running"
                                Accessible.name: "选中友方单位详情"
                            }
                            OnlineIntelWorkspace {
                                visible: !root.controller.isObserver && root.tacticalViewIndex === 2
                                controller: root.controller
                                mapCanvas: onlineCanvas
                                seats: root.controller.onlineSeats
                                reportPosition: root.intelReportPosition
                                reportPicking: root.intelReportPicking
                                Layout.fillWidth: true
                                onReportPickRequested: {
                                    root.intelReportPicking = true
                                    root.deploymentNotice = "右键选择人工情报位置"
                                }
                                onReportPickCancelled: root.intelReportPicking = false
                            }
                            ComboBox {
                                id: targetBox
                                visible: !root.controller.isObserver && root.controller.currentSeatType === "attack" && root.controller.matchPhase === "running"
                                         && root.tacticalViewIndex === 0
                                Layout.fillWidth: true
                                model: root.controller.unitStateRevision >= 0
                                    ? root.controller.detectedEnemyOptions(root.selectedUnitId, root.controller.currentSeatSide, root.controller.currentSeatSide === "red" ? "blue" : "red") : []
                                textRole: "callsign"
                                valueRole: "id"
                                enabled: count > 0
                                Component.onCompleted: root.syncAttackTargetBox()
                                onModelChanged: root.syncAttackTargetBox()
                                onActivated: root.attackTargetId = currentValue || ""
                                background: Rectangle { color: "#0b151c"; border.color: root.line; radius: 4 }
                                contentItem: Text { text: root.controller.unitStateRevision >= 0 ? root.attackTargetLabel(targetBox.currentText) : (targetBox.currentText || "选择已掌握的敌方目标"); color: root.ink; verticalAlignment: Text.AlignVCenter; leftPadding: 8; font.pixelSize: 10 }
                            }
                            ColumnLayout {
                                id: attackControl
                                visible: !root.controller.isObserver && root.controller.currentSeatType === "attack" && root.controller.matchPhase === "running"
                                         && root.tacticalViewIndex === 0
                                Layout.fillWidth: true; spacing: 5
                                property var selectedAttackUnit: root.selectedUnitSnapshot
                                property var actions: selectedAttackUnit.actions || selectedAttackUnit.actionCapabilities || ({})
                                property bool serverAllowsEngage: actions.engageTarget !== undefined
                                    ? (typeof actions.engageTarget === "object"
                                       ? Boolean(actions.engageTarget.enabled)
                                       : Boolean(actions.engageTarget))
                                    : Number(selectedAttackUnit.ammoRemaining || 0) > 0
                                      && !Boolean(selectedAttackUnit.serviceRequested)
                                      && Number(selectedAttackUnit.cooldownRemaining || 0) <= 0
                                Button { id: engageButton; Layout.fillWidth: true; enabled: root.selectedUnitId && attackControl.selectedAttackUnit.alive && root.attackTargetId.length > 0 && attackControl.serverAllowsEngage; text: "执行攻击 · 在途 " + Number(attackControl.selectedAttackUnit.activeProjectileCount || 0) + " 枚"; Accessible.name: (enabled ? "执行攻击" : root.engageAvailabilityLabel()) + "，在途 " + Number(attackControl.selectedAttackUnit.activeProjectileCount || 0) + " 枚"
                                    onClicked: root.engageFocusedTarget()
                                    ToolTip.visible: hovered && !enabled
                                    ToolTip.text: root.engageAvailabilityLabel()
                                    contentItem: Text { text: engageButton.text; color: engageButton.enabled ? root.page : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.bold: true }
                                    background: Rectangle { color: engageButton.enabled ? root.orange : root.line; radius: 4 }
                                }
                            }
                            ColumnLayout {
                                visible: !root.controller.isObserver && !root.isCommander && root.controller.matchPhase === "running"
                                         && root.tacticalViewIndex === 0
                                Layout.fillWidth: true
                                spacing: 5
                                Text { text: "本单位移动速度"; color: root.dim; font.pixelSize: 10 }
                                RowLayout {
                                    Layout.fillWidth: true; spacing: 8
                                    Slider {
                                        id: unitSpeedSlider
                                        Layout.fillWidth: true
                                        from: 1; to: root.editableUnitSpeedLimit(root.ownSeatUnitSnapshot()); stepSize: 5
                                        value: root.unitSpeedDraft
                                        onMoved: {
                                            root.unitSpeedDraft = Math.round(value)
                                            root.unitSpeedDirty = true
                                        }
                                    }
                                    Text { text: root.unitSpeedDraft + " m/s"; color: root.ink; font.family: "Consolas"; font.pixelSize: 10; Layout.preferredWidth: 70; horizontalAlignment: Text.AlignRight }
                                }
                                Button {
                                    id: applyUnitSpeedButton
                                    Layout.fillWidth: true
                                    enabled: ((root.seatById(root.controller.currentSeatId) || {}).unitId || "").length > 0
                                        && root.unitSpeedLimit(root.ownSeatUnitSnapshot()) > 0
                                    text: "应用移动速度"
                                    onClicked: {
                                        var ownSeat = root.seatById(root.controller.currentSeatId) || ({})
                                        root.unitSpeedDirty = true
                                        root.controller.command("setSpeed", { unitId: ownSeat.unitId,
                                                                           speed: root.unitSpeedDraft })
                                        root.deploymentNotice = "移动速度已提交 · " + root.unitSpeedDraft + " m/s"
                                    }
                                    contentItem: Text { text: applyUnitSpeedButton.text; color: applyUnitSpeedButton.enabled ? root.cyan : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10 }
                                    background: Rectangle { color: root.panel; border.color: applyUnitSpeedButton.enabled ? root.cyan : root.line; radius: 4 }
                                }
                            }
                            Button { id: commanderReadyButton; visible: !root.controller.isObserver && root.controller.currentSeatType === "commander" && root.tacticalViewIndex === 0; Layout.fillWidth: true; enabled: root.canDeploy ? onlineCanvas.pointerInside : (root.seatById(root.controller.currentSeatId) || ({})).deployed === true && (root.controller.seatReady || root.friendlySeatsReady()); text: root.canDeploy ? "确认当前位置部署指挥所" : root.controller.seatReady ? "取消指挥官就绪" : enabled ? "确认指挥官就绪" : root.friendlySeatsReady() ? "请先部署指挥所" : "等待本方单位部署并就绪"; onClicked: {
                                    if (root.canDeploy) {
                                        var point = onlineCanvas.pointerLogicalPos
                                        root.controller.deployOnlineUnit(root.deployUnitId, point)
                                        root.deploymentState = "submitting"
                                        root.deploymentNotice = "部署位置已提交，等待服务器确认"
                                        onlineCanvas.pulseActionAt(point, root.cyan)
                                    } else {
                                        root.controller.setSeatReady(!root.controller.seatReady)
                                    }
                                }
                                onEnabledChanged: if (enabled && root.canDeploy) Qt.callLater(commanderReadyButton.forceActiveFocus)
                                contentItem: Text { text: commanderReadyButton.text; color: root.page; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.bold: true }
                                background: Rectangle { color: commanderReadyButton.enabled ? (root.controller.seatReady ? root.orange : root.cyan) : root.line; radius: 4; Behavior on color { ColorAnimation { duration: 150 } } }
                            }
                            Button { id: participantReadyButton; visible: !root.controller.isObserver && root.controller.currentSeatType !== "commander" && root.tacticalViewIndex === 0; Layout.fillWidth: true; enabled: (root.seatById(root.controller.currentSeatId) || ({})).deployed === true; text: root.controller.seatReady ? "取消战位就绪" : enabled ? "确认部署并就绪" : "等待指挥官部署"; onClicked: root.controller.setSeatReady(!root.controller.seatReady)
                                contentItem: Text { text: participantReadyButton.text; color: root.page; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.bold: true }
                                background: Rectangle { color: participantReadyButton.enabled ? (root.controller.seatReady ? root.orange : root.cyan) : root.line; radius: 4; Behavior on color { ColorAnimation { duration: 150 } } }
                            }
                            Button {
                                id: redeployRequestButton
                                visible: !root.controller.isObserver && root.controller.currentSeatType !== "commander" && root.controller.matchPhase === "preparing" && root.tacticalViewIndex === 0
                                Layout.fillWidth: true
                                property var ownSeat: root.seatById(root.controller.currentSeatId) || ({})
                                enabled: ownSeat.deployed === true && !ownSeat.redeployRequested
                                text: ownSeat.redeployRequested ? "已申请重新部署" : "申请重新部署"
                                onClicked: root.controller.requestOnlineRedeploy()
                                contentItem: Text { text: redeployRequestButton.text; color: redeployRequestButton.enabled ? root.orange : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10 }
                                background: Rectangle { color: root.panel; border.color: redeployRequestButton.enabled ? root.orange : root.line; radius: 4 }
                            }
                            TextField {
                                id: unitNameField
                                visible: !root.controller.isObserver && root.controller.matchPhase === "preparing" && root.controller.currentSeatId !== "" && root.tacticalViewIndex === 0
                                Layout.fillWidth: true
                                placeholderText: "本单位画布显示名称"
                                maximumLength: 128
                                selectByMouse: true
                                text: root.unitNameDraft
                                onTextEdited: {
                                    root.unitNameDraft = text
                                    root.unitNameDirty = true
                                }
                                color: root.ink
                                background: Rectangle { color: root.panelAlt; border.color: root.line; radius: 4 }
                            }
                            Button {
                                id: unitNameButton
                                visible: unitNameField.visible
                                Layout.fillWidth: true
                                enabled: unitNameField.text.trim().length > 0
                                text: "更新画布名称"
                                onClicked: {
                                    root.unitNameDraft = unitNameField.text.trim()
                                    root.unitNameDirty = true
                                    root.controller.setOnlineUnitName(root.unitNameDraft)
                                    root.deploymentNotice = "单位名称已提交 · " + root.unitNameDraft
                                }
                                contentItem: Text { text: unitNameButton.text; color: unitNameButton.enabled ? root.cyan : root.dim; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 10 }
                                background: Rectangle { color: root.panel; border.color: root.line; radius: 4 }
                            }
                        }
                        }
                        }
                    }
                }
            }
        }
    }
}
