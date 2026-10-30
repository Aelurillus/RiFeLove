import { createRequire } from "node:module";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { failAliyunOssDisabled } from "./oss-disabled.mjs";

failAliyunOssDisabled();

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const siteRoot = path.join(repoRoot, "release-site");
const requireFromReleaseSite = createRequire(path.join(siteRoot, "package.json"));
const OSS = requireFromReleaseSite("ali-oss");
const storage = JSON.parse(await readFile(path.join(siteRoot, "storage.json"), "utf8"));
const bucket = process.env.OSS_BUCKET || storage.primary?.bucket;
const region = process.env.OSS_REGION || storage.primary?.region;
const endpoint = process.env.OSS_ENDPOINT || storage.primary?.endpoint || `https://${region}.aliyuncs.com`;
const accessKeyId = process.env.OSS_ACCESS_KEY_ID;
const accessKeySecret = process.env.OSS_ACCESS_KEY_SECRET;

if (!process.argv.includes("--allow-bucket-public-acls")) {
  throw new Error("This change requires the explicit --allow-bucket-public-acls flag.");
}
if (!accessKeyId || !accessKeySecret) throw new Error("Set OSS_ACCESS_KEY_ID and OSS_ACCESS_KEY_SECRET in the current process.");
if (!bucket || !region || !/^https:\/\/[a-z0-9.-]+$/.test(endpoint) || !endpoint.endsWith(".aliyuncs.com")) {
  throw new Error("The configured OSS bucket, region or HTTPS endpoint is invalid");
}

const client = new OSS({ region, bucket, accessKeyId, accessKeySecret, authorizationV4: true, endpoint, secure: true, timeout: 30_000 });
const accountBlocked = await readBlockPublicAccess(client, "");
if (accountBlocked) throw new Error("Account-level Block Public Access is enabled; this tool will not change account-wide access.");

const bucketAcl = await client.getBucketACL(bucket);
if (bucketAcl.acl !== "private") throw new Error(`Expected the ${bucket} bucket ACL to remain private; found ${bucketAcl.acl}.`);
const listing = await client.listV2({ "max-keys": 1 });
if ((listing.objects?.length || 0) !== 0 || listing.isTruncated) {
  throw new Error(`The ${bucket} bucket is not empty; refusing to change its public-access block.`);
}

const currentlyBlocked = await readBlockPublicAccess(client, bucket);
if (!currentlyBlocked) {
  console.log(`Bucket-level Block Public Access is already disabled for the empty, private-ACL ${bucket} bucket.`);
} else {
  const xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><PublicAccessBlockConfiguration><BlockPublicAccess>false</BlockPublicAccess></PublicAccessBlockConfiguration>";
  await client.request({
    method: "PUT",
    bucket,
    subres: "publicAccessBlock",
    content: Buffer.from(xml, "utf8"),
    headers: { "Content-Type": "application/xml" },
    successStatuses: [200],
  });
  const verified = await readBlockPublicAccess(client, bucket);
  if (verified) throw new Error(`OSS did not confirm that Block Public Access was disabled for ${bucket}.`);
  console.log(`Disabled bucket-level Block Public Access for the empty, private-ACL ${bucket} bucket.`);
}

console.log("The bucket ACL remains private. Only objects explicitly assigned public-read can be anonymously read.");

async function readBlockPublicAccess(client, targetBucket) {
  const params = { method: "GET", bucket: targetBucket, subres: "publicAccessBlock", successStatuses: [200], xmlResponse: true };
  const response = await client.request(params);
  const value = response?.data?.PublicAccessBlockConfiguration?.BlockPublicAccess ?? response?.data?.BlockPublicAccess;
  if (value === true || value === "true") return true;
  if (value === false || value === "false") return false;
  throw new Error(`OSS returned an unrecognized Block Public Access setting for ${targetBucket || "the account"}.`);
}
