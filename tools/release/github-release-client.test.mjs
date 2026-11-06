import assert from "node:assert/strict";
import test from "node:test";
import {
  assertGitHubReleaseAsset,
  createReleaseArtifact,
  createUploadUrl,
  MAX_GITHUB_RELEASE_ASSET_BYTES,
  verifyAnonymousReleaseAsset,
} from "./github-release-client.mjs";

const sha256 = "a".repeat(64);

test("GitHub release artifacts use the canonical versioned release download URL", () => {
  assert.deepEqual(createReleaseArtifact("0.2.0", {
    repository: "Aelurillus/RiFeLove", sha256, sizeBytes: 123,
  }), {
    fileName: "RifeLove-0.2.0-win-x64.zip",
    releaseTag: "v0.2.0",
    downloadUrl: "https://github.com/Aelurillus/RiFeLove/releases/download/v0.2.0/RifeLove-0.2.0-win-x64.zip",
    sha256,
    sizeBytes: 123,
  });
});

test("GitHub release assets must remain below the per-file 2 GiB limit", () => {
  assert.throws(() => createReleaseArtifact("0.2.0", {
    repository: "Aelurillus/RiFeLove", sha256, sizeBytes: MAX_GITHUB_RELEASE_ASSET_BYTES,
  }), /smaller than 2 GiB/);
  assert.throws(() => createReleaseArtifact("0.2.0", {
    repository: "Aelurillus/RiFeLove", sha256, sizeBytes: 0,
  }), /smaller than 2 GiB/);
});

test("release asset metadata must match its SHA-256 and size", () => {
  const expected = { name: "RifeLove-0.2.0-win-x64.zip", sha256, sizeBytes: 123 };
  assert.equal(assertGitHubReleaseAsset({ name: expected.name, size: 123, state: "uploaded", digest: `sha256:${sha256}` }, expected).name, expected.name);
  assert.throws(() => assertGitHubReleaseAsset({ name: expected.name, size: 123, state: "uploaded", digest: `sha256:${"b".repeat(64)}` }, expected), /does not match/);
});

test("asset uploads are restricted to the matching GitHub release endpoint", () => {
  const url = createUploadUrl("https://uploads.github.com/repos/Aelurillus/RiFeLove/releases/42/assets{?name,label}", {
    repository: "Aelurillus/RiFeLove", releaseId: 42, fileName: "RifeLove-0.2.0-win-x64.zip",
  });
  assert.equal(url.href, "https://uploads.github.com/repos/Aelurillus/RiFeLove/releases/42/assets?name=RifeLove-0.2.0-win-x64.zip");
  assert.throws(() => createUploadUrl("https://attacker.example/upload", {
    repository: "Aelurillus/RiFeLove", releaseId: 42, fileName: "app.zip",
  }), /unexpected release asset upload URL/);
});

test("public asset readback uses anonymous HEAD and checks size when supplied", async () => {
  let request;
  const result = await verifyAnonymousReleaseAsset(
    "https://github.com/Aelurillus/RiFeLove/releases/download/v0.2.0/RifeLove-0.2.0-win-x64.zip",
    {
      sizeBytes: 123,
      fetchImpl: async (url, options) => {
        request = { url: url.href, options };
        return new Response(null, { status: 200, headers: { "content-length": "123" } });
      },
    },
  );
  assert.deepEqual(result, { status: 200, sizeBytes: 123 });
  assert.equal(request.options.method, "HEAD");
  assert.equal(request.options.redirect, "follow");
  await assert.rejects(verifyAnonymousReleaseAsset(
    "https://github.com/Aelurillus/RiFeLove/releases/download/v0.2.0/RifeLove-0.2.0-win-x64.zip",
    { sizeBytes: 124, fetchImpl: async () => new Response(null, { status: 200, headers: { "content-length": "123" } }) },
  ), /size does not match/);
});
