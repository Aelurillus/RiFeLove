import { lstat, realpath, rm } from "node:fs/promises";
import path from "node:path";

const USAGE = "Usage: node tools/release/upload-private-preview.mjs --archive <zip> --version <version> [--remove-local-after-verify]";

export function parsePrivatePreviewArguments(args) {
  const result = { removeLocalAfterVerify: false };
  const seen = new Set();
  for (let index = 0; index < args.length; index += 1) {
    const key = args[index];
    if (key === "--remove-local-after-verify") {
      if (seen.has(key)) throw new Error(`Specify ${key} only once`);
      seen.add(key);
      result.removeLocalAfterVerify = true;
      continue;
    }
    if (!new Set(["--archive", "--version"]).has(key) || !args[index + 1] || args[index + 1].startsWith("--")) {
      throw new Error(USAGE);
    }
    if (seen.has(key)) throw new Error(`Specify ${key} only once`);
    seen.add(key);
    result[key.slice(2)] = args[++index];
  }
  if (!result.archive || !result.version) throw new Error(USAGE);
  return result;
}

export async function validateArchiveRemovalTarget(archivePath, repoRoot, version) {
  const root = path.resolve(repoRoot);
  const expectedPath = path.join(root, "out", "release-candidates", version, `RifeLove-${version}-win-x64.zip`);
  const absolutePath = path.resolve(archivePath);
  if (!samePath(absolutePath, expectedPath)) {
    throw new Error("Local deletion is limited to the matching archive under out/release-candidates/<version>.");
  }

  const canonicalRoot = await realpath(root);
  if (!samePath(canonicalRoot, root)) throw new Error("Local deletion is disabled when the repository root resolves through a link.");

  const relativeParts = ["out", "release-candidates", version, `RifeLove-${version}-win-x64.zip`];
  let currentPath = root;
  let fileInfo;
  for (let index = 0; index < relativeParts.length; index += 1) {
    currentPath = path.join(currentPath, relativeParts[index]);
    const info = await lstat(currentPath);
    const isFile = index === relativeParts.length - 1;
    if (info.isSymbolicLink() || (isFile ? !info.isFile() : !info.isDirectory())) {
      throw new Error("Local deletion requires a regular ZIP file in real, non-linked release directories.");
    }
    if (isFile) fileInfo = info;
  }

  return { path: absolutePath, fileInfo };
}

export async function removeVerifiedArchive(target) {
  const currentInfo = await lstat(target.path);
  if (currentInfo.isSymbolicLink() || !currentInfo.isFile() || !sameFileIdentity(currentInfo, target.fileInfo)) return false;
  await rm(target.path);
  return true;
}

function samePath(left, right) {
  const normalize = value => path.normalize(value);
  if (path.sep === "\\") return normalize(left).toLocaleLowerCase("en-US") === normalize(right).toLocaleLowerCase("en-US");
  return normalize(left) === normalize(right);
}

function sameFileIdentity(left, right) {
  return left.dev === right.dev && left.ino === right.ino && left.size === right.size &&
    left.mtimeMs === right.mtimeMs && left.ctimeMs === right.ctimeMs;
}
