import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { createRequire } from "node:module";
import { lstat, readFile, rename, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { validateVersion } from "./release-manifest.mjs";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const siteRoot = path.join(repoRoot, "release-site");
const requireFromReleaseSite = createRequire(path.join(siteRoot, "package.json"));
const OSS = requireFromReleaseSite("ali-oss");
const storage = JSON.parse(await readFile(path.join(siteRoot, "storage.json"), "utf8"));
const options = parseArguments(process.argv.slice(2));
const version = validateVersion(options.version);
const archivePath = path.resolve(options.archive);
const fileName = `RifeLove-${version}-win-x64.zip`;
if (path.basename(archivePath) !== fileName) throw new Error(`Archive must be named ${fileName}`);
const archiveInfo = await stat(archivePath);
if (!archiveInfo.isFile() || archiveInfo.size <= 0) throw new Error("Preview archive must be a non-empty file");
const parallel = Number(process.env.OSS_UPLOAD_PARALLEL || 2);
if (!Number.isInteger(parallel) || parallel < 1 || parallel > 8) throw new Error("OSS_UPLOAD_PARALLEL must be an integer from 1 to 8");

const bucket = process.env.OSS_BUCKET || storage.primary?.bucket;
const region = process.env.OSS_REGION || storage.primary?.region;
const endpoint = process.env.OSS_ENDPOINT || storage.primary?.endpoint || `https://${region}.aliyuncs.com`;
const accessKeyId = process.env.OSS_ACCESS_KEY_ID;
const accessKeySecret = process.env.OSS_ACCESS_KEY_SECRET;
if (!bucket || !region || !accessKeyId || !accessKeySecret) throw new Error("Set OSS bucket, region and credentials in the current process.");
if (!/^https:\/\/[a-z0-9.-]+$/.test(endpoint) || !endpoint.endsWith(".aliyuncs.com")) throw new Error("OSS_ENDPOINT must be an HTTPS Aliyun OSS endpoint");

const key = `releases/${version}/${fileName}`;
const sha256 = await hashFile(archivePath);
const checkpointPath = path.join(path.dirname(archivePath), `.${fileName}.oss-upload.json`);
const checkpointTempPath = `${checkpointPath}.tmp`;
const client = new OSS({ region, bucket, accessKeyId, accessKeySecret, authorizationV4: true, endpoint, secure: true, timeout: 600_000, retryMax: 5 });
const existing = await headObject(client, key);
let lastProgress = 0;
let checkpointWrite = Promise.resolve();
if (existing) {
  verifyObject(existing, archiveInfo.size, sha256);
  const existingAcl = await client.getACL(key);
  if (existingAcl.acl !== "private") throw new Error("The existing preview object is not private; refusing to change its ACL.");
} else {
  const savedCheckpoint = await readCheckpoint(checkpointPath);
  if (savedCheckpoint) {
    verifyCheckpoint(savedCheckpoint, archivePath, key, archiveInfo.size, sha256);
    lastProgress = Math.floor((savedCheckpoint.checkpoint.doneParts.length / Math.ceil(archiveInfo.size / savedCheckpoint.checkpoint.partSize)) * 100);
    console.log(`Resuming the private preview upload at approximately ${lastProgress}%`);
  } else {
    await abortUncheckpointedUploads(client, key);
  }
  await client.multipartUpload(key, archivePath, {
    parallel,
    partSize: 16 * 1024 * 1024,
    mime: "application/zip",
    meta: { sha256, product: "RifeLove", "size-bytes": String(archiveInfo.size), visibility: "private-preview" },
    headers: {
      "Content-Disposition": `attachment; filename="${fileName}"`,
      "Cache-Control": "private, max-age=31536000, immutable",
      "x-oss-object-acl": "private",
    },
    progress: async (percentage, checkpoint) => {
      if (checkpoint?.uploadId) {
        checkpointWrite = checkpointWrite.then(() => writeCheckpoint(checkpointPath, checkpointTempPath, {
          product: "RifeLove",
          key,
          sizeBytes: archiveInfo.size,
          sha256,
          checkpoint,
        }));
        await checkpointWrite;
      }
      const step = Math.floor(percentage * 100);
      if (step > lastProgress) {
        lastProgress = step;
        console.log(`Private preview upload: ${Math.min(step, 100)}%`);
      }
    },
    checkpoint: savedCheckpoint?.checkpoint,
  });
  console.log("Multipart transfer complete; verifying remote size, hash, private ACL and anonymous access.");
}

await client.putACL(key, "private");
const uploaded = await headObject(client, key);
verifyObject(uploaded, archiveInfo.size, sha256);
const acl = await client.getACL(key);
if (acl.acl !== "private") throw new Error("The preview archive ACL is not private after upload.");
await verifyAnonymousAccessDenied(`https://${bucket}.${region}.aliyuncs.com/${key.split("/").map(encodeURIComponent).join("/")}`);
await rm(checkpointPath, { force: true });
await rm(checkpointTempPath, { force: true });
console.log(`Stored private RifeLove ${version} preview in OSS (${archiveInfo.size} bytes, SHA-256 ${sha256}).`);
console.log("The update log remains non-downloadable until Windows distribution checks and dependency/model redistribution reviews pass.");

async function headObject(client, objectKey) {
  try {
    const result = await client.head(objectKey);
    const headers = result?.res?.headers || {};
    return {
      sizeBytes: Number(headers["content-length"]),
      sha256: headers["x-oss-meta-sha256"],
      product: headers["x-oss-meta-product"],
      declaredSize: Number(headers["x-oss-meta-size-bytes"] ?? headers["x-oss-meta-sizebytes"]),
    };
  } catch (error) {
    if (error?.status === 404 || error?.code === "NoSuchKey" || error?.code === "NotFound") return null;
    throw new Error(`Could not safely inspect the OSS release object (${error?.code || "storage error"})`);
  }
}

function verifyObject(object, expectedSize, expectedHash) {
  if (!object || Number(object.sizeBytes) !== expectedSize || object.declaredSize !== expectedSize ||
      object.sha256 !== expectedHash || object.product !== "RifeLove") {
    throw new Error("The OSS release key already contains different contents; refusing to replace it.");
  }
}

async function readCheckpoint(filePath) {
  try {
    const info = await lstat(filePath);
    if (info.isSymbolicLink() || !info.isFile()) throw new Error("The OSS upload checkpoint must be a regular file.");
    return JSON.parse(await readFile(filePath, "utf8"));
  } catch (error) {
    if (error?.code === "ENOENT") return null;
    throw new Error("The saved OSS upload checkpoint could not be safely read.");
  }
}

function verifyCheckpoint(saved, file, objectKey, expectedSize, expectedHash) {
  const checkpoint = saved?.checkpoint;
  if (saved?.product !== "RifeLove" || saved?.key !== objectKey || saved?.sizeBytes !== expectedSize ||
      saved?.sha256 !== expectedHash || !checkpoint || checkpoint.name !== objectKey ||
      path.resolve(checkpoint.file) !== file || checkpoint.fileSize !== expectedSize ||
      checkpoint.partSize !== 16 * 1024 * 1024 || !checkpoint.uploadId || !Array.isArray(checkpoint.doneParts)) {
    throw new Error("The saved OSS upload checkpoint does not match this archive; refusing to resume it.");
  }
}

async function writeCheckpoint(filePath, temporaryPath, value) {
  await writeFile(temporaryPath, `${JSON.stringify(value)}\n`, { encoding: "utf8", mode: 0o600 });
  await rename(temporaryPath, filePath);
}

async function abortUncheckpointedUploads(client, objectKey) {
  const page = await client.listUploads({ prefix: objectKey, "max-uploads": 1000 });
  if (page.isTruncated) throw new Error("Too many incomplete OSS uploads share this release key; clean them up before retrying.");
  for (const upload of page.uploads.filter(item => item.name === objectKey)) {
    await client.abortMultipartUpload(objectKey, upload.uploadId);
    console.log("Cleared an incomplete multipart upload for this exact private preview key.");
  }
}

async function verifyAnonymousAccessDenied(url) {
  try {
    const response = await fetch(url, { method: "HEAD", signal: AbortSignal.timeout(20_000) });
    if (![401, 403, 404].includes(response.status)) {
      throw new Error("Anonymous access to the private preview was not denied.");
    }
  } catch (error) {
    if (error?.message === "Anonymous access to the private preview was not denied.") throw error;
    throw new Error("Could not verify anonymous access denial for the private preview object.");
  }
}

async function hashFile(filePath) {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(filePath)) hash.update(chunk);
  return hash.digest("hex");
}

function parseArguments(args) {
  const result = {};
  for (let index = 0; index < args.length; index += 1) {
    const key = args[index];
    if (!new Set(["--archive", "--version"]).has(key) || !args[index + 1]) {
      throw new Error("Usage: node tools/release/upload-private-preview.mjs --archive <zip> --version <version>");
    }
    if (result[key.slice(2)]) throw new Error(`Specify ${key} only once`);
    result[key.slice(2)] = args[++index];
  }
  if (!result.archive || !result.version) throw new Error("Specify exactly one --archive and one --version");
  return result;
}
