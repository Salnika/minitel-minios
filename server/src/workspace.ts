import fs from "node:fs/promises";
import path from "node:path";

import type { AppConfig } from "./config.js";
import { HttpError, isErrnoException } from "./errors.js";
import { buildFileId, isSupportedFileName, parseFileId, resolveFolderPath } from "./paths.js";
import { pathExists, writeTextFileAtomic } from "./storage.js";
import {
  compareTasks,
  formatTaskLine,
  normalizeForSearch,
  parseProjectSheet,
  parseTaskLine,
  setTaskDone,
  slugify,
  startOfWeek,
  sumLoggedMinutes,
} from "./tasks.js";
import type { AddTaskInput, ListTasksOptions, ProjectSummary, SearchHit, Task } from "./types.js";

// Project manager built on top of the plain text files: every project is a folder
// `projets/<name>/` with a sheet, a task list and a time log, all regular Markdown files.
export const PROJECTS_FOLDER = "projets";
const SHEET_FILE = "projet.md";
const TASKS_FILE = "taches.md";
const TIME_FILE = "temps.md";
const SNIPPET_LENGTH = 80;

interface TextFile {
  id: string;
  folder: string;
  filePath: string;
}

async function readTextIfExists(filePath: string): Promise<string | undefined> {
  try {
    return await fs.readFile(filePath, "utf8");
  } catch (error) {
    if (isErrnoException(error) && (error.code === "ENOENT" || error.code === "EISDIR")) {
      return undefined;
    }
    throw error;
  }
}

function projectOfFolder(folder: string): string {
  const segments = folder.split("/");
  return segments[0] === PROJECTS_FOLDER && segments[1] !== undefined ? segments[1] : "";
}

function snippetAround(line: string, matchIndex: number): string {
  const trimmed = line.trim();
  if (trimmed.length <= SNIPPET_LENGTH) {
    return trimmed;
  }
  const start = Math.max(0, Math.min(matchIndex - 20, trimmed.length - SNIPPET_LENGTH));
  return trimmed.slice(start, start + SNIPPET_LENGTH);
}

