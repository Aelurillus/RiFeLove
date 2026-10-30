import assert from "node:assert/strict";
import test from "node:test";
import { mkdtemp, mkdir, rm, symlink, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { parsePrivatePreviewArguments, removeVerifiedArchive, validateArchiveRemovalTarget } from "./private-preview-archive.mjs";

test("private preview options default to preserving the local ZIP", () => {
  assert.deepEqual(parsePrivatePreviewArguments(["--archive", "RifeLove-1.2.3-win-x64.zip", "--version", "1.2.3"]), {
    archive: "RifeLove-1.2.3-win-x64.zip",
    version: "1.2.3",
    removeLocalAfterVerify: false,
  });
});

test("private preview options accept the explicit post-verification cleanup flag", () => {
  assert.deepEqual(parsePrivatePreviewArguments([
    "--archive", "RifeLove-1.2.3-win-x64.zip", "--version", "1.2.3", "--remove-local-after-verify",
  ]), {
    archive: "RifeLove-1.2.3-win-x64.zip",
    version: "1.2.3",
    removeLocalAfterVerify: true,
  });
});

test("private preview options reject missing, repeated and unknown arguments", () => {
  assert.throws(() => parsePrivatePreviewArguments(["--archive", "--version", "1.2.3"]), /Usage:/);
  assert.throws(() => parsePrivatePreviewArguments([
    "--archive", "a.zip", "--archive", "b.zip", "--version", "1.2.3",
  ]), /only once/);
  assert.throws(() => parsePrivatePreviewArguments([
    "--archive", "a.zip", "--version", "1.2.3", "--delete-everything",
  ]), /Usage:/);
});

test("deletion target is limited to the matching versioned release-candidate archive", async t => {
  const root = await createReleaseCandidate(t, "1.2.3");
  const archive = path.join(root, "out", "release-candidates", "1.2.3", "RifeLove-1.2.3-win-x64.zip");

  const target = await validateArchiveRemovalTarget(archive, root, "1.2.3");
  assert.equal(target.path, archive);
  assert.equal(await removeVerifiedArchive(target), true);
  await assert.rejects(rm(archive), { code: "ENOENT" });
});

test("deletion target refuses paths outside the repository release-candidates directory", async t => {
  const root = await createReleaseCandidate(t, "1.2.3");
  const archive = path.join(root, "out", "RifeLove-1.2.3-win-x64.zip");
  await writeFile(archive, "test archive");

  await assert.rejects(
    validateArchiveRemovalTarget(archive, root, "1.2.3"),
    /limited to the matching archive/,
  );
});

test("deletion target refuses a symbolic-link archive", async t => {
  const root = await createReleaseCandidate(t, "1.2.3");
  const folder = path.join(root, "out", "release-candidates", "1.2.3");
  const archive = path.join(folder, "RifeLove-1.2.3-win-x64.zip");
  const external = path.join(root, "keep-this.zip");
  await writeFile(external, "keep");
  await rm(archive);
  await symlink(external, archive, "file");

  await assert.rejects(
    validateArchiveRemovalTarget(archive, root, "1.2.3"),
    /regular ZIP file in real, non-linked release directories/,
  );
  assert.equal(await (await import("node:fs/promises")).readFile(external, "utf8"), "keep");
});

test("post-upload deletion preserves a ZIP whose identity changed after verification", async t => {
  const root = await createReleaseCandidate(t, "1.2.3");
  const archive = path.join(root, "out", "release-candidates", "1.2.3", "RifeLove-1.2.3-win-x64.zip");
  const target = await validateArchiveRemovalTarget(archive, root, "1.2.3");
  await writeFile(archive, "changed file with a different size");

  assert.equal(await removeVerifiedArchive(target), false);
  assert.equal((await (await import("node:fs/promises")).readFile(archive, "utf8")), "changed file with a different size");
});

async function createReleaseCandidate(t, version) {
  const root = await mkdtemp(path.join(os.tmpdir(), "rifelove-release-test-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const folder = path.join(root, "out", "release-candidates", version);
  await mkdir(folder, { recursive: true });
  await writeFile(path.join(folder, `RifeLove-${version}-win-x64.zip`), "test archive");
  return root;
}
