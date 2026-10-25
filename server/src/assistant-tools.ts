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

export interface ToolContext {
  storage: Storage;
  workspace: Workspace;
  // The device's local date (YYYY-MM-DD): "today" and "late" for the tasks.
  today: string;
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
  label: (args: ToolArguments) => string;
  // Throws when the arguments are wrong or the call cannot work (file missing...).
  prepare: (context: ToolContext, args: ToolArguments) => Promise<ReadyTool>;
}

// A mistake the model can fix: the message goes back to it.
class ToolError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ToolError";
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
    throw new ToolError(`argument "${name}" manquant`);
  }
  return value;
}

// Unlike names, a text may be empty (a cleared file, a passage to delete).
function readText(args: ToolArguments, name: string): string {
  const value = args[name];
  if (typeof value !== "string") {
    throw new ToolError(`argument "${name}" manquant`);
  }
  return value;
}

function readOptionalString(args: ToolArguments, name: string): string | undefined {
  const value = args[name];
  if (value === undefined || value === null || value === "") {
    return undefined;
  }
  if (typeof value !== "string") {
    throw new ToolError(`argument "${name}" invalide`);
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
    throw new ToolError(`argument "${name}" : nombre entier attendu`);
  }
  return parsed;
}

function readSingleLine(args: ToolArguments, name: string): string {
  const value = readString(args, name);
  if (/[\r\n]/.test(value)) {
    throw new ToolError(`argument "${name}" : une seule ligne`);
  }
  return value.trim();
}

function readFolderPath(args: ToolArguments, name: string): string {
  const folder = normalizeToolPath(readString(args, name));
  if (folder.length === 0) {
    throw new ToolError(`argument "${name}" : dossier manquant`);
  }
  return folder;
}

// Files always live in a folder, with a .md or .txt extension.
function readFilePath(args: ToolArguments, name: string): string {
  const id = normalizeToolPath(readString(args, name));
  if (!id.includes("/")) {
    throw new ToolError(`chemin "${id}" sans dossier : par exemple notes/${id}`);
  }
  if (!/\.(md|txt)$/.test(id)) {
    throw new ToolError(`chemin "${id}" : seuls les fichiers .md et .txt sont geres`);
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

function formatSize(size: number): string {
  return size < 1024 ? `${size} o` : `${(size / 1024).toFixed(1)} Ko`;
}

function limitLines(lines: string[], max: number): string[] {
  if (lines.length <= max) {
    return lines;
  }
  return [...lines.slice(0, max), `(${lines.length - max} de plus)`];
}

function plural(count: number, word: string): string {
  return `${count} ${word}${count > 1 ? "s" : ""}`;
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
    );
  }
  if (count > 1) {
    throw new ToolError(
      `texte present ${count} fois dans ${id} : ajoute du contexte pour qu'il soit unique`,
    );
  }
}

