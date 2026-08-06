import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";
import vm from "node:vm";

const source = fs.readFileSync(new URL("../server/account/static/app.js", import.meta.url), "utf8");

function loadHelpers() {
  const sandbox = {
    console,
    document: { addEventListener() {} },
    localStorage: { getItem() { return null; } },
    sessionStorage: { getItem() { return null; } },
    module: { exports: {} },
  };
  vm.runInNewContext(`${source}\nmodule.exports = { formatApiError, historyValueText, historyStatusLabel, retainedAiHistorySelection };`, sandbox, {
    filename: "server/account/static/app.js",
  });
  return sandbox.module.exports;
}

test("formats FastAPI validation detail arrays instead of object coercion", () => {
  const { formatApiError } = loadHelpers();
  const message = formatApiError({
    detail: [{ loc: ["body", "room_id"], msg: "String should have at least 2 characters" }],
  });

  assert.equal(message, "room_id: String should have at least 2 characters");
  assert.doesNotMatch(message, /\[object Object\]/);
});

test("preserves string API details and serializes object fallbacks", () => {
  const { formatApiError } = loadHelpers();

  assert.equal(formatApiError({ detail: "房间 ID 已存在" }), "房间 ID 已存在");
  assert.equal(formatApiError({ detail: { code: "ROOM_INVALID" } }), '{"code":"ROOM_INVALID"}');
  assert.equal(formatApiError({}), "请求失败");
});

test("keeps AI history payloads as literal text and maps status labels", () => {
  const { historyValueText, historyStatusLabel } = loadHelpers();
  const hostile = "<script>window.__historyXss = true</script>\nAuthorization: Bearer hidden";

  assert.equal(historyValueText(hostile), hostile);
  assert.match(historyValueText({ prompt: hostile }), /__historyXss/);
  assert.equal(historyStatusLabel("failed"), "失败");
  assert.equal(historyStatusLabel("unexpected"), "未知状态");
});

test("keeps the selected AI conversation across a background history refresh", () => {
  const { retainedAiHistorySelection } = loadHelpers();
  const items = [{ conversationId: "ai-plan:1:4" }, { conversationId: "ai-plan:1:3" }];

  assert.equal(retainedAiHistorySelection("ai-plan:1:3", items), "ai-plan:1:3");
  assert.equal(retainedAiHistorySelection("ai-plan:1:2", items), "");
});
