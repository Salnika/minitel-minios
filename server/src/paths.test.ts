import path from "node:path";

import { describe, expect, it } from "vite-plus/test";

import { parseFileId, resolveFilePaths, validateFileName, validateFolderName } from "./paths.js";

describe("path validation", () => {
  it("rejects unsafe folder names", () => {
    expect(() => validateFolderName("../notes")).toThrow("Invalid folder name");
    expect(() => validateFolderName("/notes")).toThrow("Invalid folder name");
    expect(() => validateFolderName("notes//sub")).toThrow("Invalid folder name");
  });

  it("accepts nested folder names", () => {
    expect(validateFolderName("notes/sub")).toBe("notes/sub");
  });

  it("rejects unsupported file names", () => {
    expect(() => validateFileName("")).toThrow("Invalid file name");
    expect(() => validateFileName("notes.json")).toThrow("Unsupported file extension");
    expect(() => validateFileName("../note.txt")).toThrow("Invalid file name");
  });

  it("resolves file and metadata paths inside the data root", () => {
    const dataPath = "/tmp/bibliotek-root";
    const resolved = resolveFilePaths(dataPath, "notes", "meeting.txt");

    expect(resolved.filePath).toBe(path.resolve(dataPath, "notes/meeting.txt"));
    expect(resolved.metaPath).toBe(path.resolve(dataPath, "notes/meeting.meta.json"));
  });

  it("parses nested file ids", () => {
    const resolved = parseFileId("/tmp/bibliotek-root", "notes/nested/file.txt");
    expect(resolved.folder).toBe("notes/nested");
    expect(resolved.name).toBe("file.txt");
  });
});
