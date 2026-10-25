import { HttpError } from "./errors.js";
import type { createStorage } from "./storage.js";
import { datePattern, formatTaskLine, timePattern } from "./tasks.js";
import type { TaskDueFilter, TaskStatus } from "./types.js";
import type { createWorkspace } from "./workspace.js";

// Tools of the Minitel assistant: what the model may do with the notes, the projects and
// their tasks. Each call gets a short label for the Minitel ("lit notes/courses.md"); the
// calls that change or remove existing text first ask the user (`question`).

type Storage = ReturnType<typeof createStorage>;
type Workspace = ReturnType<typeof createWorkspace>;

// The device's language. Labels, questions, results and errors are written
// tr(lang, "francais", "English") where they are made, like the firmware's TR(); the tool
// definitions stay in French, as do the names of the data (projets/, taches.md, Statut...).
export type Lang = "fr" | "en";

export function tr(lang: Lang | undefined, french: string, english: string): string {
  return lang === "en" ? english : french;
}

export interface ToolContext {
  storage: Storage;
  workspace: Workspace;
  // The device's local date (YYYY-MM-DD): "today" and "late" for the tasks.
  today: string;
  // French when absent.
  lang?: Lang;
}

export type PreparedTool =
  | { label: string; error: string }
  | { label: string; question?: string; run: () => Promise<string> };

type ToolArguments = Record<string, unknown>;

interface ReadyTool {
  label?: string;
  question?: string;
  run: () => Promise<string>;
}

interface ToolSpec {
  name: string;
  description: string;
  parameters: Record<string, unknown>;
  // Built from the raw arguments, before they are checked: it also names a failed call.
  label: (args: ToolArguments, lang?: Lang) => string;
  // Throws when the arguments are wrong or the call cannot work (file missing...).
  prepare: (context: ToolContext, args: ToolArguments) => Promise<ReadyTool>;
}

// A mistake the model can fix: the message goes back to it, in the device's language.
class ToolError extends Error {
  readonly english: string;

  constructor(french: string, english: string) {
    super(french);
    this.name = "ToolError";
    this.english = english;
  }
}

const MAX_READ_CHARS = 6000;
const MAX_LIST_LINES = 80;
const MAX_SEARCH_HITS = 30;
const MAX_TASKS = 50;
const taskFilters = ["open", "today", "overdue", "done", "all"] as const;

function stringSchema(description: string): Record<string, unknown> {
  return { type: "string", description };
}

function integerSchema(description: string): Record<string, unknown> {
  return { type: "integer", description };
}

function objectSchema(
  properties: Record<string, unknown>,
  required: string[] = [],
): Record<string, unknown> {
  return {
    type: "object",
    properties,
    ...(required.length > 0 ? { required } : {}),
  };
}

// Models write "/notes/x.md", "./notes" or "notes/": the storage wants "notes/x.md".
export function normalizeToolPath(value: string): string {
  const path = value
    .trim()
    .replace(/^(\.\/|\/)+/, "")
    .replace(/\/+$/, "");
  return path === "." ? "" : path;
}

function readString(args: ToolArguments, name: string): string {
  const value = args[name];
  if (typeof value !== "string" || value.trim().length === 0) {
    throw new ToolError(`argument "${name}" manquant`, `missing argument "${name}"`);
  }
  return value;
}

// Unlike names, a text may be empty (a cleared file, a passage to delete).
function readText(args: ToolArguments, name: string): string {
  const value = args[name];
  if (typeof value !== "string") {
    throw new ToolError(`argument "${name}" manquant`, `missing argument "${name}"`);
  }
  return value;
}

function readOptionalString(args: ToolArguments, name: string): string | undefined {
  const value = args[name];
  if (value === undefined || value === null || value === "") {
    return undefined;
  }
  if (typeof value !== "string") {
    throw new ToolError(`argument "${name}" invalide`, `invalid argument "${name}"`);
  }
  return value;
}

