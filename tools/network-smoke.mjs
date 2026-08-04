#!/usr/bin/env node

/*
 * 联网模式冒烟验证：账号单客户端、房间生命周期、指挥官优先、容量展开、
 * 战位视野与通信边界、就绪开局、战位命令以及管理员移出房间。脚本只创建临时账号。
 */

const accountUrl = (process.env.ACCOUNT_URL || "http://127.0.0.1:8080").replace(/\/$/, "");
const adminUsername = process.env.ADMIN_USERNAME || "admin";
const adminPassword = process.env.ADMIN_PASSWORD;
if (!adminPassword) throw new Error("请设置 ADMIN_PASSWORD 后运行联网冒烟验证");

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

function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function waitForRoomOperation(token, operationId, expectedStatus) {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const response = await request("/api/admin/rooms", {}, token);
    const room = response.rooms.find(item => item.roomId === "main");
    if (room?.status === expectedStatus && room.operation?.operationId === operationId
        && room.operation.state === "acknowledged") return room;
    await delay(250);
  }
  throw new Error(`房间操作 ${operationId} 未在期限内完成: ${expectedStatus}`);
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
        if (message.payload.mapMarks) this.state.mapMarks = message.payload.mapMarks;
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
      protocolVersion: 3,
      schemaVersion: 2,
      type,
      messageId,
      payload,
    }));
  }

  waitFor(predicate, timeoutMs = 7000) {
    return this.waitForAfter(0, predicate, timeoutMs);
  }

  waitForAfter(startIndex, predicate, timeoutMs = 7000) {
    const findMessage = () => this.messages.slice(startIndex).find(predicate);
    const existing = findMessage();
    if (existing) return Promise.resolve(existing);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        clearInterval(interval);
        const errors = this.messages.filter(message => message.type === "error").map(message => message.payload?.code || "error");
        const events = this.messages.filter(message => message.type === "event").map(message => message.payload);
        const counts = {};
        for (const message of this.messages) counts[message.type] = (counts[message.type] || 0) + 1;
        const messageCounts = Object.entries(counts).map(([type, count]) => `${type}:${count}`).join(",");
        const commandResults = this.messages.filter(message => message.type === "commandResult")
          .map(message => `${message.payload?.commandId}:${message.payload?.accepted ? "accepted" : message.payload?.code || "rejected"}`);
        const units = this.state?.units?.map(unit => `${unit.id}:${unit.alive}`).join(",") || "none";
        reject(new Error(`${this.name}: 等待消息超时；messages=${messageCounts}; commandResults=${commandResults.join(",")}; errors=${errors.join(",")}; events=${JSON.stringify(events.slice(-12))}; units=${units}`));
      }, timeoutMs);
      const interval = setInterval(() => {
        const message = findMessage();
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
  ["redCommander", "红方指挥官"],
  ["blueCommander", "蓝方指挥官"],
  ["redAttack", "红方攻击机"],
  ["blueAttack", "蓝方攻击机"],
].map(([key, displayName]) => ({
  key,
  username: `${suffix}-${key}`,
  display_name: displayName,
  password,
  enabled: true,
}));

let adminToken = "";
const createdUsers = [];
const sessions = [];
let roomWasOpened = false;

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

  const openRequest = await request("/api/admin/rooms/main/open", { method: "POST" }, adminToken);
  await waitForRoomOperation(adminToken, openRequest.operation.operationId, "preparing");
  roomWasOpened = true;
  const resetRequest = await request("/api/admin/rooms/main/reset", { method: "POST" }, adminToken);
  await waitForRoomOperation(adminToken, resetRequest.operation.operationId, "preparing");

  const logins = {};
  for (const account of accounts) {
    logins[account.key] = await request("/api/client/login", {
      method: "POST",
      body: JSON.stringify({ username: account.username, password }),
    });
  }
  const duplicate = await fetch(`${accountUrl}/api/client/login`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ username: accounts[0].username, password }),
  });
  assert(duplicate.status === 409 && (await duplicate.json()).detail === "USER_ALREADY_ONLINE",
    "重复登录必须返回 USER_ALREADY_ONLINE");

  const byKey = {};
  for (const account of accounts) {
    const session = new GameSession(account.key, logins[account.key].gameWebSocketUrl, logins[account.key].token);
    await session.connect();
    sessions.push(session);
    byKey[account.key] = session;
    session.send("joinRoom", { roomId: "main" });
    await session.waitFor(message => message.type === "seatState");
    assert(!session.messages.some(message => message.type === "event"
      && message.payload?.kind === "roomClosed"),
    "成功进入房间后不能收到 roomClosed 事件");
  }

  byKey.redCommander.send("claimSeat", { seatId: "red_commander" });
  await byKey.redCommander.waitFor(message => message.type === "seatState" && message.payload.yourSeatId === "red_commander");
  // Commander allocation is intentionally red-first; wait for the authoritative
  // red claim before asking the blue client to claim its commander seat.
  byKey.blueCommander.send("claimSeat", { seatId: "blue_commander" });
  await byKey.blueCommander.waitFor(message => message.type === "seatState" && message.payload.yourSeatId === "blue_commander");
  await delay(3500);
  const redCommanderSeatStates = byKey.redCommander.messages.filter(message => message.type === "seatState");
  const blueCommanderSeatStates = byKey.blueCommander.messages.filter(message => message.type === "seatState");
  assert(redCommanderSeatStates.at(-1)?.payload.yourSeatId === "red_commander",
    "红方指挥官必须在房间同步后保持战位");
  assert(blueCommanderSeatStates.at(-1)?.payload.yourSeatId === "blue_commander",
    "蓝方指挥官必须在房间同步后保持战位");
  assert(!(byKey.redCommander.state?.units || []).some(unit => unit.side === "blue"),
    "初始部署前红方视角不能显示未部署的蓝方单位");

  for (const seat of ["red_commander", "blue_commander"]) {
    const commander = seat === "red_commander" ? byKey.redCommander : byKey.blueCommander;
    commander.send("deployment", {
      unitId: byKey.redCommander.messages.findLast(message => message.type === "seatState")
        .payload.seats.find(item => item.seatId === seat).unitId,
      targetSeatId: seat,
      position: { x: seat === "red_commander" ? 1000 : 18000, y: 1200, alt: 0 },
    });
  }
  await byKey.redCommander.waitFor(message => message.type === "state"
    && message.payload.units.some(unit => unit.side === "red"));
  await byKey.blueCommander.waitFor(message => message.type === "state"
    && message.payload.units.some(unit => unit.side === "blue"));

  byKey.redAttack.send("claimSeat", { seatId: "red_attack_1" });
  byKey.blueAttack.send("claimSeat", { seatId: "blue_attack_1" });
  await byKey.redAttack.waitFor(message => message.type === "seatState" && message.payload.yourSeatId === "red_attack_1");
  await byKey.blueAttack.waitFor(message => message.type === "seatState" && message.payload.yourSeatId === "blue_attack_1");
  await byKey.redAttack.waitFor(message => message.type === "deploymentPrompt"
    && message.payload.message.includes("等待红方指挥官"));
  await byKey.blueAttack.waitFor(message => message.type === "deploymentPrompt"
    && message.payload.message.includes("等待蓝方指挥官"));
  await delay(3500);
  assert(byKey.redAttack.messages.filter(message => message.type === "seatState").at(-1)
    ?.payload.yourSeatId === "red_attack_1",
  "指挥所部署后加入的红方战位不能被同步误释放");
  assert(byKey.blueAttack.messages.filter(message => message.type === "seatState").at(-1)
    ?.payload.yourSeatId === "blue_attack_1",
  "指挥所部署后加入的蓝方战位不能被同步误释放");
  const seats = byKey.redAttack.messages.findLast(message => message.type === "seatState").payload.seats;
  assert(seats.some(seat => seat.seatId === "red_attack_2"), "房间容量必须展开为第二个攻击机战位");

  const occupiedSeats = seats.filter(seat => seat.occupied && seat.unitId);
  assert(occupiedSeats.length === 4, `四个已占用战位都应拥有单位，实际 ${occupiedSeats.length}`);
  for (const seat of occupiedSeats) {
    const commander = seat.side === "red" ? byKey.redCommander : byKey.blueCommander;
    commander.send("deployment", {
      unitId: seat.unitId,
      targetSeatId: seat.seatId,
      position: { x: seat.side === "red" ? 1000 : (seat.seatType === "attack" ? 2500 : 18000), y: seat.seatType === "commander" ? 1200 : 2600, alt: 0 },
    });
  }
  await byKey.redCommander.waitFor(message => message.type === "state" &&
    message.payload.units.some(unit => unit.alive && unit.side === "red"));
  await byKey.redAttack.waitFor(message => message.type === "state"
    && message.payload.units.some(unit => unit.alive && unit.side === "red" && unit.kind === "attackuav"));

  // Exercise the full authoritative seat-transfer lifecycle. The completed
  // transfer must replace the old unit projection, so later assertions use
  // the new seat id rather than assuming the original slot survived.
  const redAttackUser = createdUsers.find(user => user.username === accounts.find(account => account.key === "redAttack").username);
  const originalRedAttackUnitId = byKey.redAttack.state.units.find(unit => unit.alive && unit.side === "red" && unit.kind === "attackuav")?.id;
  assert(originalRedAttackUnitId, "转位前必须能看到红方攻击机");
  let lastTransferRevision = 0;
  const transferRequest = (seatId) => {
    byKey.redAttack.send("claimSeat", { seatId });
    return byKey.redAttack.waitFor(message => (message.type === "event"
      && message.payload?.kind === "transferRequested"
      && message.payload?.targetSeatId === seatId
      && message.payload?.revision > lastTransferRevision)
      || message.type === "error").then(message => {
        if (message.type === "error") throw new Error(`转位请求 ${seatId} 被拒绝: ${JSON.stringify(message.payload)}`);
        assert(message.payload.targetSeatId === seatId,
          `转位目标异常: ${JSON.stringify(message.payload)}`);
        lastTransferRevision = message.payload.revision;
        return message;
      });
  };
  const cancelledTransfer = await transferRequest("red_attack_2");
  byKey.redAttack.send("claimSeat", {
    seatId: "red_attack_2",
    cancelTransfer: true,
    requestedRevision: cancelledTransfer.payload.requestRevision,
  });
  await byKey.redAttack.waitFor(message => message.type === "event"
    && message.payload?.kind === "transferRejected"
    && message.payload?.reason === "REQUESTER_CANCELLED");
  const rejectedTransfer = await transferRequest("red_attack_2");
  byKey.redCommander.send("claimSeat", {
    seatId: "red_commander",
    rejectUserId: redAttackUser.id,
    requestedRevision: rejectedTransfer.payload.requestRevision,
  });
  await byKey.redAttack.waitFor(message => message.type === "event"
    && message.payload?.kind === "transferRejected"
    && message.payload?.reason === "COMMANDER_REJECTED");
  const approvedTransfer = await transferRequest("red_attack_2");
  byKey.redCommander.send("claimSeat", {
    seatId: "red_commander",
    approveUserId: redAttackUser.id,
    requestedRevision: approvedTransfer.payload.requestRevision,
  });
  await byKey.redAttack.waitFor(message => message.type === "event"
    && message.payload?.kind === "transferCompleted"
    && message.payload?.targetSeatId === "red_attack_2");
  await byKey.redAttack.waitFor(message => message.type === "seatState"
    && message.payload?.yourSeatId === "red_attack_2");
  const redParticipantSeat = "red_attack_2";
  await byKey.redAttack.waitFor(message => message.type === "state"
    && !message.payload.units.some(unit => unit.id === originalRedAttackUnitId));
  assert(!byKey.redAttack.state.units.some(unit => unit.id === originalRedAttackUnitId),
    "战位切换完成后旧单位必须从客户端画布状态删除");
  const transferredSeat = byKey.redCommander.messages.findLast(message => message.type === "seatState")
    .payload.seats.find(seat => seat.seatId === redParticipantSeat);
  byKey.redCommander.send("deployment", {
    unitId: transferredSeat.unitId,
    targetSeatId: redParticipantSeat,
    position: { x: 1000, y: 2600, alt: 0 },
  });
  await byKey.redCommander.waitFor(message => message.type === "state"
    && message.payload.roomState.seats.some(seat => seat.seatId === redParticipantSeat
      && seat.deployed)
    && message.payload.units.some(unit => unit.id === transferredSeat.unitId));

  byKey.redCommander.send("redeploy", { seatId: redParticipantSeat });
  await byKey.redCommander.waitFor(message => message.type === "state"
    && message.payload.roomState.seats.some(seat => seat.seatId === redParticipantSeat
      && !seat.deployed)
    && message.payload.roomState.seats.some(seat => seat.seatId === "red_commander"
      && seat.deployed));
  const redeployMessageIndex = byKey.redCommander.messages.length;
  byKey.redCommander.send("deployment", {
    unitId: transferredSeat.unitId,
    targetSeatId: redParticipantSeat,
    position: { x: 1000, y: 2600, alt: 0 },
  });
  await byKey.redCommander.waitForAfter(redeployMessageIndex, message => message.type === "state"
    && message.payload.roomState.seats.some(seat => seat.seatId === redParticipantSeat
      && seat.deployed)
    && message.payload.units.some(unit => unit.id === transferredSeat.unitId));

  byKey.redCommander.send("chat", { text: "战位通信验证", recipientSeatIds: [redParticipantSeat] });
  await byKey.redAttack.waitFor(message => message.type === "chat" && message.payload.text === "战位通信验证");
  await byKey.blueAttack.waitFor(message => message.type === "error" || message.type === "state");
  assert(!byKey.blueAttack.messages.some(message => message.type === "chat" && message.payload.text === "战位通信验证"),
    "定向聊天不能泄漏给另一阵营");

  byKey.redAttack.send("seatReady", { ready: true });
  byKey.blueAttack.send("seatReady", { ready: true });
  await byKey.redCommander.waitFor(message => message.type === "state"
    && message.payload.roomState.seats.some(seat => seat.seatId === redParticipantSeat && seat.ready)
    && message.payload.roomState.seats.some(seat => seat.seatId === "blue_attack_1" && seat.ready));
  byKey.redCommander.send("seatReady", { ready: true });
  byKey.blueCommander.send("seatReady", { ready: true });
  await byKey.redCommander.waitFor(message => message.type === "state" && message.payload.roomState.redReady);
  await byKey.blueCommander.waitFor(message => message.type === "state" && message.payload.roomState.blueReady);
  await byKey.redCommander.waitFor(message => message.type === "state"
    && message.payload.roomState.readyForStart);

  await request("/api/admin/rooms/main/start", { method: "POST" }, adminToken);
  await byKey.redAttack.waitFor(message => message.type === "state" && message.payload.roomState.phase === "running", 10000);
  await byKey.redAttack.waitFor(message => message.type === "state"
    && message.payload.roomState.phase === "running"
    && message.payload.units.some(unit => unit.side === "blue" && unit.alive && unit.kind === "attackuav"), 10000);
  const commandTarget = byKey.redAttack.state.units.find(unit => unit.side === "red" && unit.alive && unit.kind === "attackuav");
  const target = byKey.redAttack.state.units.find(unit => unit.side === "blue" && unit.alive && unit.kind === "attackuav");
  assert(commandTarget, "开局后必须能找到红方攻击机");
  assert(target, "红方攻击机必须看到可授权的蓝方攻击目标");
  const sendCommanderCommand = async (action, args, label) => {
    const commandId = crypto.randomUUID();
    byKey.redCommander.send("command", {
      commandId,
      action,
      stateRevision: byKey.redCommander.state.stateRevision,
      args,
    });
    const result = await byKey.redCommander.waitFor(message => message.type === "commandResult"
      && message.payload.commandId === commandId);
    assert(result.payload.accepted, `${label} 命令必须被权威服务器接受: ${result.payload.code}`);
    return result;
  };
  byKey.redCommander.send("command", {
    commandId: crypto.randomUUID(),
    action: "withdraw",
    stateRevision: byKey.redCommander.state.stateRevision,
    args: { unitId: commandTarget.id, pos: { x: 1400 } },
  });
  const pointFreeWithdrawal = await byKey.redCommander.waitFor(message => message.type === "error"
    && message.payload.code === "INVALID_PAYLOAD");
  assert(pointFreeWithdrawal.payload.message, "坐标不完整的撤离必须被协议层拒绝");
  await sendCommanderCommand("unitOrder", { unitId: commandTarget.id, text: "保持编队并报告状态" }, "文本");
  await byKey.redAttack.waitFor(message => message.type === "state"
    && message.payload.messages.some(item => item.type === "UnitOrder"
      && item.receiver === commandTarget.id
      && item.payload?.text === "保持编队并报告状态"));
  const pointOrders = [
    ["attackAt", { x: 1600, y: 2900 }, "攻击"],
    ["moveTo", { x: 1500, y: 2800 }, "机动"],
    ["withdraw", { x: 1400, y: 2700 }, "撤离"],
  ];
  for (const [action, pos, label] of pointOrders) {
    await sendCommanderCommand(action, { unitId: commandTarget.id, pos }, label);
    await byKey.redAttack.waitFor(message => message.type === "state"
      && message.payload.messages.some(item => item.receiver === commandTarget.id
        && Number(item.payload?.x) === pos.x && Number(item.payload?.y) === pos.y));
  }
  const ownAttack = byKey.redAttack.state.units.find(unit => unit.side === "red" && unit.kind === "attackuav" && unit.alive);
  assert(ownAttack, `攻击战位必须看到自己的攻击机，当前单位 ${byKey.redAttack.state.units.map(unit => `${unit.id}:${unit.kind}:${unit.side}:${unit.alive}`).join(",")}`);
  const commandId = crypto.randomUUID();
  byKey.redAttack.send("command", {
    commandId,
    action: "setSpeed",
    stateRevision: byKey.redAttack.state.stateRevision,
    args: { unitId: ownAttack.id, speed: 12 },
  });
  const commandResult = await byKey.redAttack.waitFor(message => message.type === "commandResult" && message.payload.commandId === commandId);
  assert(commandResult.payload.accepted, "攻击战位应能控制自己的攻击机");
  byKey.redAttack.send("mapMark", {
    position: { x: 2200, y: 3000 },
    label: "下属标记一",
    recipientSeatIds: ["red_commander"],
  });
  await byKey.redCommander.waitFor(message => message.type === "event"
    && message.payload.kind === "mapMark" && message.payload.label === "下属标记一");
  byKey.redAttack.send("mapMark", {
    position: { x: 2300, y: 3100 },
    label: "下属标记二",
    recipientSeatIds: ["red_commander"],
  });
  await byKey.redAttack.waitFor(message => message.type === "state"
    && (message.payload.mapMarks || []).filter(mark => mark.markType === "self"
      && mark.seatId === redParticipantSeat).length === 1
    && (message.payload.mapMarks || []).some(mark => mark.label === "下属标记二"));
  byKey.redCommander.send("mapMark", {
    position: { x: 2400, y: 3200 },
    label: "联网冒烟标记",
    recipientSeatIds: [redParticipantSeat],
  });
  await byKey.redAttack.waitFor(message => message.type === "event"
    && message.payload.kind === "mapMark" && message.payload.label === "联网冒烟标记");
  await byKey.redAttack.waitFor(message => message.type === "state"
    && (message.payload.mapMarks || []).some(mark => mark.markType === "self"
      && mark.label === "下属标记二")
    && (message.payload.mapMarks || []).some(mark => mark.markType === "commander"
      && mark.label === "联网冒烟标记"));

  byKey.redCommander.send("control", { action: "pause" });
  await byKey.redCommander.waitFor(message => message.type === "error" && message.payload.code === "PERMISSION_DENIED");
  byKey.redAttack.send("shareIntel", {
    targetId: target.id,
    recipientSeatIds: ["red_commander"],
    note: "联网冒烟情报共享",
  });
  await byKey.redCommander.waitFor(message => message.type === "intelShare"
    && message.payload.targetId === target.id
    && message.payload.note === "联网冒烟情报共享");
  byKey.redCommander.close();
  await byKey.redAttack.waitFor(message => message.type === "seatState"
    && message.payload.yourSeatId === "red_commander", 10000);
  byKey.redAttack.close();
  await byKey.blueAttack.waitFor(message => message.type === "event"
    && message.payload?.kind === "forfeit"
    && message.payload?.winner === "blue", 10000);
  const blueAttackUser = createdUsers.find(user => user.username === accounts.find(account => account.key === "blueAttack").username);
  await request(`/api/admin/rooms/main/occupants/${blueAttackUser.id}/kick`, {
    method: "POST", body: JSON.stringify({ reason: "联网冒烟验证移出" }),
  }, adminToken);
  await byKey.blueAttack.waitFor(message => message.type === "event"
    && message.payload?.kind === "roomClosed");
  assert(byKey.blueAttack.socket.readyState === WebSocket.OPEN,
    "房间内移除后客户端 WebSocket 必须保持连接");
  const roomListMessageIndex = byKey.blueAttack.messages.length;
  byKey.blueAttack.send("roomList", {});
  await byKey.blueAttack.waitForAfter(roomListMessageIndex,
    message => message.type === "roomDirectory");
  console.log("联网冒烟验证通过：账号单客户端、房间、战位扩容、部署/redeploy、转位取消/拒绝/批准、定向通信、情报共享、地图标记、就绪开局、文本/进攻/机动/撤离命令和无接替失效均正常。");
} finally {
  for (const session of sessions) session.close();
  if (roomWasOpened && adminToken) {
    try { await request("/api/admin/rooms/main/stop", { method: "POST" }, adminToken); } catch (_) { }
  }
  for (const user of createdUsers) {
    try { await request(`/api/admin/users/${user.id}`, { method: "DELETE" }, adminToken); }
    catch (error) { console.warn(`清理临时账号失败：${user.username}: ${error.message}`); }
  }
  if (adminToken) {
    try { await request("/api/admin/logout", { method: "POST" }, adminToken); } catch (_) { }
  }
}
