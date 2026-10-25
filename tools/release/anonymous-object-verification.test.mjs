import test from "node:test";
import assert from "node:assert/strict";
import { verifyAnonymousObject } from "./anonymous-object-verification.mjs";

const hash = "b".repeat(64);

function headResponse(status, entries) {
  return { ok: status >= 200 && status < 300, status, headers: new Headers(entries) };
}

test("accepts public gzip responses without Content-Length when OSS SHA-256 matches", async () => {
  await verifyAnonymousObject("https://example.test/index.html", 14014, hash, async () =>
    headResponse(200, { "content-encoding": "gzip", "x-oss-meta-sha256": hash }));
});

test("accepts an HTML page only when the public response can render inline", async () => {
  await verifyAnonymousObject("https://example.test/index.html", 14014, hash, async () =>
    headResponse(200, {
      "content-type": "text/html; charset=utf-8",
      "content-disposition": "inline",
      "x-oss-meta-sha256": hash,
    }), { requireInlineHtml: true });
});

test("rejects HTML that OSS forces to download even when the object hash matches", async () => {
  await assert.rejects(
    verifyAnonymousObject("https://example.test/index.html", 14014, hash, async () =>
      headResponse(200, {
        "content-type": "text/html; charset=utf-8",
        "content-disposition": "attachment",
        "x-oss-force-download": "true",
        "x-oss-meta-sha256": hash,
      }), { requireInlineHtml: true }),
    /cannot be rendered inline/,
  );
});

test("rejects a public object with the wrong declared size", async () => {
  await assert.rejects(
    verifyAnonymousObject("https://example.test/file.zip", 100, hash, async () =>
      headResponse(200, { "content-length": "99", "x-oss-meta-sha256": hash })),
    /mismatched hash\/size metadata/
  );
});

test("rejects a public object with the wrong hash metadata", async () => {
  await assert.rejects(
    verifyAnonymousObject("https://example.test/file.zip", 100, hash, async () =>
      headResponse(200, { "content-length": "100", "x-oss-meta-sha256": "c".repeat(64) })),
    /mismatched hash\/size metadata/
  );
});

test("rejects a publicly denied object", async () => {
  await assert.rejects(
    verifyAnonymousObject("https://example.test/file.zip", 100, hash, async () =>
      headResponse(403, { "x-oss-meta-sha256": hash })),
    /mismatched hash\/size metadata/
  );
});
