import test from "node:test";
import assert from "node:assert/strict";
import { assertPublicObjectAccessAllowed } from "./oss-public-access.mjs";

function createClient(accountSetting, bucketSetting) {
  const requests = [];
  return {
    requests,
    async request(params) {
      requests.push(params);
      const value = params.bucket ? bucketSetting : accountSetting;
      return { data: { PublicAccessBlockConfiguration: { BlockPublicAccess: value } } };
    },
  };
}

test("allows public uploads when account and bucket protections are disabled", async () => {
  const client = createClient("false", false);
  await assert.doesNotReject(assertPublicObjectAccessAllowed(client, "example-bucket"));
  assert.deepEqual(client.requests, [
    { method: "GET", bucket: "", subres: "publicAccessBlock", successStatuses: [200], xmlResponse: true },
    { method: "GET", bucket: "example-bucket", subres: "publicAccessBlock", successStatuses: [200], xmlResponse: true },
  ]);
});

test("refuses public uploads when account protection is enabled", async () => {
  const client = createClient(true, false);
  await assert.rejects(
    assertPublicObjectAccessAllowed(client, "example-bucket"),
    /account-level Block Public Access/,
  );
  assert.equal(client.requests.length, 1);
});

test("refuses public uploads when bucket protection is enabled", async () => {
  const client = createClient(false, "true");
  await assert.rejects(
    assertPublicObjectAccessAllowed(client, "example-bucket"),
    /bucket-level Block Public Access is enabled for example-bucket/,
  );
  assert.equal(client.requests.length, 2);
});

test("fails closed when an OSS protection setting cannot be read", async () => {
  const client = createClient(false, false);
  client.request = async () => ({ data: {} });
  await assert.rejects(
    assertPublicObjectAccessAllowed(client, "example-bucket"),
    /unrecognized Block Public Access setting/,
  );
});
