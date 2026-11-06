import { createHash, createPublicKey, verify } from "node:crypto";
import { constants, createReadStream } from "node:fs";
import { copyFile, lstat, mkdir, readdir, readFile } from "node:fs/promises";
import path from "node:path";

const MAX_PAGES_ASSET_BYTES = 25 * 1024 * 1024;
const MAX_DISTRIBUTION_MANIFEST_BYTES = 1024 * 1024;
const MAX_SIGNED_CATALOG_BYTES = 384 * 1024;
const PAGES_HOST = "rifelove-updatelog.pages.dev";
const CATALOG_KEY_ID = "rifelove-model-catalog-2026-v1";
const officialCatalogKey = await loadOfficialCatalogKey();

const EXPECTED_SOURCE_FILES = new Set([
  "model-distribution",
  "model-catalog",
]);

export async function inspectModelPagesAssets(sourceRoot, { mode = "final", trustedPublicKey = officialCatalogKey } = {}) {
  const root = path.resolve(sourceRoot);
  await assertDirectory(root, "Model Pages staging root");
  if (mode === "archive-bootstrap") return inspectModelPagesArchiveBootstrap(root);
  if (mode !== "final") throw new Error("Model Pages staging mode must be archive-bootstrap or final.");
  await assertExactNames(root, EXPECTED_SOURCE_FILES, "Model Pages staging root");

  const modelDirectory = path.join(root, "model-distribution");
  const catalogDirectory = path.join(root, "model-catalog");
  await assertDirectory(modelDirectory, "Model distribution directory");
  await assertDirectory(catalogDirectory, "Model catalog directory");

  const distributionEntries = await readdir(modelDirectory);
  if (distributionEntries.length !== 2) {
    throw new Error("Model distribution staging must contain one ZIP and one distribution manifest.");
  }
  const zipNames = distributionEntries.filter(name => name.endsWith(".zip"));
  const manifestNames = distributionEntries.filter(name => name.endsWith(".distribution.json"));
  if (zipNames.length !== 1 || manifestNames.length !== 1) {
    throw new Error("Model distribution staging must contain one ZIP and its matching distribution manifest.");
  }

  const archiveName = zipNames[0];
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}\.zip$/.test(archiveName)) {
    throw new Error("The model ZIP name is not safe for a public download URL.");
  }
  const expectedManifestName = `${archiveName.slice(0, -4)}.distribution.json`;
  if (manifestNames[0] !== expectedManifestName) {
    throw new Error("The model ZIP and distribution manifest names do not match.");
  }

  const archivePath = path.join(modelDirectory, archiveName);
  const manifestPath = path.join(modelDirectory, expectedManifestName);
  await assertRegularFile(archivePath, "Model ZIP");
  await assertRegularFile(manifestPath, "Distribution manifest");
  const archiveInfo = await lstat(archivePath);
  const manifestInfo = await lstat(manifestPath);
  if (archiveInfo.size < 1 || archiveInfo.size > MAX_PAGES_ASSET_BYTES) {
    throw new Error("The model ZIP exceeds Cloudflare Pages' 25 MiB per-asset limit.");
  }
  if (manifestInfo.size < 1 || manifestInfo.size > MAX_DISTRIBUTION_MANIFEST_BYTES) {
    throw new Error("The distribution manifest must be between 1 byte and 1 MiB.");
  }

  const manifestBytes = await readFile(manifestPath);
  const manifest = parseJson(manifestBytes, "Distribution manifest");
  assertDistributionManifest(manifest, archiveName, archiveInfo.size);
  const archiveSha256 = await hashFile(archivePath);
  if (archiveSha256 !== manifest.artifact.sha256) {
    throw new Error("The model ZIP SHA-256 does not match its finalized distribution manifest.");
  }
  const manifestSha256 = sha256(manifestBytes);

  const catalogVersionDirectory = path.join(catalogDirectory, "v1");
  await assertDirectory(catalogVersionDirectory, "Signed catalog directory");
  await assertExactNames(catalogDirectory, new Set(["v1"]), "Model catalog directory");
  await assertExactNames(catalogVersionDirectory, new Set(["catalog.signed.txt"]), "Signed catalog directory");
  const catalogPath = path.join(catalogVersionDirectory, "catalog.signed.txt");
  await assertRegularFile(catalogPath, "Signed catalog envelope");
  const catalogInfo = await lstat(catalogPath);
  if (catalogInfo.size < 1 || catalogInfo.size > MAX_SIGNED_CATALOG_BYTES) {
    throw new Error("The signed catalog must be between 1 byte and 384 KiB.");
  }
  const envelopeBytes = await readFile(catalogPath);
  const catalogPayload = verifyCatalogEnvelope(envelopeBytes, trustedPublicKey);
  assertCatalogMatchesManifest(catalogPayload, manifest, manifestBytes.length, manifestSha256);

  return {
    mode: "final",
    sourceRoot: root,
    archiveName,
    archivePath,
    archiveSizeBytes: archiveInfo.size,
    archiveSha256,
    manifestName: expectedManifestName,
    manifestPath,
    manifestSizeBytes: manifestBytes.length,
    manifestSha256,
    catalogPath,
    catalogSizeBytes: envelopeBytes.length,
    catalogSha256: sha256(envelopeBytes),
  };
}

