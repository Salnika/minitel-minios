import { randomBytes } from "node:crypto";
import type { ServerResponse } from "node:http";

import type { FastifyBaseLogger } from "fastify";

import {
  assistantTools,
  describeToolError,
  prepareTool,
  type ToolContext,
} from "./assistant-tools.js";
import type { createStorage } from "./storage.js";
import type { createWorkspace } from "./workspace.js";

// The Minitel's assistant with tools. The device sends its question with the last exchanges
// and its AI settings; Bibliotek runs the model (OpenAI-style chat completions), executes its
// tool calls on the notes and streams the reply back as chat completion chunks, the format
// the device already decodes, plus two events of its own:
//   {"action": "lit notes/courses.md"}         a tool call ran (or failed, or was refused)
//   {"confirm": {"id": "...", "text": "..."}}  a call needs the user's approval: the stream
//       ends there, and POST /assistant/confirm {id, accept} resumes the same conversation.

export interface AssistantTurn {
  user: string;
  assistant: string;
  // Actions of that exchange, one per line.
  actions?: string;
}

export interface AssistantInput {
  ai: { baseUrl: string; apiKey: string; model: string };
  // The device's local time, "YYYY-MM-DD HH:MM".
  now?: string;
  history: AssistantTurn[];
  prompt: string;
}

// One HTTP response to the Minitel.
export interface AssistantStream {
  // Aborted when the Minitel hangs up (ANNULATION, or it left the chat).
  readonly signal: AbortSignal;
  action(label: string): void;
  thinking(): void;
  content(text: string): void;
  // These three end the stream.
  confirm(id: string, question: string): void;
  finish(reason: "stop" | "length"): void;
  fail(message: string): void;
}

type Storage = ReturnType<typeof createStorage>;
type Workspace = ReturnType<typeof createWorkspace>;

export interface AssistantOptions {
  storage: Storage;
  workspace: Workspace;
  fetch?: typeof fetch;
  confirmTimeoutMs?: number;
}

const MAX_TOOL_ROUNDS = 8;
const MAX_CALLS_PER_ROUND = 8;
const MAX_TOKENS = 2048;
const UPSTREAM_TIMEOUT_MS = 120_000;
const CONFIRM_TIMEOUT_MS = 5 * 60_000;
const MAX_PENDING_CONFIRMATIONS = 8;
// The Minitel drops events over 1 KB: content goes in small pieces.
const CONTENT_PIECE_LENGTH = 160;
const MAX_LABEL_LENGTH = 100;
const MAX_QUESTION_LENGTH = 160;
// Keeps the connection busy while the model or a tool works: the Minitel gives up after 30 s
// without a byte.
const PING_INTERVAL_MS = 10_000;

const WEEKDAYS = ["dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"];
const MONTHS = [
  "janvier",
  "février",
  "mars",
  "avril",
  "mai",
  "juin",
  "juillet",
  "août",
  "septembre",
  "octobre",
  "novembre",
  "décembre",
];
const nowPattern = /^(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2})$/;

const NUDGE = "Donne maintenant ta réponse finale, brève, sans réfléchir davantage.";

interface ToolCall {
  id: string;
  name: string;
  arguments: string;
}

interface ToolCallMessage {
  id: string;
  type: "function";
  function: { name: string; arguments: string };
}

type ChatMessage =
  | { role: "system" | "user"; content: string }
  | {
      role: "assistant";
      content: string | null;
      tool_calls?: ToolCallMessage[];
      reasoning_content?: string;
    }
  | { role: "tool"; tool_call_id: string; content: string };

interface ModelTurn {
  content: string;
  reasoning: string;
  calls: Map<number, ToolCall>;
  finishReason?: string;
}

interface Session {
  input: AssistantInput;
  context: ToolContext;
  stream: AssistantStream;
  log: FastifyBaseLogger;
  messages: ChatMessage[];
  // reasoning_content goes back with the tool calls (DeepSeek models want it); off once the
  // provider refuses it.
  sendReasoning: boolean;
  callCount: number;
}

