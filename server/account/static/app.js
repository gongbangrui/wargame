const storedUiScaleValue = localStorage.getItem("wargameUiScale");
const storedUiScale = storedUiScaleValue === null ? Number.NaN : Number(storedUiScaleValue);
const roomOperationTimeoutMs = 25000;
const state = { token: sessionStorage.getItem("adminToken") || "", users: [], rooms: [], roomsLoaded: false, deleteId: null, roomDeleteId: null, roomOperations: {}, autoPausingRooms: new Set(), autoPausedOperations: {}, roomKickRequests: new Set(), roomStatusFilter: "all", openRoomMenuId: "", roomSaving: false, overview: null, events: [], terminalUnlocked: false, terminalSocket: null, monitorTimer: null, roomTimer: null, roomRefreshInFlight: false, occupantsRoomId: "", occupants: [], occupantsTimer: null, activeModal: null, modalReturnFocus: null, uiScale: Number.isFinite(storedUiScale) ? Math.max(0.85, Math.min(1.15, storedUiScale)) : 1 };
const $ = (id) => document.getElementById(id);
const seatDefinitions = [
  ["red_commander", "红方指挥官", true], ["red_attack", "红方攻击机", false], ["red_recon", "红方侦察机", false], ["red_ground", "红方地面单位", false], ["red_jammer", "红方干扰机", false],
  ["blue_commander", "蓝方指挥官", true], ["blue_attack", "蓝方攻击机", false], ["blue_recon", "蓝方侦察机", false], ["blue_ground", "蓝方地面单位", false], ["blue_jammer", "蓝方干扰机", false],
];
const defaultSeatLimits = { red_commander: 1, red_attack: 2, red_recon: 1, red_ground: 2, red_jammer: 1,
  blue_commander: 1, blue_attack: 2, blue_recon: 1, blue_ground: 2, blue_jammer: 1 };

function numberInput(value, min = 0, max = 1000000, disabled = false) {
  const input = document.createElement("input");
  input.type = "number"; input.min = String(min); input.max = String(max); input.step = "1";
  input.value = value === undefined || value === null || value === "" ? "" : value;
  input.disabled = disabled;
  return input;
}

function renderSeatEditors(room = null) {
  const limits = { ...defaultSeatLimits, ...(room?.seatLimits || {}) };
  const parameters = room?.seatParameters || {};
  const limitEditor = $("seatLimitEditor");
  const parameterEditor = $("seatParameterEditor");
  if (!limitEditor || !parameterEditor) return;
  limitEditor.replaceChildren(); parameterEditor.replaceChildren();
  for (const [key, label, commander] of seatDefinitions) {
    const limitRow = document.createElement("div"); limitRow.className = "seat-config-row capacity"; limitRow.dataset.seatKey = key;
    const title = document.createElement("strong"); title.textContent = label; limitRow.appendChild(title);
    const limit = numberInput(commander ? 1 : limits[key], 0, 64, commander); limit.className = "seat-limit"; limitRow.appendChild(limit); limitEditor.appendChild(limitRow);

    const parameterRow = document.createElement("div"); parameterRow.className = "seat-config-row"; parameterRow.dataset.seatKey = key;
    const parameterTitle = document.createElement("strong"); parameterTitle.textContent = label; parameterRow.appendChild(parameterTitle);
    const base = parameters[key] || {};
    const comm = numberInput(base.communicationRange, 0, 1000000); comm.className = "seat-communication"; comm.placeholder = "通信"; parameterRow.appendChild(comm);
    const detect = numberInput(base.detectRange, 0, 1000000); detect.className = "seat-detect"; detect.placeholder = "侦察"; parameterRow.appendChild(detect);
    parameterEditor.appendChild(parameterRow);
  }
}

function collectSeatLimits() {
  const result = {};
  document.querySelectorAll("#seatLimitEditor .seat-config-row").forEach((row) => {
    const input = row.querySelector(".seat-limit");
    const value = Number(input.value);
    if (Number.isFinite(value) && value >= 0) result[row.dataset.seatKey] = Math.min(64, Math.trunc(value));
  });
  return result;
}

function collectSeatParameters() {
  let result = {};
  try { result = JSON.parse($("roomParameters").value || "{}"); } catch (_) { throw new Error("高级战位参数 JSON 无效"); }
  document.querySelectorAll("#seatParameterEditor .seat-config-row").forEach((row) => {
    const parameter = { ...(result[row.dataset.seatKey] || {}) };
    const comm = row.querySelector(".seat-communication").value;
    const detect = row.querySelector(".seat-detect").value;
    if (comm !== "") parameter.communicationRange = Number(comm); else delete parameter.communicationRange;
    if (detect !== "") parameter.detectRange = Number(detect); else delete parameter.detectRange;
    if (Object.keys(parameter).length) result[row.dataset.seatKey] = parameter;
    else delete result[row.dataset.seatKey];
  });
  return result;
}

function formatApiError(data) {
  const detail = data?.detail;
  if (Array.isArray(detail)) {
    const messages = detail.map((entry) => {
      if (typeof entry === "string") return entry;
      if (entry && typeof entry === "object") {
        const location = Array.isArray(entry.loc) ? entry.loc.filter((part) => part !== "body") : [];
        const message = typeof entry.msg === "string" ? entry.msg : JSON.stringify(entry);
        return location.length && message ? `${location.join(".")}: ${message}` : message;
      }
      return entry === null || entry === undefined ? "" : String(entry);
    }).filter(Boolean);
    if (messages.length) return messages.join("；");
  }
  if (typeof detail === "string" && detail) return detail;
  if (detail && typeof detail === "object") {
    if (typeof detail.message === "string" && detail.message) return detail.message;
    return JSON.stringify(detail);
  }
  return "请求失败";
}

