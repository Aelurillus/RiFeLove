import { validateReleaseManifest } from "./release-manifest.mjs";

export function createWorkerSource(html, manifest) {
  validateReleaseManifest(manifest);
  if (manifest.releases.length === 0) throw new Error("Update log Worker requires at least one release");
  if (typeof html !== "string" || html.length === 0) throw new Error("Update log HTML must be a non-empty string");
  const pageLiteral = JSON.stringify(html);
  const releasesLiteral = JSON.stringify(JSON.stringify(manifest));
  return `const PAGE = ${pageLiteral};\n` +
    `const RELEASES = ${releasesLiteral};\n` +
    `const HTML_HEADERS = {"content-type":"text/html; charset=utf-8","cache-control":"no-cache","x-content-type-options":"nosniff","referrer-policy":"strict-origin-when-cross-origin"};\n` +
    `const JSON_HEADERS = {"content-type":"application/json; charset=utf-8","cache-control":"no-cache","x-content-type-options":"nosniff","referrer-policy":"strict-origin-when-cross-origin"};\n` +
    `addEventListener("fetch", event => event.respondWith(handle(event.request)));\n` +
    `async function handle(request) {\n` +
    `  if (request.method !== "GET" && request.method !== "HEAD") return new Response("Method not allowed", {status:405, headers:{"allow":"GET, HEAD","cache-control":"no-store","x-content-type-options":"nosniff"}});\n` +
    `  const pathname = new URL(request.url).pathname;\n` +
    `  const body = pathname === "/releases.json" ? RELEASES : pathname === "/" || pathname === "/index.html" ? PAGE : null;\n` +
    `  if (body === null) return new Response("Not found", {status:404, headers:{"cache-control":"no-store","x-content-type-options":"nosniff"}});\n` +
    `  const headers = pathname === "/releases.json" ? JSON_HEADERS : HTML_HEADERS;\n` +
    `  return new Response(request.method === "HEAD" ? null : body, {status:200, headers});\n` +
    `}\n`;
}
