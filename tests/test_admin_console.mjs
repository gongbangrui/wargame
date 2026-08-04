import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";
import vm from "node:vm";

const source = fs.readFileSync(new URL("../server/account/static/app.js", import.meta.url), "utf8");

function loadFormatter() {
  const sandbox = {
    console,
    document: { addEventListener() {} },
    localStorage: { getItem() { return null; } },
    sessionStorage: { getItem() { return null; } },
    module: { exports: {} },
  };
  vm.runInNewContext(`${source}\nmodule.exports = { formatApiError };`, sandbox, {
    filename: "server/account/static/app.js",
  });
  return sandbox.module.exports.formatApiError;
}

test("formats FastAPI validation detail arrays instead of object coercion", () => {
  const formatApiError = loadFormatter();
  const message = formatApiError({
    detail: [{ loc: ["body", "room_id"], msg: "String should have at least 2 characters" }],
  });

  assert.equal(message, "room_id: String should have at least 2 characters");
  assert.doesNotMatch(message, /\[object Object\]/);
});

test("preserves string API details and serializes object fallbacks", () => {
  const formatApiError = loadFormatter();

  assert.equal(formatApiError({ detail: "房间 ID 已存在" }), "房间 ID 已存在");
  assert.equal(formatApiError({ detail: { code: "ROOM_INVALID" } }), '{"code":"ROOM_INVALID"}');
  assert.equal(formatApiError({}), "请求失败");
});
