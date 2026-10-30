import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { readFile, rename, stat, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { mergePublishedRelease, validateVersion } from "./release-manifest.mjs";
import { assertPublicObjectAccessAllowed } from "./oss-public-access.mjs";
import { verifyAnonymousObject } from "./anonymous-object-verification.mjs";
import { validateReleaseArchive } from "./release-archive-safety.mjs";
import { failAliyunOssDisabled } from "./oss-disabled.mjs";

failAliyunOssDisabled();

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const siteRoot = path.join(repoRoot, "release-site");
const requireFromReleaseSite = createRequire(path.join(siteRoot, "package.json"));
const OSS = requireFromReleaseSite("ali-oss");
const { HeadObjectCommand, S3Client } = requireFromReleaseSite("@aws-sdk/client-s3");
const { Upload } = requireFromReleaseSite("@aws-sdk/lib-storage");
const manifestPath = path.join(siteRoot, "releases.json");
const policyPath = path.join(siteRoot, "release-policy.json");
const storagePath = path.join(siteRoot, "storage.json");
const options = parseArguments(process.argv.slice(2));
if (!options.archive || !options.release) {
  throw new Error("Usage: node tools/release/publish-release.mjs --archive <zip> --release <metadata.json>");
}

const archivePath = path.resolve(options.archive);
const releaseMetadataPath = path.resolve(options.release);
const archiveInfo = await stat(archivePath);
if (!archiveInfo.isFile() || archiveInfo.size <= 0) throw new Error("Release archive must be a non-empty file");
const archiveSafety = await validateReleaseArchive(archivePath);
console.log(`Release archive privacy inspection passed (${archiveSafety.entryCount} entries).`);
const release = JSON.parse(await readFile(releaseMetadataPath, "utf8"));
validateRelease(release);
const version = validateVersion(release.version);
const fileName = `RifeLove-${version}-win-x64.zip`;
if (path.basename(archivePath) !== fileName) throw new Error(`Archive must be named ${fileName}`);

const storage = JSON.parse(await readFile(storagePath, "utf8"));
const bucket = process.env.OSS_BUCKET || storage.primary?.bucket;
const region = process.env.OSS_REGION || storage.primary?.region;
const endpoint = process.env.OSS_ENDPOINT || storage.primary?.endpoint || `https://${region}.aliyuncs.com`;
const accessKeyId = process.env.OSS_ACCESS_KEY_ID;
const accessKeySecret = process.env.OSS_ACCESS_KEY_SECRET;
if (!bucket || !/^[a-z0-9][a-z0-9-]{1,61}[a-z0-9]$/.test(bucket) || !region || !/^oss-[a-z0-9-]+$/.test(region) || !accessKeyId || !accessKeySecret) {
  throw new Error("Set OSS_BUCKET, OSS_REGION, OSS_ACCESS_KEY_ID, and OSS_ACCESS_KEY_SECRET in this shell first");
}
if (!/^https:\/\/[a-z0-9.-]+$/.test(endpoint) || !endpoint.endsWith(".aliyuncs.com")) {
  throw new Error("OSS_ENDPOINT must be an HTTPS Aliyun OSS endpoint");
}

const key = `releases/${version}/${fileName}`;
const sha256 = await hashFile(archivePath);
const downloadUrl = makePublicObjectUrl(bucket, region, key);
const artifact = { fileName, key, sha256, sizeBytes: archiveInfo.size, downloadUrl };
const manifest = JSON.parse(await readFile(manifestPath, "utf8"));
const policy = JSON.parse(await readFile(policyPath, "utf8"));
const publishedAt = release.publishedAt || `${release.date}T12:00:00.000Z`;
const metadata = { ...release, storjBackup: release.storjBackup === true };
const merged = mergePublishedRelease(manifest, metadata, artifact, publishedAt, policy);

const oss = new OSS({
  region,
  bucket,
  accessKeyId,
  accessKeySecret,
  authorizationV4: true,
  endpoint,
  secure: true,
  timeout: 120_000,
});

await assertPublicObjectAccessAllowed(oss, bucket);

const existingOss = await headOssObject(oss, key);
if (existingOss) {
  verifyObject(existingOss.sizeBytes, existingOss.sha256, archiveInfo.size, sha256, "OSS");
  await oss.putACL(key, "public-read");
} else {
  await uploadToOss(oss, archivePath, key, fileName, archiveInfo.size, sha256);
}
await verifyAnonymousObject(downloadUrl, archiveInfo.size, sha256);

if (metadata.storjBackup) {
  const storjBucket = process.env.STORJ_BUCKET || storage.backup?.storjBucket;
  const storjAccessKeyId = process.env.STORJ_UPLOAD_ACCESS_KEY;
  const storjSecretAccessKey = process.env.STORJ_UPLOAD_SECRET_KEY;
  if (!storjBucket || !storjAccessKeyId || !storjSecretAccessKey) {
    throw new Error("This release is marked for Storj backup; set STORJ_BUCKET and Storj upload credentials first");
  }
  await uploadToStorj({
    archivePath,
    key,
    fileName,
    sizeBytes: archiveInfo.size,
    sha256,
    bucket: storjBucket,
    accessKeyId: storjAccessKeyId,
    secretAccessKey: storjSecretAccessKey,
    endpoint: process.env.STORJ_ENDPOINT || "https://gateway.storjshare.io",
  });
}

const temporaryPath = `${manifestPath}.${process.pid}.tmp`;
await writeFile(temporaryPath, `${JSON.stringify(merged.manifest, null, 2)}\n`, { flag: "wx" });
try {
  await rename(temporaryPath, manifestPath);
} catch (error) {
  throw new Error(`The package is stored, but the local release log could not be updated (${error?.code || "file error"})`);
}

console.log(`Published ${fileName} to OSS (${archiveInfo.size} bytes, SHA-256 ${sha256}).`);
if (metadata.storjBackup) console.log("Verified a private Storj backup for this major release.");
console.log(`Updated ${path.relative(repoRoot, manifestPath)}. Review it, then deploy the update log.`);
if (merged.removedKeys.length) {
  console.log(`${merged.removedKeys.length} older archive(s) are now marked unavailable; delete them only after deploying the updated page.`);
}

async function uploadToOss(client, filePath, objectKey, objectFileName, sizeBytes, fileHash) {
  try {
    const options = {
      parallel: 2,
      partSize: 64 * 1024 * 1024,
      mime: "application/zip",
      meta: { sha256: fileHash, product: "RifeLove", "size-bytes": String(sizeBytes) },
      headers: {
        "Content-Disposition": `attachment; filename="${objectFileName}"`,
        "Cache-Control": "public, max-age=31536000, immutable",
        "x-oss-object-acl": "public-read",
      },
      progress: async percentage => {
        const step = Math.floor(percentage * 10);
        if (step > (uploadToOss.lastProgress || 0)) {
          uploadToOss.lastProgress = step;
          console.log(`OSS upload: ${Math.min(step * 10, 100)}%`);
        }
      },
    };
    await client.multipartUpload(objectKey, filePath, options);
    const uploaded = await headOssObject(client, objectKey);
    if (!uploaded) throw new Error("OSS object did not appear after upload");
    verifyObject(uploaded.sizeBytes, uploaded.sha256, sizeBytes, fileHash, "OSS");
    await client.putACL(objectKey, "public-read");
  } catch (error) {
    throw new Error(`OSS upload or verification failed (${error?.name || "storage error"}); the update log was not changed`);
  }
}

async function uploadToStorj({ archivePath: filePath, key: objectKey, fileName: objectFileName, sizeBytes, sha256: fileHash, bucket: targetBucket, accessKeyId: targetAccessKey, secretAccessKey: targetSecret, endpoint }) {
  const client = new S3Client({
    endpoint,
    region: "us-east-1",
    forcePathStyle: true,
    credentials: { accessKeyId: targetAccessKey, secretAccessKey: targetSecret },
    requestChecksumCalculation: "WHEN_REQUIRED",
    responseChecksumValidation: "WHEN_REQUIRED",
  });
  const existing = await headStorjObject(client, targetBucket, objectKey);
  if (existing) {
    verifyObject(existing.ContentLength, existing.Metadata?.sha256, sizeBytes, fileHash, "Storj");
    return;
  }
  try {
    const upload = new Upload({
      client,
      params: {
        Bucket: targetBucket,
        Key: objectKey,
        Body: createReadStream(filePath),
        ContentLength: sizeBytes,
        ContentType: "application/zip",
        ContentDisposition: `attachment; filename="${objectFileName}"`,
        Metadata: { sha256: fileHash },
      },
      partSize: 64 * 1024 * 1024,
      queueSize: 2,
      leavePartsOnError: false,
    });
    await upload.done();
    const uploaded = await client.send(new HeadObjectCommand({ Bucket: targetBucket, Key: objectKey }));
    verifyObject(uploaded.ContentLength, uploaded.Metadata?.sha256, sizeBytes, fileHash, "Storj");
  } catch (error) {
    throw new Error(`Storj backup failed (${error?.name || "storage error"}); the release log was not changed`);
  }
}

async function headOssObject(client, objectKey) {
  try {
    const result = await client.head(objectKey);
    const headers = result?.res?.headers || {};
    return {
      sizeBytes: Number(headers["content-length"]),
      sha256: headers["x-oss-meta-sha256"],
    };
  } catch (error) {
    if (error?.status === 404 || error?.code === "NoSuchKey" || error?.code === "NotFound") return null;
    throw new Error(`Could not safely check the OSS release object (${error?.code || "storage error"})`);
  }
}

async function headStorjObject(client, targetBucket, objectKey) {
  try {
    return await client.send(new HeadObjectCommand({ Bucket: targetBucket, Key: objectKey }));
  } catch (error) {
    if (error?.name === "NotFound" || error?.$metadata?.httpStatusCode === 404) return null;
    throw new Error("Could not safely check the Storj backup object; verify bucket and permissions");
  }
}

function verifyObject(actualSize, actualHash, expectedSize, expectedHash, provider) {
  if (Number(actualSize) !== expectedSize || actualHash !== expectedHash) {
    throw new Error(`${provider} already has an object at this release key with different contents`);
  }
}

function makePublicObjectUrl(targetBucket, targetRegion, objectKey) {
  const encodedKey = objectKey.split("/").map(encodeURIComponent).join("/");
  return `https://${targetBucket}.${targetRegion}.aliyuncs.com/${encodedKey}`;
}

async function hashFile(filePath) {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(filePath)) hash.update(chunk);
  return hash.digest("hex");
}

