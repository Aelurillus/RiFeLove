/**
 * RifeLove is currently distributed in local-only mode.
 *
 * Keep this guard in the executable Aliyun OSS entry points so a stale shell
 * with OSS credentials cannot accidentally publish or mutate a bucket.
 */
export function failAliyunOssDisabled() {
  throw new Error(
    "Aliyun OSS publishing is disabled in local-only mode; keep releases and update metadata on the local filesystem."
  );
}

failAliyunOssDisabled();
