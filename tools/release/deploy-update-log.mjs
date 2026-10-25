import { createHash } from "node:crypto";
import { readFile, readdir, lstat } from "node:fs/promises";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { validateReleaseManifest } from "./release-manifest.mjs";
import { assertPublicObjectAccessAllowed } from "./oss-public-access.mjs";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const siteRoot = path.join(repoRoot, "release-site");
const requireFromReleaseSite = createRequire(path.join(siteRoot, "package.json"));
const OSS = requireFromReleaseSite("ali-oss");
const distRoot = path.resolve(siteRoot, "dist");
const storage = JSON.parse(await readFile(path.join(siteRoot, "storage.json"), "utf8"));
const manifest = JSON.parse(await readFile(path.join(siteRoot, "releases.json"), "utf8"));
validateReleaseManifest(manifest);

const bucket = process.env.OSS_BUCKET || storage.primary?.bucket;
const region = process.env.OSS_REGION || storage.primary?.region;
const endpoint = process.env.OSS_ENDPOINT || storage.primary?.endpoint || `https://${region}.aliyuncs.com`;
const prefix = (process.env.OSS_UPDATELOG_PREFIX || storage.primary?.updateLogPrefix || "rifelove-updatelog")
  .replace(/^\/+|\/+$/g, "");
const accessKeyId = process.env.OSS_ACCESS_KEY_ID;
const accessKeySecret = process.env.OSS_ACCESS_KEY_SECRET;
const dryRun = process.argv.includes("--dry-run");

if (!/^[a-z0-9][a-z0-9-]{1,61}[a-z0-9]$/.test(bucket) || !/^oss-[a-z0-9-]+$/.test(region) ||
    !/^https:\/\/[a-z0-9.-]+$/.test(endpoint) || !endpoint.endsWith(".aliyuncs.com")) {
  throw new Error("The configured OSS bucket, region or HTTPS endpoint is invalid");
}
if (!/^[a-z0-9][a-z0-9/_-]{1,99}$/.test(prefix)) throw new Error("The update log prefix is invalid");

const latest = [...manifest.releases].sort((left, right) =>
  right.date.localeCompare(left.date) || right.version.localeCompare(left.version))[0];
if (!latest) throw new Error("The release log has no versioned entries");

const siteInfo = await lstat(siteRoot);
const distInfo = await lstat(distRoot);
if (siteInfo.isSymbolicLink() || distInfo.isSymbolicLink() || !distInfo.isDirectory()) {
  throw new Error("Refusing to deploy a linked or invalid release-site/dist directory");
}

const files = await collectFiles(distRoot);
if (!files.some(file => file.relative === "index.html") || !files.some(file => file.relative === "releases.json")) {
  throw new Error("The built update log must contain index.html and releases.json");
}

const datedPrefix = `${prefix}/versions/${latest.version}-${latest.date}`;
const destinations = [prefix, datedPrefix];
const planned = [];
for (const destination of destinations) {
  for (const file of files) planned.push({ ...file, key: `${destination}/${file.relative}` });
}

const siteUrl = makePublicObjectUrl(bucket, region, `${prefix}/index.html`);
const versionUrl = makePublicObjectUrl(bucket, region, `${datedPrefix}/index.html`);
if (dryRun) {
  console.log(`Dry-run for RifeLove ${latest.version} (${latest.date}); downloads enabled: ${latest.downloadAvailable}`);
  for (const item of planned) console.log(`${item.key} (${item.sizeBytes} bytes)`);
  console.log(`Current URL: ${siteUrl}`);
  console.log(`Dated URL: ${versionUrl}`);
  process.exit(0);
}

if (!accessKeyId || !accessKeySecret) {
  throw new Error("Set OSS_ACCESS_KEY_ID and OSS_ACCESS_KEY_SECRET in the current process before deployment");
}

const client = new OSS({
  region,
  bucket,
  accessKeyId,
  accessKeySecret,
  authorizationV4: true,
  endpoint,
  secure: true,
  timeout: 120_000,
});

await assertPublicObjectAccessAllowed(client, bucket);

