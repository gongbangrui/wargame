#!/usr/bin/env node

const accountUrl = process.env.ACCOUNT_URL || "http://127.0.0.1:8080";
const password = process.env.ADMIN_PASSWORD;

if (!password) throw new Error("ADMIN_PASSWORD is required");

async function request(path, options = {}, token = "") {
  const headers = { ...(options.headers || {}) };
  if (options.body) headers["Content-Type"] = "application/json";
  if (token) headers.Authorization = `Bearer ${token}`;
  const response = await fetch(`${accountUrl}${path}`, { ...options, headers });
  const payload = await response.json().catch(() => ({}));
  return { response, payload };
}

const suffix = `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
const roomId = `contract-${suffix}`;
let token = "";

try {
  const login = await request("/api/admin/login", {
    method: "POST",
    body: JSON.stringify({ username: "admin", password }),
  });
  if (!login.response.ok) throw new Error(`管理员登录失败: ${login.response.status}`);
  token = login.payload.token;

  const created = await request("/api/admin/rooms", {
    method: "POST",
    body: JSON.stringify({ room_id: roomId, name: "托管契约回归房间" }),
  }, token);
  if (created.response.status !== 201) throw new Error(`创建临时房间失败: ${created.response.status}`);

  const action = await request(`/api/admin/rooms/${roomId}/open`, { method: "POST" }, token);
  if (action.response.status !== 409) {
    throw new Error(`非托管房间操作不应进入等待确认，实际返回 ${action.response.status}`);
  }
  console.log("房间托管契约通过：非托管房间的生命周期操作被立即拒绝。");
} finally {
  if (token) {
    await request(`/api/admin/rooms/${roomId}`, { method: "DELETE" }, token);
    await request("/api/admin/logout", { method: "POST" }, token);
  }
}
