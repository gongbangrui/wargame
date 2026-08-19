#!/usr/bin/env node

/*
 * VMF guided-strike network acceptance test.  The room must be configured with
 * communicationPolicy.format=vmf-design-v1 before the game server starts (the
 * server intentionally does not let a client replace that security boundary).
 * The script creates isolated accounts, encodes small dictionary-valid fixtures,
 * exercises the complete human-in-the-loop message chain, and always cleans up.
 */

import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname, resolve } from "node:path";
import { promisify } from "node:util";
import { execFile as execFileCallback } from "node:child_process";
import { fileURLToPath } from "node:url";

const execFile = promisify(execFileCallback);
const accountUrl = (process.env.ACCOUNT_URL || "http://127.0.0.1:8080").replace(/\/$/, "");
const adminUsername = process.env.ADMIN_USERNAME || "admin";
const adminPassword = process.env.ADMIN_PASSWORD;
const fixturesOnly = process.env.VMF_FIXTURES_ONLY === "1";
if (!fixturesOnly && !adminPassword) throw new Error("请设置 ADMIN_PASSWORD 后运行 VMF 引导打击验收");

const rootDir = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const profileDir = process.env.VMF_PROFILE_DIR
  ? resolve(process.env.VMF_PROFILE_DIR) : join(rootDir, "design", "EncoderDecoder");
const encoderCandidates = [
  process.env.VMF_ENCODE ? resolve(process.env.VMF_ENCODE) : "",
  join(profileDir, "build", "Release", "vmf_encode"),
  join(rootDir, "build", "debug", "vmf-tools", "vmf_encode"),
  join(rootDir, "build", "sanitizers", "vmf-tools", "vmf_encode"),
  join(rootDir, "build", "vmf-tools", "vmf_encode"),
].filter(candidate => candidate.length > 0);
const encoder = encoderCandidates.find(candidate => existsSync(candidate)) || encoderCandidates[0];

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

function delay(ms) { return new Promise(resolveDelay => setTimeout(resolveDelay, ms)); }

async function waitForRoomOperation(token, operationId, expectedStatus) {
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline) {
    const response = await request("/api/admin/rooms", {}, token);
    const room = response.rooms?.find(item => item.roomId === "main");
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
    this.state = null;
  }

  async connect() {
    this.socket = new WebSocket(this.url);
    await new Promise((resolveConnection, reject) => {
      const timeout = setTimeout(() => reject(new Error(`${this.name}: WebSocket 连接超时`)), 7000);
      this.socket.addEventListener("open", () => {
        clearTimeout(timeout); resolveConnection();
      }, { once: true });
      this.socket.addEventListener("error", () => {
        clearTimeout(timeout); reject(new Error(`${this.name}: WebSocket 连接失败`));
      }, { once: true });
    });
    this.socket.addEventListener("message", event => {
      const message = JSON.parse(event.data);
      this.messages.push(message);
      if (message.type === "snapshot") {
        this.state = structuredClone(message.payload);
        this.messages.push({ type: "state", payload: structuredClone(this.state) });
      }
    });
    this.send("auth", { token: this.token });
    await this.waitFor(message => message.type === "welcome");
    await this.waitFor(message => message.type === "state");
  }

  send(type, payload, messageId = crypto.randomUUID()) {
    this.socket.send(JSON.stringify({
      protocolVersion: 7,
      schemaVersion: 7,
      type,
      messageId,
      payload,
    }));
    return messageId;
  }

  waitFor(predicate, timeoutMs = 10000) {
    const existing = this.messages.find(predicate);
    if (existing) return Promise.resolve(existing);
    return new Promise((resolveMessage, reject) => {
      const timer = setTimeout(() => {
        clearInterval(interval);
        const errors = this.messages.filter(message => message.type === "error")
          .map(message => message.payload?.code || "error");
        reject(new Error(`${this.name}: 等待消息超时; errors=${errors.join(",")}`));
      }, timeoutMs);
      const interval = setInterval(() => {
        const found = this.messages.find(predicate);
        if (!found) return;
        clearTimeout(timer); clearInterval(interval); resolveMessage(found);
      }, 25);
    });
  }

  close() {
    if (this.socket && this.socket.readyState <= WebSocket.OPEN) this.socket.close();
  }
}