async function api(path, options = {}) {
  const headers = { "Content-Type": "application/json", ...(options.headers || {}) };
  if (state.token) headers.Authorization = `Bearer ${state.token}`;
  const response = await fetch(path, { ...options, headers });
  let data = {};
  try { data = await response.json(); } catch (_) { data = {}; }
  if (!response.ok) {
    if (response.status === 401 && path !== "/api/admin/login") logout(false);
    throw new Error(formatApiError(data));
  }
  return data;
}

function toast(message, error = false) {
  const node = $("toast");
  node.textContent = message;
  node.className = `toast${error ? " error" : ""}`;
  window.clearTimeout(toast.timer);
  toast.timer = window.setTimeout(() => node.classList.add("hidden"), 3200);
}

function clearToast() {
  const node = $("toast");
  if (!node) return;
  window.clearTimeout(toast.timer);
  node.className = "toast hidden";
}

function setTextWithTitle(node, value) {
  const text = String(value ?? "");
  node.textContent = text;
  node.title = text;
}

const modalFocusableSelector = "button:not([disabled]), input:not([disabled]):not([type='hidden']), select:not([disabled]), textarea:not([disabled]), [href], [tabindex]:not([tabindex='-1'])";

function openModal(modal, initialFocus) {
  state.modalReturnFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null;
  state.activeModal = modal;
  document.body.classList.add("modal-open");
  $("adminView").inert = true;
  modal.classList.remove("hidden");
  initialFocus?.focus();
}

function closeModal(modal) {
  modal?.classList.add("hidden");
  if (state.activeModal !== modal) return;
  const returnFocus = state.modalReturnFocus;
  state.activeModal = null;
  state.modalReturnFocus = null;
  document.body.classList.remove("modal-open");
  $("adminView").inert = false;
  if (returnFocus?.isConnected) returnFocus.focus();
}

function closeActiveModal() {
  if (state.activeModal === $("occupantsModal")) closeOccupants();
  else if (state.activeModal === $("roomModal")) closeRoomModal();
  else if (state.activeModal === $("userModal")) closeUserModal();
  else closeModal(state.activeModal);
}

function handleModalKeydown(event) {
  const modal = state.activeModal;
  if (!modal) return;
  if (event.key === "Escape") {
    event.preventDefault();
    closeActiveModal();
    return;
  }
  if (event.key !== "Tab") return;
  const focusable = [...modal.querySelectorAll(modalFocusableSelector)].filter((element) => element.getClientRects().length > 0);
  if (!focusable.length) {
    event.preventDefault();
    return;
  }
  const first = focusable[0];
  const last = focusable[focusable.length - 1];
  if (!modal.contains(document.activeElement)) {
    event.preventDefault();
    (event.shiftKey ? last : first).focus();
  } else if (event.shiftKey && document.activeElement === first) {
    event.preventDefault();
    last.focus();
  } else if (!event.shiftKey && document.activeElement === last) {
    event.preventDefault();
    first.focus();
  }
}

function applyUiScale() {
  document.documentElement.style.setProperty("--ui-scale", state.uiScale.toFixed(2));
  const value = $("scaleValue");
  if (value) value.textContent = `${Math.round(state.uiScale * 100)}%`;
  localStorage.setItem("wargameUiScale", String(state.uiScale));
}

function showAdmin(username) {
  $("loginView").classList.add("hidden");
  $("adminView").classList.remove("hidden");
  $("adminIdentity").textContent = username;
}

function logout(callServer = true) {
  if (callServer && state.token) api("/api/admin/logout", { method: "POST" }).catch(() => {});
  closeTerminal();
  state.token = "";
  state.users = [];
  state.rooms = [];
  state.roomsLoaded = false;
  state.openRoomMenuId = "";
  state.roomKickRequests.clear();
  state.overview = null;
  state.events = [];
  window.clearInterval(state.monitorTimer);
  window.clearInterval(state.roomTimer);
  state.roomTimer = null;
  state.roomRefreshInFlight = false;
  closeOccupants();
  closeRoomModal();
  closeUserModal();
  closeModal($("confirmModal"));
  sessionStorage.removeItem("adminToken");
  $("adminView").classList.add("hidden");
  $("loginView").classList.remove("hidden");
  $("adminPassword").value = "";
}

