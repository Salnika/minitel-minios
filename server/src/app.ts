import fastify, { type FastifyInstance, type FastifyRequest } from "fastify";

import {
  createAssistant,
  createEventStream,
  type AssistantInput,
  type AssistantTurn,
} from "./assistant.js";
import { createAuthHook } from "./auth.js";
import type { AppConfig } from "./config.js";
import { HttpError } from "./errors.js";
import { createFirmwareStore } from "./firmware.js";
import { createStorage } from "./storage.js";
import { datePattern, formatLocalDate, timePattern } from "./tasks.js";
import type {
  AddTaskInput,
  CreateFileInput,
  FileMetadata,
  FileSort,
  ListFilesOptions,
  ListTasksOptions,
  SortOrder,
  TaskDueFilter,
  TaskStatus,
  UpdateFileInput,
} from "./types.js";
import { createWorkspace } from "./workspace.js";

interface BuildAppOptions {
  logger?: boolean;
  // Client of the assistant's chat completions API: tests pass a fake model.
  fetch?: typeof fetch;
}

const MAX_ASSISTANT_TEXT = 4000;
const MAX_ASSISTANT_HISTORY = 8;
const nowPattern = /^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$/;

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function readBodyObject(body: unknown): Record<string, unknown> {
  if (!isRecord(body)) {
    throw new HttpError(400, "Request body must be an object");
  }

  return body;
}

function readRequiredStringField(body: Record<string, unknown>, fieldName: string): string {
  const value = body[fieldName];
  if (typeof value !== "string" || value.trim().length === 0) {
    throw new HttpError(400, `${fieldName} must be a non-empty string`);
  }

  return value;
}

// Unlike names, a file content may legitimately be empty (a note that was cleared).
function readStringField(body: Record<string, unknown>, fieldName: string): string {
  const value = body[fieldName];
  if (typeof value !== "string") {
    throw new HttpError(400, `${fieldName} must be a string`);
  }

  return value;
}

function readOptionalStringField(
  body: Record<string, unknown>,
  fieldName: string,
  pattern?: RegExp,
): string | undefined {
  const value = body[fieldName];
  if (value === undefined || value === "") {
    return undefined;
  }
  if (typeof value !== "string" || (pattern !== undefined && !pattern.test(value))) {
    throw new HttpError(400, `Invalid ${fieldName}`);
  }

  return value;
}

function readSingleLine(body: Record<string, unknown>, fieldName: string): string {
  const value = readRequiredStringField(body, fieldName);
  if (/[\r\n]/.test(value)) {
    throw new HttpError(400, `${fieldName} must be a single line`);
  }

  return value.trim();
}

function readQueryObject(query: unknown): Record<string, unknown> {
  if (!isRecord(query)) {
    throw new HttpError(400, "Invalid query string");
  }

  return query;
}

// The device sends its own local date so "today" follows its timezone setting.
function readToday(query: Record<string, unknown>): string {
  const today = query.today;
  if (today === undefined) {
    return formatLocalDate(new Date());
  }
  if (typeof today !== "string" || !datePattern.test(today)) {
    throw new HttpError(400, "today must be YYYY-MM-DD");
  }

  return today;
}

function readLimit(query: Record<string, unknown>, fallback: number, max: number): number {
  const limit = query.limit;
  if (limit === undefined) {
    return fallback;
  }
  const parsed = typeof limit === "string" ? Number.parseInt(limit, 10) : Number.NaN;
  if (!Number.isInteger(parsed) || parsed <= 0) {
    throw new HttpError(400, "limit must be a positive integer");
  }

  return Math.min(parsed, max);
}

function parseListTasksQuery(query: unknown): ListTasksOptions {
  const values = readQueryObject(query);
  const project = values.project;
  const status = values.status ?? "open";
  const due = values.due;

  if (project !== undefined && typeof project !== "string") {
    throw new HttpError(400, "project must be a string");
  }
  if (status !== "open" && status !== "done" && status !== "all") {
    throw new HttpError(400, "Invalid status value");
  }
  if (due !== undefined && due !== "today" && due !== "overdue") {
    throw new HttpError(400, "Invalid due value");
  }

  return {
    ...(project !== undefined ? { project } : {}),
    status: status as TaskStatus,
    ...(due !== undefined ? { due: due as TaskDueFilter } : {}),
    today: readToday(values),
    limit: readLimit(values, 50, 200),
  };
}

