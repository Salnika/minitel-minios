import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { afterEach, beforeEach, describe, expect, it } from "vite-plus/test";

import {
  assistantTools,
  describeToolError,
  normalizeToolPath,
  prepareTool,
  type ToolContext,
} from "./assistant-tools.js";
import type { AppConfig } from "./config.js";
import { createStorage } from "./storage.js";
import { createWorkspace } from "./workspace.js";

describe("assistant tools", () => {
  let tempDirectory: string;
  let context: ToolContext;

  beforeEach(async () => {
    tempDirectory = await fs.mkdtemp(path.join(os.tmpdir(), "bibliotek-tools-"));
    const config: AppConfig = {
      apiToken: "super-secret",
      dataPath: path.join(tempDirectory, "files"),
      port: 3000,
      maxFileSizeBytes: 1_048_576,
      requestTimeoutMs: 10_000,
      firmwarePath: path.join(tempDirectory, "firmware"),
      firmwareMaxBytes: 1024,
    };
    const storage = createStorage(config);
    await storage.ensureDataRoot();
    context = { storage, workspace: createWorkspace(config), today: "2026-10-20" };
  });

  afterEach(async () => {
    await fs.rm(tempDirectory, { recursive: true, force: true });
  });

  async function writeNote(id: string, content: string): Promise<void> {
    const separator = id.lastIndexOf("/");
    await context.storage.ensureFolder(id.slice(0, separator));
    await context.storage.createFile({
      folder: id.slice(0, separator),
      name: id.slice(separator + 1),
      content,
    });
  }

  // Runs a call the way the assistant does, without the user's approval.
  async function call(name: string, args: Record<string, unknown>): Promise<string> {
    const prepared = await prepareTool(context, name, JSON.stringify(args));
    if ("error" in prepared) {
      return `Erreur : ${prepared.error}`;
    }
    try {
      return await prepared.run();
    } catch (error) {
      return `Erreur : ${describeToolError(error)}`;
    }
  }

  it("describes every tool for the chat completions API", () => {
    expect(assistantTools.map((tool) => tool.function.name)).toEqual([
      "list_folder",
      "read_file",
      "search",
      "write_file",
      "append_to_file",
      "edit_file",
      "create_folder",
      "move_file",
      "delete_file",
      "list_projects",
      "list_tasks",
      "add_task",
      "set_task_done",
      "create_project",
    ]);
    for (const tool of assistantTools) {
      expect(tool.type).toBe("function");
      expect(tool.function.parameters).toMatchObject({ type: "object" });
    }
  });

  it("normalizes the paths models write", () => {
    expect(normalizeToolPath("/notes/courses.md")).toBe("notes/courses.md");
    expect(normalizeToolPath("./notes/")).toBe("notes");
    expect(normalizeToolPath(" / ")).toBe("");
    expect(normalizeToolPath(".")).toBe("");
  });

  it("lists folders and reads files", async () => {
    await writeNote("notes/courses.md", "- lait\n- pain\n");
    await context.storage.ensureFolder("notes/recettes/desserts");

    expect(await call("list_folder", {})).toBe("Racine : 1 dossier, 0 fichier\nnotes/");
    const listing = await call("list_folder", { path: "/notes/" });
    expect(listing).toContain("Dossier notes : 1 dossier, 1 fichier");
    expect(listing).toContain("notes/recettes/\n");
    expect(listing).not.toContain("desserts");
    expect(listing).toMatch(/notes\/courses\.md \(14 o, modifie le \d{4}-\d{2}-\d{2}\)/);
    expect(await call("list_folder", { path: "absent" })).toBe(
      'Erreur : dossier "absent" introuvable',
    );

    expect(await call("read_file", { path: "notes/courses.md" })).toBe(
      "notes/courses.md (2 lignes) :\n- lait\n- pain",
    );
    expect(await call("read_file", { path: "notes/absent.md" })).toBe("Erreur : File not found");
    expect(await call("read_file", { path: "courses.md" })).toContain("sans dossier");
  });

  it("reads a long file in parts", async () => {
    // 10 bytes per line with its newline: 600 lines fill one read.
    const lines = Array.from(
      { length: 1000 },
      (_, index) => `ligne${String(index + 1).padStart(4, "0")}`,
    );
    await writeNote("notes/long.md", `${lines.join("\n")}\n`);

    const first = await call("read_file", { path: "notes/long.md" });
    expect(first.split("\n")[0]).toBe(
      "notes/long.md, lignes 1 a 600 sur 1000 (suite avec start_line=601) :",
    );
    expect(first).toContain("ligne0600");
    expect(first).not.toContain("ligne0601");

    const rest = await call("read_file", { path: "notes/long.md", start_line: 601 });
    expect(rest.split("\n")[0]).toBe("notes/long.md, lignes 601 a 1000 sur 1000 :");
    expect(rest.endsWith("ligne1000")).toBe(true);
    expect(await call("read_file", { path: "notes/long.md", start_line: 1001 })).toBe(
      "Erreur : notes/long.md a 1000 lignes",
    );
  });

  it("searches the notes", async () => {
    await writeNote("notes/courses.md", "- Lait entier\n- pain\n");
    expect(await call("search", { query: "lait" })).toBe(
      "1 resultat :\nnotes/courses.md:1: - Lait entier",
    );
    expect(await call("search", { query: "beurre" })).toBe('Aucun resultat pour "beurre".');
  });

  it("creates files without asking, but asks before replacing one", async () => {
    const created = await prepareTool(
      context,
      "write_file",
      JSON.stringify({ path: "notes/neuf/idees.md", content: "Robot\nMinitel\n" }),
    );
    expect(created).toMatchObject({ label: "cree notes/neuf/idees.md" });
    expect("question" in created).toBe(false);
    if ("error" in created) {
      throw new Error(created.error);
    }
    expect(await created.run()).toBe("notes/neuf/idees.md cree (2 lignes).");
    expect((await context.storage.getFile("notes/neuf/idees.md")).content).toBe("Robot\nMinitel\n");

    const replaced = await prepareTool(
      context,
      "write_file",
      JSON.stringify({ path: "notes/neuf/idees.md", content: "Vide" }),
    );
    expect(replaced).toMatchObject({
      label: "remplace notes/neuf/idees.md",
      question: "Remplacer tout notes/neuf/idees.md (2 -> 1 ligne)",
    });
    if ("error" in replaced) {
      throw new Error(replaced.error);
    }
    await replaced.run();
    expect((await context.storage.getFile("notes/neuf/idees.md")).content).toBe("Vide");
  });

  it("edits one exact passage, after asking", async () => {
    await writeNote("notes/courses.md", "- lait\n- pain\n- pain de mie\n");

    expect(
      await call("edit_file", { path: "notes/courses.md", old_text: "beurre", new_text: "x" }),
    ).toContain("texte introuvable");
    expect(
      await call("edit_file", { path: "notes/courses.md", old_text: "pain", new_text: "x" }),
    ).toContain("texte present 2 fois");

    const prepared = await prepareTool(
      context,
      "edit_file",
      JSON.stringify({ path: "notes/courses.md", old_text: "- lait", new_text: "- lait $& 2 L" }),
    );
    expect(prepared).toMatchObject({
      label: "modifie notes/courses.md",
      question: 'Modifier notes/courses.md : "- lait" -> "- lait $& 2 L"',
    });
    if ("error" in prepared) {
      throw new Error(prepared.error);
    }
    expect(await prepared.run()).toBe("notes/courses.md modifie.");
    expect((await context.storage.getFile("notes/courses.md")).content).toBe(
      "- lait $& 2 L\n- pain\n- pain de mie\n",
    );

    const erase = await prepareTool(
      context,
      "edit_file",
      JSON.stringify({ path: "notes/courses.md", old_text: "- pain de mie\n", new_text: "" }),
    );
    expect(erase).toMatchObject({ question: 'Effacer de notes/courses.md : "- pain de mie"' });
  });

  it("appends, creates folders, moves and deletes", async () => {
    expect(
      await call("append_to_file", { path: "inbox/inbox.md", text: "- idee\n- autre\n\n" }),
    ).toBe("Texte ajoute a la fin de inbox/inbox.md.");
    expect((await context.storage.getFile("inbox/inbox.md")).content).toBe("- idee\n- autre\n");

    expect(await call("create_folder", { path: "archives/2026" })).toBe(
      "Dossier archives/2026 cree.",
    );
    expect(await call("create_folder", { path: "archives/2026" })).toBe(
      "Le dossier archives/2026 existe deja.",
    );

    const move = await prepareTool(
      context,
      "move_file",
      JSON.stringify({ path: "inbox/inbox.md", new_path: "archives/2026/vieux.md" }),
    );
    expect(move).toMatchObject({ label: "deplace inbox/inbox.md vers archives/2026/vieux.md" });
    expect("question" in move).toBe(false);
    if ("error" in move) {
      throw new Error(move.error);
    }
    await move.run();
    expect(await context.storage.fileExists("inbox/inbox.md")).toBe(false);
    expect((await context.storage.getFile("archives/2026/vieux.md")).content).toBe(
      "- idee\n- autre\n",
    );

    await writeNote("notes/a.md", "a");
    expect(
      await call("move_file", { path: "notes/a.md", new_path: "archives/2026/vieux.md" }),
    ).toBe("Erreur : archives/2026/vieux.md existe deja");

    const deletion = await prepareTool(
      context,
      "delete_file",
      JSON.stringify({ path: "notes/a.md" }),
    );
    expect(deletion).toMatchObject({
      label: "supprime notes/a.md",
      question: "Supprimer notes/a.md (1 ligne)",
    });
    if ("error" in deletion) {
      throw new Error(deletion.error);
    }
    await deletion.run();
    expect(await context.storage.fileExists("notes/a.md")).toBe(false);
  });

  it("manages projects and their tasks", async () => {
    expect(await call("create_project", { title: "Robot Minitel", goal: "Afficher" })).toBe(
      "Projet cree : projets/robot-minitel/ (projet.md, taches.md).",
    );
    expect(
      await call("add_task", {
        project: "projets/robot-minitel",
        text: "Souder",
        due: "2026-10-19",
        priority: 2,
      }),
    ).toBe("Tache ajoutee : projets/robot-minitel/taches.md:2 - [ ] Souder !! @2026-10-19");
    expect(await call("add_task", { project: "robot-minitel", text: "Peindre" })).toContain(
      "taches.md:3",
    );
    expect(await call("add_task", { project: "robot-minitel", text: "a\nb" })).toContain(
      "une seule ligne",
    );
    expect(
      await call("add_task", { project: "robot-minitel", text: "Vernir", due: "demain" }),
    ).toContain("AAAA-MM-JJ");

    expect(await call("list_projects", {})).toBe(
      "robot-minitel : Robot Minitel, statut en cours, 2 taches ouvertes, 1 en retard",
    );
    expect(await call("list_tasks", { filter: "today" })).toBe(
      "1 tache :\nprojets/robot-minitel/taches.md:2 - [ ] Souder !! @2026-10-19",
    );
    expect(await call("list_tasks", { filter: "bientot" })).toContain('filtre "bientot" inconnu');

    const done = await prepareTool(
      context,
      "set_task_done",
      JSON.stringify({ file: "projets/robot-minitel/taches.md", line: 2 }),
    );
    expect(done).toMatchObject({ label: "coche projets/robot-minitel/taches.md:2" });
    if ("error" in done) {
      throw new Error(done.error);
    }
    expect(await done.run()).toBe("Tache cochee : Souder");
    expect(
      await call("set_task_done", { file: "projets/robot-minitel/taches.md", line: 1 }),
    ).toContain("n'est pas une tache");
    expect(await call("list_tasks", { project: "robot-minitel", filter: "done" })).toBe(
      "1 tache :\nprojets/robot-minitel/taches.md:2 - [x] Souder !! @2026-10-19",
    );
  });

  it("names the calls it cannot run", async () => {
    expect(await prepareTool(context, "format_disk", "{}")).toEqual({
      label: "outil format_disk",
      error: 'outil "format_disk" inconnu',
    });
    expect(await prepareTool(context, "read_file", '{"path": "notes/a')).toEqual({
      label: "lit ?",
      error: "arguments JSON invalides ou coupes : refais l'appel, avec moins de texte",
    });
    expect(await prepareTool(context, "read_file", "{}")).toEqual({
      label: "lit ?",
      error: 'argument "path" manquant',
    });
    expect(await prepareTool(context, "list_projects", "")).toMatchObject({
      label: "liste les projets",
    });
  });
});