interface PendingConfirmation {
  answer: (decision: { accept: boolean; stream: AssistantStream } | undefined) => void;
}

// An answer of the chat completions API other than a reply. `status`: the HTTP status,
// 0 for an error event in the stream, -1 when the API could not be reached.
class UpstreamError extends Error {
  readonly status: number;

  constructor(status: number, message: string) {
    super(message);
    this.name = "UpstreamError";
    this.status = status;
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function pad(value: number): string {
  return String(value).padStart(2, "0");
}

// Splits on characters, not UTF-16 units: a piece never ends in half an emoji.
function clip(text: string, max: number): string {
  const characters = Array.from(text);
  return characters.length > max ? `${characters.slice(0, max - 3).join("")}...` : text;
}

function pieces(text: string, size: number): string[] {
  const characters = Array.from(text);
  const result: string[] = [];
  for (let start = 0; start < characters.length; start += size) {
    result.push(characters.slice(start, start + size).join(""));
  }
  return result;
}

export function createEventStream(response: ServerResponse): AssistantStream {
  const controller = new AbortController();
  let ended = false;

  response.writeHead(200, {
    "content-type": "text/event-stream; charset=utf-8",
    "cache-control": "no-cache",
    "x-accel-buffering": "no",
  });
  // Node sends the headers with the first bytes of the body: the Minitel waits for them 15 s
  // at most, the model may take longer to say anything.
  response.write(": ok\n\n");
  const ping = setInterval(() => {
    if (!ended) {
      response.write(": ping\n\n");
    }
  }, PING_INTERVAL_MS);
  ping.unref();
  response.on("close", () => {
    clearInterval(ping);
    if (!ended) {
      ended = true;
      controller.abort();
    }
  });

  function send(data: unknown): void {
    if (!ended) {
      response.write(`data: ${JSON.stringify(data)}\n\n`);
    }
  }

  function end(data: unknown): void {
    if (ended) {
      return;
    }
    send(data);
    response.write("data: [DONE]\n\n");
    ended = true;
    clearInterval(ping);
    response.end();
  }

  return {
    signal: controller.signal,
    action: (label) => send({ action: clip(label, MAX_LABEL_LENGTH) }),
    // The device only needs to know the model is thinking, not what it thinks.
    thinking: () => send({ choices: [{ index: 0, delta: { reasoning_content: "..." } }] }),
    content: (text) => {
      for (const piece of pieces(text, CONTENT_PIECE_LENGTH)) {
        send({ choices: [{ index: 0, delta: { content: piece } }] });
      }
    },
    confirm: (id, question) => end({ confirm: { id, text: clip(question, MAX_QUESTION_LENGTH) } }),
    finish: (reason) => end({ choices: [{ index: 0, delta: {}, finish_reason: reason }] }),
    fail: (message) => end({ error: { message } }),
  };
}

function describeNow(now: string | undefined): { today: string; label: string } {
  const match = now !== undefined ? nowPattern.exec(now) : null;
  const fields = match?.slice(1).map((field) => Number.parseInt(field, 10));
  let [year, month, day, hours, minutes] = fields ?? [];
  if (
    year === undefined ||
    month === undefined ||
    day === undefined ||
    hours === undefined ||
    minutes === undefined ||
    month < 1 ||
    month > 12 ||
    day < 1 ||
    day > 31
  ) {
    const date = new Date();
    year = date.getFullYear();
    month = date.getMonth() + 1;
    day = date.getDate();
    hours = date.getHours();
    minutes = date.getMinutes();
  }

  const weekday = WEEKDAYS[new Date(Date.UTC(year, month - 1, day)).getUTCDay()] ?? "";
  return {
    today: `${year}-${pad(month)}-${pad(day)}`,
    label: `${weekday} ${day} ${MONTHS[month - 1] ?? ""} ${year}, ${pad(hours)}:${pad(minutes)}`,
  };
}

function systemPrompt(nowLabel: string): string {
  return [
    `Tu es l'assistant du miniOS, affiché sur un Minitel de 40 colonnes. Nous sommes le ${nowLabel}.`,
    "Tu as accès aux notes de l'utilisateur, des fichiers texte (.md, .txt) rangés en dossiers, grâce à des outils :",
    "- utilise-les pour lire, chercher, créer ou modifier ses notes plutôt que de deviner ou de lui demander ;",
    "- lis un fichier avant de le modifier ; pour un petit changement, préfère edit_file, et append_to_file pour ajouter à la fin ;",
    "- les chemins sont relatifs, sans / au début : notes/courses.md, projets/robot/taches.md ;",
    "- projets/<nom>/ contient projet.md (titre, Statut, Objectif, Prochaine etape), taches.md (une tâche par ligne : - [ ] texte !! @AAAA-MM-JJ HH:MM) et temps.md ;",
    "- inbox/inbox.md reçoit les captures rapides, journal/AAAA-MM-JJ.md le journal du jour ;",
    "- certaines actions demandent l'accord de l'utilisateur : s'il refuse, ne recommence pas.",
    "Une réponse précédente peut commencer par (Outils utilisés : ...) : c'est un rappel, ne l'écris pas toi-même.",
    "Réponds en français, en texte brut sans markdown, en 400 caractères au plus : dis ce que tu as fait ou trouvé.",
  ].join("\n");
}

function buildMessages(input: AssistantInput, nowLabel: string): ChatMessage[] {
  const messages: ChatMessage[] = [{ role: "system", content: systemPrompt(nowLabel) }];
  for (const turn of input.history) {
    messages.push({ role: "user", content: turn.user });
    const actions = (turn.actions ?? "")
      .split("\n")
      .map((line) => line.trim())
      .filter((line) => line.length > 0);
    messages.push({
      role: "assistant",
      content:
        actions.length > 0
          ? `(Outils utilisés : ${actions.join(" ; ")})\n${turn.assistant}`
          : turn.assistant,
    });
  }
  messages.push({ role: "user", content: input.prompt });
  return messages;
}

function isJson(text: string): boolean {
  try {
    JSON.parse(text);
    return true;
  } catch {
    return false;
  }
}

function toolCallMessage(turn: ModelTurn, calls: ToolCall[]): ChatMessage {
  return {
    role: "assistant",
    content: turn.content.length > 0 ? turn.content : null,
    tool_calls: calls.map((call) => ({
      id: call.id,
      type: "function",
      // Cut arguments would make the provider reject the whole conversation.
      function: { name: call.name, arguments: isJson(call.arguments) ? call.arguments : "{}" },
    })),
    ...(turn.reasoning.length > 0 ? { reasoning_content: turn.reasoning } : {}),
  };
}

function textValue(value: unknown): string {
  if (typeof value === "string") {
    return value;
  }
  if (Array.isArray(value)) {
    return value
      .map((part: unknown) => {
        if (typeof part === "string") {
          return part;
        }
        return isRecord(part) && typeof part.text === "string" ? part.text : "";
      })
      .join("");
  }
  return "";
}

function mergeToolCalls(turn: ModelTurn, deltas: unknown[]): void {
  deltas.forEach((delta, position) => {
    if (!isRecord(delta)) {
      return;
    }
    const index = typeof delta.index === "number" ? delta.index : position;
    let call = turn.calls.get(index);
    if (call === undefined) {
      call = { id: "", name: "", arguments: "" };
      turn.calls.set(index, call);
    }
    if (typeof delta.id === "string" && delta.id.length > 0) {
      call.id = delta.id;
    }
    const fn = delta.function;
    if (!isRecord(fn)) {
      return;
    }
    // Some providers repeat the whole name in every chunk.
    if (typeof fn.name === "string" && fn.name.length > 0 && fn.name !== call.name) {
      call.name += fn.name;
    }
    if (typeof fn.arguments === "string") {
      call.arguments += fn.arguments;
    } else if (isRecord(fn.arguments)) {
      call.arguments = JSON.stringify(fn.arguments);
    }
  });
}

function upstreamErrorMessage(error: unknown): string {
  if (typeof error === "string") {
    return error;
  }
  if (isRecord(error) && typeof error.message === "string") {
    return error.message;
  }
  return "error event";
}

// One chunk of a streamed completion, or a whole completion ("message" instead of "delta").
function applyChunk(stream: AssistantStream, turn: ModelTurn, chunk: unknown): void {
  if (!isRecord(chunk)) {
    return;
  }
  if (chunk.error !== undefined && chunk.error !== null) {
    throw new UpstreamError(0, upstreamErrorMessage(chunk.error));
  }
  const choice: unknown = Array.isArray(chunk.choices) ? chunk.choices[0] : undefined;
  if (!isRecord(choice)) {
    return;
  }

  const delta = isRecord(choice.delta)
    ? choice.delta
    : isRecord(choice.message)
      ? choice.message
      : undefined;
  if (delta !== undefined) {
    // DeepSeek-style "reasoning_content", vLLM-style "reasoning".
    const reasoning = textValue(delta.reasoning_content) || textValue(delta.reasoning);
    if (reasoning.length > 0) {
      if (turn.reasoning.length === 0) {
        stream.thinking();
      }
      turn.reasoning += reasoning;
    }
    const content = textValue(delta.content);
    if (content.length > 0) {
      turn.content += content;
      stream.content(content);
    }
    if (Array.isArray(delta.tool_calls)) {
      mergeToolCalls(turn, delta.tool_calls);
    }
  }
  if (typeof choice.finish_reason === "string" && choice.finish_reason.length > 0) {
    turn.finishReason = choice.finish_reason;
  }
}

// The data of each server-sent event.
async function* readEvents(body: ReadableStream<Uint8Array>): AsyncGenerator<string> {
  const decoder = new TextDecoder();
  let buffer = "";
  let data: string[] = [];

  function takeLine(rawLine: string): string | undefined {
    const line = rawLine.endsWith("\r") ? rawLine.slice(0, -1) : rawLine;
    if (line.length === 0) {
      const event = data.length > 0 ? data.join("\n") : undefined;
      data = [];
      return event;
    }
    if (line.startsWith("data:")) {
      data.push(line.slice(line.startsWith("data: ") ? 6 : 5));
    }
    return undefined;
  }

  for await (const bytes of body) {
    buffer += decoder.decode(bytes, { stream: true });
    const lines = buffer.split("\n");
    buffer = lines.pop() ?? "";
    for (const line of lines) {
      const event = takeLine(line);
      if (event !== undefined) {
        yield event;
      }
    }
  }
  // A last line may come without its newline.
  buffer += decoder.decode();
  for (const line of [buffer, ""]) {
    const event = takeLine(line);
    if (event !== undefined) {
      yield event;
    }
  }
}

function connectionError(error: unknown): unknown {
  if (error instanceof UpstreamError) {
    return error;
  }
  if (error instanceof Error && error.name === "TimeoutError") {
    return new UpstreamError(408, "timeout");
  }
  if (error instanceof Error && error.name === "AbortError") {
    return error; // the Minitel hung up
  }
  return new UpstreamError(-1, error instanceof Error ? error.message : "unreachable");
}

// Short French message for the Minitel's status line.
function describeFailure(error: unknown): string {
  if (!(error instanceof UpstreamError)) {
    return "Erreur de l'assistant";
  }
  switch (error.status) {
    case -1:
      return "IA injoignable";
    case 0:
      return "IA erreur pendant la reponse";
    case 401:
    case 403:
      return "IA cle invalide";
    case 404:
      return "IA modele ou URL introuvable";
    case 408:
    case 504:
      return "IA timeout";
    case 429:
      return "IA quota atteint";
    case 400:
    case 413:
    case 422:
      return `IA requete refusee (${error.status})`;
    default:
      return `IA erreur serveur (${error.status})`;
  }
}

export function createAssistant(options: AssistantOptions) {
  const fetchModel = options.fetch ?? fetch;
  const confirmTimeoutMs = options.confirmTimeoutMs ?? CONFIRM_TIMEOUT_MS;
  const pending = new Map<string, PendingConfirmation>();

  function requestMessages(session: Session, nudge: boolean): ChatMessage[] {
    return session.messages.map((message, index) => {
      if (index === 0 && nudge && message.role === "system") {
        return { role: "system", content: `${message.content}\n${NUDGE}` };
      }
      if (
        !session.sendReasoning &&
        message.role === "assistant" &&
        message.reasoning_content !== undefined
      ) {
        return {
          role: "assistant",
          content: message.content,
          ...(message.tool_calls !== undefined ? { tool_calls: message.tool_calls } : {}),
        };
      }
      return message;
    });
  }

  async function callModel(
    session: Session,
    call: { tools: boolean; toolChoice: "auto" | "none"; nudge: boolean },
  ): Promise<ModelTurn> {
    const { ai } = session.input;
    const body = {
      model: ai.model,
      messages: requestMessages(session, call.nudge),
      stream: true,
      max_tokens: MAX_TOKENS,
      ...(call.tools ? { tools: assistantTools, tool_choice: call.toolChoice } : {}),
      // Only the gpt-oss models are known to accept it; others may reject the field.
      ...(ai.model.startsWith("openai-gpt-oss") ? { reasoning_effort: "low" } : {}),
    };

    const turn: ModelTurn = { content: "", reasoning: "", calls: new Map() };
    try {
      const response = await fetchModel(`${ai.baseUrl}/chat/completions`, {
        method: "POST",
        headers: {
          accept: "text/event-stream",
          authorization: `Bearer ${ai.apiKey}`,
          "content-type": "application/json",
        },
        body: JSON.stringify(body),
        signal: AbortSignal.any([session.stream.signal, AbortSignal.timeout(UPSTREAM_TIMEOUT_MS)]),
      });
      if (!response.ok) {
        const detail = await response.text().catch(() => "");
        throw new UpstreamError(response.status, detail.slice(0, 300));
      }

      if (!(response.headers.get("content-type") ?? "").includes("text/event-stream")) {
        applyChunk(session.stream, turn, await response.json());
      } else if (response.body !== null) {
        for await (const data of readEvents(response.body)) {
          if (data === "[DONE]") {
            break;
          }
          let chunk: unknown;
          try {
            chunk = JSON.parse(data);
          } catch {
            continue; // keep-alive or unknown event
          }
          applyChunk(session.stream, turn, chunk);
        }
      }
    } catch (error) {
      throw connectionError(error);
    }
    return turn;
  }

  // Asks the Minitel; resolves with the answer, or undefined when none came in time.
  function askUser(session: Session, question: string): Promise<boolean | undefined> {
    if (pending.size >= MAX_PENDING_CONFIRMATIONS) {
      const oldest = pending.values().next().value;
      oldest?.answer(undefined);
    }

    return new Promise((resolve) => {
      const id = randomBytes(12).toString("hex");
      let timer: NodeJS.Timeout | undefined;
      const answer: PendingConfirmation["answer"] = (decision) => {
        clearTimeout(timer);
        pending.delete(id);
        if (decision !== undefined) {
          session.stream = decision.stream;
        }
        resolve(decision?.accept);
      };
      timer = setTimeout(() => answer(undefined), confirmTimeoutMs);
      timer.unref();
      pending.set(id, { answer });
      session.stream.confirm(id, question);
    });
  }

  // The result for the model, or undefined when the conversation stops (the user left).
  async function runTool(session: Session, call: ToolCall): Promise<string | undefined> {
    const prepared = await prepareTool(session.context, call.name, call.arguments);
    if ("error" in prepared) {
      session.log.info({ tool: call.name, error: prepared.error }, "assistant tool refused");
      session.stream.action(`${prepared.label} (echec)`);
      return `Erreur : ${prepared.error}`;
    }

    if (prepared.question !== undefined) {
      const accepted = await askUser(session, prepared.question);
      if (accepted === undefined) {
        session.log.info({ tool: call.name }, "assistant confirmation expired");
        return undefined;
      }
      if (!accepted) {
        session.stream.action(`${prepared.label} (refuse)`);
        return "L'utilisateur a refusé cette action : ne la refais pas.";
      }
    }
    if (session.stream.signal.aborted) {
      return undefined;
    }

    try {
      const result = await prepared.run();
      session.log.info({ tool: call.name }, "assistant tool ran");
      session.stream.action(prepared.label);
      return result;
    } catch (error) {
      const message = describeToolError(error);
      session.log.info({ tool: call.name, error: message }, "assistant tool failed");
      session.stream.action(`${prepared.label} (echec)`);
      return `Erreur : ${message}`;
    }
  }

  async function converse(session: Session): Promise<void> {
    let useTools = true;
    let nudged = false;
    let round = 0;
    for (;;) {
      const lastRound = round >= MAX_TOOL_ROUNDS;
      let turn: ModelTurn;
      try {
        turn = await callModel(session, {
          tools: useTools,
          toolChoice: lastRound ? "none" : "auto",
          nudge: nudged,
        });
      } catch (error) {
        if (!(error instanceof UpstreamError) || (error.status !== 400 && error.status !== 422)) {
          throw error;
        }
        if (round === 0 && useTools) {
          // The model may not take tools: answer without them.
          session.log.warn(
            { status: error.status, detail: error.message },
            "assistant without tools",
          );
          useTools = false;
          session.stream.action("modele sans outils");
          continue;
        }
        const reasoningSent = session.messages.some(
          (message) => message.role === "assistant" && message.reasoning_content !== undefined,
        );
        if (session.sendReasoning && reasoningSent) {
          session.sendReasoning = false;
          continue;
        }
        throw error;
      }

      const calls = [...turn.calls.entries()]
        .sort(([left], [right]) => left - right)
        .map(([, call]) => call)
        .filter((call) => call.name.length > 0);
      if (calls.length > 0 && useTools && !lastRound) {
        for (const call of calls) {
          if (call.id.length === 0) {
            session.callCount += 1;
            call.id = `call_${session.callCount}`;
          }
        }
        session.messages.push(toolCallMessage(turn, calls));
        for (const [index, call] of calls.entries()) {
          const result =
            index < MAX_CALLS_PER_ROUND
              ? await runTool(session, call)
              : "Erreur : trop d'appels a la fois, refais celui-ci ensuite.";
          if (result === undefined) {
            return;
          }
          session.messages.push({ role: "tool", tool_call_id: call.id, content: result });
        }
        round += 1;
        continue;
      }

      if (turn.content.trim().length === 0) {
        if (!nudged) {
          // A reasoning model may spend its tokens thinking: ask once for the answer only.
          nudged = true;
          continue;
        }
        session.stream.fail("Reponse vide");
        return;
      }
      session.stream.finish(turn.finishReason === "length" ? "length" : "stop");
      return;
    }
  }

  async function run(session: Session): Promise<void> {
    try {
      await converse(session);
    } catch (error) {
      if (session.stream.signal.aborted) {
        session.log.info({}, "assistant stopped: the Minitel hung up");
        return;
      }
      session.log.warn(
        { error: error instanceof Error ? error.message : String(error) },
        "assistant failed",
      );
      session.stream.fail(describeFailure(error));
    }
  }

  function start(input: AssistantInput, stream: AssistantStream, log: FastifyBaseLogger): void {
    const now = describeNow(input.now);
    const session: Session = {
      input,
      context: { storage: options.storage, workspace: options.workspace, today: now.today },
      stream,
      log,
      messages: buildMessages(input, now.label),
      sendReasoning: true,
      callCount: 0,
    };
    void run(session);
  }

  function isPending(id: string): boolean {
    return pending.has(id);
  }

  // Resumes the conversation waiting for `id` on a new stream; false when it is gone.
  function confirm(id: string, accept: boolean, stream: AssistantStream): boolean {
    const entry = pending.get(id);
    if (entry === undefined) {
      return false;
    }
    entry.answer({ accept, stream });
    return true;
  }

  function close(): void {
    for (const entry of pending.values()) {
      entry.answer(undefined);
    }
  }

  return { start, isPending, confirm, close };
}
