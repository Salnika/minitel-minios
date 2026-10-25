import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { afterEach, beforeEach, describe, expect, it } from "vite-plus/test";

import { buildApp } from "./app.js";
import type { AppConfig } from "./config.js";

function authHeader(token: string): { authorization: string } {
  return { authorization: `Bearer ${token}` };
}

describe("text storage API", () => {
  let tempDirectory: string;
  let config: AppConfig;
  let app!: ReturnType<typeof buildApp>;

  beforeEach(async () => {
    tempDirectory = await fs.mkdtemp(path.join(os.tmpdir(), "bibliotek-app-"));
    config = {
      apiToken: "super-secret",
      dataPath: path.join(tempDirectory, "files"),
      port: 3000,
      maxFileSizeBytes: 64,
      requestTimeoutMs: 10_000,
      firmwarePath: path.join(tempDirectory, "firmware"),
      firmwareMaxBytes: 1024,
    };
    app = buildApp(config);
    await app.ready();
  });

  afterEach(async () => {
    await app.close();
    await fs.rm(tempDirectory, { recursive: true, force: true });
  });

  it("keeps health public and protects other routes", async () => {
    const healthResponse = await app.inject({
      method: "GET",
      url: "/health",
    });
    expect(healthResponse.statusCode).toBe(200);
    expect(healthResponse.json()).toEqual({ status: "ok" });

    const missingAuthResponse = await app.inject({
      method: "GET",
      url: "/folders",
    });
    expect(missingAuthResponse.statusCode).toBe(401);
    expect(missingAuthResponse.json()).toEqual({
      error: "Missing authorization token",
    });

    const invalidAuthResponse = await app.inject({
      method: "GET",
      url: "/folders",
      headers: authHeader("wrong-token"),
    });
    expect(invalidAuthResponse.statusCode).toBe(403);
    expect(invalidAuthResponse.json()).toEqual({
      error: "Invalid API token",
    });
  });

  it("creates folders and runs the full file lifecycle", async () => {
    const headers = authHeader(config.apiToken);

    const createFolderResponse = await app.inject({
      method: "POST",
      url: "/folders",
      headers,
      payload: { name: "notes" },
    });
    expect(createFolderResponse.statusCode).toBe(201);

    const createFileResponse = await app.inject({
      method: "POST",
      url: "/files",
      headers,
      payload: {
        folder: "notes",
        name: "entry.txt",
        content: "hello",
        title: "My entry",
        tags: ["draft"],
        sortOrder: 3,
      },
    });
    expect(createFileResponse.statusCode).toBe(201);
    expect(createFileResponse.json()).toMatchObject({
      id: "notes/entry.txt",
      title: "My entry",
      tags: ["draft"],
      sortOrder: 3,
      content: "hello",
    });

    const getFileResponse = await app.inject({
      method: "GET",
      url: "/files/notes/entry.txt",
      headers,
    });
    expect(getFileResponse.statusCode).toBe(200);
    expect(getFileResponse.json()).toMatchObject({
      id: "notes/entry.txt",
      content: "hello",
    });

    const updateFileResponse = await app.inject({
      method: "PUT",
      url: "/files/notes/entry.txt",
      headers,
      payload: {
        content: "updated",
        title: "Updated entry",
      },
    });
    expect(updateFileResponse.statusCode).toBe(200);
    expect(updateFileResponse.json()).toMatchObject({
      title: "Updated entry",
      tags: ["draft"],
      sortOrder: 3,
      content: "updated",
    });

    const deleteFileResponse = await app.inject({
      method: "DELETE",
      url: "/files/notes/entry.txt",
      headers,
    });
    expect(deleteFileResponse.statusCode).toBe(200);
    expect(deleteFileResponse.json()).toEqual({ ok: true });

    const missingFileResponse = await app.inject({
      method: "GET",
      url: "/files/notes/entry.txt",
      headers,
    });
    expect(missingFileResponse.statusCode).toBe(404);
  });

  it("renames and moves files with metadata preserved", async () => {
    const headers = authHeader(config.apiToken);

    await app.inject({
      method: "POST",
      url: "/folders",
      headers,
      payload: { name: "notes" },
    });
    await app.inject({
      method: "POST",
      url: "/folders",
      headers,
      payload: { name: "journal" },
    });
    await app.inject({
      method: "POST",
      url: "/files",
      headers,
      payload: {
        folder: "notes",
        name: "entry.txt",
        content: "hello",
        title: "Original title",
        sortOrder: 9,
      },
    });

    const renameResponse = await app.inject({
      method: "POST",
      url: "/files/notes/entry.txt/rename",
      headers,
      payload: { newName: "entry.md" },
    });
    expect(renameResponse.statusCode).toBe(200);
    expect(renameResponse.json()).toMatchObject({
      id: "notes/entry.md",
      title: "Original title",
      sortOrder: 9,
    });

    const moveResponse = await app.inject({
      method: "POST",
      url: "/files/notes/entry.md/move",
      headers,
      payload: { targetFolder: "journal" },
    });
    expect(moveResponse.statusCode).toBe(200);
    expect(moveResponse.json()).toMatchObject({
      id: "journal/entry.md",
      folder: "journal",
      title: "Original title",
      sortOrder: 9,
    });

    const journalFilesResponse = await app.inject({
      method: "GET",
      url: "/files?folder=journal",
      headers,
    });
    expect(journalFilesResponse.statusCode).toBe(200);
    expect(journalFilesResponse.json()).toEqual([
      expect.objectContaining({
        id: "journal/entry.md",
      }),
    ]);
  });

  it("supports nested folders in listings and file routes", async () => {
    const headers = authHeader(config.apiToken);

    const createParentResponse = await app.inject({
      method: "POST",
      url: "/folders",
      headers,
      payload: { name: "projects" },
    });
    expect(createParentResponse.statusCode).toBe(201);

    const createChildResponse = await app.inject({
      method: "POST",
      url: "/folders",
      headers,
      payload: { name: "projects/minios" },
    });
    expect(createChildResponse.statusCode).toBe(201);

    const listFoldersResponse = await app.inject({
      method: "GET",
      url: "/folders",
      headers,
    });
    expect(listFoldersResponse.statusCode).toBe(200);
    expect(listFoldersResponse.json()).toEqual(["projects", "projects/minios"]);

    const createFileResponse = await app.inject({
      method: "POST",
      url: "/files",
      headers,
      payload: {
        folder: "projects/minios",
        name: "readme.txt",
        content: "hello",
      },
    });
    expect(createFileResponse.statusCode).toBe(201);
    expect(createFileResponse.json()).toMatchObject({
      id: "projects/minios/readme.txt",
      folder: "projects/minios",
    });

    const getFileResponse = await app.inject({
      method: "GET",
      url: "/files/projects%2Fminios/readme.txt",
      headers,
    });
    expect(getFileResponse.statusCode).toBe(200);
    expect(getFileResponse.json()).toMatchObject({
      id: "projects/minios/readme.txt",
      content: "hello",
    });
  });

  it("lists files with sorting and filtering", async () => {
    const headers = authHeader(config.apiToken);
    await fs.mkdir(path.join(config.dataPath, "notes"), { recursive: true });
    await fs.mkdir(path.join(config.dataPath, "drafts"), { recursive: true });
    await fs.writeFile(path.join(config.dataPath, "notes/a.txt"), "a", "utf8");
    await fs.writeFile(path.join(config.dataPath, "notes/b.txt"), "bb", "utf8");
    await fs.writeFile(path.join(config.dataPath, "drafts/c.md"), "ccc", "utf8");
    await fs.writeFile(
      path.join(config.dataPath, "notes/a.meta.json"),
      JSON.stringify({ sortOrder: 2 }),
      "utf8",
    );
    await fs.writeFile(
      path.join(config.dataPath, "notes/b.meta.json"),
      JSON.stringify({ sortOrder: 1 }),
      "utf8",
    );

    const folderResponse = await app.inject({
      method: "GET",
      url: "/files?folder=notes&sort=manual",
      headers,
    });
    expect(folderResponse.statusCode).toBe(200);
    expect(folderResponse.json().map((file: { name: string }) => file.name)).toEqual([
      "b.txt",
      "a.txt",
    ]);

    const globalResponse = await app.inject({
      method: "GET",
      url: "/files?sort=size&order=desc",
      headers,
    });
    expect(globalResponse.statusCode).toBe(200);
    expect(globalResponse.json().map((file: { name: string }) => file.name)).toEqual([
      "c.md",
      "b.txt",
      "a.txt",
    ]);
  });

  it("rejects unsupported extensions and oversized files", async () => {
    const headers = authHeader(config.apiToken);

    await app.inject({
      method: "POST",
      url: "/folders",
      headers,
      payload: { name: "notes" },
    });

    const invalidExtensionResponse = await app.inject({
      method: "POST",
      url: "/files",
      headers,
      payload: {
        folder: "notes",
        name: "entry.json",
        content: "hello",
      },
    });
    expect(invalidExtensionResponse.statusCode).toBe(400);

    const largeFileResponse = await app.inject({
      method: "POST",
      url: "/files",
      headers,
      payload: {
        folder: "notes",
        name: "entry.txt",
        content: "x".repeat(65),
      },
    });
    expect(largeFileResponse.statusCode).toBe(400);
    expect(largeFileResponse.json()).toEqual({ error: "File too large" });
  });
});