function xmlHeader() {
  return `<Header><Field name="version">1</Field><Field name="length">0</Field>`
    + `<Field name="messageId">1</Field><Field name="originator">2</Field>`
    + `<Field name="destination">3</Field></Header>`;
}

function targetReportXml() {
  return `<MessageContent message="Target Report">${xmlHeader()}<Body>`
    + `<Group name="TargetReport"><GRI DFI="4045" DUI="001">0</GRI>`
    + `<DataUnit DFI="4200" DUI="001" name="TargetType">1</DataUnit>`
    + `<DataUnit DFI="4201" DUI="001" name="TargetQuantity">1</DataUnit>`
    + `<DataUnit DFI="4202" DUI="001" name="IdentificationFriendOrFoe">0</DataUnit>`
    + `<DataUnit DFI="281" DUI="014" name="Latitude00051">2000000</DataUnit>`
    + `<DataUnit DFI="282" DUI="014" name="Longtitude00051">4000000</DataUnit>`
    + `<DataUnit DFI="4203" DUI="001" name="TargetStatus">0</DataUnit>`
    + `<Field name="Motion"><FPI DFI="4014" DUI="002">0</FPI></Field>`
    + `<Group name="ObservationTime"><GPI DFI="4014" DUI="001">1</GPI>`
    + `<DataUnit DFI="4099" DUI="001" name="Month">8</DataUnit>`
    + `<DataUnit DFI="4019" DUI="001" name="DayOfMonth">17</DataUnit>`
    + `<DataUnit DFI="792" DUI="001" name="Hour">12</DataUnit>`
    + `<DataUnit DFI="797" DUI="004" name="Minute">0</DataUnit>`
    + `<DataUnit DFI="380" DUI="417" name="DataCollectionSecond">0</DataUnit>`
    + `</Group></Group></Body></MessageContent>`;
}

function landRouteXml() {
  return `<MessageContent message="Land Route">${xmlHeader()}<Body>`
    + `<Group name="MultipleRoute"><GRI DFI="4045" DUI="001">0</GRI>`
    + `<Group name="RouteExtremeties"><GRI DFI="4045" DUI="001">0</GRI>`
    + `<DataUnit DFI="281" DUI="014" name="Latitude00051">2000000</DataUnit>`
    + `<DataUnit DFI="282" DUI="014" name="Longtitude00051">4000000</DataUnit>`
    + `</Group><Group name="ReportTime"><GPI DFI="4014" DUI="001">1</GPI>`
    + `<DataUnit DFI="4099" DUI="001" name="Month">8</DataUnit>`
    + `<DataUnit DFI="4019" DUI="001" name="DayOfMonth">17</DataUnit>`
    + `<DataUnit DFI="792" DUI="001" name="Hour">12</DataUnit>`
    + `<DataUnit DFI="797" DUI="004" name="Minute">0</DataUnit>`
    + `</Group><Group name="RouteData"><GPI DFI="4014" DUI="001">0</GPI>`
    + `</Group></Group></Body></MessageContent>`;
}

async function encodeXml(dictionary, xml, tempDirectory) {
  const input = join(tempDirectory, `${dictionary}.xml`);
  const output = join(tempDirectory, `${dictionary}.bin`);
  await writeFile(input, xml, "utf8");
  const result = await execFile(encoder, [
    join(profileDir, "msgStruct", dictionary === "target" ? "msg_target_report.xml" : "msg4_2.xml"),
    join(profileDir, "dic_content.xml"), input, output,
  ]);
  const bytes = await readFile(output);
  const match = result.stdout.match(/Encoded bits:\s*(\d+)/);
  assert(match, `${dictionary}: vmf_encode 未输出实际 bit 长度`);
  return { wireBytes: bytes.toString("base64"), wireBitLength: Number(match[1]) };
}

