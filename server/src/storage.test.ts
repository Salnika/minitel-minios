import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { afterEach, beforeEach, describe, expect, it } from "vite-plus/test";

import type { AppConfig } from "./config.js";
import { createStorage, sortFileSummaries } from "./storage.js";

describe("storage", () => {
  let tempDirectory: string;
  let config: AppConfig;

  beforeEach(async () => {
    tempDirectory = await fs.mkdtemp(path.join(os.tmpdir(), "bibliotek-storage-"));
    config = {
      apiToken: "test-token",
      dataPath: path.join(tempDirectory, "files"),
      port: 3000,
      maxFileSizeBytes: 1024,
      requestTimeoutMs: 10_000,
      firmwarePath: path.join(tempDirectory, "firmware"),
      firmwareMaxBytes: 1024,
    };
  });

  afterEach(async () => {
    await fs.rm(tempDirectory, { recursive: true, force: true });
  });

  it("writes and updates sidecar metadata files", async () => {
    const storage = createStorage(config);

    await storage.ensureDataRoot();
    await storage.createFolder("notes");
    await storage.createFile({
      folder: "notes",
      name: "entry.txt",
      content: "hello",
      title: "Entry",
      tags: ["daily"],
      sortOrder: 2,
    });

    const metaPath = path.join(config.dataPath, "notes/entry.meta.json");
    const createdMeta = JSON.parse(await fs.readFile(metaPath, "utf8")) as {
      title: string;
      tags: string[];
      sortOrder: number;
    };
    expect(createdMeta).toEqual({
      title: "Entry",
      tags: ["daily"],
      sortOrder: 2,
    });

    const updated = await storage.updateFile("notes/entry.txt", {
      content: "updated",
      title: "Updated entry",
    });

    expect(updated.title).toBe("Updated entry");
    expect(updated.tags).toEqual(["daily"]);
    expect(updated.sortOrder).toBe(2);
  });

  it("sorts manual order with name fallback", () => {
    const files = [
      {
        id: "notes/b.txt",
        name: "b.txt",
        folder: "notes",
        updatedAt: "2026-03-24T00:00:00.000Z",
        size: 10,
      },
      {
        id: "notes/a.txt",
        name: "a.txt",
        title: "A",
        folder: "notes",
        updatedAt: "2026-03-24T00:00:00.000Z",
        size: 10,
        sortOrder: 1,
      },
      {
        id: "notes/c.txt",
        name: "c.txt",
        folder: "notes",
        updatedAt: "2026-03-24T00:00:00.000Z",
        size: 10,
        sortOrder: 5,
      },
    ];

    expect(sortFileSummaries(files, "manual", "asc").map((file) => file.name)).toEqual([
      "a.txt",
      "c.txt",
      "b.txt",
    ]);
  });
});
