import { copyFile, lstat, mkdir, readFile, readdir, realpath, rm, stat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { validateReleaseManifest } from "./release-manifest.mjs";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const siteRoot = path.join(projectRoot, "release-site");
const output = path.join(siteRoot, "dist");
const expected = path.resolve(siteRoot, "dist");
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

await rm(output, { recursive: true, force: true });
await mkdir(output, { recursive: true });
await copyFile(path.join(siteRoot, "index.html"), path.join(output, "index.html"));
await copyFile(path.join(siteRoot, "releases.json"), path.join(output, "releases.json"));
const files = await readdir(output);
if (files.length !== 2 || !(await stat(path.join(output, "index.html"))).isFile()) {
  throw new Error("Static update log output contains an unexpected file set");
}
await realpath(output);
console.log(`Built local update log into ${output}`);
