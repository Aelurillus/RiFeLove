import assert from "node:assert/strict";
import { createHash, generateKeyPairSync, sign } from "node:crypto";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { copyVerifiedModelPagesAssets, inspectModelPagesAssets } from "./model-pages-assets.mjs";

const archiveName = "RifeLove-RIFE-4.25-FP32-SDR8-v3.zip";
const manifestName = "RifeLove-RIFE-4.25-FP32-SDR8-v3.distribution.json";
const modelSha256 = "a".repeat(64);

test("accepts a finalized, signed model bundle for the configured Pages channel", async t => {
  const { root, publicKey, archiveBytes, manifestBytes, envelopeBytes } = await createBundle(t);
  const assets = await inspectModelPagesAssets(root, { trustedPublicKey: publicKey });
  assert.equal(assets.archiveName, archiveName);
  assert.equal(assets.manifestName, manifestName);
  assert.equal(assets.archiveSha256, sha256(archiveBytes));
  assert.equal(assets.manifestSha256, sha256(manifestBytes));
  assert.equal(assets.catalogSha256, sha256(envelopeBytes));

  const output = path.join(root, "output");
  await mkdir(output);
  await copyVerifiedModelPagesAssets(assets, output);
  assert.deepEqual(await readFile(path.join(output, "model-distribution", archiveName)), archiveBytes);
  assert.deepEqual(await readFile(path.join(output, "model-distribution", manifestName)), manifestBytes);
  assert.deepEqual(await readFile(path.join(output, "model-catalog", "v1", "catalog.signed.txt")), envelopeBytes);
});

test("uses the catalog trust root shared with the native client", async () => {
  const sharedVerifier = await readFile(new URL("../model_catalog/ModelRelease.Common.ps1", import.meta.url), "utf8");
  const roots = [...sharedVerifier.matchAll(/^\s*\$rootHex\s*=\s*'([0-9a-f]{128})'\s*$/gim)];
  assert.equal(roots.length, 1);
  const nativeSource = await readFile(new URL("../../apps/native_desktop/native_model_catalog.cpp", import.meta.url), "utf8");
  const trustStart = nativeSource.indexOf("static const NativeModelCatalogTrust trust{");
  assert.notEqual(trustStart, -1);
  const trustEnd = nativeSource.indexOf("};", trustStart);
  assert.notEqual(trustEnd, -1);
  const nativeTrust = nativeSource.slice(trustStart, trustEnd);
  const nativeRootHex = [...nativeTrust.matchAll(/0x([0-9a-f]{2})/gi)].map(match => match[1]).join("");
  assert.equal(nativeRootHex, roots[0][1]);
  assert.ok(nativeTrust.includes("rifelove-model-catalog-2026-v1"));
});

