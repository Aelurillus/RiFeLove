import assert from "node:assert/strict";
import test from "node:test";
import { createWorkerSource } from "./worker-source.mjs";

const manifest = {
  schemaVersion: 1,
  product: "RifeLove",
  updatedAt: "2026-10-26T00:00:00.000Z",
  releases: [{
    version: "0.1.2", date: "2026-10-26", channel: "Preview", releaseReady: false,
    downloadAvailable: false, storjBackup: false, title: "RifeLove preview",
    summary: "A local-only preview.", highlights: ["Public downloads remain disabled."], artifact: null,
  }],
};

test("worker source embeds safe local content and exposes the update-log routes", () => {
  const html = "<!doctype html><script>const x = \"</script>\";</script>";
  const source = createWorkerSource(html, manifest);
  assert.ok(source.includes(`const PAGE = ${JSON.stringify(html)};`));
  assert.ok(source.includes(`const RELEASES = ${JSON.stringify(JSON.stringify(manifest))};`));
  assert.match(source, /pathname === "\/releases\.json"/);
  assert.match(source, /request\.method === "HEAD" \? null : body/);
  assert.match(source, /status:405/);
  assert.match(source, /status:404/);
  assert.match(source, /x-content-type-options/);
});

test("worker generator rejects an invalid release manifest", () => {
  assert.throws(() => createWorkerSource("<html></html>", { ...manifest, releases: [] }), /requires at least one release/);
});
