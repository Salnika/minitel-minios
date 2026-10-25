import { createHash, randomUUID } from "node:crypto";
import { createReadStream, type ReadStream } from "node:fs";
import fs from "node:fs/promises";
import path from "node:path";

import type { AppConfig } from "./config.js";
import { HttpError, isErrnoException } from "./errors.js";
import type { FirmwareManifest } from "./types.js";

// Firmware published for over-the-air updates of the Minitel: one binary plus a manifest
// (version, size, MD5) that the device checks before flashing.
const BINARY_FILE = "firmware.bin";
const MANIFEST_FILE = "manifest.json";
const versionPattern = /^\d{1,4}(\.\d{1,4}){0,3}$/;

export function createFirmwareStore(config: AppConfig) {
  const binaryPath = path.join(config.firmwarePath, BINARY_FILE);
  const manifestPath = path.join(config.firmwarePath, MANIFEST_FILE);

  async function getManifest(): Promise<FirmwareManifest> {
    try {
      return JSON.parse(await fs.readFile(manifestPath, "utf8")) as FirmwareManifest;
    } catch (error) {
      if (isErrnoException(error) && error.code === "ENOENT") {
        throw new HttpError(404, "No firmware published");
      }
      throw error;
    }
  }

  async function openBinary(): Promise<{ stream: ReadStream; size: number }> {
    try {
      const stats = await fs.stat(binaryPath);
      return { stream: createReadStream(binaryPath), size: stats.size };
    } catch (error) {
      if (isErrnoException(error) && error.code === "ENOENT") {
        throw new HttpError(404, "No firmware published");
      }
      throw error;
    }
  }

  async function publish(version: string, data: Buffer): Promise<FirmwareManifest> {
    if (!versionPattern.test(version)) {
      throw new HttpError(400, "version must look like 1.2.0");
    }
    if (data.length === 0) {
      throw new HttpError(400, "Firmware is empty");
    }

    await fs.mkdir(config.firmwarePath, { recursive: true });
    const manifest: FirmwareManifest = {
      version,
      size: data.length,
      md5: createHash("md5").update(data).digest("hex"),
      publishedAt: new Date().toISOString(),
    };

    // Binary first, manifest last: a device never sees a manifest for a missing binary.
    const tempBinary = `${binaryPath}.${randomUUID()}.tmp`;
    await fs.writeFile(tempBinary, data);
    await fs.rename(tempBinary, binaryPath);
    const tempManifest = `${manifestPath}.${randomUUID()}.tmp`;
    await fs.writeFile(tempManifest, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
    await fs.rename(tempManifest, manifestPath);
    return manifest;
  }

  return { getManifest, openBinary, publish };
}
