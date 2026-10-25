// Plain-text conventions used by the Minitel project manager. Everything stays readable
// and editable as Markdown:
//   - [ ] Commander les pieces !! @2026-10-25 14:30
//   - [x] Souder le connecteur
// A project sheet starts with "Field: value" lines; the time log has one line per session.

export interface ParsedTask {
  done: boolean;
  text: string;
  due?: string;
  time?: string;
  priority: number;
}

export interface ProjectSheet {
  title?: string;
  status?: string;
  goal?: string;
  next?: string;
}

const taskPattern = /^\s*[-*] \[( |x|X)\] (.*)$/;
const toggleablePattern = /^(\s*[-*] \[)( |x|X)(\] .*)$/;
const duePattern = /(^|\s)@(\d{4}-\d{2}-\d{2})(?:[ T](\d{2}:\d{2}))?(?=\s|$)/;
const priorityPattern = /(^|\s)(!{1,3})(?=\s|$)/;
const fieldPattern = /^\s*([^:#]{2,30}):\s*(.*)$/;
const timeLogPattern = /^\s*[-*]\s+(\d{4}-\d{2}-\d{2})\s+(?:\d{2}:\d{2}\s+)?\+(\d{1,4})\s*min\b/;

export const datePattern = /^\d{4}-\d{2}-\d{2}$/;
export const timePattern = /^\d{2}:\d{2}$/;

export function normalizeForSearch(value: string): string {
  return value
    .normalize("NFD")
    .replace(/[̀-ͯ]/g, "")
    .toLowerCase();
}

// "Mon Projet Été" -> "mon-projet-ete": a valid folder segment for the storage paths.
export function slugify(value: string): string {
  return normalizeForSearch(value)
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 40)
    .replace(/-+$/g, "");
}

export function parseTaskLine(line: string): ParsedTask | undefined {
  const match = taskPattern.exec(line);
  if (!match) {
    return undefined;
  }

  let text = match[2] ?? "";
  let due: string | undefined;
  let time: string | undefined;
  const dueMatch = duePattern.exec(text);
  if (dueMatch) {
    due = dueMatch[2];
    time = dueMatch[3];
    text = text.replace(dueMatch[0], " ");
  }

  let priority = 0;
  const priorityMatch = priorityPattern.exec(text);
  if (priorityMatch) {
    priority = priorityMatch[2]?.length ?? 0;
    text = text.replace(priorityMatch[0], " ");
  }

  return {
    done: match[1] !== " ",
    text: text.replace(/\s+/g, " ").trim(),
    ...(due !== undefined ? { due } : {}),
    ...(time !== undefined ? { time } : {}),
    priority,
  };
}

export function formatTaskLine(task: {
  text: string;
  done?: boolean;
  due?: string;
  time?: string;
  priority?: number;
}): string {
  const parts = [`- [${task.done === true ? "x" : " "}] ${task.text.trim()}`];
  if (task.priority !== undefined && task.priority > 0) {
    parts.push("!".repeat(Math.min(3, task.priority)));
  }
  if (task.due !== undefined) {
    parts.push(task.time !== undefined ? `@${task.due} ${task.time}` : `@${task.due}`);
  }
  return parts.join(" ");
}

// Rewrites only the checkbox so the rest of the line keeps its exact formatting.
export function setTaskDone(line: string, done: boolean): string | undefined {
  const match = toggleablePattern.exec(line);
  if (!match) {
    return undefined;
  }
  return `${match[1]}${done ? "x" : " "}${match[3]}`;
}

export function parseProjectSheet(content: string): ProjectSheet {
  const sheet: ProjectSheet = {};
  const lines = content.split("\n").slice(0, 30);
  for (const line of lines) {
    if (sheet.title === undefined && line.startsWith("# ")) {
      sheet.title = line.slice(2).trim();
      continue;
    }

    const match = fieldPattern.exec(line);
    if (!match) {
      continue;
    }

    const key = normalizeForSearch(match[1] ?? "").trim();
    const value = (match[2] ?? "").trim();
    if (value.length === 0) {
      continue;
    }
    if (key === "statut" || key === "status") {
      sheet.status = value;
    } else if (key === "objectif" || key === "goal") {
      sheet.goal = value;
    } else if (key === "prochaine etape" || key === "next") {
      sheet.next = value;
    }
  }
  return sheet;
}

// Minutes logged since `sinceDate` (inclusive) in lines like "- 2026-10-20 14:00 +90 min note".
export function sumLoggedMinutes(content: string, sinceDate: string): number {
  let total = 0;
  for (const line of content.split("\n")) {
    const match = timeLogPattern.exec(line);
    if (match && (match[1] ?? "") >= sinceDate) {
      total += Number.parseInt(match[2] ?? "0", 10);
    }
  }
  return total;
}

export function formatLocalDate(date: Date): string {
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, "0");
  const day = String(date.getDate()).padStart(2, "0");
  return `${year}-${month}-${day}`;
}

// Monday of the week containing `today` (YYYY-MM-DD).
export function startOfWeek(today: string): string {
  const date = new Date(`${today}T12:00:00`);
  const weekday = (date.getDay() + 6) % 7;
  date.setDate(date.getDate() - weekday);
  return formatLocalDate(date);
}

// Overdue first, then by due date, priority, project and position; undated tasks last.
export function compareTasks(
  left: { due?: string; priority: number; project: string; file: string; line: number },
  right: { due?: string; priority: number; project: string; file: string; line: number },
): number {
  if (left.due !== right.due) {
    if (left.due === undefined) {
      return 1;
    }
    if (right.due === undefined) {
      return -1;
    }
    return left.due < right.due ? -1 : 1;
  }
  if (left.priority !== right.priority) {
    return right.priority - left.priority;
  }
  if (left.project !== right.project) {
    return left.project < right.project ? -1 : 1;
  }
  if (left.file !== right.file) {
    return left.file < right.file ? -1 : 1;
  }
  return left.line - right.line;
}