export function createWorkspace(config: AppConfig) {
  async function collectTextFiles(folder: string): Promise<TextFile[]> {
    const { folderPath, folder: safeFolder } = resolveFolderPath(config.dataPath, folder);
    let entries;
    try {
      entries = await fs.readdir(folderPath, { withFileTypes: true });
    } catch (error) {
      if (isErrnoException(error) && error.code === "ENOENT") {
        return [];
      }
      throw error;
    }

    const files: TextFile[] = [];
    for (const entry of entries.sort((left, right) => left.name.localeCompare(right.name))) {
      if (entry.isDirectory()) {
        files.push(...(await collectTextFiles(`${safeFolder}/${entry.name}`)));
      } else if (entry.isFile() && isSupportedFileName(entry.name)) {
        files.push({
          id: buildFileId(safeFolder, entry.name),
          folder: safeFolder,
          filePath: path.join(folderPath, entry.name),
        });
      }
    }
    return files;
  }

  async function collectAllTextFiles(): Promise<TextFile[]> {
    const entries = await fs.readdir(config.dataPath, { withFileTypes: true }).catch(() => []);
    const files: TextFile[] = [];
    for (const entry of entries) {
      if (entry.isDirectory()) {
        files.push(...(await collectTextFiles(entry.name)));
      }
    }
    return files;
  }

  async function tasksOfFiles(files: TextFile[]): Promise<Task[]> {
    const tasks: Task[] = [];
    for (const file of files) {
      const content = (await readTextIfExists(file.filePath)) ?? "";
      content.split("\n").forEach((line, index) => {
        const parsed = parseTaskLine(line.replace(/\r$/, ""));
        if (parsed) {
          tasks.push({
            file: file.id,
            line: index + 1,
            project: projectOfFolder(file.folder),
            ...parsed,
          });
        }
      });
    }
    return tasks;
  }

  async function projectNames(): Promise<string[]> {
    const { folderPath } = resolveFolderPath(config.dataPath, PROJECTS_FOLDER);
    const entries = await fs.readdir(folderPath, { withFileTypes: true }).catch(() => []);
    return entries
      .filter((entry) => entry.isDirectory())
      .map((entry) => entry.name)
      .sort((left, right) => left.localeCompare(right));
  }

  async function summarizeProject(name: string, today: string): Promise<ProjectSummary> {
    const folder = `${PROJECTS_FOLDER}/${name}`;
    const { folderPath } = resolveFolderPath(config.dataPath, folder);
    const files = await collectTextFiles(folder);
    const tasks = await tasksOfFiles(files);
    const sheet = parseProjectSheet(
      (await readTextIfExists(path.join(folderPath, SHEET_FILE))) ?? "",
    );
    const timeLog = (await readTextIfExists(path.join(folderPath, TIME_FILE))) ?? "";

    let updatedAtMs = (await fs.stat(folderPath)).mtimeMs;
    for (const file of files) {
      updatedAtMs = Math.max(updatedAtMs, (await fs.stat(file.filePath)).mtimeMs);
    }

    const openTasks = tasks.filter((task) => !task.done);
    return {
      name,
      title: sheet.title ?? name,
      ...(sheet.status !== undefined ? { status: sheet.status } : {}),
      ...(sheet.goal !== undefined ? { goal: sheet.goal } : {}),
      ...(sheet.next !== undefined ? { next: sheet.next } : {}),
      openTasks: openTasks.length,
      overdueTasks: openTasks.filter((task) => task.due !== undefined && task.due < today).length,
      weekMinutes: sumLoggedMinutes(timeLog, startOfWeek(today)),
      updatedAt: new Date(updatedAtMs).toISOString(),
    };
  }

  async function listProjects(today: string): Promise<ProjectSummary[]> {
    const names = await projectNames();
    return Promise.all(names.map((name) => summarizeProject(name, today)));
  }

  async function createProject(
    title: string,
    goal: string | undefined,
    today: string,
  ): Promise<ProjectSummary> {
    const name = slugify(title);
    if (name.length === 0) {
      throw new HttpError(400, "Invalid project name");
    }
    const { folderPath } = resolveFolderPath(config.dataPath, `${PROJECTS_FOLDER}/${name}`);
    if (await pathExists(folderPath)) {
      throw new HttpError(400, "Project already exists");
    }

    await fs.mkdir(folderPath, { recursive: true });
    const sheet = [
      `# ${title.trim()}`,
      "Statut: en cours",
      `Objectif: ${goal?.trim() ?? ""}`,
      "Prochaine etape: ",
      "",
      "Notes:",
      "",
    ].join("\n");
    await writeTextFileAtomic(path.join(folderPath, SHEET_FILE), sheet);
    await writeTextFileAtomic(path.join(folderPath, TASKS_FILE), "# Taches\n");
    return summarizeProject(name, today);
  }

  async function listTasks(options: ListTasksOptions): Promise<{ items: Task[]; total: number }> {
    const folder =
      options.project !== undefined ? `${PROJECTS_FOLDER}/${options.project}` : PROJECTS_FOLDER;
    let tasks = await tasksOfFiles(await collectTextFiles(folder));

    if (options.status === "open") {
      tasks = tasks.filter((task) => !task.done);
    } else if (options.status === "done") {
      tasks = tasks.filter((task) => task.done);
    }
    if (options.due === "today") {
      tasks = tasks.filter((task) => task.due !== undefined && task.due <= options.today);
    } else if (options.due === "overdue") {
      tasks = tasks.filter((task) => task.due !== undefined && task.due < options.today);
    }

    tasks.sort(compareTasks);
    return { items: tasks.slice(0, options.limit), total: tasks.length };
  }

  async function addTask(input: AddTaskInput): Promise<Task> {
    const folder = `${PROJECTS_FOLDER}/${input.project}`;
    const { folderPath, folder: safeFolder } = resolveFolderPath(config.dataPath, folder);
    if (!(await pathExists(folderPath))) {
      throw new HttpError(404, "Project not found");
    }

    const filePath = path.join(folderPath, TASKS_FILE);
    const existing = (await readTextIfExists(filePath)) ?? "# Taches\n";
    const separator = existing.length > 0 && !existing.endsWith("\n") ? "\n" : "";
    const line = formatTaskLine(input);
    const content = `${existing}${separator}${line}\n`;
    if (Buffer.byteLength(content, "utf8") > config.maxFileSizeBytes) {
      throw new HttpError(400, "File too large");
    }
    await writeTextFileAtomic(filePath, content);

    const parsed = parseTaskLine(line);
    if (!parsed) {
      throw new HttpError(500, "Task could not be written");
    }
    return {
      file: buildFileId(safeFolder, TASKS_FILE),
      line: content.split("\n").length - 1,
      project: input.project,
      ...parsed,
    };
  }

  async function toggleTask(
    id: string,
    lineNumber: number,
    done: boolean | undefined,
  ): Promise<Task> {
    const resolved = parseFileId(config.dataPath, id);
    const content = await readTextIfExists(resolved.filePath);
    if (content === undefined) {
      throw new HttpError(404, "File not found");
    }

    const lines = content.split("\n");
    const rawLine = lines[lineNumber - 1];
    // Keep a CRLF line ending as it was.
    const lineEnding = rawLine?.endsWith("\r") === true ? "\r" : "";
    const current = rawLine?.slice(0, rawLine.length - lineEnding.length);
    const parsed = current !== undefined ? parseTaskLine(current) : undefined;
    if (current === undefined || !parsed) {
      // The file changed since the client listed it.
      throw new HttpError(409, "Line is not a task");
    }

    const updated = setTaskDone(current, done ?? !parsed.done);
    if (updated === undefined) {
      throw new HttpError(409, "Line is not a task");
    }
    lines[lineNumber - 1] = `${updated}${lineEnding}`;
    await writeTextFileAtomic(resolved.filePath, lines.join("\n"));

    return {
      file: resolved.id,
      line: lineNumber,
      project: projectOfFolder(resolved.folder),
      ...parsed,
      done: done ?? !parsed.done,
    };
  }

  // Appends one entry (inbox capture, journal, time log) without rewriting the file from
  // the client: the Minitel only holds 2 KB of a document.
  async function appendToFile(id: string, text: string): Promise<{ id: string; size: number }> {
    const resolved = parseFileId(config.dataPath, id);
    await fs.mkdir(path.dirname(resolved.filePath), { recursive: true });

    const existing = (await readTextIfExists(resolved.filePath)) ?? "";
    const separator = existing.length > 0 && !existing.endsWith("\n") ? "\n" : "";
    const entry = text.endsWith("\n") ? text : `${text}\n`;
    const content = `${existing}${separator}${entry}`;
    const size = Buffer.byteLength(content, "utf8");
    if (size > config.maxFileSizeBytes) {
      throw new HttpError(400, "File too large");
    }
    await writeTextFileAtomic(resolved.filePath, content);
    return { id: resolved.id, size };
  }

  // Case- and accent-insensitive substring search over every text file.
  async function search(
    query: string,
    folder: string | undefined,
    limit: number,
  ): Promise<SearchHit[]> {
    const needle = normalizeForSearch(query.trim());
    if (needle.length === 0) {
      throw new HttpError(400, "q must be a non-empty string");
    }

    const files =
      folder !== undefined ? await collectTextFiles(folder) : await collectAllTextFiles();
    const hits: SearchHit[] = [];
    for (const file of files) {
      const content = (await readTextIfExists(file.filePath)) ?? "";
      const lines = content.split("\n");
      for (let index = 0; index < lines.length && hits.length < limit; ++index) {
        const line = lines[index] ?? "";
        const matchIndex = normalizeForSearch(line).indexOf(needle);
        if (matchIndex >= 0) {
          hits.push({ id: file.id, line: index + 1, snippet: snippetAround(line, matchIndex) });
        }
      }
      if (hits.length >= limit) {
        break;
      }
    }
    return hits;
  }

  return {
    listProjects,
    createProject,
    listTasks,
    addTask,
    toggleTask,
    appendToFile,
    search,
  };
}
