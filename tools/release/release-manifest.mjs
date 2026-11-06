import { MAX_GITHUB_RELEASE_ASSET_BYTES } from "./github-release-client.mjs";

const VERSION_PATTERN = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/;
const HASH_PATTERN = /^[0-9a-f]{64}$/;
const RELEASE_REPOSITORY = "Aelurillus/RiFeLove";

export function validateVersion(version) {
  if (typeof version !== "string" || !VERSION_PATTERN.test(version)) {
    throw new Error(`Invalid release version: ${String(version)}`);
  }
  return version;
}

export function validateArtifact(artifact, version) {
  validateVersion(version);
  const expectedFile = `RifeLove-${version}-win-x64.zip`;
  const expectedTag = `v${version}`;
  const expectedUrl = `https://github.com/${RELEASE_REPOSITORY}/releases/download/${expectedTag}/${expectedFile}`;
  if (!artifact || artifact.fileName !== expectedFile || artifact.releaseTag !== expectedTag ||
      artifact.downloadUrl !== expectedUrl ||
      !HASH_PATTERN.test(artifact.sha256) || !Number.isSafeInteger(artifact.sizeBytes) ||
      artifact.sizeBytes <= 0 || artifact.sizeBytes >= MAX_GITHUB_RELEASE_ASSET_BYTES) {
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
    if (release.artifact) validateArtifact(release.artifact, release.version);
    if (release.downloadAvailable && (!release.releaseReady || !release.artifact)) {
      throw new Error(`Release ${release.version} cannot be downloadable before package qualification`);
    }
  }
  return manifest;
}

export function mergePublishedRelease(existingManifest, releaseDetails, artifact, publishedAt) {
  validateReleaseManifest(existingManifest);
  const version = validateVersion(releaseDetails?.version);
  validateArtifact(artifact, version);
  if (!releaseDetails.releaseReady) throw new Error(`Release ${version} is not marked ready for distribution`);
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
  const retainedBytes = releases.reduce((total, item) =>
    item.downloadAvailable && item.artifact ? total + item.artifact.sizeBytes : total, 0);
  if (!Number.isSafeInteger(retainedBytes)) throw new Error("Published release archive sizes exceed the supported total");

  const manifest = {
    ...existingManifest,
    updatedAt: timestamp.toISOString(),
    releases,
  };
  validateReleaseManifest(manifest);
  return { manifest, retainedBytes };
}