async function encodeFixtures(tempDirectory) {
  assert(existsSync(encoder), `找不到 vmf_encode: ${encoder}`);
  const target = await encodeXml("target", targetReportXml(), tempDirectory);
  const route = await encodeXml("route", landRouteXml(), tempDirectory);
  const networkInput = join(profileDir, "msg_pass.xml");
  const networkOutput = join(tempDirectory, "network.bin");
  const result = await execFile(encoder, [join(profileDir, "msgStruct", "msg0_1.xml"),
    join(profileDir, "dic_content.xml"), networkInput, networkOutput]);
  const networkBytes = await readFile(networkOutput);
  const match = result.stdout.match(/Encoded bits:\s*(\d+)/);
  assert(match, "network: vmf_encode 未输出实际 bit 长度");
  return { target, route,
    network: { wireBytes: networkBytes.toString("base64"), wireBitLength: Number(match[1]) } };
}

function seatById(session, seatId) {
  const stateMessage = [...session.messages].reverse().find(message => message.type === "seatState");
  return stateMessage?.payload?.seats?.find(seat => seat.seatId === seatId);
}

async function claim(session, seatId) {
  session.send("claimSeat", { seatId });
  await session.waitFor(message => message.type === "seatState"
    && message.payload?.yourSeatId === seatId);
}

async function deploy(commander, seatId, position) {
  const seat = seatById(commander, seatId);
  assert(seat?.unitId, `没有找到 ${seatId} 对应的单位`);
  commander.send("deployment", { unitId: seat.unitId, targetSeatId: seatId, position });
  await commander.waitFor(message => message.type === "state"
    && message.payload?.roomState?.seats?.some(item => item.seatId === seatId && item.deployed));
  return seat.unitId;
}