function readOptionalInteger(args: ToolArguments, name: string): number | undefined {
  const value = args[name];
  if (value === undefined || value === null || value === "") {
    return undefined;
  }
  const parsed = typeof value === "string" ? Number(value) : value;
  if (typeof parsed !== "number" || !Number.isInteger(parsed)) {
    throw new ToolError(
      `argument "${name}" : nombre entier attendu`,
      `argument "${name}": integer expected`,
    );
  }
  return parsed;
}

function readSingleLine(args: ToolArguments, name: string): string {
  const value = readString(args, name);
  if (/[\r\n]/.test(value)) {
    throw new ToolError(
      `argument "${name}" : une seule ligne`,
      `argument "${name}": one line only`,
    );
  }
  return value.trim();
}

function readFolderPath(args: ToolArguments, name: string): string {
  const folder = normalizeToolPath(readString(args, name));
  if (folder.length === 0) {
    throw new ToolError(
      `argument "${name}" : dossier manquant`,
      `argument "${name}": missing folder`,
    );
  }
  return folder;
}

// Files always live in a folder, with a .md or .txt extension.
function readFilePath(args: ToolArguments, name: string): string {
  const id = normalizeToolPath(readString(args, name));
  if (!id.includes("/")) {
    throw new ToolError(
      `chemin "${id}" sans dossier : par exemple notes/${id}`,
      `path "${id}" has no folder: for example notes/${id}`,
    );
  }
  if (!/\.(md|txt)$/.test(id)) {
    throw new ToolError(
      `chemin "${id}" : seuls les fichiers .md et .txt sont geres`,
      `path "${id}": only .md and .txt files are supported`,
    );
  }
  return id;
}

// For the labels, built before the arguments are checked.
function pathOf(value: unknown): string {
  return typeof value === "string" && value.trim().length > 0 ? normalizeToolPath(value) : "?";
}

function textOf(value: unknown): string {
  return typeof value === "string" && value.trim().length > 0 ? value.trim() : "?";
}

function quote(text: string, max: number): string {
  const flat = text.replace(/\s+/g, " ").trim();
  return `"${flat.length > max ? `${flat.slice(0, max - 3)}...` : flat}"`;
}

function countLines(content: string): number {
  if (content.length === 0) {
    return 0;
  }
  return content.split("\n").length - (content.endsWith("\n") ? 1 : 0);
}

function splitFileId(id: string): { folder: string; name: string } {
  const separator = id.lastIndexOf("/");
  return { folder: id.slice(0, separator), name: id.slice(separator + 1) };
}

function formatSize(size: number, lang?: Lang): string {
  return size < 1024
    ? `${size} ${tr(lang, "o", "B")}`
    : `${(size / 1024).toFixed(1)} ${tr(lang, "Ko", "KB")}`;
}

function limitLines(lines: string[], max: number, lang?: Lang): string[] {
  if (lines.length <= max) {
    return lines;
  }
  const more = lines.length - max;
  return [...lines.slice(0, max), tr(lang, `(${more} de plus)`, `(${more} more)`)];
}

// French says "0 fichier", English "0 files".
function plural(count: number, word: string, lang?: Lang): string {
  const several = lang === "en" ? count !== 1 : count > 1;
  return `${count} ${word}${several ? "s" : ""}`;
}

