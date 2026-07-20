#!/usr/bin/env node

/*
 * 可重复执行的本地联网冒烟验证。
 * 需要账号服务处于准备阶段；脚本会创建带时间戳的临时账号和单元，并在结束时清理。
 */

const accountUrl = (process.env.ACCOUNT_URL || "http://127.0.0.1:8080").replace(/\/$/, "");
const adminUsername = process.env.ADMIN_USERNAME || "admin";
const adminPassword = process.env.ADMIN_PASSWORD;

if (!adminPassword) {
  throw new Error("请设置 ADMIN_PASSWORD 后运行联网冒烟验证");
}

async function request(path, options = {}, token = "") {
  const headers = { "Content-Type": "application/json", ...(options.headers || {}) };
  if (token) headers.Authorization = `Bearer ${token}`;
  const response = await fetch(`${accountUrl}${path}`, { ...options, headers });
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    const detail = typeof body.detail === "string" ? body.detail : JSON.stringify(body.detail || body);
    throw new Error(`${path}: ${detail || response.statusText}`);
  }
  return body;
}

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

class GameSession {
  constructor(name, url, token) {
    this.name = name;
    this.url = url;
    this.token = token;
    this.messages = [];
  }

  async connect() {
    this.socket = new WebSocket(this.url);
    await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error(`${this.name}: WebSocket 连接超时`)), 5000);
      this.socket.addEventListener("open", () => { clearTimeout(timeout); resolve(); }, { once: true });
      this.socket.addEventListener("error", () => { clearTimeout(timeout); reject(new Error(`${this.name}: WebSocket 连接失败`)); }, { once: true });
    });
    this.socket.addEventListener("message", event => {
      const message = JSON.parse(event.data);
      this.messages.push(message);
      if (message.type === "snapshot") {
        this.state = structuredClone(message.payload);
        this.messages.push({ type: "state", payload: structuredClone(this.state) });
      } else if (message.type === "delta" && this.state
                 && message.payload.baseStateRevision === this.state.stateRevision) {
        const byId = new Map(this.state.units.map(unit => [unit.id, unit]));
        for (const unit of message.payload.units || []) byId.set(unit.id, unit);
        this.state.units = [...byId.values()].sort((a, b) => a.id.localeCompare(b.id));
        this.state.roomState = message.payload.roomState;
        if (message.payload.messages) this.state.messages = message.payload.messages;
        this.state.stateRevision = message.payload.stateRevision;
        this.messages.push({ type: "state", payload: structuredClone(this.state) });
      }
    });
    this.send("auth", { token: this.token });
    await this.waitFor(message => message.type === "welcome");
    return this.waitFor(message => message.type === "state");
  }

  send(type, payload) {
    this.sendWithId(crypto.randomUUID(), type, payload);
  }

  sendWithId(messageId, type, payload) {
    this.socket.send(JSON.stringify({
      protocolVersion: 2,
      schemaVersion: 1,
      type,
      messageId,
      payload,
    }));
  }

  sendCommand(commandId, action, args) {
    this.send("command", { commandId, action, args });
  }

  waitFor(predicate, timeoutMs = 5000) {
    const existing = this.messages.find(predicate);
    if (existing) return Promise.resolve(existing);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        clearInterval(interval);
        const recent = this.messages.slice(-5).map(message => ({
          type: message.type,
          code: message.payload?.code,
          phase: message.payload?.roomState?.phase,
          redReady: message.payload?.roomState?.redReady,
          blueReady: message.payload?.roomState?.blueReady,
        }));
        reject(new Error(`${this.name}: 等待服务器消息超时，最近消息：${JSON.stringify(recent)}`));
      }, timeoutMs);
      const interval = setInterval(() => {
        const message = this.messages.find(predicate);
        if (!message) return;
        clearTimeout(timer);
        clearInterval(interval);
        resolve(message);
      }, 25);
    });
  }

  close() {
    if (this.socket && this.socket.readyState <= WebSocket.OPEN) this.socket.close();
  }
}

const suffix = `smoke${Date.now().toString(36)}`;
const password = `Smoke-${suffix}-Pass`;
const accounts = [
  ["director", "导演席"],
  ["editor", "编辑席"],
  ["red", "红方"],
  ["blue", "蓝方"],
].map(([role, displayName]) => ({ username: `${suffix}-${role}`, display_name: displayName, role, password, enabled: true }));

let adminToken = "";
let createdUsers = [];
let sessions = [];
let testUnitId = "";
let initialEditorSnapshot = null;
const initialSnapshots = {};

