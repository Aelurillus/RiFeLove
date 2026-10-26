import { open } from "node:fs/promises";

const EOCD_SIGNATURE = 0x06054b50;
const ZIP64_LOCATOR_SIGNATURE = 0x07064b50;
const ZIP64_EOCD_SIGNATURE = 0x06064b50;
const CENTRAL_DIRECTORY_SIGNATURE = 0x02014b50;
const MAX_EOCD_SEARCH = 22 + 0xffff;
const MAX_CENTRAL_DIRECTORY_BYTES = 128 * 1024 * 1024;
const MAX_ENTRIES = 1_000_000;

const PRIVATE_STATE_EXTENSIONS = new Set([".sqlite", ".sqlite3", ".db", ".log", ".jsonl"]);
const SOURCE_MEDIA_EXTENSIONS = new Set([
  ".3gp", ".aac", ".avi", ".flac", ".flv", ".m2ts", ".m4a", ".m4v", ".mkv", ".mov",
  ".mp3", ".mp4", ".mpeg", ".mpg", ".mts", ".ogg", ".ts", ".vob", ".wav", ".webm", ".wmv",
]);
const PRIVATE_STATE_BASENAMES = new Set([
  "language.ini", "preferences.ini", "queue.sqlite-wal", "queue.sqlite-shm",
]);
const SECRET_EXTENSIONS = new Set([".pem", ".p12", ".pfx"]);

export async function validateReleaseArchive(archivePath) {
  const file = await open(archivePath, "r");
  try {
    const { size } = await file.stat();
    if (!Number.isSafeInteger(size) || size < 22) throw new Error("Release archive is too small to be a ZIP file.");

    const tailLength = Math.min(size, MAX_EOCD_SEARCH);
    const tail = await readExactly(file, tailLength, size - tailLength);
    const eocdIndex = findEndOfCentralDirectory(tail);
    if (eocdIndex < 0) throw new Error("Release archive has no valid ZIP directory footer.");
    const eocdOffset = size - tailLength + eocdIndex;
    const eocd = tail.subarray(eocdIndex);
    const diskNumber = eocd.readUInt16LE(4);
    const centralDirectoryDisk = eocd.readUInt16LE(6);
    if (diskNumber !== 0 || centralDirectoryDisk !== 0) throw new Error("Multi-disk ZIP archives are not supported.");

    let entryCount = eocd.readUInt16LE(10);
    let centralDirectorySize = eocd.readUInt32LE(12);
    let centralDirectoryOffset = eocd.readUInt32LE(16);
    if (entryCount === 0xffff || centralDirectorySize === 0xffffffff || centralDirectoryOffset === 0xffffffff) {
      const zip64 = await readZip64Directory(file, eocdOffset);
      entryCount = zip64.entryCount;
      centralDirectorySize = zip64.centralDirectorySize;
      centralDirectoryOffset = zip64.centralDirectoryOffset;
    }

    if (!Number.isSafeInteger(entryCount) || entryCount < 1 || entryCount > MAX_ENTRIES) {
      throw new Error("Release archive has an invalid or unreasonable file count.");
    }
    if (!Number.isSafeInteger(centralDirectorySize) || centralDirectorySize <= 0 || centralDirectorySize > MAX_CENTRAL_DIRECTORY_BYTES ||
        !Number.isSafeInteger(centralDirectoryOffset) || centralDirectoryOffset < 0 ||
        centralDirectoryOffset + centralDirectorySize > eocdOffset) {
      throw new Error("Release archive has an invalid or unreasonable ZIP directory.");
    }

    const directory = await readExactly(file, centralDirectorySize, centralDirectoryOffset);
    const unsafeEntries = [];
    let offset = 0;
    for (let index = 0; index < entryCount; index += 1) {
      if (offset + 46 > directory.length || directory.readUInt32LE(offset) !== CENTRAL_DIRECTORY_SIGNATURE) {
        throw new Error("Release archive has a damaged ZIP directory.");
      }
      const flags = directory.readUInt16LE(offset + 8);
      const nameLength = directory.readUInt16LE(offset + 28);
      const extraLength = directory.readUInt16LE(offset + 30);
      const commentLength = directory.readUInt16LE(offset + 32);
      const recordEnd = offset + 46 + nameLength + extraLength + commentLength;
      if (recordEnd > directory.length) throw new Error("Release archive has a truncated ZIP directory entry.");
      if ((flags & 0x0001) !== 0) throw new Error("Encrypted release archives cannot be inspected safely.");

      const nameBytes = directory.subarray(offset + 46, offset + 46 + nameLength);
      const name = decodeZipName(nameBytes, (flags & 0x0800) !== 0);
      const reason = getUnsafeEntryReason(name);
      if (reason) unsafeEntries.push(`${name} (${reason})`);
      offset = recordEnd;
    }
    if (offset !== directory.length) throw new Error("Release archive contains unsupported or malformed ZIP directory records.");
    if (unsafeEntries.length) {
      throw new Error(`Release archive failed local-data checks: ${unsafeEntries.slice(0, 12).join(", ")}${unsafeEntries.length > 12 ? `, and ${unsafeEntries.length - 12} more` : ""}`);
    }

    return { entryCount, centralDirectoryBytes: centralDirectorySize };
  } finally {
    await file.close();
  }
}

