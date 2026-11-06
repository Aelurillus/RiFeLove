import assert from "node:assert/strict";
import test from "node:test";
import { MAX_GITHUB_RELEASE_ASSET_BYTES } from "./github-release-client.mjs";
import { mergePublishedRelease, validateReleaseManifest } from "./release-manifest.mjs";

const makeRelease = (version, date, releaseReady = true, downloadAvailable = false) => ({
  version, date, channel: "Preview", releaseReady, downloadAvailable,
  title: `RifeLove ${version}`, summary: "Local test fixture", highlights: ["Keeps a permanent update log"], artifact: null,
});
const makeArtifact = version => ({
  fileName: `RifeLove-${version}-win-x64.zip`,
  releaseTag: `v${version}`,
  downloadUrl: `https://github.com/Aelurillus/RiFeLove/releases/download/v${version}/RifeLove-${version}-win-x64.zip`,
  sha256: "a".repeat(64), sizeBytes: 100,
});
const seed = { schemaVersion: 1, product: "RifeLove", updatedAt: "2026-10-26T00:00:00.000Z", releases: [] };

test("release manifest validates preview metadata with no public package", () => {
  const manifest = { ...seed, releases: [makeRelease("0.1.0", "2026-10-26", false)] };
  assert.equal(validateReleaseManifest(manifest), manifest);
});

test("publishing preserves every downloadable release and its metadata", () => {
  let manifest = { ...seed, releases: [] };
  for (const [version, date] of [["0.1.0", "2026-10-20"], ["0.2.0", "2026-10-24"]]) {
    const result = mergePublishedRelease(manifest, makeRelease(version, date), makeArtifact(version), `${date}T10:00:00Z`);
    manifest = result.manifest;
  }
  const result = mergePublishedRelease(manifest, makeRelease("0.3.0", "2026-10-26"), makeArtifact("0.3.0"), "2026-10-26T12:00:00Z");
  assert.equal(result.retainedBytes, 300);
  assert.deepEqual(result.manifest.releases.map(release => release.version), ["0.3.0", "0.2.0", "0.1.0"]);
  assert.equal(result.manifest.releases[0].downloadAvailable, true);
  assert.equal(result.manifest.releases[2].downloadAvailable, true);
  assert.ok(result.manifest.releases[2].artifact.sha256);
});

test("publisher refuses unqualified archives and URLs outside the official GitHub Release", () => {
  assert.throws(() => mergePublishedRelease(seed, makeRelease("0.1.0", "2026-10-26", false), makeArtifact("0.1.0"), "2026-10-26T12:00:00Z"), /not marked ready/);
  assert.throws(() => mergePublishedRelease(seed, makeRelease("0.1.0", "2026-10-26"), { ...makeArtifact("0.1.0"), downloadUrl: "https://attacker.example/installer.zip" }, "2026-10-26T12:00:00Z"), /metadata is invalid/);
  assert.throws(() => mergePublishedRelease(seed, makeRelease("0.1.0", "2026-10-26"), { ...makeArtifact("0.1.0"), releaseTag: "other-tag" }, "2026-10-26T12:00:00Z"), /metadata is invalid/);
  assert.throws(() => mergePublishedRelease(seed, makeRelease("0.1.0", "2026-10-26"), { ...makeArtifact("0.1.0"), sizeBytes: MAX_GITHUB_RELEASE_ASSET_BYTES }, "2026-10-26T12:00:00Z"), /metadata is invalid/);
});