function readOptionalMetadata(body: Record<string, unknown>): FileMetadata {
  const metadata: FileMetadata = {};

  if ("title" in body) {
    const title = body.title;
    if (typeof title !== "string") {
      throw new HttpError(400, "title must be a string");
    }

    metadata.title = title;
  }

  if ("tags" in body) {
    const tags = body.tags;
    if (!Array.isArray(tags) || tags.some((tag) => typeof tag !== "string")) {
      throw new HttpError(400, "tags must be an array of strings");
    }

    metadata.tags = [...tags];
  }

  if ("sortOrder" in body) {
    const sortOrder = body.sortOrder;
    if (typeof sortOrder !== "number" || !Number.isInteger(sortOrder)) {
      throw new HttpError(400, "sortOrder must be an integer");
    }

    metadata.sortOrder = sortOrder;
  }

  return metadata;
}

function parseListFilesQuery(query: unknown): ListFilesOptions {
  if (!isRecord(query)) {
    throw new HttpError(400, "Invalid query string");
  }

  const folder = query.folder;
  const sort = query.sort;
  const order = query.order;

  const parsedSort: FileSort = sort === undefined ? "name" : parseSort(sort);
  const parsedOrder: SortOrder = order === undefined ? "asc" : parseOrder(order);

  if (folder !== undefined && typeof folder !== "string") {
    throw new HttpError(400, "folder must be a string");
  }

  return {
    ...(folder !== undefined ? { folder } : {}),
    sort: parsedSort,
    order: parsedOrder,
  };
}

function parseSort(value: unknown): FileSort {
  if (value === "name" || value === "updatedAt" || value === "size" || value === "manual") {
    return value;
  }

  throw new HttpError(400, "Invalid sort value");
}

function parseOrder(value: unknown): SortOrder {
  if (value === "asc" || value === "desc") {
    return value;
  }

  throw new HttpError(400, "Invalid order value");
}

function readFileIdFromParams(request: FastifyRequest): string {
  const params = request.params as {
    folder?: string;
    name?: string;
  };
  if (!params.folder || !params.name) {
    throw new HttpError(400, "File id is required");
  }

  return `${params.folder}/${params.name}`;
}

// `/file?id=projets/robot/taches.md`: same as `/files/:folder/:name` without an encoded
// slash in the path, which reverse proxies such as Traefik may reject.
function readFileIdFromQuery(request: FastifyRequest): string {
  const query = readQueryObject(request.query);
  if (typeof query.id !== "string" || query.id.length === 0) {
    throw new HttpError(400, "File id is required");
  }

  return query.id;
}

function readAssistantText(body: Record<string, unknown>, fieldName: string): string {
  const value = readRequiredStringField(body, fieldName);
  if (value.length > MAX_ASSISTANT_TEXT) {
    throw new HttpError(400, `${fieldName} is too long`);
  }

  return value;
}

function parseAssistantHistory(value: unknown): AssistantTurn[] {
  if (value === undefined) {
    return [];
  }
  if (!Array.isArray(value) || value.length > MAX_ASSISTANT_HISTORY) {
    throw new HttpError(400, "Invalid history");
  }

  return value.map((turn: unknown) => {
    if (
      !isRecord(turn) ||
      typeof turn.user !== "string" ||
      typeof turn.assistant !== "string" ||
      (turn.actions !== undefined && typeof turn.actions !== "string")
    ) {
      throw new HttpError(400, "Invalid history");
    }
    const actions = typeof turn.actions === "string" ? turn.actions : "";
    return {
      user: turn.user.slice(0, MAX_ASSISTANT_TEXT),
      assistant: turn.assistant.slice(0, MAX_ASSISTANT_TEXT),
      ...(actions.length > 0 ? { actions: actions.slice(0, MAX_ASSISTANT_TEXT) } : {}),
    };
  });
}

// The device sends its own AI settings: the key never has to be configured twice.
function parseAssistantInput(body: unknown): AssistantInput {
  const values = readBodyObject(body);
  const ai = values.ai;
  if (!isRecord(ai)) {
    throw new HttpError(400, "ai must be an object");
  }
  const baseUrl = readRequiredStringField(ai, "baseUrl").trim().replace(/\/+$/, "");
  if (!baseUrl.startsWith("https://")) {
    throw new HttpError(400, "ai.baseUrl must be an https URL");
  }
  const now = readOptionalStringField(values, "now", nowPattern);

  return {
    ai: {
      baseUrl,
      apiKey: readRequiredStringField(ai, "apiKey").trim(),
      model: readRequiredStringField(ai, "model").trim(),
    },
    // French unless the device asks for English: older firmware sends no lang.
    lang: values.lang === "en" ? "en" : "fr",
    ...(now !== undefined ? { now } : {}),
    history: parseAssistantHistory(values.history),
    prompt: readAssistantText(values, "prompt"),
  };
}

