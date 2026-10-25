import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { afterEach, beforeEach, describe, expect, it } from "vite-plus/test";

import { buildApp } from "./app.js";
import type { AppConfig } from "./config.js";

const headers = { authorization: "Bearer super-secret" };
const ai = { baseUrl: "https://ai.test/v1/", apiKey: "sk-test", model: "deepseek-v4.1-flash" };

type Chunk = Record<string, unknown>;

// A streamed chat completion, as the provider sends it.
function stream(chunks: Chunk[]): Response {
  const body = `${chunks.map((chunk) => `data: ${JSON.stringify(chunk)}\n\n`).join("")}data: [DONE]\n\n`;
  return new Response(body, { headers: { "content-type": "text/event-stream" } });
}

function text(content: string): Chunk {
  return { choices: [{ index: 0, delta: { content }, finish_reason: null }] };
}

function finish(reason: string): Chunk {
  return { choices: [{ index: 0, delta: {}, finish_reason: reason }] };
}

function toolCall(index: number, id: string, name: string, args: string): Chunk {
  return {
    choices: [
      {
        index: 0,
        delta: {
          tool_calls: [{ index, id, type: "function", function: { name, arguments: args } }],
        },
      },
    ],
  };
}

function moreArguments(index: number, args: string): Chunk {
  return {
    choices: [{ index: 0, delta: { tool_calls: [{ index, function: { arguments: args } }] } }],
  };
}

interface ModelRequest {
  url: string;
  authorization: string;
  body: Record<string, unknown> & { messages: Array<Record<string, unknown>> };
}

// Answers the assistant's chat completion requests in order, and records them.
function fakeModel(...responses: Response[]) {
  const requests: ModelRequest[] = [];
  const modelFetch = async (input: string | URL | Request, init?: RequestInit) => {
    requests.push({
      url: typeof input === "string" ? input : input instanceof URL ? input.href : input.url,
      authorization: new Headers(init?.headers).get("authorization") ?? "",
      body: JSON.parse(typeof init?.body === "string" ? init.body : "{}") as ModelRequest["body"],
    });
    const response = responses.shift();
    if (response === undefined) {
      throw new Error("unexpected model call");
    }
    return response;
  };
  return { fetch: modelFetch as typeof fetch, requests };
}

interface StreamSummary {
  actions: string[];
  text: string;
  thinking: boolean;
  confirm?: { id: string; text: string };
  finishReason?: string;
  error?: string;
  done: boolean;
}

// What the Minitel gets from a /assistant response.
function summarize(payload: string): StreamSummary {
  const summary: StreamSummary = { actions: [], text: "", thinking: false, done: false };
  for (const block of payload.split("\n\n")) {
    if (!block.startsWith("data: ")) {
      continue;
    }
    const data = block.slice(6);
    if (data === "[DONE]") {
      summary.done = true;
      continue;
    }
    const event = JSON.parse(data) as {
      action?: string;
      confirm?: { id: string; text: string };
      error?: { message: string };
      choices?: Array<{
        delta: { content?: string; reasoning_content?: string };
        finish_reason?: string;
      }>;
    };
    if (event.action !== undefined) {
      summary.actions.push(event.action);
    }
    if (event.confirm !== undefined) {
      summary.confirm = event.confirm;
    }
    if (event.error !== undefined) {
      summary.error = event.error.message;
    }
    const choice = event.choices?.[0];
    if (choice !== undefined) {
      summary.text += choice.delta.content ?? "";
      summary.thinking ||= choice.delta.reasoning_content !== undefined;
      if (choice.finish_reason !== undefined) {
        summary.finishReason = choice.finish_reason;
      }
    }
  }
  return summary;
}