function renderUsers() {
  const body = $("userRows");
  body.replaceChildren();
  for (const [index, user] of state.users.entries()) {
    const tr = document.createElement("tr");
    tr.style.setProperty("--row-index", index);
    tr.innerHTML = `
      <td class="user-cell"></td><td class="display"></td>
      <td><span class="badge role-player">联网账号</span></td>
      <td><span class="${user.enabled ? "enabled" : "disabled"}">${user.enabled ? "已启用" : "已停用"}</span><span class="${user.online ? "online" : "offline"}">${user.online ? "在线" : "离线"}</span></td>
      <td class="updated"></td>
      <td class="actions"><button class="row-button edit">编辑</button><button class="row-button kick-user" ${user.online ? "" : "disabled"}>踢下线</button><button class="row-button delete">删除</button></td>`;
    setTextWithTitle(tr.querySelector(".user-cell"), user.username);
    setTextWithTitle(tr.querySelector(".display"), user.displayName);
    setTextWithTitle(tr.querySelector(".updated"), new Date(user.updatedAt).toLocaleString("zh-CN", { hour12: false }));
    tr.querySelector(".edit").addEventListener("click", () => openUserModal(user));
    tr.querySelector(".kick-user").addEventListener("click", async () => {
      if (!user.online || !window.confirm(`确定将账号“${user.username}”强制下线吗？`)) return;
      try {
        await api(`/api/admin/users/${user.id}/kick`, { method: "POST", body: JSON.stringify({ reason: "管理员强制下线" }) });
        toast("已发送强制下线请求");
        await loadUsers();
      } catch (error) { toast(error.message, true); }
    });
    tr.querySelector(".delete").addEventListener("click", () => openDelete(user));
    body.appendChild(tr);
  }
  $("emptyState").classList.toggle("hidden", state.users.length !== 0);
  $("totalCount").textContent = state.users.length;
  $("enabledCount").textContent = state.users.filter((u) => u.enabled).length;
  $("sideCount").textContent = `${state.users.filter((u) => u.enabled).length} 个可登录`;
  $("staffCount").textContent = "进入房间后分配";
  document.querySelectorAll("#usersPage .metric-strip strong").forEach((node) => {
    node.classList.remove("metric-flash");
    requestAnimationFrame(() => node.classList.add("metric-flash"));
  });
}

async function loadUsers() {
  const data = await api("/api/admin/users");
  state.users = data.users;
  renderUsers();
}

const roomStatusLabels = {
  stopped: "停止",
  preparing: "准备",
  paused: "暂停",
  running: "推演",
  finished: "已结束",
  ready: "已就绪",
  stale: "状态过期",
  unknown: "状态未知",
};

function roomCapacity(room) {
  const limits = Object.entries(room.seatLimits || {});
  const total = limits.reduce((sum, [, value]) => sum + Number(value || 0), 0);
  const red = limits.filter(([key]) => key.startsWith("red_")).reduce((sum, [, value]) => sum + Number(value || 0), 0);
  const blue = limits.filter(([key]) => key.startsWith("blue_")).reduce((sum, [, value]) => sum + Number(value || 0), 0);
  return { total, detail: limits.length ? `红 ${red} · 蓝 ${blue}` : "默认战位" };
}

function roomOperation(room) {
  const local = state.roomOperations[room.roomId];
  const remote = room.operation || null;
  if (local?.state === "pending"
      && (!local.operationId || local.operationId !== remote?.operationId)) {
    return local;
  }
  return remote || local || null;
}

function operationTimedOut(operation) {
  const requestedAt = Date.parse(operation?.requestedAt || "");
  return Number.isFinite(requestedAt) && Date.now() - requestedAt >= roomOperationTimeoutMs;
}

function roomActionAllowed(room, action, pending) {
  if (action === "force-stop") return room.status !== "stopped";
  if (pending) return false;
  if (action === "open") return ["stopped", "finished"].includes(room.status);
  if (action === "start") return ["preparing", "paused"].includes(room.status) && room.readyForStart;
  if (action === "resume") return room.status === "paused";
  if (action === "pause") return room.status === "running";
  if (action === "finish") return ["running", "preparing", "paused"].includes(room.status);
  if (action === "stop") return room.status !== "stopped";
  return room.status !== "running";
}

function roomActionTitle(room, action, pending) {
  if (action === "force-stop") return "立即停止并终止等待确认的操作";
  if (pending) return "等待兵棋服务确认上一项操作";
  if (action === "start" && !room.readyForStart) return "所有已占用战位完成部署并就绪后才能开始推演";
  if (!roomActionAllowed(room, action, false)) return `当前“${roomStatusLabels[room.status] || "未知"}”状态不能执行此操作`;
  return action === "reset" || action === "redeploy" ? "执行前将要求确认" : "执行房间操作";
}

async function requestRoomAction(room, action, automatic = false) {
  const destructive = { reset: "重置会移除当前部署和就绪状态。是否继续？", redeploy: "重新部署会移除当前部署位置。是否继续？", stop: "停止会中断当前准备或推演。是否继续？", "force-stop": "强制停止会终止当前等待确认并停止房间。是否继续？", finish: "结束后需要重新开启准备阶段才能继续。是否继续？" };
  if (!automatic && destructive[action] && !window.confirm(destructive[action])) return;
  state.roomOperations[room.roomId] = { action, state: "pending" };
  renderRooms();
  try {
    const response = await api(`/api/admin/rooms/${encodeURIComponent(room.roomId)}/${action}`, { method: "POST" });
    if (response.operation && response.operation.state === "acknowledged") {
      delete state.roomOperations[room.roomId];
    } else {
      state.roomOperations[room.roomId] = response.operation;
    }
    await loadRooms();
    toast(automatic ? "兵棋服务未响应，房间已暂停等待" : response.operation.state === "pending" ? "操作已请求，等待兵棋服务确认" : "房间状态已确认");
  } catch (error) {
    state.roomOperations[room.roomId] = { action, state: "failed" };
    renderRooms();
    toast(error.message, true);
  }
}

function pauseTimedOutOperation(room, operation) {
  const operationKey = operation.operationId || operation.requestedAt;
  if (!operationTimedOut(operation) || !["preparing", "running"].includes(room.status)
      || state.autoPausingRooms.has(room.roomId)
      || state.autoPausedOperations[room.roomId] === operationKey) return;
  state.autoPausingRooms.add(room.roomId);
  state.autoPausedOperations[room.roomId] = operationKey;
  requestRoomAction(room, "pause", true)
    .catch(() => {})
    .finally(() => state.autoPausingRooms.delete(room.roomId));
}

