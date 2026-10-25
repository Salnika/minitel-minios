import { describe, expect, it } from "vite-plus/test";

import {
  formatTaskLine,
  parseProjectSheet,
  parseTaskLine,
  setTaskDone,
  slugify,
  startOfWeek,
  sumLoggedMinutes,
} from "./tasks.js";

describe("task conventions", () => {
  it("parses checkboxes, due dates, times and priorities", () => {
    expect(parseTaskLine("- [ ] Commander les pieces !! @2026-10-25 14:30")).toEqual({
      done: false,
      text: "Commander les pieces",
      due: "2026-10-25",
      time: "14:30",
      priority: 2,
    });
    expect(parseTaskLine("* [X] Souder @2026-01-02")).toEqual({
      done: true,
      text: "Souder",
      due: "2026-01-02",
      priority: 0,
    });
    expect(parseTaskLine("Pas une tache")).toBeUndefined();
    expect(parseTaskLine("- [ ] email a@b.c reste du texte")).toMatchObject({
      text: "email a@b.c reste du texte",
    });
  });

  it("formats tasks that parse back to the same values", () => {
    const line = formatTaskLine({ text: "Tester", due: "2026-10-21", time: "09:00", priority: 1 });
    expect(line).toBe("- [ ] Tester ! @2026-10-21 09:00");
    expect(parseTaskLine(line)).toEqual({
      done: false,
      text: "Tester",
      due: "2026-10-21",
      time: "09:00",
      priority: 1,
    });
  });

  it("toggles only the checkbox", () => {
    expect(setTaskDone("  - [ ] Garde   les espaces !", true)).toBe(
      "  - [x] Garde   les espaces !",
    );
    expect(setTaskDone("- [x] Fait", false)).toBe("- [ ] Fait");
    expect(setTaskDone("Texte", true)).toBeUndefined();
  });

  it("reads the project sheet fields", () => {
    expect(
      parseProjectSheet(
        "# Robot Minitel\nStatut: en pause\nObjectif: Afficher\nProchaine étape: Souder\n\nNotes:\n",
      ),
    ).toEqual({ title: "Robot Minitel", status: "en pause", goal: "Afficher", next: "Souder" });
  });

  it("sums logged minutes since a date", () => {
    const log =
      "# Temps\n- 2026-10-12 10:00 +30 min Avant\n- 2026-10-19 +45 min\n- 2026-10-20 14:00 +90 min Soudure\n";
    expect(sumLoggedMinutes(log, "2026-10-19")).toBe(135);
  });

  it("builds folder-safe project names and week starts", () => {
    expect(slugify("  Mon Projet Été ! ")).toBe("mon-projet-ete");
    expect(slugify("!!!")).toBe("");
    expect(startOfWeek("2026-10-22")).toBe("2026-10-19");
    expect(startOfWeek("2026-10-25")).toBe("2026-10-19");
    expect(startOfWeek("2026-10-19")).toBe("2026-10-19");
  });
});
