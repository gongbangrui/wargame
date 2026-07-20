const state = { token: sessionStorage.getItem("adminToken") || "", users: [], deleteId: null, overview: null, events: [], terminalUnlocked: false, terminalSocket: null, monitorTimer: null };
const $ = (id) => document.getElementById(id);
const roles = { director: "导演席", editor: "编辑席", red: "红方", blue: "蓝方" };

async function api(path, options = {}) {
  const headers = { "Content-Type": "application/json", ...(options.headers || {}) };
  if (state.token) headers.Authorization = `Bearer ${state.token}`;
  const response = await fetch(path, { ...options, headers });
  let data = {};
  try { data = await response.json(); } catch (_) { data = {}; }
  if (!response.ok) {
    if (response.status === 401 && path !== "/api/admin/login") logout(false);
    throw new Error(data.detail || "请求失败");
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
  state.overview = null;
  state.events = [];
  window.clearInterval(state.monitorTimer);
  sessionStorage.removeItem("adminToken");
  $("adminView").classList.add("hidden");
  $("loginView").classList.remove("hidden");
  $("adminPassword").value = "";
}

function renderUsers() {
  const body = $("userRows");
  body.replaceChildren();
  for (const user of state.users) {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td class="user-cell"></td><td class="display"></td>
      <td><span class="badge role-${user.role}">${roles[user.role]}</span></td>
      <td><span class="${user.enabled ? "enabled" : "disabled"}">${user.enabled ? "已启用" : "已停用"}</span></td>
      <td class="updated"></td>
      <td class="actions"><button class="row-button edit">编辑</button><button class="row-button delete">删除</button></td>`;
    tr.querySelector(".user-cell").textContent = user.username;
    tr.querySelector(".display").textContent = user.displayName;
    tr.querySelector(".updated").textContent = new Date(user.updatedAt).toLocaleString("zh-CN", { hour12: false });
    tr.querySelector(".edit").addEventListener("click", () => openUserModal(user));
    tr.querySelector(".delete").addEventListener("click", () => openDelete(user));
    body.appendChild(tr);
  }
  $("emptyState").classList.toggle("hidden", state.users.length !== 0);
  $("totalCount").textContent = state.users.length;
  $("enabledCount").textContent = state.users.filter((u) => u.enabled).length;
  $("sideCount").textContent = `${state.users.filter((u) => u.role === "red").length} / ${state.users.filter((u) => u.role === "blue").length}`;
  $("staffCount").textContent = `${state.users.filter((u) => u.role === "director").length} / ${state.users.filter((u) => u.role === "editor").length}`;
}

async function loadUsers() {
  const data = await api("/api/admin/users");
  state.users = data.users;
  renderUsers();
}

function monitorStateLabel(value) {
  return value === "healthy" ? "正常" : value === "unknown" ? "未连接" : "异常";
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
    $("terminalOutput").textContent = "$ 等待管理员密码认证\n$ 将进入 account-web 容器内的 wargame 用户 Shell";
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
  const socket = new WebSocket(`${scheme}://${window.location.host}/api/admin/monitor/terminal/ws?ticket=${encodeURIComponent(ticket)}`);
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
  $("accountServiceState").textContent = monitorStateLabel(overview.accountStatus);
  $("gameServiceState").textContent = monitorStateLabel(game.status);
  $("gameClientCount").textContent = game.connectedClients ?? 0;
  $("playerSessionCount").textContent = overview.activePlayerSessions ?? 0;
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
    const time = document.createElement("span"); time.className = "event-time"; time.textContent = event.time ? new Date(event.time).toLocaleString("zh-CN", { hour12: false }) : "--";
    const category = document.createElement("span"); category.className = `event-category ${event.category || ""}`; category.textContent = event.category || "事件";
    const type = document.createElement("span"); type.className = "event-type"; type.textContent = detail.type || detail.event || "--";
    const user = document.createElement("span"); user.className = "event-user"; user.textContent = detail.user || detail.peer || "系统";
    const summary = document.createElement("span"); summary.className = "event-summary"; summary.textContent = detail.summary || detail.displayName || "--";
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
  $("role").value = user ? user.role : "red";
  $("enabled").checked = user ? user.enabled : true;
  $("userPassword").required = !user;
  $("passwordHint").textContent = user ? "留空表示不修改密码" : "至少 8 个字符";
  $("userModal").classList.remove("hidden");
  $("username").focus();
}

function closeUserModal() { $("userModal").classList.add("hidden"); }

function openDelete(user) {
  state.deleteId = user.id;
  $("confirmText").textContent = `确定删除账号“${user.username}”（${roles[user.role]}）吗？该账号现有登录会话将立即失效。`;
  $("confirmModal").classList.remove("hidden");
}

document.addEventListener("DOMContentLoaded", async () => {
  $("loginForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    $("loginError").textContent = "";
    try {
      const data = await api("/api/admin/login", { method: "POST", body: JSON.stringify({ username: $("adminUsername").value.trim(), password: $("adminPassword").value }) });
      state.token = data.token;
      sessionStorage.setItem("adminToken", state.token);
      showAdmin(data.username);
      await loadUsers();
    } catch (error) { $("loginError").textContent = error.message; }
  });

  $("logoutButton").addEventListener("click", () => logout(true));
  $("createButton").addEventListener("click", () => openUserModal());
  $("modalClose").addEventListener("click", closeUserModal);
  $("modalCancel").addEventListener("click", closeUserModal);
  $("userModal").addEventListener("click", (event) => { if (event.target === $("userModal")) closeUserModal(); });
  $("userForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    const id = $("userId").value;
    const body = { username: $("username").value.trim(), display_name: $("displayName").value.trim(), role: $("role").value, password: $("userPassword").value || null, enabled: $("enabled").checked };
    try {
      await api(id ? `/api/admin/users/${id}` : "/api/admin/users", { method: id ? "PUT" : "POST", body: JSON.stringify(body) });
      closeUserModal();
      await loadUsers();
      toast(id ? "账号已更新" : "账号已创建");
    } catch (error) { toast(error.message, true); }
  });

  $("confirmCancel").addEventListener("click", () => $("confirmModal").classList.add("hidden"));
  $("confirmDelete").addEventListener("click", async () => {
    try {
      await api(`/api/admin/users/${state.deleteId}`, { method: "DELETE" });
      $("confirmModal").classList.add("hidden");
      await loadUsers();
      toast("账号已删除");
    } catch (error) { toast(error.message, true); }
  });

  document.querySelectorAll(".nav-item").forEach((button) => button.addEventListener("click", () => {
    document.querySelectorAll(".nav-item").forEach((item) => item.classList.toggle("active", item === button));
    const page = button.dataset.page;
    $("usersPage").classList.toggle("hidden", page !== "users");
    $("monitorPage").classList.toggle("hidden", page !== "monitor");
    $("securityPage").classList.toggle("hidden", page !== "security");
    $("pageTitle").textContent = page === "users" ? "兵棋账号" : page === "monitor" ? "服务器监控" : "安全设置";
    if (page === "monitor") startMonitorRefresh(); else window.clearInterval(state.monitorTimer);
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
      await loadUsers();
    } catch (_) { logout(false); }
  }
});