function validateRelease(release) {
  if (!release || release.releaseReady !== true) {
    throw new Error("Release metadata must set releaseReady to true only after Windows distribution checks pass");
  }
  if (!/^\d{4}-\d{2}-\d{2}$/.test(release.date || "") || Number.isNaN(Date.parse(`${release.date}T00:00:00Z`))) {
    throw new Error("Release metadata needs an exact YYYY-MM-DD release date");
  }
  for (const field of ["channel", "title", "summary"]) {
    if (typeof release[field] !== "string" || !release[field].trim()) throw new Error(`Release metadata needs ${field}`);
  }
  if (!Array.isArray(release.highlights) || release.highlights.length === 0 ||
      release.highlights.some(item => typeof item !== "string" || !item.trim())) {
    throw new Error("Release metadata needs a non-empty highlights list");
  }
  if (release.storjBackup !== undefined && typeof release.storjBackup !== "boolean") {
    throw new Error("storjBackup must be true or false");
  }
}

function parseArguments(args) {
  const result = {};
  for (let index = 0; index < args.length; index += 1) {
    const key = args[index];
    if (!new Set(["--archive", "--release"]).has(key) || !args[index + 1]) {
      throw new Error("Usage: node tools/release/publish-release.mjs --archive <zip> --release <metadata.json>");
    }
    if (result[key.slice(2)]) throw new Error(`Specify ${key} only once`);
    result[key.slice(2)] = args[++index];
  }
  if (Object.keys(result).length !== 2) throw new Error("Specify exactly one --archive and one --release");
  return result;
}
