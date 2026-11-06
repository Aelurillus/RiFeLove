import { copyFile, lstat, mkdir, readFile, readdir, realpath, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { copyVerifiedModelPagesAssets, inspectModelPagesAssets } from "./model-pages-assets.mjs";
import { validateReleaseManifest } from "./release-manifest.mjs";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const siteRoot = path.join(projectRoot, "release-site");
const output = path.join(siteRoot, "dist");
const expected = path.resolve(siteRoot, "dist");
const modelPagesStageValue = process.env.RIFELOVE_MODEL_PAGES_STAGE?.trim();
const modelPagesStageMode = process.env.RIFELOVE_MODEL_PAGES_MODE?.trim();
if (output !== expected || path.dirname(output) !== siteRoot) throw new Error("Refusing to build outside release-site/dist");
const siteInfo = await lstat(siteRoot);
if (siteInfo.isSymbolicLink()) throw new Error("Refusing to use a linked release-site directory");
const outputInfo = await lstat(output).catch(() => null);
if (outputInfo?.isSymbolicLink()) throw new Error("Refusing to replace a linked release-site/dist path");

const manifest = JSON.parse(await readFile(path.join(siteRoot, "releases.json"), "utf8"));
validateReleaseManifest(manifest);
const html = await readFile(path.join(siteRoot, "index.html"), "utf8");
for (const required of ["releaseList", "latestVersion", "latestDate", "./releases.json", "downloadUrl"]) {
  if (!html.includes(required)) throw new Error(`Update log page is missing expected content: ${required}`);
}

let modelPagesAssets = null;
if (modelPagesStageValue) {
  const sourceRoot = path.resolve(modelPagesStageValue);
  if (isWithin(output, sourceRoot)) {
    throw new Error("RIFELOVE_MODEL_PAGES_STAGE must be outside release-site/dist, which the build replaces.");
  }
  modelPagesAssets = await inspectModelPagesAssets(sourceRoot, { mode: modelPagesStageMode || "final" });
} else if (modelPagesStageMode) {
  throw new Error("RIFELOVE_MODEL_PAGES_MODE requires RIFELOVE_MODEL_PAGES_STAGE.");
}

await rm(output, { recursive: true, force: true });
await mkdir(output, { recursive: true });
await copyFile(path.join(siteRoot, "index.html"), path.join(output, "index.html"));
await copyFile(path.join(siteRoot, "releases.json"), path.join(output, "releases.json"));
if (modelPagesAssets) {
  await copyVerifiedModelPagesAssets(modelPagesAssets, output);
  await writeFile(path.join(output, "_headers"), [
    "/model-distribution/*",
    "  Cache-Control: public, max-age=31536000, immutable",
    "/model-catalog/v1/catalog.signed.txt",
    "  Cache-Control: public, max-age=300",
    "",
  ].join("\n"), { flag: "wx" });
}
const files = await readdir(output);
const expectedFiles = new Set(["index.html", "releases.json"]);
if (modelPagesAssets) {
  expectedFiles.add("_headers");
  expectedFiles.add("model-distribution");
  if (modelPagesAssets.mode === "final") expectedFiles.add("model-catalog");
}
if (files.length !== expectedFiles.size || files.some(file => !expectedFiles.has(file)) ||
    !(await stat(path.join(output, "index.html"))).isFile()) {
  throw new Error("Static update log output contains an unexpected file set");
}
await realpath(output);
const buildDescription = modelPagesAssets?.mode === "archive-bootstrap"
  ? "update log and hash-verified bootstrap model archive"
  : modelPagesAssets
    ? "update log and verified model distribution plus signed catalog"
    : "local update log";
console.log(`Built ${buildDescription} into ${output}`);

function isWithin(parentPath, candidatePath) {
  const relative = path.relative(parentPath, candidatePath);
  return relative === "" || (!path.isAbsolute(relative) && relative !== ".." && !relative.startsWith(`..${path.sep}`));
}
