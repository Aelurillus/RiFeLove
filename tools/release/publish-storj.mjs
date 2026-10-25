import { createReadStream } from "node:fs";
import { readFile, rename, stat, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { mergePublishedRelease, validateVersion } from "./release-manifest.mjs";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const siteRoot = path.join(repoRoot, "release-site");
const requireFromReleaseSite = createRequire(path.join(siteRoot, "package.json"));
const { HeadObjectCommand, S3Client } = requireFromReleaseSite("@aws-sdk/client-s3");
const { Upload } = requireFromReleaseSite("@aws-sdk/lib-storage");
const manifestPath = path.join(siteRoot, "releases.json");
const policyPath = path.join(siteRoot, "release-policy.json");
const options = parseArguments(process.argv.slice(2));

if (!options.archive || !options.release) {
  throw new Error("Usage: node tools/release/publish-storj.mjs --archive <zip> --release <metadata.json>");
}

const archivePath = path.resolve(options.archive);
const releaseMetadataPath = path.resolve(options.release);
const archiveInfo = await stat(archivePath);
if (!archiveInfo.isFile() || archiveInfo.size <= 0) throw new Error("Release archive must be a non-empty file");
const release = JSON.parse(await readFile(releaseMetadataPath, "utf8"));
validateRelease(release);
const version = validateVersion(release.version);
const fileName = `RifeLove-${version}-win-x64.zip`;
if (path.basename(archivePath) !== fileName) throw new Error(`Archive must be named ${fileName}`);
const manifest = JSON.parse(await readFile(manifestPath, "utf8"));
const policy = JSON.parse(await readFile(policyPath, "utf8"));
const sha256 = await hashFile(archivePath);
const artifact = {
  fileName,
  key: `releases/${version}/${fileName}`,
  sha256,
  sizeBytes: archiveInfo.size,
};
const publishedAt = release.publishedAt || `${release.date}T12:00:00.000Z`;
const merged = mergePublishedRelease(manifest, release, artifact, publishedAt, policy);

const bucket = process.env.STORJ_BUCKET;
const accessKeyId = process.env.STORJ_UPLOAD_ACCESS_KEY;
const secretAccessKey = process.env.STORJ_UPLOAD_SECRET_KEY;
if (!bucket || !accessKeyId || !secretAccessKey) {
  throw new Error("Set STORJ_BUCKET, STORJ_UPLOAD_ACCESS_KEY, and STORJ_UPLOAD_SECRET_KEY in this shell first");
}

const client = new S3Client({
  endpoint: process.env.STORJ_ENDPOINT || "https://gateway.storjshare.io",
  region: "us-east-1",
  forcePathStyle: true,
  credentials: { accessKeyId, secretAccessKey },
  requestChecksumCalculation: "WHEN_REQUIRED",
  responseChecksumValidation: "WHEN_REQUIRED",
});
let exists = false;
try {
  const existing = await client.send(new HeadObjectCommand({ Bucket: bucket, Key: artifact.key }));
  exists = true;
  if (existing.ContentLength !== artifact.sizeBytes || existing.Metadata?.sha256 !== sha256) {
    throw new Error("That release key already exists with different contents; refusing to replace it");
  }
} catch (error) {
  if (exists) throw error;
  if (error?.name !== "NotFound" && error?.$metadata?.httpStatusCode !== 404) {
    throw new Error("Could not safely check the Storj release key; verify bucket and key permissions");
  }
}

if (!exists) {
  try {
    const upload = new Upload({
      client,
      params: {
        Bucket: bucket,
        Key: artifact.key,
        Body: createReadStream(archivePath),
        ContentLength: artifact.sizeBytes,
        ContentType: "application/zip",
        ContentDisposition: `attachment; filename="${fileName}"`,
        CacheControl: "public, max-age=31536000, immutable",
        Metadata: { sha256 },
      },
      partSize: 64 * 1024 * 1024,
      queueSize: 2,
      leavePartsOnError: false,
    });
    await upload.done();
    const uploaded = await client.send(new HeadObjectCommand({ Bucket: bucket, Key: artifact.key }));
    if (uploaded.ContentLength !== artifact.sizeBytes || uploaded.Metadata?.sha256 !== sha256) {
      throw new Error("Uploaded object verification did not match the release archive");
    }
  } catch (error) {
    if (error?.message === "Uploaded object verification did not match the release archive") throw error;
    throw new Error(`Storj upload failed (${error?.name || "storage error"}); the release log was not changed`);
  }
}

const temporaryPath = `${manifestPath}.${process.pid}.tmp`;
await writeFile(temporaryPath, `${JSON.stringify(merged.manifest, null, 2)}\n`, { flag: "wx" });
try {
  await rename(temporaryPath, manifestPath);
} catch (error) {
  throw new Error(`The archive is stored, but the local release log could not be updated (${error?.code || "file error"})`);
}

console.log(`Uploaded and verified ${fileName} (${artifact.sizeBytes} bytes, SHA-256 ${sha256}).`);
console.log(`Updated ${path.relative(repoRoot, manifestPath)}. Review it, then deploy the update log.`);
if (merged.removedKeys.length) {
  console.log(`${merged.removedKeys.length} older archive(s) are now marked unavailable; remove them from Storj only after the updated page is deployed.`);
}

async function hashFile(filePath) {
  const { createHash } = await import("node:crypto");
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
}

function parseArguments(args) {
  const result = {};
  for (let index = 0; index < args.length; index += 1) {
    const key = args[index];
    if (!new Set(["--archive", "--release"]).has(key) || !args[index + 1]) {
      throw new Error("Usage: node tools/release/publish-storj.mjs --archive <zip> --release <metadata.json>");
    }
    if (result[key.slice(2)]) throw new Error(`Specify ${key} only once`);
    result[key.slice(2)] = args[++index];
  }
  if (Object.keys(result).length !== 2) throw new Error("Specify exactly one --archive and one --release");
  return result;
}