async function listFolder(context: ToolContext, folder: string): Promise<string> {
  const folders = await context.storage.listFolders();
  if (folder.length > 0 && !folders.includes(folder)) {
    throw new ToolError(`dossier "${folder}" introuvable`);
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
  const title = folder.length > 0 ? `Dossier ${folder}` : "Racine";
  if (subfolders.length === 0 && files.length === 0) {
    return `${title} : vide.`;
  }

  const lines = [
    ...subfolders.map((name) => `${name}/`),
    ...files.map(
      (file) => `${file.id} (${formatSize(file.size)}, modifie le ${file.updatedAt.slice(0, 10)})`,
    ),
  ];
  return [
    `${title} : ${plural(subfolders.length, "dossier")}, ${plural(files.length, "fichier")}`,
    ...limitLines(lines, MAX_LIST_LINES),
  ].join("\n");
}

async function readFile(context: ToolContext, id: string, startLine: number): Promise<string> {
  const file = await context.storage.getFile(id);
  if (file.content.length === 0) {
    return `${id} : fichier vide.`;
  }

  const lines = file.content.split("\n");
  if (lines.at(-1) === "") {
    lines.pop();
  }
  const total = lines.length;
  if (startLine < 1 || startLine > total) {
    throw new ToolError(`${id} a ${plural(total, "ligne")}`);
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

  const header =
    startLine === 1 && end === total
      ? `${id} (${plural(total, "ligne")}) :`
      : `${id}, lignes ${startLine} a ${end} sur ${total}${
          end < total ? ` (suite avec start_line=${end + 1})` : ""
        } :`;
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
    label: (args) => {
      const folder = typeof args.path === "string" ? normalizeToolPath(args.path) : "";
      return folder.length > 0 ? `liste ${folder}/` : "liste la racine";
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
    label: (args) => `lit ${pathOf(args.path)}`,
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
    label: (args) => `cherche ${quote(textOf(args.query), 30)}`,
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
            return `Aucun resultat pour ${quote(query, 60)}.`;
          }
          return [
            `${plural(hits.length, "resultat")} :`,
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
    label: (args) => `ecrit ${pathOf(args.path)}`,
    async prepare(context, args) {
      const id = readFilePath(args, "path");
      const content = readText(args, "content");
      if (await context.storage.fileExists(id)) {
        const current = await context.storage.getFile(id);
        return {
          label: `remplace ${id}`,
          question: `Remplacer tout ${id} (${countLines(current.content)} -> ${plural(
            countLines(content),
            "ligne",
          )})`,
          run: async () => {
            await context.storage.updateFile(id, { content });
            return `${id} remplace (${plural(countLines(content), "ligne")}).`;
          },
        };
      }

      return {
        label: `cree ${id}`,
        run: async () => {
          const { folder, name } = splitFileId(id);
          await context.storage.ensureFolder(folder);
          await context.storage.createFile({ folder, name, content });
          return `${id} cree (${plural(countLines(content), "ligne")}).`;
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
    label: (args) => `ajoute a ${pathOf(args.path)}`,
    async prepare(context, args) {
      const id = readFilePath(args, "path");
      const text = readString(args, "text").replace(/\s+$/, "");
      return {
        run: async () => {
          await context.workspace.appendToFile(id, text);
          return `Texte ajoute a la fin de ${id}.`;
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
    label: (args) => `modifie ${pathOf(args.path)}`,
    async prepare(context, args) {
      const id = readFilePath(args, "path");
      const oldText = readText(args, "old_text");
      const newText = readText(args, "new_text");
      if (oldText.length === 0) {
        throw new ToolError('argument "old_text" vide');
      }
      const file = await context.storage.getFile(id);
      checkUnique(file.content, oldText, id);
      return {
        question:
          newText.length === 0
            ? `Effacer de ${id} : ${quote(oldText, 60)}`
            : `Modifier ${id} : ${quote(oldText, 40)} -> ${quote(newText, 40)}`,
        run: async () => {
          // The file may have changed while the user was deciding.
          const current = await context.storage.getFile(id);
          checkUnique(current.content, oldText, id);
          await context.storage.updateFile(id, {
            content: current.content.replace(oldText, () => newText),
          });
          return `${id} modifie.`;
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
    label: (args) => `cree le dossier ${pathOf(args.path)}/`,
    async prepare(context, args) {
      const folder = readFolderPath(args, "path");
      return {
        run: async () =>
          (await context.storage.ensureFolder(folder))
            ? `Dossier ${folder} cree.`
            : `Le dossier ${folder} existe deja.`,
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
    label: (args) => `deplace ${pathOf(args.path)} vers ${pathOf(args.new_path)}`,
    async prepare(context, args) {
      const from = readFilePath(args, "path");
      const to = readFilePath(args, "new_path");
      if (!(await context.storage.fileExists(from))) {
        throw new ToolError(`${from} introuvable`);
      }
      if (from !== to && (await context.storage.fileExists(to))) {
        throw new ToolError(`${to} existe deja`);
      }
      return {
        run: async () => {
          await context.storage.relocateFile(from, to);
          return `${from} deplace vers ${to}.`;
        },
      };
    },
  },
  {
    name: "delete_file",
    description: "Supprime un fichier.",
    parameters: objectSchema({ path: stringSchema(pathDescription) }, ["path"]),
    label: (args) => `supprime ${pathOf(args.path)}`,
    async prepare(context, args) {
      const id = readFilePath(args, "path");
      const file = await context.storage.getFile(id);
      return {
        question: `Supprimer ${id} (${plural(countLines(file.content), "ligne")})`,
        run: async () => {
          await context.storage.deleteFile(id);
          return `${id} supprime.`;
        },
      };
    },
  },
  {
    name: "list_projects",
    description: "Liste les projets (dossiers de projets/) avec leur statut et leurs taches.",
    parameters: objectSchema({}),
    label: () => "liste les projets",
    async prepare(context) {
      return {
        run: async () => {
          const projects = await context.workspace.listProjects(context.today);
          if (projects.length === 0) {
            return "Aucun projet.";
          }
          return projects
            .map((project) => {
              const parts = [`${project.name} : ${project.title}`];
              if (project.status !== undefined) {
                parts.push(`statut ${project.status}`);
              }
              const s = project.openTasks > 1 ? "s" : "";
              parts.push(`${project.openTasks} tache${s} ouverte${s}`);
              if (project.overdueTasks > 0) {
                parts.push(`${project.overdueTasks} en retard`);
              }
              if (project.next !== undefined) {
                parts.push(`prochaine etape : ${project.next}`);
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
    label: (args) =>
      typeof args.project === "string" && args.project.trim().length > 0
        ? `liste les taches de ${normalizeToolPath(args.project).replace(/^projets\//, "")}`
        : "liste les taches",
    async prepare(context, args) {
      const project =
        readOptionalString(args, "project") !== undefined
          ? readProject(args, "project")
          : undefined;
      const filter = readOptionalString(args, "filter") ?? "open";
      if (!(taskFilters as readonly string[]).includes(filter)) {
        throw new ToolError(`filtre "${filter}" inconnu : ${taskFilters.join(", ")}`);
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
            return "Aucune tache.";
          }
          return [
            items.length < total
              ? `${items.length} taches sur ${total} :`
              : `${plural(total, "tache")} :`,
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
    label: (args) => `ajoute une tache a ${pathOf(args.project).replace(/^projets\//, "")}`,
    async prepare(context, args) {
      const project = readProject(args, "project");
      const text = readSingleLine(args, "text");
      const due = readOptionalString(args, "due");
      const time = readOptionalString(args, "time");
      const priority = readOptionalInteger(args, "priority");
      if (due !== undefined && !datePattern.test(due)) {
        throw new ToolError('argument "due" : date AAAA-MM-JJ attendue');
      }
      if (time !== undefined && !timePattern.test(time)) {
        throw new ToolError('argument "time" : heure HH:MM attendue');
      }
      if (time !== undefined && due === undefined) {
        throw new ToolError('argument "time" : il faut aussi "due"');
      }
      if (priority !== undefined && (priority < 0 || priority > 3)) {
        throw new ToolError('argument "priority" : de 0 a 3');
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
          return `Tache ajoutee : ${task.file}:${task.line} ${formatTaskLine(task)}`;
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
    label: (args) =>
      `${args.done === false ? "decoche" : "coche"} ${pathOf(args.file)}:${
        typeof args.line === "number" || typeof args.line === "string" ? String(args.line) : "?"
      }`,
    async prepare(context, args) {
      const file = readFilePath(args, "file");
      const line = readOptionalInteger(args, "line");
      if (line === undefined || line <= 0) {
        throw new ToolError('argument "line" manquant');
      }
      const done = args.done !== false;
      return {
        run: async () => {
          const task = await context.workspace.toggleTask(file, line, done);
          return `Tache ${done ? "cochee" : "decochee"} : ${task.text}`;
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
    label: (args) => `cree le projet ${quote(textOf(args.title), 30)}`,
    async prepare(context, args) {
      const title = readSingleLine(args, "title");
      const goal = readOptionalString(args, "goal");
      if (goal !== undefined && /[\r\n]/.test(goal)) {
        throw new ToolError('argument "goal" : une seule ligne');
      }
      return {
        run: async () => {
          const project = await context.workspace.createProject(title, goal, context.today);
          return `Projet cree : projets/${project.name}/ (projet.md, taches.md).`;
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
export function describeToolError(error: unknown): string {
  if (error instanceof ToolError) {
    return error.message;
  }
  if (error instanceof HttpError) {
    if (error.statusCode === 409) {
      return "la ligne n'est pas une tache, le fichier a change : relis-le";
    }
    return error.message;
  }
  return "erreur interne";
}

function parseArguments(raw: string): ToolArguments {
  if (raw.trim().length === 0) {
    return {};
  }
  const parsed = JSON.parse(raw) as unknown;
  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
    throw new ToolError("arguments : objet JSON attendu");
  }
  return parsed as ToolArguments;
}

// Checks a call of the model. Never throws: a call that cannot run comes back with `error`.
export async function prepareTool(
  context: ToolContext,
  name: string,
  rawArguments: string,
): Promise<PreparedTool> {
  const spec = toolSpecs.find((candidate) => candidate.name === name);
  if (spec === undefined) {
    return { label: `outil ${name.length > 0 ? name : "?"}`, error: `outil "${name}" inconnu` };
  }

  let args: ToolArguments;
  try {
    args = parseArguments(rawArguments);
  } catch {
    return {
      label: spec.label({}),
      error: "arguments JSON invalides ou coupes : refais l'appel, avec moins de texte",
    };
  }

  const label = spec.label(args);
  try {
    const ready = await spec.prepare(context, args);
    return {
      label: ready.label ?? label,
      ...(ready.question !== undefined ? { question: ready.question } : {}),
      run: ready.run,
    };
  } catch (error) {
    return { label, error: describeToolError(error) };
  }
}
