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
const runtimeOnly = process.env.VMF_RUNTIME_ONLY === "1";
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
      } else if (message.type === "delta" && this.state
                 && message.payload.baseStateRevision === this.state.stateRevision) {
        const byId = new Map(this.state.units.map(unit => [unit.id, unit]));
        for (const unit of message.payload.units || []) byId.set(unit.id, unit);
        this.state.units = [...byId.values()].sort((a, b) => a.id.localeCompare(b.id));
        if (message.payload.projectiles) {
          this.state.projectiles = structuredClone(message.payload.projectiles);
        }
        if (message.payload.roomState) {
          this.state.roomState = structuredClone(message.payload.roomState);
        }
        if (message.payload.messages) this.state.messages = structuredClone(message.payload.messages);
        if (message.payload.mapMarks) this.state.mapMarks = structuredClone(message.payload.mapMarks);
        if (message.payload.intelDelta) {
          const delta = message.payload.intelDelta;
          const current = this.state.intelState || { revision: 0, records: [], shareTargets: [] };
          assert(delta.baseRevision === current.revision,
            `${this.name}: 情报增量基线不连续`);
          const records = new Map((current.records || []).map(record => [record.intelId, record]));
          for (const record of delta.upserts || []) records.set(record.intelId, structuredClone(record));
          for (const intelId of delta.deletedIntelIds || []) records.delete(intelId);
          this.state.intelState = {
            revision: delta.revision,
            records: [...records.values()].sort((a, b) => a.intelId.localeCompare(b.intelId)),
            shareTargets: structuredClone(delta.shareTargets || []),
          };
        }
        this.state.stateRevision = message.payload.stateRevision;
        this.messages.push({ type: "state", payload: structuredClone(this.state) });
      }
    });
    this.send("auth", { token: this.token });
    await this.waitFor(message => message.type === "welcome");
    await this.waitFor(message => message.type === "state");
  }

  send(type, payload, messageId = crypto.randomUUID()) {
    this.socket.send(JSON.stringify({
      protocolVersion: 8,
      schemaVersion: 8,
      type,
      messageId,
      payload,
    }));
    return messageId;
  }

  waitFor(predicate, timeoutMs = 10000) {
    return this.waitForAfter(0, predicate, timeoutMs);
  }

  waitForAfter(startIndex, predicate, timeoutMs = 10000, label = "消息") {
    const findMessage = () => this.messages.slice(startIndex).find(predicate);
    const existing = findMessage();
    if (existing) return Promise.resolve(existing);
    return new Promise((resolveMessage, reject) => {
      const timer = setTimeout(() => {
        clearInterval(interval);
        const errors = this.messages.filter(message => message.type === "error")
          .map(message => `${message.payload?.code || "error"}:`
            + `${message.payload?.message || ""}`);
        const states = this.messages.slice(startIndex).filter(message => message.type === "state");
        const latest = states.at(-1)?.payload;
        const room = latest?.roomState || {};
        const scanUnits = (latest?.units || [])
          .filter(unit => unit.abilities?.scan || unit.scanCooldownRemaining !== undefined)
          .map(unit => `${unit.id}:${scanCooldown(latest, unit.id).toFixed(2)}`)
          .join(",");
        reject(new Error(`${this.name}: 等待${label}超时; errors=${errors.join(",")}; `
          + `latestSim=${Number(room.simTime || 0).toFixed(2)}; `
          + `latestRevision=${room.stateRevision ?? latest?.stateRevision ?? "?"}; `
          + `phase=${room.phase || "?"}; engineRunning=${room.running ?? "?"}; `
          + `scan=${scanUnits || "none"}; states=${states.length}`));
      }, timeoutMs);
      const interval = setInterval(() => {
        const found = findMessage();
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
  await session.waitFor(message => message.type === "seatState"
    && message.payload?.seats?.some(seat => seat.seatId === seatId));
  session.send("claimSeat", { seatId });
  await session.waitFor(message => message.type === "seatState"
    && message.payload?.yourSeatId === seatId);
}

async function deploy(commander, seatId, position) {
  const seat = seatById(commander, seatId);
  assert(seat?.unitId, `没有找到 ${seatId} 对应的单位`);
  const start = commander.messages.length;
  commander.send("deployment", { unitId: seat.unitId, targetSeatId: seatId, position });
  const outcome = await commander.waitForAfter(start, message =>
    (message.type === "state"
      && message.payload?.roomState?.seats?.some(item => item.seatId === seatId && item.deployed))
    || message.type === "error");
  assert(outcome.type === "state",
    `${seatId} 部署失败: ${outcome.payload?.code || "UNKNOWN"}: ${outcome.payload?.message || ""}`);
  return seat.unitId;
}

function scanCooldown(state, reconUnitId) {
  const recon = state?.units?.find(unit => unit.id === reconUnitId);
  if (!recon) return null;
  return Number(recon.abilities?.scan?.cooldownRemaining
    ?? recon.scanCooldownRemaining ?? 0);
}

function taskFromState(state, taskId) {
  return state?.roomState?.vmfTasks?.tasks?.find(task => task.taskId === taskId);
}

async function runRuntimeAcceptance(adminToken, login, suffix, sessions) {
  const commander = new GameSession("runtimeCommander", login.gameWebSocketUrl, login.token);
  sessions.push(commander);
  await commander.connect();
  commander.send("joinRoom", { roomId: "main" });
  await commander.waitFor(message => message.type === "seatState");
  await claim(commander, "red_commander");

  const reconSeat = seatById(commander, "red_recon_1");
  assert(reconSeat?.unitId, "严格 VMF 自动侦察战位没有运行单位");
  assert(reconSeat.controllerType === "placeholder" && reconSeat.controlMode === "vmf-auto",
    "红方侦察战位未保持服务器自动托管");

  const readyStart = commander.messages.length;
  commander.send("seatReady", { ready: true });
  await commander.waitForAfter(readyStart, message => message.type === "state"
    && message.payload?.roomState?.readyForStart === true);

  const runStart = commander.messages.length;
  const start = await request("/api/admin/rooms/main/start", { method: "POST" }, adminToken);
  if (start.operation?.operationId) {
    await waitForRoomOperation(adminToken, start.operation.operationId, "running");
  }
  const running = await commander.waitForAfter(runStart, message => message.type === "state"
    && message.payload?.roomState?.phase === "running", 15000, "running");
  const simulationStartedAt = Number(running.payload.roomState.simTime || 0);
  const wallStartedAt = Date.now();

  const scanStarted = await commander.waitForAfter(runStart, message => message.type === "state"
    && scanCooldown(message.payload, reconSeat.unitId) > 0.1, 15000, "scan started");
  const scanStartedIndex = commander.messages.indexOf(scanStarted);
  const intelState = await commander.waitForAfter(runStart, message => message.type === "state"
    && message.payload?.intelState?.records?.some(record => record.type === "sensorContact"
      && record.actionable && record.knownAttributes?.side === "blue"), 15000, "intel acquired");
  const contact = intelState.payload.intelState.records.find(record => record.type === "sensorContact"
    && record.actionable && record.knownAttributes?.side === "blue");
  assert(contact?.targetId, "自动侦察敌情没有提供可用于任务的目标 ID");

  const taskId = `runtime-task-${suffix}`;
  const requestId = `runtime-create-${suffix}`;
  const correlationId = `runtime-correlation-${suffix}`;
  const resultStart = commander.messages.length;
  commander.send("vmfTaskCommand", {
    requestId,
    taskId,
    expectedTaskRevision: 0,
    action: "createTask",
    messages: [],
    commanderSeatId: "red_commander",
    reconSeatId: "red_recon_1",
    attackSeatId: "red_attack_1",
    groundSeatId: "red_ground_1",
    targetId: contact.targetId,
    correlationId,
  }, requestId);
  const taskResult = await commander.waitForAfter(resultStart,
    message => message.type === "vmfTaskResult" && message.payload?.requestId === requestId,
    10000, "task result");
  assert(taskResult.payload.code === "OK"
    && ["accepted", "blocked"].includes(taskResult.payload.status),
  `指挥席创建严格 VMF 任务失败: ${taskResult.payload.code}`);
  await commander.waitForAfter(resultStart, message => message.type === "state"
    && taskFromState(message.payload, taskId)?.stage === "targetReported", 15000, "task stage");

  const cooldownEnded = await commander.waitForAfter(scanStartedIndex + 1,
    message => message.type === "state"
      && scanCooldown(message.payload, reconSeat.unitId) !== null
      && scanCooldown(message.payload, reconSeat.unitId) <= 0.05, 60000, "cooldown ended");
  const cooldownEndedIndex = commander.messages.indexOf(cooldownEnded);
  const cooldownEndedAt = Number(cooldownEnded.payload.roomState.simTime || 0);
  const afterCooldown = await commander.waitForAfter(cooldownEndedIndex + 1,
    message => message.type === "state"
      && message.payload?.roomState?.phase === "running"
      && Number(message.payload.roomState.simTime || 0) >= cooldownEndedAt + 2, 10000,
    "post-cooldown progress");

  let priorCooldown = 0;
  let scanActivations = 0;
  for (const message of commander.messages.slice(runStart)) {
    if (message.type !== "state") continue;
    const cooldown = scanCooldown(message.payload, reconSeat.unitId);
    if (cooldown === null) continue;
    if (cooldown > 0.1 && priorCooldown <= 0.1) ++scanActivations;
    priorCooldown = cooldown;
  }
  assert(scanActivations === 1,
    `自动侦察跨冷却周期重复释放，检测到 ${scanActivations} 次冷却上升`);

  const simulationElapsed = Number(afterCooldown.payload.roomState.simTime || 0)
    - simulationStartedAt;
  const wallElapsed = (Date.now() - wallStartedAt) / 1000;
  const clockRatio = simulationElapsed / Math.max(0.001, wallElapsed);
  assert(simulationElapsed >= 45 && clockRatio >= 0.7 && clockRatio <= 1.4,
    `推演时钟推进异常: sim=${simulationElapsed.toFixed(2)}s, wall=${wallElapsed.toFixed(2)}s`);
  assert(taskFromState(afterCooldown.payload, taskId), "长时运行后严格 VMF 任务状态丢失");

  console.log("VMF 运行态联网验收通过：自动侦察仅释放 1 次，侦察目标可创建任务，"
    + `任务已进入目标报告阶段；sim=${simulationElapsed.toFixed(2)}s, `
    + `wall=${wallElapsed.toFixed(2)}s, ratio=${clockRatio.toFixed(3)}, `
    + `intel=${afterCooldown.payload.intelState?.records?.length || 0}。`);
}

async function main() {
  const tempDirectory = await mkdtemp(join(tmpdir(), "wargame-vmf-smoke-"));
  const suffix = `vmf${Date.now().toString(36)}`;
  const password = `Smoke-${suffix}-Pass`;
  const accountSpecs = runtimeOnly
    ? [["redCommander", "red_commander", "红方指挥官"]]
    : [
      ["redCommander", "red_commander", "红方指挥官"],
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
    const roomDirectory = await request("/api/admin/rooms", {}, adminToken);
    const currentRoom = roomDirectory.rooms?.find(item => item.roomId === "main");
    if (currentRoom && currentRoom.status !== "stopped" && currentRoom.status !== "finished") {
      const stop = await request("/api/admin/rooms/main/stop", { method: "POST" }, adminToken);
      if (stop.operation?.operationId) {
        await waitForRoomOperation(adminToken, stop.operation.operationId, "stopped");
      }
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
    if (runtimeOnly) {
      await runRuntimeAcceptance(adminToken, logins.redCommander, suffix, sessions);
      return;
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
    await claim(byKey.redRecon, "red_recon_1");
    await claim(byKey.redAttack, "red_attack_1");
    await claim(byKey.redGround, "red_ground_1");
    const redCommander = byKey.redCommander;
    const unitIds = {};
    unitIds.redCp = await deploy(redCommander, "red_commander", { x: 2000, y: 7500, alt: 50 });
    unitIds.recon = await deploy(redCommander, "red_recon_1", { x: 3000, y: 7500, alt: 3000 });
    unitIds.attack = await deploy(redCommander, "red_attack_1", { x: 4000, y: 7500, alt: 2000 });
    unitIds.ground = await deploy(redCommander, "red_ground_1", { x: 5000, y: 7500, alt: 0 });

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
      const event = await session.waitForAfter(start, message =>
        (message.type === "vmfEvent" && message.payload?.traceId === traceId)
        || message.type === "error");
      assert(event.type === "vmfEvent",
        `${messageType} 被拒绝: ${event.payload?.code || "UNKNOWN"}: ${event.payload?.message || ""}`);
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
      && message.payload?.code === "OBSERVER_READ_ONLY");
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