for (const item of planned) {
  const sha256 = await hashFile(item.filePath);
  const existing = await headObject(client, item.key);
  if (existing && existing.product !== "RifeLove") {
    throw new Error(`Refusing to overwrite an OSS object not marked as a RifeLove update-log object: ${item.key}`);
  }
  if (existing && existing.sizeBytes === item.sizeBytes && existing.sha256 === sha256) {
    continue;
  }
  if (item.key.startsWith(`${datedPrefix}/`) && existing) {
    throw new Error(`Refusing to replace a different dated update-log object: ${item.key}`);
  }
  await client.put(item.key, item.filePath, {
    headers: {
      "Content-Type": contentType(item.relative),
      "Cache-Control": item.relative === "index.html" || item.relative === "releases.json"
        ? "public, max-age=60, must-revalidate"
        : "public, max-age=31536000, immutable",
      "x-oss-object-acl": storage.primary?.publicObjectAcl || "public-read",
    },
    meta: { sha256, product: "RifeLove" },
  });
  await client.putACL(item.key, storage.primary?.publicObjectAcl || "public-read");
  const uploaded = await headObject(client, item.key);
  if (!uploaded || uploaded.sizeBytes !== item.sizeBytes || uploaded.sha256 !== sha256) {
    throw new Error(`OSS verification failed for ${item.key}`);
  }
  await verifyAnonymousDownload(makePublicObjectUrl(bucket, region, item.key), item.sizeBytes);
  console.log(`Verified OSS object ${item.key} (${item.sizeBytes} bytes, SHA-256 ${sha256})`);
}

for (const [url, file] of [[siteUrl, "index.html"], [versionUrl, "index.html"]]) {
  const expected = files.find(item => item.relative === file);
  await verifyAnonymousDownload(url, expected.sizeBytes);
}
console.log(`Update log is available at ${siteUrl}`);
console.log(`Version snapshot (${latest.version}, ${latest.date}) is available at ${versionUrl}`);

async function collectFiles(root, relative = "") {
  const entries = await readdir(path.join(root, relative), { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    const child = relative ? `${relative}/${entry.name}` : entry.name;
    if (entry.isSymbolicLink()) throw new Error(`Refusing to upload a symbolic link from the update log: ${child}`);
    if (entry.isDirectory()) {
      files.push(...await collectFiles(root, child));
      continue;
    }
    if (!entry.isFile()) throw new Error(`Refusing to upload a non-file update-log entry: ${child}`);
    if (!/\.(html|json|js|css|svg|png|ico|woff2)$/.test(entry.name)) {
      throw new Error(`The update log contains an unsupported file type: ${child}`);
    }
    const filePath = path.join(root, child);
    const info = await lstat(filePath);
    if (info.isSymbolicLink()) throw new Error(`Refusing to upload a symbolic link from the update log: ${child}`);
    files.push({ filePath, relative: child, sizeBytes: info.size });
  }
  return files.sort((left, right) => left.relative.localeCompare(right.relative));
}

async function headObject(oss, key) {
  try {
    const result = await oss.head(key);
    const headers = result?.res?.headers || {};
    return {
      sizeBytes: Number(headers["content-length"]),
      sha256: headers["x-oss-meta-sha256"],
      product: headers["x-oss-meta-product"],
    };
  } catch (error) {
    if (error?.status === 404 || error?.code === "NoSuchKey" || error?.code === "NotFound") return null;
    throw new Error(`Could not safely inspect the OSS update-log object (${error?.code || "storage error"})`);
  }
}

async function verifyAnonymousDownload(url, expectedSize) {
  try {
    const response = await fetch(url, { method: "HEAD", signal: AbortSignal.timeout(20_000) });
    if (!response.ok || Number(response.headers.get("content-length")) !== expectedSize) {
      throw new Error("anonymous object check failed");
    }
  } catch {
    throw new Error(`OSS update-log object is not publicly readable or has the wrong size: ${url}`);
  }
}

function contentType(fileName) {
  const extension = path.extname(fileName).toLowerCase();
  return ({
    ".html": "text/html; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".woff2": "font/woff2",
  })[extension];
}

function makePublicObjectUrl(targetBucket, targetRegion, objectKey) {
  const encodedKey = objectKey.split("/").map(encodeURIComponent).join("/");
  return `https://${targetBucket}.${targetRegion}.aliyuncs.com/${encodedKey}`;
}

async function hashFile(filePath) {
  const hash = createHash("sha256");
  hash.update(await readFile(filePath));
  return hash.digest("hex");
}