const roomActionLabels = {
  open: "进入准备",
  start: "开始推演",
  resume: "继续推演",
  pause: "暂停推演",
  finish: "结束推演",
  reset: "重置房间",
  redeploy: "重新部署",
  stop: "停止房间",
  "force-stop": "强制停止",
};

function roomPrimaryAction(room) {
  if (room.status === "preparing") return { action: "start", label: room.readyForStart ? "开始推演" : "等待就绪" };
  if (room.status === "paused") return { action: "resume", label: "继续推演" };
  if (room.status === "running") return { action: "pause", label: "暂停推演" };
  return { action: "open", label: "进入准备" };
}

function renderRooms() {
  const grid = $("roomRows");
  if (!grid) return;
  grid.replaceChildren();
  const filter = state.roomStatusFilter || "all";
  const rooms = state.rooms.filter((room) => filter === "all" || room.status === filter);
  const visibleCount = $("roomVisibleCount");
  if (visibleCount) visibleCount.textContent = `${rooms.length} / ${state.rooms.length}`;
  for (const [index, room] of rooms.entries()) {
    const card = document.createElement("article");
    card.className = "room-card";
    card.style.setProperty("--row-index", index);
    card.dataset.status = room.status;
    card.innerHTML = `
      <header class="room-card-head">
        <div class="room-title"><strong class="room-name"></strong><span class="room-id"></span></div>
        <div class="room-status-main"><span class="badge room-status-badge"></span><span class="room-enabled"></span></div>
      </header>
      <div class="room-card-body">
        <div class="room-fact"><span>战位</span><strong class="capacity-total"></strong><small class="capacity-detail"></small></div>
        <div class="room-fact"><span>就绪</span><strong class="room-readiness"></strong><small class="room-operation"></small></div>
      </div>
      <footer class="room-card-actions">
        <button class="room-occupants secondary compact" type="button">战位与用户</button>
        <button class="room-primary primary compact" type="button"></button>
        <details class="room-menu"><summary aria-label="更多房间操作" title="更多房间操作">⋯</summary><div class="room-menu-panel">
          <button class="room-edit" type="button">编辑房间</button>
          <button class="room-action" data-action="reset" type="button">重置房间</button>
          <button class="room-action" data-action="redeploy" type="button">重新部署</button>
          <button class="room-action" data-action="finish" type="button">结束推演</button>
          <button class="room-action" data-action="stop" type="button">停止房间</button>
          <button class="room-action danger-text" data-action="force-stop" type="button">强制停止</button>
          <button class="room-delete danger-text" type="button">删除房间</button>
        </div></details>
      </footer>`;
    setTextWithTitle(card.querySelector(".room-name"), room.name);
    setTextWithTitle(card.querySelector(".room-id"), `${room.roomId} · ${room.scenarioId || "default"}`);
    const operation = roomOperation(room);
    const pending = operation?.state === "pending";
    const badge = card.querySelector(".room-status-badge");
    badge.className = `badge room-status-badge status-${room.status || "unknown"}`;
    setTextWithTitle(badge, roomStatusLabels[room.status] || roomStatusLabels.unknown);
    const enabled = card.querySelector(".room-enabled");
    enabled.className = `room-enabled ${room.enabled ? "enabled" : "disabled"}`;
    setTextWithTitle(enabled, room.enabled ? "可进入" : "已关闭");
    const readiness = `红 ${room.redCommanderReady ? "就绪" : "未就绪"} · 蓝 ${room.blueCommanderReady ? "就绪" : "未就绪"}`;
    setTextWithTitle(card.querySelector(".room-readiness"), readiness);
    const operationNode = card.querySelector(".room-operation");
    if (operation) {
      const timedOut = operation.state === "pending" && operationTimedOut(operation);
      setTextWithTitle(operationNode, timedOut ? "等待确认（未响应）" : operation.state === "pending" ? "等待确认" : operation.state === "acknowledged" ? "已确认" : "操作失败");
      operationNode.className = `room-operation ${operation.state}${timedOut ? " timed-out" : ""}`;
      if (timedOut) pauseTimedOutOperation(room, operation);
    }
    const capacity = roomCapacity(room);
    setTextWithTitle(card.querySelector(".capacity-total"), `${capacity.total} 席`);
    setTextWithTitle(card.querySelector(".capacity-detail"), capacity.detail);
    const menu = card.querySelector(".room-menu");
    if (state.openRoomMenuId === room.roomId) menu.open = true;
    menu.addEventListener("toggle", () => {
      if (menu.open) {
        state.openRoomMenuId = room.roomId;
        document.querySelectorAll(".room-menu[open]").forEach((candidate) => {
          if (candidate !== menu) candidate.removeAttribute("open");
        });
      } else if (state.openRoomMenuId === room.roomId) {
        state.openRoomMenuId = "";
      }
    });
    const editButton = card.querySelector(".room-edit");
    editButton.disabled = room.status === "running" || pending;
    editButton.title = pending ? "等待兵棋服务确认上一项操作" : room.status === "running" ? "推演进行中不能修改房间配置" : "编辑房间";
    editButton.addEventListener("click", () => { menu.removeAttribute("open"); openRoomModal(room); });
    const occupantsButton = card.querySelector(".room-occupants");
    occupantsButton.addEventListener("click", () => openOccupants(room));
    occupantsButton.title = "查看在线用户、部署与就绪回报";
    const primary = roomPrimaryAction(room);
    const primaryButton = card.querySelector(".room-primary");
    primaryButton.textContent = pending ? "等待确认" : primary.label;
    primaryButton.disabled = !roomActionAllowed(room, primary.action, pending);
    primaryButton.title = roomActionTitle(room, primary.action, pending);
    primaryButton.addEventListener("click", () => requestRoomAction(room, primary.action));
    const deleteButton = card.querySelector(".room-delete");
    deleteButton.addEventListener("click", async () => {
      menu.removeAttribute("open");
      if (!window.confirm(`确定删除房间“${room.name}”吗？`)) return;
      try { await api(`/api/admin/rooms/${encodeURIComponent(room.roomId)}`, { method: "DELETE" }); await loadRooms(); toast("房间已删除"); }
      catch (error) { toast(error.message, true); }
    });
    deleteButton.disabled = room.status === "running" || pending;
    deleteButton.title = pending ? "等待兵棋服务确认上一项操作" : room.status === "running" ? "推演进行中不能删除房间" : "删除房间";
    card.querySelectorAll(".room-action").forEach((button) => {
      const action = button.dataset.action;
      button.disabled = !roomActionAllowed(room, action, pending);
      button.title = roomActionTitle(room, action, pending);
      button.textContent = roomActionLabels[action];
      button.addEventListener("click", () => { menu.removeAttribute("open"); requestRoomAction(room, action); });
    });
    grid.appendChild(card);
  }
  const emptyState = $("roomEmptyState");
  if (emptyState) {
    emptyState.textContent = state.rooms.length && !rooms.length ? "没有符合当前状态的房间。" : "暂无推演房间。";
    emptyState.classList.toggle("hidden", rooms.length !== 0);
  }
}