export async function copyVerifiedModelPagesAssets(assets, outputRoot) {
  const output = path.resolve(outputRoot);
  if (assets.mode === "archive-bootstrap") {
    const archiveDestination = path.join(output, "model-distribution", assets.archiveName);
    await mkdir(path.dirname(archiveDestination), { recursive: true });
    await copyFile(assets.archivePath, archiveDestination, constants.COPYFILE_EXCL);
    const copiedArchive = await lstat(archiveDestination);
    if (copiedArchive.size !== assets.archiveSizeBytes || await hashFile(archiveDestination) !== assets.archiveSha256) {
      throw new Error("The bootstrap Cloudflare Pages archive changed while it was copied.");
    }
    return;
  }
  if (assets.mode !== "final") throw new Error("Unknown verified model Pages asset mode.");
  const archiveDestination = path.join(output, "model-distribution", assets.archiveName);
  const manifestDestination = path.join(output, "model-distribution", assets.manifestName);
  const catalogDestination = path.join(output, "model-catalog", "v1", "catalog.signed.txt");
  await mkdir(path.dirname(archiveDestination), { recursive: true });
  await mkdir(path.dirname(catalogDestination), { recursive: true });

  await copyFile(assets.archivePath, archiveDestination, constants.COPYFILE_EXCL);
  await copyFile(assets.manifestPath, manifestDestination, constants.COPYFILE_EXCL);
  await copyFile(assets.catalogPath, catalogDestination, constants.COPYFILE_EXCL);

  const copiedArchive = await lstat(archiveDestination);
  const copiedManifest = await readFile(manifestDestination);
  const copiedCatalog = await readFile(catalogDestination);
  if (copiedArchive.size !== assets.archiveSizeBytes || await hashFile(archiveDestination) !== assets.archiveSha256 ||
      copiedManifest.length !== assets.manifestSizeBytes || sha256(copiedManifest) !== assets.manifestSha256 ||
      copiedCatalog.length !== assets.catalogSizeBytes || sha256(copiedCatalog) !== assets.catalogSha256) {
    throw new Error("The staged Cloudflare Pages model assets changed while they were copied.");
  }
}