describe("assistant API", () => {
  let tempDirectory: string;
  let config: AppConfig;
  let app!: ReturnType<typeof buildApp>;

  beforeEach(async () => {
    tempDirectory = await fs.mkdtemp(path.join(os.tmpdir(), "bibliotek-assistant-"));
    config = {
      apiToken: "super-secret",
      dataPath: path.join(tempDirectory, "files"),
      port: 3000,
      maxFileSizeBytes: 4096,
      requestTimeoutMs: 10_000,
      firmwarePath: path.join(tempDirectory, "firmware"),
      firmwareMaxBytes: 1024,
    };
  });

  afterEach(async () => {
    await app.close();
    await fs.rm(tempDirectory, { recursive: true, force: true });
  });

  async function start(model: ReturnType<typeof fakeModel>): Promise<void> {
    app = buildApp(config, { fetch: model.fetch });
    await app.ready();
  }

  async function writeNote(id: string, content: string): Promise<void> {
    const separator = id.lastIndexOf("/");
    await fs.mkdir(path.join(config.dataPath, id.slice(0, separator)), { recursive: true });
    await fs.writeFile(path.join(config.dataPath, id), content);
  }

  async function readNote(id: string): Promise<string> {
    return fs.readFile(path.join(config.dataPath, id), "utf8");
  }

  async function ask(payload: Record<string, unknown>) {
    return app.inject({
      method: "POST",
      url: "/assistant",
      headers,
      payload: { ai, ...payload },
    });
  }

  async function answer(id: string, accept: boolean) {
    return app.inject({
      method: "POST",
      url: "/assistant/confirm",
      headers,
      payload: { id, accept },
    });
  }

  it("reads the notes it needs, then streams the answer", async () => {
    await writeNote("notes/courses.md", "- lait\n- pain\n");
    const model = fakeModel(
      stream([
        { choices: [{ index: 0, delta: { reasoning_content: "Je dois lire la liste." } }] },
        toolCall(0, "call_a", "read_file", '{"path":'),
        moreArguments(0, ' "notes/courses.md"}'),
        finish("tool_calls"),
      ]),
      stream([text("Il reste du lait "), text("et du pain."), finish("stop")]),
    );
    await start(model);

    const response = await ask({
      now: "2026-10-20 14:32",
      history: [
        { user: "Bonjour", assistant: "Salut !" },
        { user: "Range", assistant: "C'est fait.", actions: "lit notes/a.md\ncree notes/b.md" },
      ],
      prompt: "Que reste-t-il sur ma liste de courses ?",
    });

    expect(response.statusCode).toBe(200);
    expect(response.headers["content-type"]).toBe("text/event-stream; charset=utf-8");
    expect(summarize(response.body)).toEqual({
      actions: ["lit notes/courses.md"],
      text: "Il reste du lait et du pain.",
      thinking: true,
      finishReason: "stop",
      done: true,
    });

    expect(model.requests).toHaveLength(2);
    const [first, second] = model.requests;
    expect(first?.url).toBe("https://ai.test/v1/chat/completions");
    expect(first?.authorization).toBe("Bearer sk-test");
    expect(first?.body).toMatchObject({
      model: "deepseek-v4.1-flash",
      stream: true,
      tool_choice: "auto",
    });
    expect(first?.body.tools).toHaveLength(14);
    expect(first?.body.messages[0]).toMatchObject({ role: "system" });
    expect(String(first?.body.messages[0]?.content)).toContain("mardi 20 octobre 2026, 14:32");
    expect(first?.body.messages.slice(1)).toEqual([
      { role: "user", content: "Bonjour" },
      { role: "assistant", content: "Salut !" },
      { role: "user", content: "Range" },
      {
        role: "assistant",
        content: "(Outils utilisés : lit notes/a.md ; cree notes/b.md)\nC'est fait.",
      },
      { role: "user", content: "Que reste-t-il sur ma liste de courses ?" },
    ]);

    expect(second?.body.messages.slice(-2)).toEqual([
      {
        role: "assistant",
        content: null,
        reasoning_content: "Je dois lire la liste.",
        tool_calls: [
          {
            id: "call_a",
            type: "function",
            function: { name: "read_file", arguments: '{"path": "notes/courses.md"}' },
          },
        ],
      },
      {
        role: "tool",
        tool_call_id: "call_a",
        content: "notes/courses.md (2 lignes) :\n- lait\n- pain",
      },
    ]);
  });

  it("asks before changing a note, then goes on in the confirmation's response", async () => {
    await writeNote("notes/courses.md", "- lait\n");
    const model = fakeModel(
      stream([
        toolCall(
          0,
          "call_edit",
          "edit_file",
          JSON.stringify({
            path: "notes/courses.md",
            old_text: "- lait",
            new_text: "- lait\n- oeufs",
          }),
        ),
        finish("tool_calls"),
      ]),
      stream([text("C'est ajoute."), finish("stop")]),
    );
    await start(model);

    const asked = summarize((await ask({ prompt: "Ajoute des oeufs aux courses" })).body);
    expect(asked).toMatchObject({
      actions: [],
      text: "",
      confirm: { text: 'Modifier notes/courses.md : "- lait" -> "- lait - oeufs"' },
      done: true,
    });
    expect(asked.confirm?.id).toMatch(/^[0-9a-f]{24}$/);
    expect(await readNote("notes/courses.md")).toBe("- lait\n");

    const resumed = await answer(asked.confirm?.id ?? "", true);
    expect(resumed.statusCode).toBe(200);
    expect(summarize(resumed.body)).toEqual({
      actions: ["modifie notes/courses.md"],
      text: "C'est ajoute.",
      thinking: false,
      finishReason: "stop",
      done: true,
    });
    expect(await readNote("notes/courses.md")).toBe("- lait\n- oeufs\n");
    expect(model.requests[1]?.body.messages.at(-1)).toEqual({
      role: "tool",
      tool_call_id: "call_edit",
      content: "notes/courses.md modifie.",
    });

    // Answered once only.
    const again = await answer(asked.confirm?.id ?? "", true);
    expect(again.statusCode).toBe(404);
    expect(again.json()).toEqual({ error: "Confirmation expired" });
  });

  it("tells the model when the user refuses", async () => {
    await writeNote("notes/courses.md", "- lait\n");
    const model = fakeModel(
      stream([
        toolCall(0, "call_rm", "delete_file", '{"path":"notes/courses.md"}'),
        finish("tool_calls"),
      ]),
      stream([text("D'accord, je la garde."), finish("stop")]),
    );
    await start(model);

    const asked = summarize((await ask({ prompt: "Supprime ma liste" })).body);
    expect(asked.confirm?.text).toBe("Supprimer notes/courses.md (1 ligne)");

    const refused = summarize((await answer(asked.confirm?.id ?? "", false)).body);
    expect(refused).toMatchObject({
      actions: ["supprime notes/courses.md (refuse)"],
      text: "D'accord, je la garde.",
    });
    expect(await readNote("notes/courses.md")).toBe("- lait\n");
    expect(model.requests[1]?.body.messages.at(-1)).toMatchObject({
      role: "tool",
      tool_call_id: "call_rm",
      content: expect.stringContaining("refusé") as unknown,
    });
  });

  it("runs harmless calls without asking and reports failed ones", async () => {
    const model = fakeModel(
      stream([
        toolCall(0, "", "write_file", '{"path":"notes/idees.md","content":"Robot\\n"}'),
        toolCall(1, "", "read_file", '{"path":"notes/absent.md"}'),
        finish("tool_calls"),
      ]),
      stream([text("Note creee."), finish("stop")]),
    );
    await start(model);

    expect(summarize((await ask({ prompt: "Note mon idee" })).body)).toMatchObject({
      actions: ["cree notes/idees.md", "lit notes/absent.md (echec)"],
      text: "Note creee.",
    });
    expect(await readNote("notes/idees.md")).toBe("Robot\n");
    expect(model.requests[1]?.body.messages.slice(-2)).toEqual([
      { role: "tool", tool_call_id: "call_1", content: "notes/idees.md cree (1 ligne)." },
      { role: "tool", tool_call_id: "call_2", content: "Erreur : File not found" },
    ]);
  });

  it("answers without tools when the model refuses them", async () => {
    const model = fakeModel(
      new Response('{"error":{"message":"tools are not supported"}}', { status: 400 }),
      stream([text("Bonjour."), finish("stop")]),
    );
    await start(model);

    expect(summarize((await ask({ prompt: "Salut" })).body)).toMatchObject({
      actions: ["modele sans outils"],
      text: "Bonjour.",
      finishReason: "stop",
    });
    expect(model.requests[1]?.body.tools).toBeUndefined();
  });

  it("asks a reasoning model once more for an answer it did not write", async () => {
    const model = fakeModel(
      stream([{ choices: [{ index: 0, delta: { reasoning: "Hmm..." } }] }, finish("length")]),
      stream([text("Oui."), finish("stop")]),
    );
    await start(model);

    expect(summarize((await ask({ prompt: "Ca va ?" })).body)).toMatchObject({
      text: "Oui.",
      thinking: true,
    });
    expect(String(model.requests[1]?.body.messages[0]?.content)).toContain(
      "Donne maintenant ta réponse finale",
    );
  });

  it("reads a reply that was not streamed, and cuts long text into small events", async () => {
    const long = "abcdefghij".repeat(40);
    const model = fakeModel(
      Response.json({ choices: [{ message: { content: long }, finish_reason: "length" }] }),
    );
    await start(model);

    const response = await ask({ prompt: "Raconte" });
    const contents = response.body.split("\n\n").filter((block) => block.includes('"content"'));
    expect(contents).toHaveLength(3);
    expect(contents.every((block) => Buffer.byteLength(block) < 1024)).toBe(true);
    expect(summarize(response.body)).toMatchObject({ text: long, finishReason: "length" });
  });

  it("reports model errors in a few French words", async () => {
    const model = fakeModel(
      new Response("unauthorized", { status: 401 }),
      stream([{ error: { message: "overloaded" } }]),
    );
    await start(model);

    expect(summarize((await ask({ prompt: "Salut" })).body)).toMatchObject({
      error: "IA cle invalide",
      done: true,
    });
    expect(summarize((await ask({ prompt: "Salut" })).body)).toMatchObject({
      error: "IA erreur pendant la reponse",
    });
  });

  it("checks the request before calling the model", async () => {
    const model = fakeModel();
    await start(model);

    const unauthorized = await app.inject({ method: "POST", url: "/assistant", payload: {} });
    expect(unauthorized.statusCode).toBe(401);

    expect((await ask({})).json()).toEqual({ error: "prompt must be a non-empty string" });
    const plain = await app.inject({
      method: "POST",
      url: "/assistant",
      headers,
      payload: { ai: { ...ai, baseUrl: "http://ai.test/v1" }, prompt: "Salut" },
    });
    expect(plain.statusCode).toBe(400);
    expect(plain.json()).toEqual({ error: "ai.baseUrl must be an https URL" });
    expect((await ask({ prompt: "Salut", now: "demain" })).json()).toEqual({
      error: "Invalid now",
    });
    expect((await ask({ prompt: "Salut", history: [{ user: "a" }] })).json()).toEqual({
      error: "Invalid history",
    });

    expect((await answer("inconnu", true)).statusCode).toBe(404);
    const noAnswer = await app.inject({
      method: "POST",
      url: "/assistant/confirm",
      headers,
      payload: { id: "inconnu" },
    });
    expect(noAnswer.json()).toEqual({ error: "accept must be a boolean" });
    expect(model.requests).toHaveLength(0);
  });
});