const seatTypeLabels = { commander: "指挥官", attack: "攻击机", recon: "侦察机", ground: "地面单位", jammer: "干扰机" };
function seatLabel(occupant) {
  if (!occupant.seatId) return "房间大厅";
  const side = occupant.side === "red" ? "红方" : occupant.side === "blue" ? "蓝方" : "";
  return `${side}${seatTypeLabels[occupant.seatType] || occupant.seatType || occupant.seatId}`;
}

function renderOccupants() {
  const list = $("occupantsList");
  if (!list) return;
  list.replaceChildren();
  if (!state.occupants.length) {
    list.innerHTML = '<div class="empty">当前没有在线用户。</div>';
  } else {
    for (const occupant of state.occupants) {
      const row = document.createElement("div");
      row.className = "occupant-row";
      const requestKey = `${state.occupantsRoomId}:${occupant.userId}`;
      const kickPending = state.roomKickRequests.has(requestKey);
      row.innerHTML = `<div class="occupant-main"><strong class="occupant-name"></strong><span class="occupant-user"></span></div><div class="occupant-seat"></div><div class="occupant-time"></div><button class="danger compact occupant-kick" type="button" ${kickPending ? "disabled" : ""}>${kickPending ? "移出中" : "移出房间"}</button>`;
      setTextWithTitle(row.querySelector(".occupant-name"), occupant.displayName || occupant.username);
      setTextWithTitle(row.querySelector(".occupant-user"), `@${occupant.username}`);
      setTextWithTitle(row.querySelector(".occupant-seat"), seatLabel(occupant));
      const connected = occupant.connectedAt ? new Date(occupant.connectedAt).toLocaleTimeString("zh-CN", { hour12: false }) : "--";
      const lastSeen = occupant.lastSeenAt ? new Date(occupant.lastSeenAt).toLocaleTimeString("zh-CN", { hour12: false }) : "--";
      setTextWithTitle(row.querySelector(".occupant-time"), `在线 · 进入 ${connected} · 活动 ${lastSeen}`);
      row.querySelector(".occupant-kick").title = kickPending ? "等待房间服务确认" : "请求将用户移出当前房间";
      row.querySelector(".occupant-kick").addEventListener("click", async () => {
        if (!window.confirm(`确定将“${occupant.displayName || occupant.username}”移出房间吗？账号不会被删除。`)) return;
        state.roomKickRequests.add(requestKey);
        renderOccupants();
        try {
          await api(`/api/admin/rooms/${encodeURIComponent(state.occupantsRoomId)}/occupants/${occupant.userId}/kick`, { method: "POST", body: JSON.stringify({ reason: "房间管理员移出房间" }) });
          toast("已发送移出请求");
          await loadOccupants();
        } catch (error) { toast(error.message, true); }
        finally { state.roomKickRequests.delete(requestKey); renderOccupants(); }
      });
      list.appendChild(row);
    }
  }
  $("occupantsUpdated").textContent = `共 ${state.occupants.length} 人 · ${new Date().toLocaleTimeString("zh-CN", { hour12: false })} 更新`;
}

async function loadOccupants() {
  if (!state.occupantsRoomId) return;
  const data = await api(`/api/admin/rooms/${encodeURIComponent(state.occupantsRoomId)}/occupants`);
  state.occupants = data.occupants || [];
  renderOccupants();
}

function closeOccupants() {
  closeModal($("occupantsModal"));
  window.clearInterval(state.occupantsTimer);
  state.occupantsTimer = null;
  state.occupantsRoomId = "";
}