function normalizeError(error: unknown): { statusCode: number; message: string } {
  if (error instanceof HttpError) {
    return {
      statusCode: error.statusCode,
      message: error.message,
    };
  }

  const errorWithStatus = error as { statusCode?: number };
  if (errorWithStatus.statusCode === 413) {
    return { statusCode: 400, message: "File too large" };
  }

  if (errorWithStatus.statusCode === 400) {
    return { statusCode: 400, message: "Invalid request" };
  }

  return {
    statusCode: 500,
    message: "Internal server error",
  };
}

export function buildApp(config: AppConfig, options: BuildAppOptions = {}): FastifyInstance {
  const app = fastify({
    logger: options.logger ?? false,
    requestTimeout: config.requestTimeoutMs,
    bodyLimit: config.maxFileSizeBytes + 16 * 1024,
  });
  const storage = createStorage(config);
  const workspace = createWorkspace(config);
  const firmware = createFirmwareStore(config);
  const assistant = createAssistant({
    storage,
    workspace,
    ...(options.fetch !== undefined ? { fetch: options.fetch } : {}),
  });

  // Firmware uploads are raw binaries (`curl --data-binary @firmware.bin`).
  app.addContentTypeParser(
    "application/octet-stream",
    { parseAs: "buffer", bodyLimit: config.firmwareMaxBytes },
    (_request, body, done) => {
      done(null, body);
    },
  );

  app.addHook("onReady", async () => {
    await storage.ensureDataRoot();
  });
  app.addHook("onClose", async () => {
    assistant.close();
  });
  app.addHook("preHandler", createAuthHook(config.apiToken));

  app.setNotFoundHandler((_request, reply) => {
    reply.code(404).send({ error: "Not found" });
  });
  app.setErrorHandler((error, _request, reply) => {
    const normalized = normalizeError(error);
    reply.code(normalized.statusCode).send({ error: normalized.message });
  });

  app.get("/health", { config: { public: true } }, async () => ({ status: "ok" }));

  app.get("/folders", async () => storage.listFolders());

  app.post("/folders", async (request, reply) => {
    const body = readBodyObject(request.body);
    const folder = await storage.createFolder(readRequiredStringField(body, "name"));
    reply.code(201).send({ name: folder });
  });

  app.get("/files", async (request) => storage.listFiles(parseListFilesQuery(request.query)));

  app.post("/files", async (request, reply) => {
    const body = readBodyObject(request.body);
    const input: CreateFileInput = {
      folder: readRequiredStringField(body, "folder"),
      name: readRequiredStringField(body, "name"),
      content: readStringField(body, "content"),
      ...readOptionalMetadata(body),
    };

    const file = await storage.createFile(input);
    reply.code(201).send(file);
  });

  app.post("/files/append", async (request) => {
    const body = readBodyObject(request.body);
    return workspace.appendToFile(
      readRequiredStringField(body, "id"),
      readSingleLine(body, "text"),
    );
  });

  app.post("/files/:folder/:name/rename", async (request) => {
    const body = readBodyObject(request.body);
    return storage.renameFile(
      readFileIdFromParams(request),
      readRequiredStringField(body, "newName"),
    );
  });

  app.post("/files/:folder/:name/move", async (request) => {
    const body = readBodyObject(request.body);
    return storage.moveFile(
      readFileIdFromParams(request),
      readRequiredStringField(body, "targetFolder"),
    );
  });

  app.get("/files/:folder/:name", async (request) =>
    storage.getFile(readFileIdFromParams(request)),
  );

  app.put("/files/:folder/:name", async (request) => {
    const body = readBodyObject(request.body);
    const input: UpdateFileInput = {
      content: readStringField(body, "content"),
      ...readOptionalMetadata(body),
    };

    return storage.updateFile(readFileIdFromParams(request), input);
  });

  app.get("/file", async (request) => storage.getFile(readFileIdFromQuery(request)));

  app.put("/file", async (request) => {
    const body = readBodyObject(request.body);
    const input: UpdateFileInput = {
      content: readStringField(body, "content"),
      ...readOptionalMetadata(body),
    };

    return storage.updateFile(readFileIdFromQuery(request), input);
  });

  app.delete("/file", async (request, reply) => {
    await storage.deleteFile(readFileIdFromQuery(request));
    reply.send({ ok: true });
  });

  app.delete("/files/:folder/:name", async (request, reply) => {
    await storage.deleteFile(readFileIdFromParams(request));
    reply.send({ ok: true });
  });

  app.get("/projects", async (request) => ({
    items: await workspace.listProjects(readToday(readQueryObject(request.query))),
  }));

  app.post("/projects", async (request, reply) => {
    const body = readBodyObject(request.body);
    const project = await workspace.createProject(
      readSingleLine(body, "title"),
      readOptionalStringField(body, "goal"),
      readToday(readQueryObject(request.query)),
    );
    reply.code(201).send(project);
  });

  app.get("/tasks", async (request) => workspace.listTasks(parseListTasksQuery(request.query)));

  app.post("/tasks", async (request, reply) => {
    const body = readBodyObject(request.body);
    const priority = body.priority;
    if (priority !== undefined && (!Number.isInteger(priority) || (priority as number) < 0)) {
      throw new HttpError(400, "priority must be a positive integer");
    }
    const due = readOptionalStringField(body, "due", datePattern);
    const time = readOptionalStringField(body, "time", timePattern);
    const input: AddTaskInput = {
      project: readRequiredStringField(body, "project"),
      text: readSingleLine(body, "text"),
      ...(due !== undefined ? { due } : {}),
      ...(time !== undefined ? { time } : {}),
      ...(priority !== undefined ? { priority: priority as number } : {}),
    };
    reply.code(201).send(await workspace.addTask(input));
  });

  app.post("/tasks/toggle", async (request) => {
    const body = readBodyObject(request.body);
    const line = body.line;
    const done = body.done;
    if (!Number.isInteger(line) || (line as number) <= 0) {
      throw new HttpError(400, "line must be a positive integer");
    }
    if (done !== undefined && typeof done !== "boolean") {
      throw new HttpError(400, "done must be a boolean");
    }
    return workspace.toggleTask(readRequiredStringField(body, "file"), line as number, done);
  });

  app.get("/search", async (request) => {
    const query = readQueryObject(request.query);
    const folder = query.folder;
    if (typeof query.q !== "string") {
      throw new HttpError(400, "q must be a non-empty string");
    }
    if (folder !== undefined && typeof folder !== "string") {
      throw new HttpError(400, "folder must be a string");
    }
    return { items: await workspace.search(query.q, folder, readLimit(query, 20, 100)) };
  });

  // Server-sent events: the reply of the assistant with the actions it runs (assistant.ts).
  app.post("/assistant", async (request, reply) => {
    const input = parseAssistantInput(request.body);
    reply.hijack();
    assistant.start(input, createEventStream(reply.raw), request.log);
  });

  // The user's answer to a pending confirmation; the conversation goes on in this response.
  app.post("/assistant/confirm", async (request, reply) => {
    const body = readBodyObject(request.body);
    const id = readRequiredStringField(body, "id");
    const accept = body.accept;
    if (typeof accept !== "boolean") {
      throw new HttpError(400, "accept must be a boolean");
    }
    if (!assistant.isPending(id)) {
      throw new HttpError(404, "Confirmation expired");
    }
    reply.hijack();
    assistant.confirm(id, accept, createEventStream(reply.raw));
  });

  app.get("/firmware/manifest", async () => firmware.getManifest());

  app.get("/firmware/download", async (_request, reply) => {
    const { stream, size } = await firmware.openBinary();
    reply.header("content-type", "application/octet-stream");
    reply.header("content-length", size);
    return reply.send(stream);
  });

  app.put("/firmware", { bodyLimit: config.firmwareMaxBytes }, async (request, reply) => {
    const query = readQueryObject(request.query);
    if (!Buffer.isBuffer(request.body)) {
      throw new HttpError(400, "Send the firmware as application/octet-stream");
    }
    if (typeof query.version !== "string") {
      throw new HttpError(400, "version is required");
    }
    reply.code(201).send(await firmware.publish(query.version, request.body));
  });

  return app;
}