try {
  const admin = await request("/api/admin/login", {
    method: "POST",
    body: JSON.stringify({ username: adminUsername, password: adminPassword }),
  });
  adminToken = admin.token;

  for (const account of accounts) {
    const created = await request("/api/admin/users", {
      method: "POST",
      body: JSON.stringify(account),
    }, adminToken);
    createdUsers.push(created.user);
  }

  const logins = {};
  for (const account of accounts) {
    logins[account.role] = await request("/api/client/login", {
      method: "POST",
      body: JSON.stringify({ username: account.username, password }),
    });
  }

  for (const account of accounts) {
    const login = logins[account.role];
    const session = new GameSession(account.role, login.gameWebSocketUrl, login.token);
    const snapshot = await session.connect();
    sessions.push(session);
    initialSnapshots[account.role] = snapshot;
    assert(snapshot.payload.roomState.phase === "preparing", "冒烟测试只能在准备阶段启动");
    if (account.role === "editor") initialEditorSnapshot = snapshot;
  }

  const byRole = Object.fromEntries(sessions.map(session => [session.name, session]));
  assert(initialEditorSnapshot, "未收到编辑席初始快照");
  assert(!initialEditorSnapshot.payload.roomState.redReady && !initialEditorSnapshot.payload.roomState.blueReady,
    "已有阵营提交就绪，冒烟测试不会重置其他参与者的准备状态");
  for (const side of ["red", "blue"]) {
    const commandPosts = initialEditorSnapshot.payload.scenario.units.filter(unit => unit.side === side && unit.kind === "commandpost");
    assert(commandPosts.length === 1, `${side === "red" ? "红方" : "蓝方"}指挥所数量异常，冒烟测试不会修改无效场景`);
  }
  for (const role of ["red", "blue"]) {
    const enemySide = role === "red" ? "blue" : "red";
    const observedEnemies = initialSnapshots[role].payload.units.filter(unit => unit.side === enemySide);
    for (const unit of observedEnemies) {
      assert(!("schedule" in unit) && !("recentPath" in unit)
        && !("sharedKnowledge" in unit) && !("detections" in unit)
        && !("detectRange" in unit) && !("attackRange" in unit)
        && !("commRange" in unit) && !("attackPower" in unit),
      `${role} 席位收到的敌方投影包含内部行为或能力数据`);
    }
  }

  byRole.editor.send("control", { action: "start" });
  await byRole.editor.waitFor(message => message.type === "error"
    && message.payload.code === "PERMISSION_DENIED");

  byRole.red.send("chat", { text: "x".repeat(501) });
  await byRole.red.waitFor(message => message.type === "error"
    && message.payload.code === "INVALID_PAYLOAD");

  const duplicateMessageId = crypto.randomUUID();
  byRole.red.sendWithId(duplicateMessageId, "ping", {});
  await byRole.red.waitFor(message => message.type === "pong");
  byRole.red.sendWithId(duplicateMessageId, "ping", {});
  await byRole.red.waitFor(message => message.type === "error"
    && message.payload.code === "DUPLICATE_MESSAGE");
  const unitId = `${suffix}-red-unit`;
  testUnitId = unitId;
  byRole.editor.send("scenarioUpsert", {
    unit: {
      id: unitId, callsign: "联网冒烟红方单元", kind: "groundscout", side: "red",
      x: 3200, y: 3200, alt: 0, detectRange: 3000, attackRange: 0,
      commRange: 10000, speed: 6, maxHp: 80, attackPower: 0, schedule: [],
    },
  });
  await byRole.editor.waitFor(message => message.type === "state"
    && message.payload.scenario.units.some(unit => unit.id === unitId));

  byRole.red.send("scenarioUpsert", {
    unit: {
      id: `${suffix}-blue-denied`, callsign: "越权单元", kind: "groundscout", side: "blue",
      x: 16000, y: 3200, alt: 0, detectRange: 3000, attackRange: 0,
      commRange: 10000, speed: 6, maxHp: 80, attackPower: 0, schedule: [],
    },
  });
  await byRole.red.waitFor(message => message.type === "error"
    && message.payload.code === "UNIT_NOT_OWNED");

  byRole.red.send("chat", { text: "联网冒烟聊天验证" });
  await byRole.blue.waitFor(message => message.type === "chat"
    && message.payload.text === "联网冒烟聊天验证");

  byRole.red.send("setReady", { ready: true });
  await byRole.red.waitFor(message => message.type === "event"
    && message.payload.kind === "readiness");
  byRole.blue.send("setReady", { ready: true });
  await byRole.blue.waitFor(message => message.type === "event"
    && message.payload.kind === "readiness");
  await byRole.director.waitFor(message => message.type === "state"
    && message.payload.roomState.redReady && message.payload.roomState.blueReady);

  byRole.director.send("control", { action: "start" });
  await byRole.director.waitFor(message => message.type === "state"
    && message.payload.roomState.phase === "running");

  const commandId = crypto.randomUUID();
  byRole.red.sendCommand(commandId, "setSpeed", { unitId, speed: 12 });
  const firstResult = await byRole.red.waitFor(message => message.type === "commandResult"
    && message.payload.commandId === commandId);
  assert(firstResult.payload.accepted && firstResult.payload.code === "OK",
    "首次命令应被服务器接受");

  const blueMovable = initialEditorSnapshot.payload.scenario.units.find(
    unit => unit.side === "blue" && unit.kind !== "commandpost");
  assert(blueMovable, "缺少用于权限验证的蓝方机动单元");
  const foreignCommandId = crypto.randomUUID();
  byRole.red.sendCommand(foreignCommandId, "setSpeed", { unitId: blueMovable.id, speed: 10 });
  const foreignResult = await byRole.red.waitFor(message => message.type === "commandResult"
    && message.payload.commandId === foreignCommandId);
  assert(!foreignResult.payload.accepted && foreignResult.payload.code === "UNIT_NOT_OWNED",
    "红方控制蓝方单元必须被服务端拒绝");

  const editorCommandId = crypto.randomUUID();
  byRole.editor.sendCommand(editorCommandId, "setSpeed", { unitId, speed: 10 });
  const editorResult = await byRole.editor.waitFor(message => message.type === "commandResult"
    && message.payload.commandId === editorCommandId);
  assert(!editorResult.payload.accepted && editorResult.payload.code === "PERMISSION_DENIED",
    "编辑席下达推演命令必须被服务端拒绝");

  const hiddenEnemy = initialEditorSnapshot.payload.scenario.units.find(unit =>
    unit.side === "blue" && !byRole.red.state.units.some(visible => visible.id === unit.id));
  const redAttacker = byRole.red.state.units.find(
    unit => unit.side === "red" && unit.kind === "attackuav" && unit.alive);
  if (hiddenEnemy && redAttacker) {
    const hiddenTargetCommandId = crypto.randomUUID();
    byRole.red.sendCommand(hiddenTargetCommandId, "pursue", {
      attackerId: redAttacker.id,
      targetId: hiddenEnemy.id,
    });
    const hiddenTargetResult = await byRole.red.waitFor(message =>
      message.type === "commandResult"
      && message.payload.commandId === hiddenTargetCommandId);
    assert(!hiddenTargetResult.payload.accepted
      && hiddenTargetResult.payload.code === "TARGET_NOT_VISIBLE",
    "猜测未探测敌方 ID 的攻击命令必须被服务端拒绝");
  }

  const reconnectedRed = new GameSession("red-reconnected", logins.red.gameWebSocketUrl, logins.red.token);
  await reconnectedRed.connect();
  sessions.push(reconnectedRed);
  reconnectedRed.sendCommand(commandId, "setSpeed", { unitId, speed: 999 });
  const duplicateResult = await reconnectedRed.waitFor(message => message.type === "commandResult"
    && message.payload.commandId === commandId);
  assert(duplicateResult.payload.accepted
    && duplicateResult.payload.code === firstResult.payload.code
    && duplicateResult.payload.serverTime === firstResult.payload.serverTime,
    "断线重连后的重复 commandId 必须返回原结果且不能再次执行");

  byRole.director.send("control", { action: "end" });
  await byRole.editor.waitFor(message => message.type === "state"
    && message.payload.roomState.phase === "preparing"
    && !message.payload.roomState.redReady && !message.payload.roomState.blueReady);

  byRole.editor.send("scenarioRemove", { unitId });
  await byRole.editor.waitFor(message => message.type === "state"
    && !message.payload.scenario.units.some(unit => unit.id === unitId));
  testUnitId = "";

  console.log("联网冒烟验证通过：认证、权限矩阵、视野投影、协议防护、增量同步、命令幂等、聊天、就绪、开局、重置和准备阶段编辑均正常。");
} finally {
  const editor = sessions.find(session => session.name === "editor");
  if (editor && testUnitId && editor.socket?.readyState === WebSocket.OPEN) {
    try {
      editor.send("scenarioRemove", { unitId: testUnitId });
      await editor.waitFor(message => message.type === "state"
        && !message.payload.scenario.units.some(unit => unit.id === testUnitId), 2000);
      testUnitId = "";
    } catch (_) {
      console.warn(`测试单位可能仍存在：${testUnitId}`);
    }
  }
  for (const session of sessions) session.close();
  for (const user of createdUsers) {
    try {
      await request(`/api/admin/users/${user.id}`, { method: "DELETE" }, adminToken);
    } catch (error) {
      console.warn(`清理临时账号失败：${user.username}: ${error.message}`);
    }
  }
  if (adminToken) {
    try { await request("/api/admin/logout", { method: "POST" }, adminToken); } catch (_) { }
  }
}