test("bootstrap publishes only the local-candidate archive and keeps its manifest local", async t => {
  const root = await mkdtemp(path.join(os.tmpdir(), "rifelove-model-pages-bootstrap-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const distributionRoot = path.join(root, "model-distribution");
  await mkdir(distributionRoot, { recursive: true });
  const archiveBytes = Buffer.from("candidate ZIP fixture");
  const manifest = {
    schemaVersion: 1,
    packageId: "rifelove-rife-4.25-fp32-sdr8-v3",
    state: "local-candidate-not-published",
    model: {
      id: "rife-4.25-fp32-sdr8",
      version: "4.25",
      family: "rife",
      displayName: "RIFE 4.25 FP32 SDR8",
      modelSha256,
    },
    artifact: { fileName: archiveName, sizeBytes: archiveBytes.length, sha256: sha256(archiveBytes) },
    downloadChannels: [],
  };
  await writeFile(path.join(distributionRoot, archiveName), archiveBytes);
  await writeFile(path.join(distributionRoot, manifestName), `${JSON.stringify(manifest)}\n`);

  const assets = await inspectModelPagesAssets(root, { mode: "archive-bootstrap" });
  const output = path.join(root, "output");
  await mkdir(output);
  await copyVerifiedModelPagesAssets(assets, output);
  assert.deepEqual(await readFile(path.join(output, "model-distribution", archiveName)), archiveBytes);
  await assert.rejects(readFile(path.join(output, "model-distribution", manifestName)), { code: "ENOENT" });
});

test("rejects an archive whose bytes differ from the finalized manifest", async t => {
  const { root, publicKey, archiveBytes } = await createBundle(t);
  await writeFile(path.join(root, "model-distribution", archiveName), Buffer.alloc(archiveBytes.length, 0x58));
  await assert.rejects(inspectModelPagesAssets(root, { trustedPublicKey: publicKey }), /SHA-256 does not match/);
});

test("rejects a validly signed catalog from a key other than the client trust root", async t => {
  const { root } = await createBundle(t);
  await assert.rejects(inspectModelPagesAssets(root), /does not verify against the native client trust root/);
});

test("rejects an unsigned local candidate even when its archive hash is correct", async t => {
  const { root, publicKey } = await createBundle(t);
  const manifestPath = path.join(root, "model-distribution", manifestName);
  const manifest = JSON.parse(await readFile(manifestPath, "utf8"));
  manifest.state = "local-candidate-not-published";
  await writeFile(manifestPath, `${JSON.stringify(manifest)}\n`);
  await assert.rejects(inspectModelPagesAssets(root, { trustedPublicKey: publicKey }), /finalized RIFE archive/);
});

async function createBundle(t) {
  const root = await mkdtemp(path.join(os.tmpdir(), "rifelove-model-pages-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const distributionRoot = path.join(root, "model-distribution");
  const catalogRoot = path.join(root, "model-catalog", "v1");
  await mkdir(distributionRoot, { recursive: true });
  await mkdir(catalogRoot, { recursive: true });

  const archiveBytes = Buffer.from("model ZIP fixture");
  const pagesChannel = {
    name: "Cloudflare Pages",
    archiveUrl: `https://rifelove-updatelog.pages.dev/model-distribution/${archiveName}`,
    manifestUrl: `https://rifelove-updatelog.pages.dev/model-distribution/${manifestName}`,
  };
  const githubChannel = {
    name: "GitHub Releases",
    archiveUrl: `https://github.com/Aelurillus/RiFeLove/releases/download/rife-4.25/${archiveName}`,
    manifestUrl: `https://github.com/Aelurillus/RiFeLove/releases/download/rife-4.25/${manifestName}`,
  };
  const manifest = {
    schemaVersion: 1,
    packageId: "rifelove-rife-4.25-fp32-sdr8-v3",
    state: "archive-channels-verified",
    model: {
      id: "rife-4.25-fp32-sdr8",
      version: "4.25",
      family: "rife",
      displayName: "RIFE 4.25 FP32 SDR8",
      modelSha256,
    },
    artifact: {
      fileName: archiveName,
      sizeBytes: archiveBytes.length,
      sha256: sha256(archiveBytes),
    },
    downloadChannels: [pagesChannel, githubChannel],
  };
  const manifestBytes = Buffer.from(`${JSON.stringify(manifest)}\n`);
  const { privateKey, publicKey } = generateKeyPairSync("ec", { namedCurve: "prime256v1" });
  const payloadBytes = Buffer.from(JSON.stringify({
    schemaVersion: 1,
    models: [{
      id: manifest.model.id,
      version: manifest.model.version,
      family: "rife",
      name: manifest.model.displayName,
      packageId: manifest.packageId,
      modelSha256,
      recommended: true,
      manifest: { sizeBytes: manifestBytes.length, sha256: sha256(manifestBytes) },
      channels: [pagesChannel, githubChannel],
    }],
  }));
  const signature = sign("sha256", payloadBytes, { key: privateKey, dsaEncoding: "ieee-p1363" });
  const envelopeBytes = Buffer.from([
    "RIFELOVE MODEL CATALOG 1",
    "key-id=rifelove-model-catalog-2026-v1",
    `payload=${payloadBytes.toString("base64")}`,
    `signature=${signature.toString("base64")}`,
    "",
  ].join("\n"));

  await writeFile(path.join(distributionRoot, archiveName), archiveBytes);
  await writeFile(path.join(distributionRoot, manifestName), manifestBytes);
  await writeFile(path.join(catalogRoot, "catalog.signed.txt"), envelopeBytes);
  return { root, publicKey, archiveBytes, manifestBytes, envelopeBytes };
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}
