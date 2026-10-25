import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { afterEach, beforeEach, describe, expect, it } from "vite-plus/test";

import { buildApp } from "./app.js";
import type { AppConfig } from "./config.js";

const headers = { authorization: "Bearer super-secret" };

describe("Minitel project manager API", () => {
  let tempDirectory: string;
  let app!: ReturnType<typeof buildApp>;

  beforeEach(async () => {
    tempDirectory = await fs.mkdtemp(path.join(os.tmpdir(), "bibliotek-workspace-"));
    const config: AppConfig = {
      apiToken: "super-secret",
      dataPath: path.join(tempDirectory, "files"),
      port: 3000,
      maxFileSizeBytes: 4096,
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

  async function createProject(): Promise<void> {
    const response = await app.inject({
      method: "POST",
      url: "/projects?today=2026-10-20",
      headers,
      payload: { title: "Robot Minitel", goal: "Afficher" },
    });
    expect(response.statusCode).toBe(201);
    expect(response.json()).toMatchObject({ name: "robot-minitel", title: "Robot Minitel" });
  }

  it("creates projects and summarizes their sheet, tasks and time", async () => {
    await createProject();
    const duplicate = await app.inject({
      method: "POST",
      url: "/projects",
      headers,
      payload: { title: "robot minitel" },
    });
    expect(duplicate.statusCode).toBe(400);

    await app.inject({
      method: "POST",
      url: "/tasks",
      headers,
      payload: { project: "robot-minitel", text: "Commander", due: "2026-10-19" },
    });
    await app.inject({
      method: "POST",
      url: "/files/append",
      headers,
      payload: { id: "projets/robot-minitel/temps.md", text: "- 2026-10-20 14:00 +90 min Soudure" },
    });

    const projects = await app.inject({
      method: "GET",
      url: "/projects?today=2026-10-21",
      headers,
    });
    expect(projects.statusCode).toBe(200);
    expect(projects.json().items).toEqual([
      expect.objectContaining({
        name: "robot-minitel",
        title: "Robot Minitel",
        status: "en cours",
        goal: "Afficher",
        openTasks: 1,
        overdueTasks: 1,
        weekMinutes: 90,
      }),
    ]);
  });

  it("adds, filters and toggles tasks", async () => {
    await createProject();
    const added = await app.inject({
      method: "POST",
      url: "/tasks",
      headers,
      payload: { project: "robot-minitel", text: "Commander", due: "2026-10-19", priority: 1 },
    });
    expect(added.statusCode).toBe(201);
    expect(added.json()).toEqual({
      file: "projets/robot-minitel/taches.md",
      line: 2,
      project: "robot-minitel",
      done: false,
      text: "Commander",
      due: "2026-10-19",
      priority: 1,
    });
    await app.inject({
      method: "POST",
      url: "/tasks",
      headers,
      payload: { project: "robot-minitel", text: "Documenter" },
    });

    const missingProject = await app.inject({
      method: "POST",
      url: "/tasks",
      headers,
      payload: { project: "inconnu", text: "X" },
    });
    expect(missingProject.statusCode).toBe(404);
    const multiline = await app.inject({
      method: "POST",
      url: "/tasks",
      headers,
      payload: { project: "robot-minitel", text: "a\nb" },
    });
    expect(multiline.statusCode).toBe(400);

    const all = await app.inject({ method: "GET", url: "/tasks?project=robot-minitel", headers });
    expect(all.json().items.map((task: { text: string }) => task.text)).toEqual([
      "Commander",
      "Documenter",
    ]);
    const today = await app.inject({
      method: "GET",
      url: "/tasks?due=today&today=2026-10-20",
      headers,
    });
    expect(today.json()).toMatchObject({ total: 1, items: [{ text: "Commander" }] });

    const toggled = await app.inject({
      method: "POST",
      url: "/tasks/toggle",
      headers,
      payload: { file: "projets/robot-minitel/taches.md", line: 2 },
    });
    expect(toggled.json()).toMatchObject({ done: true, text: "Commander" });
    const file = await app.inject({
      method: "GET",
      url: "/files/projets%2Frobot-minitel/taches.md",
      headers,
    });
    expect(file.json().content).toBe("# Taches\n- [x] Commander ! @2026-10-19\n- [ ] Documenter\n");

    const open = await app.inject({ method: "GET", url: "/tasks", headers });
    expect(open.json().total).toBe(1);
    const stale = await app.inject({
      method: "POST",
      url: "/tasks/toggle",
      headers,
      payload: { file: "projets/robot-minitel/taches.md", line: 1 },
    });
    expect(stale.statusCode).toBe(409);
  });

  it("appends lines, creating folders and files as needed", async () => {
    for (const text of ["Idee", "Autre"]) {
      const response = await app.inject({
        method: "POST",
        url: "/files/append",
        headers,
        payload: { id: "inbox/inbox.md", text },
      });
      expect(response.statusCode).toBe(200);
    }
    const file = await app.inject({ method: "GET", url: "/files/inbox/inbox.md", headers });
    expect(file.json().content).toBe("Idee\nAutre\n");

    const unsafe = await app.inject({
      method: "POST",
      url: "/files/append",
      headers,
      payload: { id: "../evil.md", text: "x" },
    });
    expect(unsafe.statusCode).toBe(400);
  });

  it("searches across files ignoring case and accents", async () => {
    await app.inject({
      method: "POST",
      url: "/files/append",
      headers,
      payload: { id: "notes/reunion.md", text: "Décision: passer l'API en HTTPS" },
    });
    const response = await app.inject({ method: "GET", url: "/search?q=DECISION", headers });
    expect(response.json()).toEqual({
      items: [{ id: "notes/reunion.md", line: 1, snippet: "Décision: passer l'API en HTTPS" }],
    });
  });

  it("reads, writes and deletes files by id in the query string", async () => {
    await app.inject({
      method: "POST",
      url: "/files/append",
      headers,
      payload: { id: "projets/robot/notes.md", text: "v1" },
    });
    const read = await app.inject({
      method: "GET",
      url: "/file?id=projets/robot/notes.md",
      headers,
    });
    expect(read.json()).toMatchObject({ id: "projets/robot/notes.md", content: "v1\n" });

    const written = await app.inject({
      method: "PUT",
      url: "/file?id=projets/robot/notes.md",
      headers,
      payload: { content: "v2", title: "Notes" },
    });
    expect(written.json()).toMatchObject({ content: "v2", title: "Notes" });

    const deleted = await app.inject({
      method: "DELETE",
      url: "/file?id=projets/robot/notes.md",
      headers,
    });
    expect(deleted.statusCode).toBe(200);
    const gone = await app.inject({
      method: "GET",
      url: "/file?id=projets/robot/notes.md",
      headers,
    });
    expect(gone.statusCode).toBe(404);
  });

  it("accepts empty contents when a note is cleared", async () => {
    await app.inject({ method: "POST", url: "/folders", headers, payload: { name: "notes" } });
    const created = await app.inject({
      method: "POST",
      url: "/files",
      headers,
      payload: { folder: "notes", name: "vide.md", content: "" },
    });
    expect(created.statusCode).toBe(201);
    const updated = await app.inject({
      method: "PUT",
      url: "/files/notes/vide.md",
      headers,
      payload: { content: "" },
    });
    expect(updated.statusCode).toBe(200);
  });

  it("publishes and serves firmware for over-the-air updates", async () => {
    const missing = await app.inject({ method: "GET", url: "/firmware/manifest", headers });
    expect(missing.statusCode).toBe(404);

    const binary = Buffer.from(Array.from({ length: 100 }, (_value, index) => index));
    const published = await app.inject({
      method: "PUT",
      url: "/firmware?version=1.2.0",
      headers: { ...headers, "content-type": "application/octet-stream" },
      payload: binary,
    });
    expect(published.statusCode).toBe(201);
    expect(published.json()).toMatchObject({
      version: "1.2.0",
      size: 100,
      md5: "7acedd1a84a4cfcb6e7a16003242945e",
    });

    const manifest = await app.inject({ method: "GET", url: "/firmware/manifest", headers });
    expect(manifest.json()).toMatchObject({ version: "1.2.0", size: 100 });
    const download = await app.inject({ method: "GET", url: "/firmware/download", headers });
    expect(download.headers["content-length"]).toBe("100");
    expect(download.rawPayload.equals(binary)).toBe(true);

    const badVersion = await app.inject({
      method: "PUT",
      url: "/firmware?version=latest",
      headers: { ...headers, "content-type": "application/octet-stream" },
      payload: binary,
    });
    expect(badVersion.statusCode).toBe(400);
    const anonymous = await app.inject({ method: "GET", url: "/firmware/download" });
    expect(anonymous.statusCode).toBe(401);
  });
});
