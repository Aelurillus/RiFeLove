export async function verifyAnonymousObject(
  url,
  expectedSizeBytes,
  expectedSha256,
  fetchImpl = fetch,
  { requireInlineHtml = false } = {},
) {
  try {
    const response = await fetchImpl(url, { method: "HEAD", signal: AbortSignal.timeout(20_000) });
    const contentLength = response.headers.get("content-length");
    const declaredSize = response.headers.get("x-oss-meta-size-bytes");
    const declaredSha256 = response.headers.get("x-oss-meta-sha256");
    if (!response.ok || declaredSha256 !== expectedSha256 ||
        (contentLength !== null && Number(contentLength) !== expectedSizeBytes) ||
        (declaredSize !== null && Number(declaredSize) !== expectedSizeBytes)) {
      throw new Error("anonymous object check failed");
    }
    if (requireInlineHtml) {
      const disposition = response.headers.get("content-disposition") || "";
      const contentType = response.headers.get("content-type") || "";
      if (response.headers.get("x-oss-force-download") === "true" || /^attachment(?:\s*;|$)/i.test(disposition) ||
          !/^text\/html(?:\s*;|$)/i.test(contentType)) {
        throw new Error("The public OSS URL forces HTML to download. Bind a custom HTTPS domain to the bucket.");
      }
    }
  } catch {
    if (requireInlineHtml) {
      throw new Error(`OSS update page cannot be rendered inline at ${url}; bind a custom HTTPS domain to the bucket.`);
    }
    throw new Error(`OSS object is not anonymously readable or has mismatched hash/size metadata: ${url}`);
  }
}