async function inspectModelPagesArchiveBootstrap(root) {
  await assertExactNames(root, new Set(["model-distribution"]), "Model Pages bootstrap staging root");
  const modelDirectory = path.join(root, "model-distribution");
  await assertDirectory(modelDirectory, "Model distribution directory");
  const entries = await readdir(modelDirectory);
  if (entries.length !== 2) {
    throw new Error("Model bootstrap staging must contain one ZIP and its local candidate manifest.");
  }
  const zipNames = entries.filter(name => name.endsWith(".zip"));
  const manifestNames = entries.filter(name => name.endsWith(".distribution.json"));
  if (zipNames.length !== 1 || manifestNames.length !== 1) {
    throw new Error("Model bootstrap staging must contain one ZIP and one local candidate manifest.");
  }
  const archiveName = zipNames[0];
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}\.zip$/.test(archiveName)) {
    throw new Error("The model ZIP name is not safe for a public download URL.");
  }
  const manifestName = `${archiveName.slice(0, -4)}.distribution.json`;
  if (manifestNames[0] !== manifestName) throw new Error("The model ZIP and local candidate manifest names do not match.");

  const archivePath = path.join(modelDirectory, archiveName);
  const manifestPath = path.join(modelDirectory, manifestName);
  await assertRegularFile(archivePath, "Model ZIP");
  await assertRegularFile(manifestPath, "Local candidate manifest");
  const archiveInfo = await lstat(archivePath);
  const manifestInfo = await lstat(manifestPath);
  if (archiveInfo.size < 1 || archiveInfo.size > MAX_PAGES_ASSET_BYTES) {
    throw new Error("The model ZIP exceeds Cloudflare Pages' 25 MiB per-asset limit.");
  }
  if (manifestInfo.size < 1 || manifestInfo.size > MAX_DISTRIBUTION_MANIFEST_BYTES) {
    throw new Error("The local candidate manifest must be between 1 byte and 1 MiB.");
  }
  const manifest = parseJson(await readFile(manifestPath), "Local candidate manifest");
  if (!manifest || manifest.schemaVersion !== 1 || manifest.state !== "local-candidate-not-published" ||
      !Array.isArray(manifest.downloadChannels) || manifest.downloadChannels.length !== 0 ||
      typeof manifest.packageId !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(manifest.packageId) ||
      manifest.model?.family !== "rife" || typeof manifest.model.id !== "string" ||
      typeof manifest.model.version !== "string" || typeof manifest.model.displayName !== "string" ||
      !/^[0-9a-f]{64}$/.test(manifest.model.modelSha256 || "") || manifest.artifact?.fileName !== archiveName ||
      manifest.artifact?.sizeBytes !== archiveInfo.size || !/^[0-9a-f]{64}$/.test(manifest.artifact?.sha256 || "")) {
    throw new Error("Bootstrap staging accepts only a local RIFE candidate with no claimed download channels.");
  }
  const archiveSha256 = await hashFile(archivePath);
  if (archiveSha256 !== manifest.artifact.sha256) {
    throw new Error("The bootstrap model ZIP SHA-256 does not match its local candidate manifest.");
  }
  return { mode: "archive-bootstrap", archiveName, archivePath, archiveSizeBytes: archiveInfo.size, archiveSha256 };
}

