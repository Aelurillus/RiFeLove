const VERSION_PATTERN = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/;
const HASH_PATTERN = /^[0-9a-f]{64}$/;

export function validateVersion(version) {
  if (typeof version !== "string" || !VERSION_PATTERN.test(version)) {
    throw new Error(`Invalid release version: ${String(version)}`);
  }
  return version;
}

export function validateArtifact(artifact, version) {
  validateVersion(version);
  const expectedFile = `RifeLove-${version}-win-x64.zip`;
  const expectedKey = `releases/${version}/${expectedFile}`;
  let downloadUrl;
  try { downloadUrl = new URL(artifact?.downloadUrl); } catch { throw new Error(`Release artifact metadata is invalid for ${version}`); }
  const expectedPath = `/${expectedKey}`;
  if (!artifact || artifact.fileName !== expectedFile || artifact.key !== expectedKey ||
      downloadUrl.protocol !== "https:" || !/^[a-z0-9-]+\.oss-[a-z0-9-]+\.aliyuncs\.com$/i.test(downloadUrl.hostname) ||
      downloadUrl.pathname !== expectedPath || downloadUrl.search || downloadUrl.username || downloadUrl.password ||
      !HASH_PATTERN.test(artifact.sha256) || !Number.isSafeInteger(artifact.sizeBytes) || artifact.sizeBytes <= 0) {
    throw new Error(`Release artifact metadata is invalid for ${version}`);
  }
  return artifact;
}

export function validateReleaseManifest(manifest) {
  if (!manifest || manifest.schemaVersion !== 1 || manifest.product !== "RifeLove" || !Array.isArray(manifest.releases)) {
    throw new Error("Release log must use schema version 1 and contain a releases array");
  }
  const seen = new Set();
  for (const release of manifest.releases) {
    validateVersion(release.version);
    if (seen.has(release.version)) throw new Error(`Release log contains duplicate version ${release.version}`);
    seen.add(release.version);
    if (!/^\d{4}-\d{2}-\d{2}$/.test(release.date || "") || !release.title || !Array.isArray(release.highlights)) {
      throw new Error(`Release ${release.version} is missing its date, title, or highlights`);
    }
    if (typeof release.releaseReady !== "boolean" || typeof release.downloadAvailable !== "boolean") {
      throw new Error(`Release ${release.version} is missing explicit release/download readiness fields`);
    }
    if (release.storjBackup !== undefined && typeof release.storjBackup !== "boolean") {
      throw new Error(`Release ${release.version} has an invalid Storj backup flag`);
    }
    if (release.artifact) validateArtifact(release.artifact, release.version);
    if (release.downloadAvailable && (!release.releaseReady || !release.artifact)) {
      throw new Error(`Release ${release.version} cannot be downloadable before package qualification`);
    }
  }
  return manifest;
}

export function mergePublishedRelease(existingManifest, releaseDetails, artifact, publishedAt, policy) {
  validateReleaseManifest(existingManifest);
  const version = validateVersion(releaseDetails?.version);
  validateArtifact(artifact, version);
  if (!releaseDetails.releaseReady) throw new Error(`Release ${version} is not marked ready for distribution`);
  if (!Number.isInteger(policy?.retainedDownloads) || policy.retainedDownloads < 1 || policy.retainedDownloads > 100) {
    throw new Error("retainedDownloads must be an integer from 1 to 100");
  }
  if (!Number.isSafeInteger(policy?.releaseStorageBudgetBytes) || policy.releaseStorageBudgetBytes < 1) {
    throw new Error("releaseStorageBudgetBytes must be a positive integer");
  }
  const timestamp = new Date(publishedAt);
  if (Number.isNaN(timestamp.getTime())) throw new Error("GitHub release must provide a valid publishedAt timestamp");

  const release = {
    ...releaseDetails,
    date: timestamp.toISOString().slice(0, 10),
    downloadAvailable: true,
    artifact,
  };
  const releasesByVersion = new Map(existingManifest.releases.map(item => [item.version, item]));
  releasesByVersion.set(version, release);
  const releases = [...releasesByVersion.values()].sort((left, right) =>
    right.date.localeCompare(left.date) || right.version.localeCompare(left.version));
  const downloadable = releases.filter(item => item.downloadAvailable && item.artifact);
  const retainedVersions = new Set(downloadable.slice(0, policy.retainedDownloads).map(item => item.version));
  const removedKeys = [];
  for (const item of releases) {
    if (item.artifact && !retainedVersions.has(item.version) && item.downloadAvailable) {
      removedKeys.push(item.artifact.key);
      item.downloadAvailable = false;
    }
  }

  const retainedBytes = releases.reduce((total, item) =>
    item.downloadAvailable && item.artifact ? total + item.artifact.sizeBytes : total, 0);
  if (!Number.isSafeInteger(retainedBytes) || retainedBytes > policy.releaseStorageBudgetBytes) {
    throw new Error(`Retained release archives would use ${retainedBytes} bytes, above the configured storage budget`);
  }

  const manifest = {
    ...existingManifest,
    updatedAt: timestamp.toISOString(),
    releases,
  };
  validateReleaseManifest(manifest);
  return { manifest, removedKeys, retainedBytes };
}
