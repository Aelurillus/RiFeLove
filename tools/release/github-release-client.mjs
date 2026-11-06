const VERSION_PATTERN = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/;
const REPOSITORY_PATTERN = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/;
export const MAX_GITHUB_RELEASE_ASSET_BYTES = 2 * 1024 * 1024 * 1024;

export function createReleaseArtifact(version, { repository, sha256, sizeBytes } = {}) {
  if (typeof version !== "string" || !VERSION_PATTERN.test(version)) {
    throw new Error(`Invalid release version: ${String(version)}`);
  }
  if (typeof repository !== "string" || !REPOSITORY_PATTERN.test(repository)) {
    throw new Error("The GitHub release repository must use owner/name format");
  }
  if (!/^[0-9a-f]{64}$/.test(sha256 || "")) throw new Error("A lowercase SHA-256 digest is required");
  if (!Number.isSafeInteger(sizeBytes) || sizeBytes <= 0 || sizeBytes >= MAX_GITHUB_RELEASE_ASSET_BYTES) {
    throw new Error("Each GitHub release asset must be smaller than 2 GiB");
  }

  const fileName = `RifeLove-${version}-win-x64.zip`;
  const releaseTag = `v${version}`;
  return {
    fileName,
    releaseTag,
    downloadUrl: `https://github.com/${repository}/releases/download/${releaseTag}/${fileName}`,
    sha256,
    sizeBytes,
  };
}

export function assertGitHubReleaseAsset(asset, expected) {
  if (!asset || asset.name !== expected.name || asset.state !== "uploaded" ||
      asset.size !== expected.sizeBytes || asset.digest !== `sha256:${expected.sha256}` ||
      (expected.downloadUrl && asset.browser_download_url !== expected.downloadUrl)) {
    throw new Error(`GitHub release asset ${expected.name} does not match its expected size and SHA-256`);
  }
  return asset;
}

export function buildReleaseBody(release, artifact) {
  const highlights = release.highlights.map(item => `- ${item}`).join("\n");
  return [
    release.summary.trim(),
    "## Highlights",
    highlights,
    "## Windows x64 package",
    `- File: \`${artifact.fileName}\``,
    `- Size: ${artifact.sizeBytes} bytes`,
    `- SHA-256: \`${artifact.sha256}\``,
  ].join("\n\n");
}

export function createUploadUrl(uploadUrl, { repository, releaseId, fileName }) {
  if (typeof repository !== "string" || !REPOSITORY_PATTERN.test(repository) ||
      !Number.isSafeInteger(releaseId) || releaseId <= 0 || typeof fileName !== "string" ||
      !/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(fileName)) {
    throw new Error("GitHub release upload metadata is invalid");
  }
  const templateFreeUrl = String(uploadUrl || "").replace(/\{\?name,label\}$/, "");
  let parsed;
  try { parsed = new URL(templateFreeUrl); } catch { throw new Error("GitHub returned an invalid release asset upload URL"); }
  const expectedPath = `/repos/${repository}/releases/${releaseId}/assets`;
  if (parsed.protocol !== "https:" || parsed.hostname !== "uploads.github.com" || parsed.pathname !== expectedPath ||
      parsed.username || parsed.password || parsed.search || parsed.hash) {
    throw new Error("GitHub returned an unexpected release asset upload URL");
  }
  parsed.searchParams.set("name", fileName);
  return parsed;
}

export async function verifyAnonymousReleaseAsset(url, { sizeBytes, fetchImpl = fetch } = {}) {
  if (!Number.isSafeInteger(sizeBytes) || sizeBytes <= 0) throw new Error("A positive expected release asset size is required");
  let parsed;
  try { parsed = new URL(url); } catch { throw new Error("The public GitHub release asset URL is invalid"); }
  if (parsed.protocol !== "https:" || parsed.hostname !== "github.com" || parsed.username || parsed.password ||
      parsed.search || parsed.hash || !parsed.pathname.startsWith("/Aelurillus/RiFeLove/releases/download/")) {
    throw new Error("The public release asset must use the official RiFeLove GitHub Releases host");
  }
  const response = await fetchImpl(parsed, { method: "HEAD", redirect: "follow" });
  if (!response.ok) throw new Error(`Anonymous GitHub release asset readback returned HTTP ${response.status}`);
  const contentLength = response.headers.get("content-length");
  if (contentLength !== null && Number(contentLength) !== sizeBytes) {
    throw new Error("Anonymous GitHub release asset size does not match the published package");
  }
  return { status: response.status, sizeBytes: contentLength === null ? null : Number(contentLength) };
}
