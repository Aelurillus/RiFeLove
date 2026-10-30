export async function assertPublicObjectAccessAllowed(client, bucket) {
  const accountBlocked = await isBlocked(client, "");
  if (accountBlocked) {
    throw new Error("OSS account-level Block Public Access is enabled; refusing to upload public RifeLove objects");
  }

  const bucketBlocked = await isBlocked(client, bucket);
  if (bucketBlocked) {
    throw new Error(`OSS bucket-level Block Public Access is enabled for ${bucket}; refusing to upload public RifeLove objects`);
  }
}

async function isBlocked(client, bucket) {
  if (typeof client?.request !== "function") {
    throw new Error("The OSS client cannot safely inspect Block Public Access settings");
  }

  const params = {
    method: "GET",
    bucket,
    subres: "publicAccessBlock",
    successStatuses: [200],
    xmlResponse: true,
  };

  let response;
  try {
    response = await client.request(params);
  } catch (error) {
    throw new Error(`Could not inspect OSS Block Public Access settings (${error?.code || "storage error"})`);
  }

  const value = response?.data?.PublicAccessBlockConfiguration?.BlockPublicAccess ??
    response?.data?.BlockPublicAccess;
  if (value === true || value === "true") return true;
  if (value === false || value === "false") return false;
  throw new Error("OSS returned an unrecognized Block Public Access setting; refusing public uploads");
}