async function readZip64Directory(file, eocdOffset) {
  const locatorOffset = eocdOffset - 20;
  if (locatorOffset < 0) throw new Error("Release archive is missing its ZIP64 directory locator.");
  const locator = await readExactly(file, 20, locatorOffset);
  if (locator.readUInt32LE(0) !== ZIP64_LOCATOR_SIGNATURE || locator.readUInt32LE(4) !== 0 || locator.readUInt32LE(16) !== 1) {
    throw new Error("Release archive has an invalid ZIP64 directory locator.");
  }
  const recordOffset = safeNumber(locator.readBigUInt64LE(8), "ZIP64 directory offset");
  const header = await readExactly(file, 56, recordOffset);
  if (header.readUInt32LE(0) !== ZIP64_EOCD_SIGNATURE || header.readBigUInt64LE(4) < 44n) {
    throw new Error("Release archive has an invalid ZIP64 directory record.");
  }
  if (header.readUInt32LE(16) !== 0 || header.readUInt32LE(20) !== 0) {
    throw new Error("Multi-disk ZIP64 archives are not supported.");
  }
  return {
    entryCount: safeNumber(header.readBigUInt64LE(32), "ZIP64 file count"),
    centralDirectorySize: safeNumber(header.readBigUInt64LE(40), "ZIP64 directory size"),
    centralDirectoryOffset: safeNumber(header.readBigUInt64LE(48), "ZIP64 directory offset"),
  };
}

function findEndOfCentralDirectory(buffer) {
  const firstPossibleIndex = Math.max(0, buffer.length - MAX_EOCD_SEARCH);
  for (let index = buffer.length - 22; index >= firstPossibleIndex; index -= 1) {
    if (buffer.readUInt32LE(index) !== EOCD_SIGNATURE) continue;
    const commentLength = buffer.readUInt16LE(index + 20);
    if (index + 22 + commentLength === buffer.length) return index;
  }
  return -1;
}

function getUnsafeEntryReason(rawName) {
  if (!rawName || /[\u0000-\u001f]/.test(rawName)) return "invalid path";
  const name = rawName.replaceAll("\\", "/");
  const parts = name.split("/");
  const fileParts = name.endsWith("/") ? parts.slice(0, -1) : parts;
  if (name.startsWith("/") || /^\/?[a-z]:/i.test(name) || name.startsWith("//")) return "absolute path";
  if (parts.some(part => part === ".." || part === "." || part === "" && parts.indexOf(part) !== parts.length - 1)) {
    return "unsafe path traversal";
  }
  if (fileParts.some(part => part.includes(":") || /[ .]$/.test(part))) return "Windows-unsafe path component";

  const lowerParts = fileParts.map(part => part.toLowerCase());
  if (lowerParts.includes("data")) return "local settings or task data directory";
  if (lowerParts.includes("logs")) return "local log directory";
  const baseName = lowerParts.at(-1) ?? "";
  const extension = baseName.includes(".") ? baseName.slice(baseName.lastIndexOf(".")) : "";
  if (PRIVATE_STATE_BASENAMES.has(baseName) || PRIVATE_STATE_EXTENSIONS.has(extension)) return "local settings, database, or log file";
  if (SOURCE_MEDIA_EXTENSIONS.has(extension)) return "source or generated media file";
  if (SECRET_EXTENSIONS.has(extension) || baseName === ".env" || baseName.startsWith(".env.")) return "credential or private-key file";
  if (baseName === "first-run.png" || baseName === "studio-smoke.png") return "generated workstation screenshot";
  return null;
}

function decodeZipName(bytes, isUtf8) {
  try {
    return new TextDecoder(isUtf8 ? "utf-8" : "latin1", { fatal: true }).decode(bytes);
  } catch {
    throw new Error("Release archive contains a filename that cannot be inspected safely.");
  }
}

function safeNumber(value, label) {
  if (value > BigInt(Number.MAX_SAFE_INTEGER)) throw new Error(`Release archive ${label} is too large to inspect safely.`);
  return Number(value);
}

async function readExactly(file, length, position) {
  const buffer = Buffer.alloc(length);
  let bytesRead = 0;
  while (bytesRead < length) {
    const result = await file.read(buffer, bytesRead, length - bytesRead, position + bytesRead);
    if (result.bytesRead === 0) throw new Error("Release archive is truncated.");
    bytesRead += result.bytesRead;
  }
  return buffer;
}