function assertDistributionManifest(manifest, archiveName, archiveSizeBytes) {
  if (!manifest || manifest.schemaVersion !== 1 || manifest.state !== "archive-channels-verified" ||
      typeof manifest.packageId !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(manifest.packageId) ||
      !manifest.model || manifest.model.family !== "rife" ||
      typeof manifest.model.id !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(manifest.model.id) ||
      typeof manifest.model.version !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(manifest.model.version) ||
      typeof manifest.model.displayName !== "string" || manifest.model.displayName.trim().length < 1 ||
      manifest.model.displayName.length > 128 ||
      !/^[0-9a-f]{64}$/.test(manifest.model.modelSha256 || "") ||
      manifest.artifact?.fileName !== archiveName || manifest.artifact?.sizeBytes !== archiveSizeBytes ||
      !/^[0-9a-f]{64}$/.test(manifest.artifact?.sha256 || "")) {
    throw new Error("The distribution manifest must identify a finalized RIFE archive with exact size and hashes.");
  }

  const manifestFileName = `${archiveName.slice(0, -4)}.distribution.json`;
  const channels = manifest.downloadChannels;
  if (!Array.isArray(channels) || channels.length < 2 || channels.length > 5) {
    throw new Error("The finalized distribution manifest must contain two to five download channels.");
  }
  const names = new Set();
  const hosts = new Set();
  let pagesChannelFound = false;
  for (const channel of channels) {
    if (!channel || typeof channel.name !== "string" || channel.name.trim().length < 1 || channel.name.length > 64 ||
        typeof channel.archiveUrl !== "string" || typeof channel.manifestUrl !== "string" ||
        names.has(channel.name.toLowerCase())) {
      throw new Error("Every finalized model channel needs a unique name and stable archive and manifest URLs.");
    }
    names.add(channel.name.toLowerCase());
    const archiveUrl = parseStableHttpsUrl(channel.archiveUrl, archiveName);
    const manifestUrl = parseStableHttpsUrl(channel.manifestUrl, manifestFileName);
    if (archiveUrl.hostname.toLowerCase() !== manifestUrl.hostname.toLowerCase()) {
      throw new Error("A model channel must serve its archive and manifest from the same host.");
    }
    hosts.add(archiveUrl.hostname.toLowerCase());
    if (archiveUrl.hostname.toLowerCase() === PAGES_HOST) {
      const expectedArchiveUrl = `https://${PAGES_HOST}/model-distribution/${archiveName}`;
      const expectedManifestUrl = `https://${PAGES_HOST}/model-distribution/${manifestFileName}`;
      if (channel.archiveUrl !== expectedArchiveUrl || channel.manifestUrl !== expectedManifestUrl) {
        throw new Error("The Cloudflare Pages model channel must use the configured update-log site paths.");
      }
      pagesChannelFound = true;
    }
  }
  if (hosts.size !== channels.length) {
    throw new Error("Model download channels must use distinct public hosts.");
  }
  if (!pagesChannelFound) {
    throw new Error("The finalized model channels do not include this Cloudflare Pages mirror.");
  }
}

function assertCatalogMatchesManifest(payload, manifest, manifestSizeBytes, manifestSha256) {
  const models = payload?.models;
  if (payload?.schemaVersion !== 1 || !Array.isArray(models) || models.length < 1 || models.length > 32) {
    throw new Error("The signed catalog payload must use schema version 1 and contain 1–32 models.");
  }
  const recommended = models.filter(model => model?.recommended === true);
  if (recommended.length !== 1) throw new Error("The signed catalog must contain exactly one recommended model.");
  const model = recommended[0];
  if (model.family !== "rife" || model.id !== manifest.model.id || model.version !== manifest.model.version ||
      model.name !== manifest.model.displayName || model.packageId !== manifest.packageId ||
      model.modelSha256 !== manifest.model.modelSha256 || model.manifest?.sizeBytes !== manifestSizeBytes ||
      model.manifest?.sha256 !== manifestSha256 || !Array.isArray(model.channels) ||
      model.channels.length !== manifest.downloadChannels.length) {
    throw new Error("The signed catalog recommendation does not match the finalized distribution manifest.");
  }
  for (let index = 0; index < model.channels.length; index += 1) {
    const expected = manifest.downloadChannels[index];
    const actual = model.channels[index];
    if (actual?.name !== expected.name || actual?.archiveUrl !== expected.archiveUrl || actual?.manifestUrl !== expected.manifestUrl) {
      throw new Error("The signed catalog channel list does not match the finalized distribution manifest.");
    }
  }
}