function openOccupants(room) {
  state.occupantsRoomId = room.roomId;
  state.occupants = [];
  $("occupantsModalTitle").textContent = room.name;
  openModal($("occupantsModal"), $("occupantsClose"));
  renderOccupants();
  loadOccupants().catch((error) => toast(error.message, true));
  window.clearInterval(state.occupantsTimer);
  state.occupantsTimer = window.setInterval(() => loadOccupants().catch(() => {}), 3000);
}

function openRoomModal(room = null) {
  clearToast();
  $("roomForm").reset();
  $("roomModalTitle").textContent = room ? "编辑房间" : "创建房间";
  $("roomId").value = room ? room.roomId : "";
  $("roomKey").value = room ? room.roomId : "";
  $("roomKey").disabled = !!room;
  $("roomName").value = room ? room.name : "";
  $("roomDescription").value = room ? room.description : "";
  $("roomScenario").value = room ? room.scenarioId : "default";
  $("roomParameters").value = JSON.stringify(room ? room.seatParameters : {}, null, 2);
  $("roomEnabled").checked = room ? room.enabled : true;
  renderSeatEditors(room);
  openModal($("roomModal"), $("roomName"));
}

function closeRoomModal() {
  closeModal($("roomModal"));
  state.roomSaving = false;
  const save = $("roomSave");
  if (save) save.disabled = false;
}

async function loadRooms() {
  const data = await api("/api/admin/rooms");
  const rooms = data.rooms || [];
  for (const room of rooms) {
    const local = state.roomOperations[room.roomId];
    if (local && local.state === "pending") {
      const remote = room.operation;
      if (remote && (remote.state === "acknowledged" || remote.state === "failed")) {
        delete state.roomOperations[room.roomId];
      }
    }
  }
  const changed = !state.roomsLoaded || JSON.stringify(rooms) !== JSON.stringify(state.rooms);
  state.rooms = rooms;
  state.roomsLoaded = true;
  if (changed) renderRooms();
}

function startRoomRefresh() {
  window.clearInterval(state.roomTimer);
  state.roomTimer = window.setInterval(() => {
    if (state.roomRefreshInFlight || document.hidden) return;
    state.roomRefreshInFlight = true;
    loadRooms().catch(() => {}).finally(() => { state.roomRefreshInFlight = false; });
  }, 2000);
}

function monitorStateLabel(value) {
  return value === "healthy" ? "正常" : value === "unknown" ? "未连接" : "异常";
}

function renderServiceBadge(accountState, gameState) {
  const node = $("serverState");
  const text = $("serverStateText");
  if (!node || !text) return;
  const healthy = accountState === "healthy" && gameState === "healthy";
  const degraded = accountState === "healthy" || gameState === "healthy";
  node.classList.toggle("degraded", !healthy && degraded);
  node.classList.toggle("offline", !healthy && !degraded);
  text.textContent = healthy ? "账号与推演服务正常" : degraded ? "部分服务需要检查" : "服务连接异常";
}

function renderTerminal() {
  const unlocked = state.terminalUnlocked;
  $("terminalState").textContent = unlocked ? "已连接" : "未登录";
  $("terminalState").classList.toggle("ready", unlocked);
  $("terminalLoginForm").classList.toggle("hidden", unlocked);
  $("terminalCommandForm").classList.toggle("locked", !unlocked);
  $("terminalCommand").disabled = !unlocked;
  $("terminalExecute").disabled = !unlocked;
  $("terminalCommand").placeholder = unlocked ? "输入 Shell 命令，按 Enter 执行" : "完成终端认证后可输入 Shell 命令";
  $("terminalDisconnect").classList.toggle("hidden", !unlocked);
  if (!unlocked) {
    $("terminalOutput").textContent = "$ 等待认证";
  }
}

function appendTerminalOutput(text) {
  const output = $("terminalOutput");
  output.textContent += text;
  if (output.textContent.length > 120000) output.textContent = output.textContent.slice(-90000);
  output.scrollTop = output.scrollHeight;
}

function closeTerminal(message = "") {
  if (state.terminalSocket) {
    state.terminalSocket.onclose = null;
    state.terminalSocket.close();
    state.terminalSocket = null;
  }
  state.terminalUnlocked = false;
  renderTerminal();
  if (message) appendTerminalOutput(`\n${message}\n`);
}

function openTerminal(ticket) {
  const scheme = window.location.protocol === "https:" ? "wss" : "ws";
  const socket = new WebSocket(`${scheme}://${window.location.host}/api/admin/monitor/terminal/ws`, ["wargame-terminal", ticket]);
  state.terminalSocket = socket;
  $("terminalOutput").textContent = "$ 正在连接 Shell...\n";
  socket.addEventListener("open", () => {
    state.terminalUnlocked = true;
    renderTerminal();
    $("terminalCommand").focus();
  });
  socket.addEventListener("message", (event) => appendTerminalOutput(event.data));
  socket.addEventListener("error", () => toast("终端连接失败", true));
  socket.addEventListener("close", () => {
    if (state.terminalSocket !== socket) return;
    state.terminalSocket = null;
    state.terminalUnlocked = false;
    renderTerminal();
    appendTerminalOutput("\n$ Shell 会话已断开\n");
  });
}

