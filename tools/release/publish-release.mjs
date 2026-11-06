import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { lstat, readFile, rename, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { mergePublishedRelease, validateReleaseManifest, validateVersion } from "./release-manifest.mjs";
import {
  assertGitHubReleaseAsset,
  buildReleaseBody,
  createReleaseArtifact,
  createUploadUrl,
  MAX_GITHUB_RELEASE_ASSET_BYTES,
  verifyAnonymousReleaseAsset,
} from "./github-release-client.mjs";
import { validateReleaseArchive } from "./release-archive-safety.mjs";

const API_ROOT = "https://api.github.com";
const API_VERSION = "2022-11-28";
const OFFICIAL_REPOSITORY = "Aelurillus/RiFeLove";
const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const siteRoot = path.join(repoRoot, "release-site");
const manifestPath = path.join(siteRoot, "releases.json");
const policyPath = path.join(siteRoot, "release-policy.json");
const options = parseArguments(process.argv.slice(2));

if (!options.archive || !options.release) {
  throw new Error("Usage: node tools/release/publish-release.mjs --archive <zip> --release <metadata.json> [--dry-run]");
}

const archivePath = path.resolve(options.archive);
const releaseMetadataPath = path.resolve(options.release);
const archiveInfo = await lstat(archivePath);
if (!archiveInfo.isFile() || archiveInfo.isSymbolicLink() || archiveInfo.size <= 0 || archiveInfo.size >= MAX_GITHUB_RELEASE_ASSET_BYTES) {
  throw new Error("The Windows release archive must be a non-empty regular file smaller than 2 GiB");
}
const archiveSafety = await validateReleaseArchive(archivePath);
console.log(`Release archive privacy inspection passed (${archiveSafety.entryCount} entries).`);

const release = JSON.parse(await readFile(releaseMetadataPath, "utf8"));
validateRelease(release);
const releasePolicy = JSON.parse(await readFile(policyPath, "utf8"));
if (releasePolicy.maxGitHubReleaseAssetBytesExclusive !== MAX_GITHUB_RELEASE_ASSET_BYTES ||
    releasePolicy.retainPublishedReleases !== true) {
  throw new Error("The release policy must match GitHub's asset limit and retain published versions");
}
const version = validateVersion(release.version);
const artifact = createReleaseArtifact(version, {
  repository: OFFICIAL_REPOSITORY,
  sha256: await hashFile(archivePath),
  sizeBytes: archiveInfo.size,
});
if (path.basename(archivePath) !== artifact.fileName) throw new Error(`Archive must be named ${artifact.fileName}`);
const releaseBody = buildReleaseBody(release, artifact);
const checksumName = `${artifact.fileName}.sha256`;
const checksumBody = Buffer.from(`${artifact.sha256}  ${artifact.fileName}\n`, "utf8");
const checksumHash = hashBuffer(checksumBody);

if (options.dryRun) {
  const manifest = JSON.parse(await readFile(manifestPath, "utf8"));
  validateReleaseManifest(manifest);
  mergePublishedRelease(manifest, release, artifact, `${release.date}T12:00:00.000Z`);
  console.log(`Dry run only: ${artifact.releaseTag} would publish ${artifact.fileName} (${artifact.sizeBytes} bytes, SHA-256 ${artifact.sha256}).`);
  console.log(`Download URL: ${artifact.downloadUrl}`);
  process.exit(0);
}

const token = process.env.GH_TOKEN || process.env.GITHUB_TOKEN;
if (!token) throw new Error("Set GH_TOKEN or GITHUB_TOKEN with contents:write access to publish a GitHub Release");
const repository = process.env.GITHUB_REPOSITORY || OFFICIAL_REPOSITORY;
if (repository !== OFFICIAL_REPOSITORY) throw new Error(`This publisher is restricted to ${OFFICIAL_REPOSITORY}`);
const repositoryPath = repository.split("/").map(encodeURIComponent).join("/");
const prerelease = release.channel.toLowerCase() !== "stable";
const tag = artifact.releaseTag;
const releaseEndpoint = `/repos/${repositoryPath}/releases/tags/${encodeURIComponent(tag)}`;
const existingRelease = await githubRequest(releaseEndpoint, { token, allowNotFound: true });
let remoteRelease = existingRelease;

if (!remoteRelease) {
  remoteRelease = await githubRequest(`/repos/${repositoryPath}/releases`, {
    token,
    method: "POST",
    body: {
      tag_name: tag,
      target_commitish: "main",
      name: release.title,
      body: releaseBody,
      draft: true,
      prerelease,
    },
  });
} else if (remoteRelease.tag_name !== tag) {
  throw new Error("GitHub returned a release for a different tag");
}

if (!Number.isSafeInteger(remoteRelease.id) || remoteRelease.id <= 0 || !Array.isArray(remoteRelease.assets)) {
  throw new Error("GitHub returned an incomplete release record");
}
if (!remoteRelease.draft && (remoteRelease.name !== release.title || remoteRelease.body !== releaseBody || remoteRelease.prerelease !== prerelease)) {
  throw new Error("A published release with this version already exists but has different release notes");
}
if (remoteRelease.draft) {
  remoteRelease = await githubRequest(`/repos/${repositoryPath}/releases/${remoteRelease.id}`, {
    token,
    method: "PATCH",
    body: { name: release.title, body: releaseBody, prerelease, draft: true },
  });
}

const expectedArchiveAsset = {
  name: artifact.fileName,
  sizeBytes: artifact.sizeBytes,
  sha256: artifact.sha256,
  downloadUrl: artifact.downloadUrl,
};
const expectedChecksumAsset = {
  name: checksumName,
  sizeBytes: checksumBody.length,
  sha256: checksumHash,
  downloadUrl: `${artifact.downloadUrl}.sha256`,
};
let assets = remoteRelease.assets;
assets = await ensureReleaseAsset(remoteRelease, assets, expectedArchiveAsset, archivePath, "application/zip", token, repository);
assets = await ensureReleaseAsset(remoteRelease, assets, expectedChecksumAsset, checksumBody, "text/plain; charset=utf-8", token, repository);

if (remoteRelease.draft) {
  remoteRelease = await githubRequest(`/repos/${repositoryPath}/releases/${remoteRelease.id}`, {
    token,
    method: "PATCH",
    body: { draft: false, prerelease },
  });
}
remoteRelease = await githubRequest(releaseEndpoint, { token });
if (remoteRelease.draft || remoteRelease.tag_name !== tag) {
  throw new Error("GitHub did not publish the expected release");
}

const publishedAssets = remoteRelease.assets || assets;
assertGitHubReleaseAsset(findAsset(publishedAssets, artifact.fileName), expectedArchiveAsset);
assertGitHubReleaseAsset(findAsset(publishedAssets, checksumName), expectedChecksumAsset);
await verifyAnonymousReleaseAsset(artifact.downloadUrl, { sizeBytes: artifact.sizeBytes });
await verifyAnonymousReleaseAsset(expectedChecksumAsset.downloadUrl, { sizeBytes: checksumBody.length });

const manifest = JSON.parse(await readFile(manifestPath, "utf8"));
const publishedAt = remoteRelease.published_at;
const merged = mergePublishedRelease(manifest, release, artifact, publishedAt);
const manifestInfo = await lstat(manifestPath);
if (!manifestInfo.isFile() || manifestInfo.isSymbolicLink()) throw new Error("The release manifest must remain a regular, non-linked file");
const temporaryPath = `${manifestPath}.${process.pid}.tmp`;
await writeFile(temporaryPath, `${JSON.stringify(merged.manifest, null, 2)}\n`, { flag: "wx" });
try {
  await rename(temporaryPath, manifestPath);
} catch (error) {
  throw new Error(`The GitHub Release is published, but the local update log could not be updated (${error?.code || "file error"})`);
}

console.log(`Published GitHub Release ${tag}: ${artifact.downloadUrl}`);
console.log(`Verified archive and SHA-256 sidecar anonymously; updated ${path.relative(repoRoot, manifestPath)}.`);
console.log("Build and deploy the Cloudflare Pages update log separately after reviewing the generated manifest.");

async function ensureReleaseAsset(releaseRecord, existingAssets, expected, source, contentType, accessToken, fullRepository) {
  const matches = existingAssets.filter(asset => asset.name === expected.name);
  if (matches.length > 1) throw new Error(`GitHub release contains duplicate assets named ${expected.name}`);
  if (matches.length === 1) {
    assertGitHubReleaseAsset(matches[0], expected);
    return existingAssets;
  }
  if (!releaseRecord.draft) throw new Error(`The published GitHub Release is missing ${expected.name}`);
  const uploaded = await uploadReleaseAsset(releaseRecord, expected, source, contentType, accessToken, fullRepository);
  assertGitHubReleaseAsset(uploaded, expected);
  return [...existingAssets, uploaded];
}

async function uploadReleaseAsset(releaseRecord, expected, source, contentType, accessToken, fullRepository) {
  const url = createUploadUrl(releaseRecord.upload_url, {
    repository: fullRepository,
    releaseId: releaseRecord.id,
    fileName: expected.name,
  });
  const sizeBytes = expected.sizeBytes;
  const response = await fetch(url, {
    method: "POST",
    headers: {
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${accessToken}`,
      "X-GitHub-Api-Version": API_VERSION,
      "Content-Type": contentType,
      "Content-Length": String(sizeBytes),
      "User-Agent": "RifeLove-Release-Publisher",
    },
    body: typeof source === "string" ? createReadStream(source) : source,
    ...(typeof source === "string" ? { duplex: "half" } : {}),
  });
  const payload = await parseResponse(response);
  if (!response.ok) throw new Error(`GitHub asset upload failed with HTTP ${response.status}: ${payload.message || "request failed"}`);
  return payload;
}

async function githubRequest(endpoint, { token: accessToken, method = "GET", body, allowNotFound = false } = {}) {
  const response = await fetch(new URL(endpoint, API_ROOT), {
    method,
    headers: {
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${accessToken}`,
      "X-GitHub-Api-Version": API_VERSION,
      "User-Agent": "RifeLove-Release-Publisher",
      ...(body ? { "Content-Type": "application/json" } : {}),
    },
    ...(body ? { body: JSON.stringify(body) } : {}),
  });
  const payload = await parseResponse(response);
  if (allowNotFound && response.status === 404) return null;
  if (!response.ok) throw new Error(`GitHub API ${method} ${endpoint} failed with HTTP ${response.status}: ${payload.message || "request failed"}`);
  return payload;
}

async function parseResponse(response) {
  const text = await response.text();
  if (!text) return {};
  try { return JSON.parse(text); } catch { throw new Error(`GitHub returned an invalid JSON response (HTTP ${response.status})`); }
}

function findAsset(assets, name) {
  const matches = assets.filter(asset => asset.name === name);
  if (matches.length !== 1) throw new Error(`GitHub release must contain exactly one ${name} asset`);
  return matches[0];
}

function hashBuffer(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

async function hashFile(filePath) {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(filePath)) hash.update(chunk);
  return hash.digest("hex");
}

function validateRelease(value) {
  if (!value || value.releaseReady !== true) {
    throw new Error("Release metadata must set releaseReady to true only after Windows distribution checks pass");
  }
  if (!/^\d{4}-\d{2}-\d{2}$/.test(value.date || "") || Number.isNaN(Date.parse(`${value.date}T00:00:00Z`))) {
    throw new Error("Release metadata needs an exact YYYY-MM-DD release date");
  }
  const parsedDate = new Date(`${value.date}T00:00:00.000Z`);
  if (Number.isNaN(parsedDate.getTime()) || parsedDate.toISOString().slice(0, 10) !== value.date) {
    throw new Error("Release metadata date must be a real calendar date");
  }
  for (const field of ["channel", "title", "summary"]) {
    if (typeof value[field] !== "string" || !value[field].trim()) throw new Error(`Release metadata needs ${field}`);
  }
  if (!new Set(["stable", "preview"]).has(value.channel.toLowerCase())) {
    throw new Error("Release channel must be Stable or Preview");
  }
  if (!Array.isArray(value.highlights) || value.highlights.length === 0 ||
      value.highlights.some(item => typeof item !== "string" || !item.trim())) {
    throw new Error("Release metadata needs a non-empty highlights list");
  }
}

function parseArguments(args) {
  const result = { dryRun: false };
  const seen = new Set();
  for (let index = 0; index < args.length; index += 1) {
    const key = args[index];
    if (key === "--dry-run") {
      if (seen.has(key)) throw new Error(`Specify ${key} only once`);
      seen.add(key);
      result.dryRun = true;
      continue;
    }
    if (!new Set(["--archive", "--release"]).has(key) || !args[index + 1] || args[index + 1].startsWith("--")) {
      throw new Error("Usage: node tools/release/publish-release.mjs --archive <zip> --release <metadata.json> [--dry-run]");
    }
    if (seen.has(key)) throw new Error(`Specify ${key} only once`);
    seen.add(key);
    result[key.slice(2)] = args[++index];
  }
  return result;
}