function verifyCatalogEnvelope(bytes, publicKey) {
  let text;
  try {
    text = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  } catch {
    throw new Error("The signed catalog envelope is not valid UTF-8.");
  }
  const lines = text.split("\n");
  if (lines.at(-1) === "") lines.pop();
  if (lines.length !== 4 || lines.some(line => line.includes("\r")) ||
      lines[0] !== "RIFELOVE MODEL CATALOG 1" || lines[1] !== `key-id=${CATALOG_KEY_ID}` ||
      !lines[2].startsWith("payload=") || !lines[3].startsWith("signature=")) {
    throw new Error("The signed catalog envelope has an unsupported format or key ID.");
  }
  const payloadBytes = decodeCanonicalBase64(lines[2].slice("payload=".length), "catalog payload");
  const signatureBytes = decodeCanonicalBase64(lines[3].slice("signature=".length), "catalog signature");
  if (payloadBytes.length < 1 || payloadBytes.length > 256 * 1024 || signatureBytes.length !== 64 ||
      !verify("sha256", payloadBytes, { key: publicKey, dsaEncoding: "ieee-p1363" }, signatureBytes)) {
    throw new Error("The signed catalog does not verify against the native client trust root.");
  }
  return parseJson(payloadBytes, "Signed catalog payload");
}

function decodeCanonicalBase64(value, description) {
  if (!value || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) {
    throw new Error(`The ${description} is not canonical base64.`);
  }
  const decoded = Buffer.from(value, "base64");
  if (decoded.toString("base64") !== value) throw new Error(`The ${description} is not canonical base64.`);
  return decoded;
}

function parseStableHttpsUrl(value, expectedName) {
  let url;
  try {
    url = new URL(value);
  } catch {
    throw new Error("Every model channel must use a stable HTTPS URL.");
  }
  const lastSegment = decodeURIComponent(url.pathname.slice(url.pathname.lastIndexOf("/") + 1));
  if (url.protocol !== "https:" || !url.hostname || url.username || url.password || url.search || url.hash ||
      lastSegment !== expectedName) {
    throw new Error("Every model channel must use an HTTPS URL ending in its exact artifact filename.");
  }
  return url;
}

async function assertDirectory(directoryPath, description) {
  let info;
  try {
    info = await lstat(directoryPath);
  } catch {
    throw new Error(`${description} is missing.`);
  }
  if (!info.isDirectory() || info.isSymbolicLink()) {
    throw new Error(`${description} must be a real directory without reparse points.`);
  }
}

async function assertRegularFile(filePath, description) {
  let info;
  try {
    info = await lstat(filePath);
  } catch {
    throw new Error(`${description} is missing.`);
  }
  if (!info.isFile() || info.isSymbolicLink()) {
    throw new Error(`${description} must be a regular file without reparse points.`);
  }
}

async function assertExactNames(directoryPath, expectedNames, description) {
  const entries = await readdir(directoryPath);
  if (entries.length !== expectedNames.size || entries.some(name => !expectedNames.has(name))) {
    throw new Error(`${description} contains unexpected or missing entries.`);
  }
}

function parseJson(bytes, description) {
  try {
    return JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  } catch {
    throw new Error(`${description} is not valid UTF-8 JSON.`);
  }
}

async function hashFile(filePath) {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(filePath)) hash.update(chunk);
  return hash.digest("hex");
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

async function loadOfficialCatalogKey() {
  const sharedVerifier = await readFile(new URL("../model_catalog/ModelRelease.Common.ps1", import.meta.url), "utf8");
  const roots = [...sharedVerifier.matchAll(/^\s*\$rootHex\s*=\s*'([0-9a-f]{128})'\s*$/gim)];
  if (roots.length !== 1) throw new Error("Could not load the single official catalog trust root from the shared verifier.");
  const point = Buffer.from(roots[0][1], "hex");
  return createPublicKey({
    key: {
      kty: "EC",
      crv: "P-256",
      x: point.subarray(0, 32).toString("base64url"),
      y: point.subarray(32).toString("base64url"),
      ext: true,
    },
    format: "jwk",
  });
}