function renderMonitor() {
  const overview = state.overview || {};
  const game = overview.gameStatus || {};
  const room = game.roomState || {};
  const metrics = game.metrics || {};
  $("accountServiceState").textContent = monitorStateLabel(overview.accountStatus);
  $("gameServiceState").textContent = monitorStateLabel(game.status);
  renderServiceBadge(overview.accountStatus, game.status);
  $("gameClientCount").textContent = game.connectedClients ?? 0;
  $("playerSessionCount").textContent = overview.activePlayerSessions ?? 0;
  $("resyncRequestCount").textContent = metrics.resyncRequests ?? 0;
  const uptime = Number(metrics.uptimeSeconds || 0);
  $("gameUptime").textContent = uptime > 0 ? `${Math.floor(uptime / 3600)}h ${Math.floor(uptime / 60) % 60}m` : "--";
  $("monitorUpdated").textContent = game.updatedAt ? `最近更新：${new Date(game.updatedAt).toLocaleString("zh-CN", { hour12: false })}` : "尚未收到兵棋服务状态，请检查服务是否已启动";
  $("matchPhase").textContent = room.phase === "preparing" ? "准备阶段" : room.phase === "running" ? "推演进行中" : room.phase === "finished" ? "推演结束" : "等待状态";
  $("simTime").textContent = `${Number(room.simTime || 0).toFixed(1)} s`;
  $("scenarioRevision").textContent = room.scenarioRevision ?? "--";
  $("redReady").textContent = room.redReady ? "已就绪" : "未就绪";
  $("blueReady").textContent = room.blueReady ? "已就绪" : "未就绪";
  const list = $("monitorEvents");
  list.replaceChildren();
  for (const event of state.events) {
    const detail = event.detail || {};
    const row = document.createElement("div");
    row.className = "event-row";
    const time = document.createElement("span"); time.className = "event-time"; setTextWithTitle(time, event.time ? new Date(event.time).toLocaleString("zh-CN", { hour12: false }) : "--");
    const category = document.createElement("span"); category.className = `event-category ${event.category || ""}`; setTextWithTitle(category, event.category || "事件");
    const type = document.createElement("span"); type.className = "event-type"; setTextWithTitle(type, detail.type || detail.event || "--");
    const user = document.createElement("span"); user.className = "event-user"; setTextWithTitle(user, detail.user || detail.peer || "系统");
    const summary = document.createElement("span"); summary.className = "event-summary"; setTextWithTitle(summary, detail.summary || detail.displayName || "--");
    row.append(time, category, type, user, summary);
    list.appendChild(row);
  }
  if (!state.events.length) list.innerHTML = '<div class="empty">暂无符合条件的服务器事件。</div>';
  $("eventCount").textContent = `${state.events.length} 条`;
  renderTerminal();
}

async function loadMonitor(showError = false) {
  try {
    const category = $("monitorFilter").value;
    const [overview, events] = await Promise.all([api("/api/admin/monitor/overview"), api(`/api/admin/monitor/events?category=${encodeURIComponent(category)}&limit=120`)]);
    state.overview = overview;
    state.events = events.events || [];
    renderMonitor();
  } catch (error) {
    if (showError) toast(error.message, true);
  }
}

function startMonitorRefresh() {
  window.clearInterval(state.monitorTimer);
  loadMonitor(true);
  state.monitorTimer = window.setInterval(() => loadMonitor(false), 5000);
}

function openUserModal(user = null) {
  $("userForm").reset();
  $("userId").value = user ? user.id : "";
  $("modalTitle").textContent = user ? "编辑账号" : "创建账号";
  $("username").value = user ? user.username : "";
  $("displayName").value = user ? user.displayName : "";
  $("enabled").checked = user ? user.enabled : true;
  $("userPassword").required = false;
  $("passwordHint").textContent = user ? "留空表示不修改密码" : "";
  openModal($("userModal"), $("username"));
}

function closeUserModal() { closeModal($("userModal")); }

function openDelete(user) {
  state.deleteId = user.id;
  $("confirmText").textContent = `确定删除账号“${user.username}”吗？该账号现有登录会话将立即失效。`;
  openModal($("confirmModal"), $("confirmCancel"));
}