async function main() {
  const tempDirectory = await mkdtemp(join(tmpdir(), "wargame-vmf-smoke-"));
  const suffix = `vmf${Date.now().toString(36)}`;
  const password = `Smoke-${suffix}-Pass`;
  const accountSpecs = [
    ["redCommander", "red_commander", "红方指挥官"],
    ["blueCommander", "blue_commander", "蓝方指挥官"],
    ["redRecon", "red_recon_1", "红方侦察机"],
    ["redAttack", "red_attack_1", "红方攻击机"],
    ["redGround", "red_ground_1", "红方地面分队"],
    ["observer", null, "观摩人员"],
  ];
  const accounts = accountSpecs.map(([key, seatId, displayName]) => ({
    key, seatId, username: `${suffix}-${key}`, display_name: displayName,
    password, enabled: true,
  }));
  const createdUsers = [];
  const sessions = [];
  let adminToken = "";
  let roomWasOpened = false;
  try {
    const fixtures = await encodeFixtures(tempDirectory);
    if (fixturesOnly) {
      console.log(`VMF 离线 fixture 验证通过：Target Report=${fixtures.target.wireBitLength} bits, `
        + `Land Route=${fixtures.route.wireBitLength} bits, `
        + `NetworkMonitoring=${fixtures.network.wireBitLength} bits。`);
      return;
    }
    const admin = await request("/api/admin/login", {
      method: "POST", body: JSON.stringify({ username: adminUsername, password: adminPassword }),
    });
    adminToken = admin.token;
    for (const account of accounts) {
      const created = await request("/api/admin/users", {
        method: "POST", body: JSON.stringify(account),
      }, adminToken);
      createdUsers.push(created.user);
    }
    const open = await request("/api/admin/rooms/main/open", { method: "POST" }, adminToken);
    if (open.operation?.operationId) {
      await waitForRoomOperation(adminToken, open.operation.operationId, "preparing");
    }
    roomWasOpened = true;
    // The account service stores lifecycle timestamps with second precision;
    // let the open operation become the unambiguous latest row before reset.
    await delay(1100);
    const reset = await request("/api/admin/rooms/main/reset", { method: "POST" }, adminToken);
    if (reset.operation?.operationId) {
      await waitForRoomOperation(adminToken, reset.operation.operationId, "preparing");
    }
    const logins = {};
    for (const account of accounts) {
      logins[account.key] = await request("/api/client/login", {
        method: "POST", body: JSON.stringify({ username: account.username, password }),
      });
    }
    const byKey = {};
    for (const account of accounts) {
      const session = new GameSession(account.key,
        logins[account.key].gameWebSocketUrl, logins[account.key].token);
      await session.connect();
      session.send("joinRoom", { roomId: "main", ...(account.seatId ? {} : { asObserver: true }) });
      await session.waitFor(message => message.type === (account.seatId ? "seatState" : "state"));
      sessions.push(session); byKey[account.key] = session;
    }
    assert(byKey.redCommander.state?.scenario?.communicationPolicy?.format === "vmf-design-v1",
      "房间当前未启用 communicationPolicy.format=vmf-design-v1，请使用 VMF 场景启动服务器");

    await claim(byKey.redCommander, "red_commander");
    await claim(byKey.blueCommander, "blue_commander");
    await claim(byKey.redRecon, "red_recon_1");
    await claim(byKey.redAttack, "red_attack_1");
    await claim(byKey.redGround, "red_ground_1");
    const redCommander = byKey.redCommander;
    const unitIds = {};
    unitIds.redCp = await deploy(redCommander, "red_commander", { x: 2000, y: 7500, alt: 50 });
    unitIds.recon = await deploy(redCommander, "red_recon_1", { x: 3000, y: 7500, alt: 3000 });
    unitIds.attack = await deploy(redCommander, "red_attack_1", { x: 4000, y: 7500, alt: 2000 });
    unitIds.ground = await deploy(redCommander, "red_ground_1", { x: 5000, y: 7500, alt: 0 });
    await deploy(byKey.blueCommander, "blue_commander", { x: 18000, y: 7500, alt: 50 });

    const correlationId = `guided-${suffix}`;
    let traceCounter = 0;
    const post = async (session, fixture, messageType, sender, receiver, payload) => {
      const traceId = `${correlationId}-${++traceCounter}`;
      const start = session.messages.length;
      session.send("vmfMessage", {
        messageType, senderUnitId: sender, receiverUnitId: receiver,
        traceId, correlationId, vmfMessage: fixture.vmfMessage,
        wireFormat: "vmf-design-v1", wireBytes: fixture.wireBytes,
        wireBitLength: fixture.wireBitLength, requiresAck: true, retryCount: 0,
        fieldCount: 0, payload,
      });
      const event = await session.waitFor(message => session.messages.indexOf(message) >= start
        && message.type === "vmfEvent" && message.payload?.traceId === traceId);
      assert(event.payload.acked === true, `${messageType} 未得到自动 ACK`);
      return traceId;
    };

    const targetId = process.env.VMF_TARGET_ID || "blue_cp";
    const targetTrace = await post(byKey.redRecon, { ...fixtures.target, vmfMessage: "Target Report" },
      "TargetReport", unitIds.recon, unitIds.redCp,
      { targetId, targetType: "armored", targetCount: 1, friendFoe: "enemy",
        x: 17000, y: 7500, status: "intact" });
    await post(redCommander, { ...fixtures.route, vmfMessage: "Land Route" },
      "StrikePlan", unitIds.redCp, unitIds.attack,
      { targetId, waypoints: [{ x: 6000, y: 7500 }] });
    await post(redCommander, { ...fixtures.route, vmfMessage: "Land Route" },
      "AttackOrder", unitIds.redCp, unitIds.attack,
      { targetId, waypoints: [{ x: 6000, y: 7500 }], fireNow: false });
    await post(redCommander, { ...fixtures.network, vmfMessage: "NetworkMonitoring" },
      "GroundGuideOrder", unitIds.redCp, unitIds.ground,
      { targetId, attackerId: unitIds.attack });
    await post(byKey.redGround, { ...fixtures.route, vmfMessage: "Land Route" },
      "GroundAttackConfirm", unitIds.ground, unitIds.attack,
      { targetId, waypoints: [{ x: 6000, y: 7500 }], fireNow: true });
    await post(byKey.redAttack, { ...fixtures.target, vmfMessage: "Target Report" },
      "TargetDestroyed", unitIds.attack, unitIds.redCp,
      { targetId, attackerId: unitIds.attack, targetType: "armored", targetCount: 1,
        friendFoe: "enemy", x: 17000, y: 7500, status: "destroyed" });
    await post(redCommander, { ...fixtures.route, vmfMessage: "Land Route" },
      "WithdrawOrder", unitIds.redCp, unitIds.attack,
      { targetId, homeX: 2000, homeY: 7500, x: 2000, y: 7500 });

    const observerEvent = byKey.observer.messages.find(message => message.type === "event"
      && message.payload?.kind === "vmfMessage");
    assert(observerEvent, "观察员没有收到 VMF 阶段摘要");
    assert(!observerEvent.payload.wireBytes && !observerEvent.payload.payload,
      "观察员 VMF 事件不能包含 wire 或原始 payload");
    assert(observerEvent.payload.catalogId && observerEvent.payload.informationValue,
      "观察员摘要缺少目录编号或信息价值");

    const duplicateTrace = targetTrace;
    const duplicatePayload = {
      messageType: "TargetReport", senderUnitId: unitIds.recon, receiverUnitId: unitIds.redCp,
      traceId: duplicateTrace, correlationId, vmfMessage: "Target Report",
      wireFormat: "vmf-design-v1", wireBytes: fixtures.target.wireBytes,
      wireBitLength: fixtures.target.wireBitLength, requiresAck: true, retryCount: 0,
      fieldCount: 0, payload: { targetId, targetType: "armored", targetCount: 1,
        friendFoe: "enemy", x: 17000, y: 7500, status: "intact" },
    };
    byKey.redRecon.send("vmfMessage", duplicatePayload);
    const duplicateError = await byKey.redRecon.waitFor(message => message.type === "error"
      && message.payload?.code === "DUPLICATE_MESSAGE");
    assert(duplicateError, "重复 trace 必须被拒绝");
    byKey.redRecon.send("vmfMessage", duplicatePayload);
    await byKey.redRecon.waitFor(message => message.type === "error"
      && message.payload?.code === "DUPLICATE_MESSAGE");

    byKey.redRecon.send("vmfMessage", { ...duplicatePayload,
      traceId: `${correlationId}-forged`, senderUnitId: unitIds.attack });
    await byKey.redRecon.waitFor(message => message.type === "error"
      && message.payload?.code === "UNIT_NOT_OWNED");
    byKey.redGround.send("vmfMessage", { ...duplicatePayload,
      traceId: `${correlationId}-role`, senderUnitId: unitIds.ground });
    await byKey.redGround.waitFor(message => message.type === "error"
      && message.payload?.code === "VMF_ROLE_FORBIDDEN");
    redCommander.send("vmfMessage", {
      ...duplicatePayload, traceId: `${correlationId}-out-of-order`,
      messageType: "GroundGuideOrder", senderUnitId: unitIds.redCp,
      receiverUnitId: unitIds.ground, vmfMessage: "NetworkMonitoring",
      wireBytes: fixtures.network.wireBytes, wireBitLength: fixtures.network.wireBitLength,
      payload: { targetId, attackerId: unitIds.attack },
    });
    await redCommander.waitFor(message => message.type === "error"
      && message.payload?.code === "VMF_SEQUENCE_INVALID");
    byKey.observer.send("vmfMessage", duplicatePayload);
    await byKey.observer.waitFor(message => message.type === "error"
      && message.payload?.code === "SEAT_REQUIRED");
    console.log("VMF 引导打击联网验收通过：目录映射、ACK、工作流、重复 trace、角色/归属校验和观察员脱敏均正常。");
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
    await rm(tempDirectory, { recursive: true, force: true });
  }
}

await main();
