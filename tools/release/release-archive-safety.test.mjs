import assert from "node:assert/strict";
import test from "node:test";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { validateReleaseArchive } from "./release-archive-safety.mjs";

test("release archive accepts application files and model metadata", async t => {
  const archive = await makeArchive(t, ["RifeLove.exe", "models/rife/model.onnx", "models/rife/validation.json", "使用说明.txt"]);
  const result = await validateReleaseArchive(archive);
  assert.equal(result.entryCount, 4);
});

test("release archive rejects local settings, queue databases, and logs", async t => {
  for (const name of ["data/preferences.ini", "data/queue.sqlite", "logs/rifelove.jsonl"]) {
    const archive = await makeArchive(t, ["RifeLove.exe", name]);
    await assert.rejects(validateReleaseArchive(archive), /failed local-data checks/);
  }
});

test("release archive rejects bundled media and workstation screenshots", async t => {
  for (const name of ["source.mp4", "results/output.mkv", "studio-smoke.png"]) {
    const archive = await makeArchive(t, ["RifeLove.exe", name]);
    await assert.rejects(validateReleaseArchive(archive), /failed local-data checks/);
  }
});

test("release archive rejects absolute and traversal paths", async t => {
  for (const name of ["../outside.txt", "C:/Users/test/preferences.ini", "/absolute/path.txt"]) {
    const archive = await makeArchive(t, [name]);
    await assert.rejects(validateReleaseArchive(archive), /failed local-data checks/);
  }
});

test("release archive rejects malformed files", async t => {
  const directory = await makeTempDirectory(t);
  const archive = path.join(directory, "not-a-zip.zip");
  await writeFile(archive, "not a zip file");
  await assert.rejects(validateReleaseArchive(archive), /too small to be a ZIP/);
});

async function makeArchive(t, names) {
  const directory = await makeTempDirectory(t);
  const archive = path.join(directory, "release.zip");
  await writeFile(archive, buildStoredZip(names));
  return archive;
}

async function makeTempDirectory(t) {
  const directory = await mkdtemp(path.join(os.tmpdir(), "rifelove-archive-safety-"));
  t.after(() => rm(directory, { recursive: true, force: true }));
  return directory;
}

function buildStoredZip(names) {
  const localRecords = [];
  const centralRecords = [];
  let localOffset = 0;
  for (const name of names) {
    const nameBytes = Buffer.from(name, "utf8");
    const local = Buffer.alloc(30 + nameBytes.length);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x0800, 6);
    local.writeUInt16LE(0, 8);
    local.writeUInt32LE(0, 14);
    local.writeUInt32LE(0, 18);
    local.writeUInt32LE(0, 22);
    local.writeUInt16LE(nameBytes.length, 26);
    local.writeUInt16LE(0, 28);
    nameBytes.copy(local, 30);
    localRecords.push(local);

    const central = Buffer.alloc(46 + nameBytes.length);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(0x0314, 4);
    central.writeUInt16LE(20, 6);
    central.writeUInt16LE(0x0800, 8);
    central.writeUInt16LE(0, 10);
    central.writeUInt32LE(0, 16);
    central.writeUInt32LE(0, 20);
    central.writeUInt32LE(0, 24);
    central.writeUInt16LE(nameBytes.length, 28);
    central.writeUInt16LE(0, 30);
    central.writeUInt16LE(0, 32);
    central.writeUInt16LE(0, 34);
    central.writeUInt32LE(0, 38);
    central.writeUInt32LE(localOffset, 42);
    nameBytes.copy(central, 46);
    centralRecords.push(central);

    localOffset += local.length;
  }
  const centralDirectory = Buffer.concat(centralRecords);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(0, 4);
  end.writeUInt16LE(0, 6);
  end.writeUInt16LE(names.length, 8);
  end.writeUInt16LE(names.length, 10);
  end.writeUInt32LE(centralDirectory.length, 12);
  end.writeUInt32LE(localOffset, 16);
  end.writeUInt16LE(0, 20);
  return Buffer.concat([...localRecords, centralDirectory, end]);
}