document.addEventListener("DOMContentLoaded", async () => {
  applyUiScale();
  $("scaleDown")?.addEventListener("click", () => { state.uiScale = Math.max(0.85, +(state.uiScale - 0.05).toFixed(2)); applyUiScale(); });
  $("scaleUp")?.addEventListener("click", () => { state.uiScale = Math.min(1.15, +(state.uiScale + 0.05).toFixed(2)); applyUiScale(); });
  $("scaleReset")?.addEventListener("click", () => { state.uiScale = 1; applyUiScale(); });
  document.addEventListener("keydown", handleModalKeydown);
  $("loginForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    $("loginError").textContent = "";
    try {
      const data = await api("/api/admin/login", { method: "POST", body: JSON.stringify({ username: $("adminUsername").value.trim(), password: $("adminPassword").value }) });
      state.token = data.token;
      sessionStorage.setItem("adminToken", state.token);
      showAdmin(data.username);
      await Promise.all([loadUsers(), loadRooms()]);
      startRoomRefresh();
      startMonitorRefresh();
    } catch (error) { $("loginError").textContent = error.message; }
  });

  $("logoutButton").addEventListener("click", () => logout(true));
  $("createButton").addEventListener("click", () => openUserModal());
  $("roomRefresh")?.addEventListener("click", () => loadRooms().catch((error) => toast(error.message, true)));
  $("roomCreate")?.addEventListener("click", () => openRoomModal());
  $("occupantsClose")?.addEventListener("click", closeOccupants);
  $("occupantsDone")?.addEventListener("click", closeOccupants);
  $("occupantsRefresh")?.addEventListener("click", () => loadOccupants().catch((error) => toast(error.message, true)));
  $("occupantsModal")?.addEventListener("click", (event) => { if (event.target === $("occupantsModal")) closeOccupants(); });
  $("roomModalClose")?.addEventListener("click", closeRoomModal);
  $("roomCancel")?.addEventListener("click", closeRoomModal);
  $("roomModal")?.addEventListener("click", (event) => { if (event.target === $("roomModal")) closeRoomModal(); });
  $("roomStatusFilter")?.addEventListener("change", (event) => { state.roomStatusFilter = event.target.value; renderRooms(); });
  $("roomForm")?.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (state.roomSaving) return;
    state.roomSaving = true;
    $("roomSave").disabled = true;
    try {
      const seatLimits = collectSeatLimits();
      const seatParameters = collectSeatParameters();
      const roomId = $("roomId").value || $("roomKey").value.trim();
      const body = { room_id: roomId, name: $("roomName").value.trim(), description: $("roomDescription").value.trim(), scenario_id: $("roomScenario").value.trim(), seat_limits: seatLimits, seat_parameters: seatParameters, enabled: $("roomEnabled").checked };
      await api(roomId && $("roomId").value ? `/api/admin/rooms/${encodeURIComponent(roomId)}` : "/api/admin/rooms", { method: roomId && $("roomId").value ? "PUT" : "POST", body: JSON.stringify(body) });
      closeRoomModal(); await loadRooms(); toast("房间已保存");
    } catch (error) { toast(error.message || "房间配置 JSON 无效", true); state.roomSaving = false; $("roomSave").disabled = false; }
  });
  $("modalClose").addEventListener("click", closeUserModal);
  $("modalCancel").addEventListener("click", closeUserModal);
  $("userModal").addEventListener("click", (event) => { if (event.target === $("userModal")) closeUserModal(); });
  $("userForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    const id = $("userId").value;
    const enteredPassword = $("userPassword").value;
    const body = { username: $("username").value.trim(), display_name: $("displayName").value.trim(), password: id && enteredPassword === "" ? null : enteredPassword, enabled: $("enabled").checked };
    try {
      await api(id ? `/api/admin/users/${id}` : "/api/admin/users", { method: id ? "PUT" : "POST", body: JSON.stringify(body) });
      closeUserModal();
      await loadUsers();
      toast(id ? "账号已更新" : "账号已创建");
    } catch (error) { toast(error.message, true); }
  });

  $("confirmCancel").addEventListener("click", () => closeModal($("confirmModal")));
  $("confirmDelete").addEventListener("click", async () => {
    try {
      await api(`/api/admin/users/${state.deleteId}`, { method: "DELETE" });
      closeModal($("confirmModal"));
      await loadUsers();
      toast("账号已删除");
    } catch (error) { toast(error.message, true); }
  });

  document.querySelectorAll(".nav-item").forEach((button) => button.addEventListener("click", () => {
    document.querySelectorAll(".nav-item").forEach((item) => item.classList.toggle("active", item === button));
    const page = button.dataset.page;
    $("usersPage").classList.toggle("hidden", page !== "users");
    $("roomsPage").classList.toggle("hidden", page !== "rooms");
    $("monitorPage").classList.toggle("hidden", page !== "monitor");
    $("securityPage").classList.toggle("hidden", page !== "security");
    $("pageTitle").textContent = page === "users" ? "兵棋账号" : page === "rooms" ? "推演房间" : page === "monitor" ? "服务器监控" : "安全设置";
    if (page === "rooms") loadRooms().catch((error) => toast(error.message, true));
    if (page === "monitor") loadMonitor(true);
  }));

  $("monitorRefresh").addEventListener("click", () => loadMonitor(true));
  $("monitorFilter").addEventListener("change", () => loadMonitor(true));
  $("terminalLoginForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    try {
      const data = await api("/api/admin/monitor/terminal/login", { method: "POST", body: JSON.stringify({ password: $("terminalPassword").value }) });
      $("terminalPassword").value = "";
      if (data.authenticated !== true || !data.terminalTicket) throw new Error("终端授权失败");
      openTerminal(data.terminalTicket);
      toast(data.message);
    } catch (error) { toast(error.message, true); }
  });

  $("terminalCommandForm").addEventListener("submit", (event) => {
    event.preventDefault();
    const command = $("terminalCommand").value;
    if (!command) return;
    if (!state.terminalSocket || state.terminalSocket.readyState !== WebSocket.OPEN) {
      toast("Shell 未连接", true);
      return;
    }
    state.terminalSocket.send(`${command}\n`);
    $("terminalCommand").value = "";
  });
  $("terminalDisconnect").addEventListener("click", () => closeTerminal("$ 已断开 Shell 会话"));

  $("passwordForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    if ($("newPassword").value !== $("confirmPassword").value) { toast("两次输入的新密码不一致", true); return; }
    try {
      const data = await api("/api/admin/password", { method: "PUT", body: JSON.stringify({ current_password: $("currentPassword").value, new_password: $("newPassword").value }) });
      toast(data.message);
      window.setTimeout(() => logout(false), 900);
    } catch (error) { toast(error.message, true); }
  });

  if (state.token) {
    try {
      const me = await api("/api/admin/me");
      showAdmin(me.username);
      await Promise.all([loadUsers(), loadRooms()]);
      startRoomRefresh();
      startMonitorRefresh();
    } catch (_) { logout(false); }
  }
});
