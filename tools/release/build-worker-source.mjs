import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { validateReleaseManifest } from "./release-manifest.mjs";
import { createWorkerSource } from "./worker-source.mjs";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const siteRoot = path.join(projectRoot, "release-site");
const outputRoot = path.join(projectRoot, "out", "release-worker");
const outputPath = path.join(outputRoot, "worker.js");

const html = await readFile(path.join(siteRoot, "index.html"), "utf8");
const manifest = JSON.parse(await readFile(path.join(siteRoot, "releases.json"), "utf8"));
validateReleaseManifest(manifest);
for (const required of ["releaseList", "latestVersion", "latestDate", "./releases.json", "downloadUrl"]) {
  if (!html.includes(required)) throw new Error(`Update log page is missing expected content: ${required}`);
}

await mkdir(outputRoot, { recursive: true });
await writeFile(outputPath, createWorkerSource(html, manifest), { encoding: "utf8" });
console.log(`Built update-log Worker source at ${outputPath}`);