// "projets/robot" or "robot": the project is the folder name under projets/.
function readProject(args: ToolArguments, name: string): string {
  return normalizeToolPath(readString(args, name)).replace(/^projets\//, "");
}

function occurrences(content: string, text: string): number {
  let count = 0;
  for (let index = content.indexOf(text); index >= 0; index = content.indexOf(text, index + 1)) {
    count += 1;
  }
  return count;
}

function checkUnique(content: string, text: string, id: string): void {
  const count = occurrences(content, text);
  if (count === 0) {
    throw new ToolError(
      `texte introuvable dans ${id} : relis le fichier et recopie le passage exact`,
      `text not found in ${id}: read the file again and copy the exact passage`,
    );
  }
  if (count > 1) {
    throw new ToolError(
      `texte present ${count} fois dans ${id} : ajoute du contexte pour qu'il soit unique`,
      `text found ${count} times in ${id}: add some context to make it unique`,
    );
  }
}

async function listFolder(context: ToolContext, folder: string): Promise<string> {
  const { lang } = context;
  const folders = await context.storage.listFolders();
  if (folder.length > 0 && !folders.includes(folder)) {
    throw new ToolError(`dossier "${folder}" introuvable`, `folder "${folder}" not found`);
  }

  const prefix = folder.length > 0 ? `${folder}/` : "";
  const subfolders = folders.filter(
    (candidate) =>
      candidate.startsWith(prefix) &&
      candidate.length > prefix.length &&
      !candidate.slice(prefix.length).includes("/"),
  );
  const files =
    folder.length > 0
      ? await context.storage.listFiles({ folder, sort: "name", order: "asc" })
      : [];
  const title =
    folder.length > 0
      ? tr(lang, `Dossier ${folder}`, `Folder ${folder}`)
      : tr(lang, "Racine", "Root");
  if (subfolders.length === 0 && files.length === 0) {
    return tr(lang, `${title} : vide.`, `${title}: empty.`);
  }

  const lines = [
    ...subfolders.map((name) => `${name}/`),
    ...files.map((file) => {
      const size = formatSize(file.size, lang);
      const date = file.updatedAt.slice(0, 10);
      return tr(
        lang,
        `${file.id} (${size}, modifie le ${date})`,
        `${file.id} (${size}, modified ${date})`,
      );
    }),
  ];
  return [
    tr(
      lang,
      `${title} : ${plural(subfolders.length, "dossier")}, ${plural(files.length, "fichier")}`,
      `${title}: ${plural(subfolders.length, "folder", "en")}, ${plural(files.length, "file", "en")}`,
    ),
    ...limitLines(lines, MAX_LIST_LINES, lang),
  ].join("\n");
}

async function readFile(context: ToolContext, id: string, startLine: number): Promise<string> {
  const { lang } = context;
  const file = await context.storage.getFile(id);
  if (file.content.length === 0) {
    return tr(lang, `${id} : fichier vide.`, `${id}: empty file.`);
  }

  const lines = file.content.split("\n");
  if (lines.at(-1) === "") {
    lines.pop();
  }
  const total = lines.length;
  if (startLine < 1 || startLine > total) {
    throw new ToolError(
      `${id} a ${plural(total, "ligne")}`,
      `${id} has ${plural(total, "line", "en")}`,
    );
  }

  const shown: string[] = [];
  let size = 0;
  let end = startLine - 1;
  while (end < total) {
    const line = lines[end] ?? "";
    if (shown.length > 0 && size + line.length + 1 > MAX_READ_CHARS) {
      break;
    }
    shown.push(line.length > MAX_READ_CHARS ? `${line.slice(0, MAX_READ_CHARS)}...` : line);
    size += line.length + 1;
    end += 1;
  }

  const next =
    end < total
      ? tr(lang, ` (suite avec start_line=${end + 1})`, ` (continue with start_line=${end + 1})`)
      : "";
  const header =
    startLine === 1 && end === total
      ? tr(lang, `${id} (${plural(total, "ligne")}) :`, `${id} (${plural(total, "line", "en")}):`)
      : tr(
          lang,
          `${id}, lignes ${startLine} a ${end} sur ${total}${next} :`,
          `${id}, lines ${startLine} to ${end} of ${total}${next}:`,
        );
  return `${header}\n${shown.join("\n")}`;
}

function taskQuery(filter: (typeof taskFilters)[number]): {
  status: TaskStatus;
  due?: TaskDueFilter;
} {
  switch (filter) {
    case "today":
      return { status: "open", due: "today" };
    case "overdue":
      return { status: "open", due: "overdue" };
    case "done":
      return { status: "done" };
    case "all":
      return { status: "all" };
    default:
      return { status: "open" };
  }
}

const pathDescription = 'Fichier, par exemple "notes/courses.md".';

const toolSpecs: ToolSpec[] = [
  {
    name: "list_folder",
    description: "Liste les sous-dossiers et les fichiers d'un dossier.",
    parameters: objectSchema({
      path: stringSchema('Dossier, par exemple "notes" ou "projets/robot". Vide pour la racine.'),
    }),
    label: (args, lang) => {
      const folder = typeof args.path === "string" ? normalizeToolPath(args.path) : "";
      return folder.length > 0
        ? tr(lang, `liste ${folder}/`, `list ${folder}/`)
        : tr(lang, "liste la racine", "list the root");
    },
    async prepare(context, args) {
      const folder = normalizeToolPath(readOptionalString(args, "path") ?? "");
      return { run: async () => listFolder(context, folder) };
    },
  },
  {
    name: "read_file",
    description: "Lit le contenu d'un fichier texte.",
    parameters: objectSchema(
      {
        path: stringSchema(pathDescription),
        start_line: integerSchema("Premiere ligne a lire, pour la suite d'un long fichier."),
      },
      ["path"],
    ),
    label: (args, lang) => tr(lang, `lit ${pathOf(args.path)}`, `read ${pathOf(args.path)}`),
    async prepare(context, args) {
      const id = readFilePath(args, "path");
      const startLine = readOptionalInteger(args, "start_line") ?? 1;
      return { run: async () => readFile(context, id, startLine) };
    },
  },
  {
    name: "search",
    description:
      "Cherche un texte dans toutes les notes, sans tenir compte des accents ni des majuscules. Donne le fichier et la ligne de chaque resultat.",
    parameters: objectSchema(
      {
        query: stringSchema("Texte a chercher."),
        folder: stringSchema("Optionnel : ne cherche que dans ce dossier."),
      },
      ["query"],
    ),
    label: (args, lang) => {
      const query = quote(textOf(args.query), 30);
      return tr(lang, `cherche ${query}`, `search ${query}`);
    },
    async prepare(context, args) {
      const query = readString(args, "query");
      const folderArgument = readOptionalString(args, "folder");
      const folder = folderArgument !== undefined ? normalizeToolPath(folderArgument) : undefined;
      return {
        run: async () => {
          const hits = await context.workspace.search(
            query,
            folder !== undefined && folder.length > 0 ? folder : undefined,
            MAX_SEARCH_HITS,
          );
          if (hits.length === 0) {
            return tr(
              context.lang,
              `Aucun resultat pour ${quote(query, 60)}.`,
              `No results for ${quote(query, 60)}.`,
            );
          }
          return [
            tr(
              context.lang,
              `${plural(hits.length, "resultat")} :`,
              `${plural(hits.length, "result", "en")}:`,
            ),
            ...hits.map((hit) => `${hit.id}:${hit.line}: ${hit.snippet}`),
          ].join("\n");
        },
      };
    },
  },
  {
    name: "write_file",
    description:
      "Cree un fichier (et ses dossiers) avec ce contenu, ou remplace tout le contenu d'un fichier existant. Pour un petit changement, prefere edit_file ; pour ajouter a la fin, append_to_file.",
    parameters: objectSchema(
      {
        path: stringSchema(pathDescription),
        content: stringSchema("Contenu complet du fichier."),
      },
      ["path", "content"],
    ),
    label: (args, lang) => tr(lang, `ecrit ${pathOf(args.path)}`, `write ${pathOf(args.path)}`),
    async prepare(context, args) {
      const { lang } = context;
      const id = readFilePath(args, "path");
      const content = readText(args, "content");
      const lines = countLines(content);
      if (await context.storage.fileExists(id)) {
        const current = await context.storage.getFile(id);
        const before = countLines(current.content);
        return {
          label: tr(lang, `remplace ${id}`, `replace ${id}`),
          question: tr(
            lang,
            `Remplacer tout ${id} (${before} -> ${plural(lines, "ligne")})`,
            `Overwrite ${id} (${before} -> ${plural(lines, "line", "en")})`,
          ),
          run: async () => {
            await context.storage.updateFile(id, { content });
            return tr(
              lang,
              `${id} remplace (${plural(lines, "ligne")}).`,
              `${id} replaced (${plural(lines, "line", "en")}).`,
            );
          },
        };
      }

      return {
        label: tr(lang, `cree ${id}`, `new ${id}`),
        run: async () => {
          const { folder, name } = splitFileId(id);
          await context.storage.ensureFolder(folder);
          await context.storage.createFile({ folder, name, content });
          return tr(
            lang,
            `${id} cree (${plural(lines, "ligne")}).`,
            `${id} created (${plural(lines, "line", "en")}).`,
          );
        },
      };
    },
  },
  {
    name: "append_to_file",
    description:
      "Ajoute du texte a la fin d'un fichier, sur une nouvelle ligne. Cree le fichier et ses dossiers s'ils n'existent pas.",
    parameters: objectSchema(
      {
        path: stringSchema(pathDescription),
        text: stringSchema("Texte a ajouter, une ou plusieurs lignes."),
      },
      ["path", "text"],
    ),
    label: (args, lang) => tr(lang, `ajoute a ${pathOf(args.path)}`, `add to ${pathOf(args.path)}`),
    async prepare(context, args) {
      const id = readFilePath(args, "path");
      const text = readString(args, "text").replace(/\s+$/, "");
      return {
        run: async () => {
          await context.workspace.appendToFile(id, text);
          return tr(
            context.lang,
            `Texte ajoute a la fin de ${id}.`,
            `Text added at the end of ${id}.`,
          );
        },
      };
    },
  },
  {
    name: "edit_file",
    description:
      "Remplace un passage d'un fichier par un autre texte. old_text doit apparaitre une seule fois dans le fichier : recopie-le exactement depuis read_file. new_text vide supprime le passage.",
    parameters: objectSchema(
      {
        path: stringSchema(pathDescription),
        old_text: stringSchema("Passage exact a remplacer."),
        new_text: stringSchema("Nouveau texte."),
      },
      ["path", "old_text", "new_text"],
    ),
    label: (args, lang) => tr(lang, `modifie ${pathOf(args.path)}`, `edit ${pathOf(args.path)}`),
    async prepare(context, args) {
      const { lang } = context;
      const id = readFilePath(args, "path");
      const oldText = readText(args, "old_text");
      const newText = readText(args, "new_text");
      if (oldText.length === 0) {
        throw new ToolError('argument "old_text" vide', 'empty argument "old_text"');
      }
      const file = await context.storage.getFile(id);
      checkUnique(file.content, oldText, id);
      return {
        question:
          newText.length === 0
            ? tr(
                lang,
                `Effacer de ${id} : ${quote(oldText, 60)}`,
                `Remove from ${id}: ${quote(oldText, 60)}`,
              )
            : tr(
                lang,
                `Modifier ${id} : ${quote(oldText, 40)} -> ${quote(newText, 40)}`,
                `Edit ${id}: ${quote(oldText, 40)} -> ${quote(newText, 40)}`,
              ),
        run: async () => {
          // The file may have changed while the user was deciding.
          const current = await context.storage.getFile(id);
          checkUnique(current.content, oldText, id);
          await context.storage.updateFile(id, {
            content: current.content.replace(oldText, () => newText),
          });
          return tr(lang, `${id} modifie.`, `${id} edited.`);
        },
      };
    },
  },
  {
    name: "create_folder",
    description: "Cree un dossier, avec ses dossiers parents.",
    parameters: objectSchema({ path: stringSchema('Dossier, par exemple "notes/recettes".') }, [
      "path",
    ]),
    label: (args, lang) =>
      tr(lang, `cree le dossier ${pathOf(args.path)}/`, `new folder ${pathOf(args.path)}/`),
    async prepare(context, args) {
      const folder = readFolderPath(args, "path");
      return {
        run: async () =>
          (await context.storage.ensureFolder(folder))
            ? tr(context.lang, `Dossier ${folder} cree.`, `Folder ${folder} created.`)
            : tr(
                context.lang,
                `Le dossier ${folder} existe deja.`,
                `Folder ${folder} already exists.`,
              ),
      };
    },
  },
  {
    name: "move_file",
    description:
      "Deplace ou renomme un fichier, en creant le dossier de destination. La destination ne doit pas exister.",
    parameters: objectSchema(
      {
        path: stringSchema(pathDescription),
        new_path: stringSchema('Nouveau chemin, par exemple "archives/courses.md".'),
      },
      ["path", "new_path"],
    ),
    label: (args, lang) => {
      const from = pathOf(args.path);
      const to = pathOf(args.new_path);
      return tr(lang, `deplace ${from} vers ${to}`, `move ${from} to ${to}`);
    },
    async prepare(context, args) {
      const from = readFilePath(args, "path");
      const to = readFilePath(args, "new_path");
      if (!(await context.storage.fileExists(from))) {
        throw new ToolError(`${from} introuvable`, `${from} not found`);
      }
      if (from !== to && (await context.storage.fileExists(to))) {
        throw new ToolError(`${to} existe deja`, `${to} already exists`);
      }
      return {
        run: async () => {
          await context.storage.relocateFile(from, to);
          return tr(context.lang, `${from} deplace vers ${to}.`, `${from} moved to ${to}.`);
        },
      };
    },
  },
  {
    name: "delete_file",
    description: "Supprime un fichier.",
    parameters: objectSchema({ path: stringSchema(pathDescription) }, ["path"]),
    label: (args, lang) => tr(lang, `supprime ${pathOf(args.path)}`, `delete ${pathOf(args.path)}`),
    async prepare(context, args) {
      const { lang } = context;
      const id = readFilePath(args, "path");
      const file = await context.storage.getFile(id);
      const lines = countLines(file.content);
      return {
        question: tr(
          lang,
          `Supprimer ${id} (${plural(lines, "ligne")})`,
          `Delete ${id} (${plural(lines, "line", "en")})`,
        ),
        run: async () => {
          await context.storage.deleteFile(id);
          return tr(lang, `${id} supprime.`, `${id} deleted.`);
        },
      };
    },
  },
  {
    name: "list_projects",
    description: "Liste les projets (dossiers de projets/) avec leur statut et leurs taches.",
    parameters: objectSchema({}),
    label: (_args, lang) => tr(lang, "liste les projets", "list projects"),
    async prepare(context) {
      const { lang } = context;
      return {
        run: async () => {
          const projects = await context.workspace.listProjects(context.today);
          if (projects.length === 0) {
            return tr(lang, "Aucun projet.", "No projects.");
          }
          return projects
            .map((project) => {
              const parts = [
                tr(lang, `${project.name} : ${project.title}`, `${project.name}: ${project.title}`),
              ];
              if (project.status !== undefined) {
                parts.push(tr(lang, `statut ${project.status}`, `status ${project.status}`));
              }
              const s = project.openTasks > 1 ? "s" : "";
              parts.push(
                tr(
                  lang,
                  `${project.openTasks} tache${s} ouverte${s}`,
                  plural(project.openTasks, "open task", "en"),
                ),
              );
              if (project.overdueTasks > 0) {
                parts.push(
                  tr(lang, `${project.overdueTasks} en retard`, `${project.overdueTasks} overdue`),
                );
              }
              if (project.next !== undefined) {
                parts.push(
                  tr(lang, `prochaine etape : ${project.next}`, `next step: ${project.next}`),
                );
              }
              return parts.join(", ");
            })
            .join("\n");
        },
      };
    },
  },
  {
    name: "list_tasks",
    description:
      "Liste les taches des projets, avec le fichier et la ligne de chacune (pour set_task_done).",
    parameters: objectSchema({
      project: stringSchema("Optionnel : dossier du projet, par exemple robot."),
      filter: {
        type: "string",
        enum: [...taskFilters],
        description:
          "open (par defaut) : non cochees ; today : a faire aujourd'hui ou en retard ; overdue : en retard ; done : cochees ; all : toutes.",
      },
    }),
    label: (args, lang) => {
      if (typeof args.project === "string" && args.project.trim().length > 0) {
        const project = normalizeToolPath(args.project).replace(/^projets\//, "");
        return tr(lang, `liste les taches de ${project}`, `list tasks of ${project}`);
      }
      return tr(lang, "liste les taches", "list tasks");
    },
    async prepare(context, args) {
      const { lang } = context;
      const project =
        readOptionalString(args, "project") !== undefined
          ? readProject(args, "project")
          : undefined;
      const filter = readOptionalString(args, "filter") ?? "open";
      if (!(taskFilters as readonly string[]).includes(filter)) {
        throw new ToolError(
          `filtre "${filter}" inconnu : ${taskFilters.join(", ")}`,
          `unknown filter "${filter}": ${taskFilters.join(", ")}`,
        );
      }
      return {
        run: async () => {
          const { items, total } = await context.workspace.listTasks({
            ...(project !== undefined ? { project } : {}),
            ...taskQuery(filter as (typeof taskFilters)[number]),
            today: context.today,
            limit: MAX_TASKS,
          });
          if (items.length === 0) {
            return tr(lang, "Aucune tache.", "No tasks.");
          }
          return [
            items.length < total
              ? tr(
                  lang,
                  `${items.length} taches sur ${total} :`,
                  `${items.length} tasks of ${total}:`,
                )
              : tr(lang, `${plural(total, "tache")} :`, `${plural(total, "task", "en")}:`),
            ...items.map((task) => `${task.file}:${task.line} ${formatTaskLine(task)}`),
          ].join("\n");
        },
      };
    },
  },
  {
    name: "add_task",
    description: "Ajoute une tache a la fin de projets/<project>/taches.md.",
    parameters: objectSchema(
      {
        project: stringSchema("Dossier du projet, par exemple robot."),
        text: stringSchema("Texte de la tache, sur une ligne."),
        due: stringSchema("Optionnel : echeance AAAA-MM-JJ."),
        time: stringSchema("Optionnel : heure HH:MM de l'echeance."),
        priority: integerSchema("Optionnel : priorite de 1 a 3."),
      },
      ["project", "text"],
    ),
    label: (args, lang) => {
      const project = pathOf(args.project).replace(/^projets\//, "");
      return tr(lang, `ajoute une tache a ${project}`, `add a task to ${project}`);
    },
    async prepare(context, args) {
      const project = readProject(args, "project");
      const text = readSingleLine(args, "text");
      const due = readOptionalString(args, "due");
      const time = readOptionalString(args, "time");
      const priority = readOptionalInteger(args, "priority");
      if (due !== undefined && !datePattern.test(due)) {
        throw new ToolError(
          'argument "due" : date AAAA-MM-JJ attendue',
          'argument "due": date YYYY-MM-DD expected',
        );
      }
      if (time !== undefined && !timePattern.test(time)) {
        throw new ToolError(
          'argument "time" : heure HH:MM attendue',
          'argument "time": time HH:MM expected',
        );
      }
      if (time !== undefined && due === undefined) {
        throw new ToolError(
          'argument "time" : il faut aussi "due"',
          'argument "time": "due" is needed too',
        );
      }
      if (priority !== undefined && (priority < 0 || priority > 3)) {
        throw new ToolError('argument "priority" : de 0 a 3', 'argument "priority": 0 to 3');
      }
      return {
        run: async () => {
          const task = await context.workspace.addTask({
            project,
            text,
            ...(due !== undefined ? { due } : {}),
            ...(time !== undefined ? { time } : {}),
            ...(priority !== undefined ? { priority } : {}),
          });
          const line = `${task.file}:${task.line} ${formatTaskLine(task)}`;
          return tr(context.lang, `Tache ajoutee : ${line}`, `Task added: ${line}`);
        },
      };
    },
  },
  {
    name: "set_task_done",
    description:
      "Coche ou decoche une tache, reperee par son fichier et sa ligne (donnes par list_tasks).",
    parameters: objectSchema(
      {
        file: stringSchema('Fichier de la tache, par exemple "projets/robot/taches.md".'),
        line: integerSchema("Numero de la ligne de la tache."),
        done: {
          type: "boolean",
          description: "true pour cocher (par defaut), false pour decocher.",
        },
      },
      ["file", "line"],
    ),
    label: (args, lang) => {
      const line =
        typeof args.line === "number" || typeof args.line === "string" ? String(args.line) : "?";
      const task = `${pathOf(args.file)}:${line}`;
      return args.done === false
        ? tr(lang, `decoche ${task}`, `untick ${task}`)
        : tr(lang, `coche ${task}`, `tick ${task}`);
    },
    async prepare(context, args) {
      const file = readFilePath(args, "file");
      const line = readOptionalInteger(args, "line");
      if (line === undefined || line <= 0) {
        throw new ToolError('argument "line" manquant', 'missing argument "line"');
      }
      const done = args.done !== false;
      return {
        run: async () => {
          const task = await context.workspace.toggleTask(file, line, done);
          return done
            ? tr(context.lang, `Tache cochee : ${task.text}`, `Task ticked: ${task.text}`)
            : tr(context.lang, `Tache decochee : ${task.text}`, `Task unticked: ${task.text}`);
        },
      };
    },
  },
  {
    name: "create_project",
    description: "Cree un projet : projets/<nom>/ avec sa fiche projet.md et taches.md.",
    parameters: objectSchema(
      {
        title: stringSchema("Titre du projet."),
        goal: stringSchema("Optionnel : objectif du projet."),
      },
      ["title"],
    ),
    label: (args, lang) => {
      const title = quote(textOf(args.title), 30);
      return tr(lang, `cree le projet ${title}`, `new project ${title}`);
    },
    async prepare(context, args) {
      const title = readSingleLine(args, "title");
      const goal = readOptionalString(args, "goal");
      if (goal !== undefined && /[\r\n]/.test(goal)) {
        throw new ToolError('argument "goal" : une seule ligne', 'argument "goal": one line only');
      }
      return {
        run: async () => {
          const project = await context.workspace.createProject(title, goal, context.today);
          return tr(
            context.lang,
            `Projet cree : projets/${project.name}/ (projet.md, taches.md).`,
            `Project created: projets/${project.name}/ (projet.md, taches.md).`,
          );
        },
      };
    },
  },
];

// The `tools` parameter of the chat completion.
export const assistantTools = toolSpecs.map((spec) => ({
  type: "function" as const,
  function: { name: spec.name, description: spec.description, parameters: spec.parameters },
}));

// Message for the model when a call fails.
export function describeToolError(error: unknown, lang?: Lang): string {
  if (error instanceof ToolError) {
    return tr(lang, error.message, error.english);
  }
  if (error instanceof HttpError) {
    if (error.statusCode === 409) {
      return tr(
        lang,
        "la ligne n'est pas une tache, le fichier a change : relis-le",
        "the line is not a task, the file changed: read it again",
      );
    }
    return error.message;
  }
  return tr(lang, "erreur interne", "internal error");
}

function parseArguments(raw: string): ToolArguments {
  if (raw.trim().length === 0) {
    return {};
  }
  const parsed = JSON.parse(raw) as unknown;
  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
    throw new ToolError("arguments : objet JSON attendu", "arguments: JSON object expected");
  }
  return parsed as ToolArguments;
}

// Checks a call of the model. Never throws: a call that cannot run comes back with `error`.
export async function prepareTool(
  context: ToolContext,
  name: string,
  rawArguments: string,
): Promise<PreparedTool> {
  const { lang } = context;
  const spec = toolSpecs.find((candidate) => candidate.name === name);
  if (spec === undefined) {
    const shown = name.length > 0 ? name : "?";
    return {
      label: tr(lang, `outil ${shown}`, `tool ${shown}`),
      error: tr(lang, `outil "${name}" inconnu`, `unknown tool "${name}"`),
    };
  }

  let args: ToolArguments;
  try {
    args = parseArguments(rawArguments);
  } catch {
    return {
      label: spec.label({}, lang),
      error: tr(
        lang,
        "arguments JSON invalides ou coupes : refais l'appel, avec moins de texte",
        "invalid or cut-off JSON arguments: retry the call with less text",
      ),
    };
  }

  const label = spec.label(args, lang);
  try {
    const ready = await spec.prepare(context, args);
    return {
      label: ready.label ?? label,
      ...(ready.question !== undefined ? { question: ready.question } : {}),
      run: ready.run,
    };
  } catch (error) {
    return { label, error: describeToolError(error, lang) };
  }
}
