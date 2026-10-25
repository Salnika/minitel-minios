import { randomUUID } from "node:crypto";
import fs from "node:fs/promises";
import path from "node:path";

import type { AppConfig } from "./config.js";
import { HttpError, isErrnoException } from "./errors.js";
import {
  buildFileId,
  isSupportedFileName,
  parseFileId,
  resolveFilePaths,
  resolveFolderPath,
} from "./paths.js";
import type {
  CreateFileInput,
  FileDetails,
  FileMetadata,
  FileSort,
  FileSummary,
  ListFilesOptions,
  SortOrder,
  UpdateFileInput,
} from "./types.js";

interface StoredFileRecord extends FileSummary {
  tags?: string[];
  sortOrder?: number;
  filePath: string;
  metaPath: string;
  updatedAtMs: number;
}

function hasMetadata(metadata: FileMetadata): boolean {
  return (
    metadata.title !== undefined || metadata.tags !== undefined || metadata.sortOrder !== undefined
  );
}

function cloneMetadata(metadata: FileMetadata): FileMetadata {
  return {
    ...(metadata.title !== undefined ? { title: metadata.title } : {}),
    ...(metadata.tags !== undefined ? { tags: [...metadata.tags] } : {}),
    ...(metadata.sortOrder !== undefined ? { sortOrder: metadata.sortOrder } : {}),
  };
}

function validateMetadata(input: FileMetadata, source: string): FileMetadata {
  const metadata = cloneMetadata(input);

  if (metadata.title !== undefined && typeof metadata.title !== "string") {
    throw new HttpError(400, `Invalid title in ${source}`);
  }

  if (metadata.tags !== undefined) {
    if (!Array.isArray(metadata.tags) || metadata.tags.some((tag) => typeof tag !== "string")) {
      throw new HttpError(400, `Invalid tags in ${source}`);
    }
  }

  if (
    metadata.sortOrder !== undefined &&
    (!Number.isFinite(metadata.sortOrder) || !Number.isInteger(metadata.sortOrder))
  ) {
    throw new HttpError(400, `Invalid sortOrder in ${source}`);
  }

  return metadata;
}

export async function pathExists(candidatePath: string): Promise<boolean> {
  try {
    await fs.access(candidatePath);
    return true;
  } catch (error) {
    if (isErrnoException(error) && error.code === "ENOENT") {
      return false;
    }

    throw error;
  }
}

async function ensureDirectoryExists(directoryPath: string): Promise<void> {
  try {
    const stats = await fs.stat(directoryPath);
    if (!stats.isDirectory()) {
      throw new HttpError(404, "Folder not found");
    }
  } catch (error) {
    if (isErrnoException(error) && error.code === "ENOENT") {
      throw new HttpError(404, "Folder not found");
    }

    throw error;
  }
}

async function ensureFileExists(filePath: string): Promise<void> {
  try {
    const stats = await fs.stat(filePath);
    if (!stats.isFile()) {
      throw new HttpError(404, "File not found");
    }
  } catch (error) {
    if (isErrnoException(error) && error.code === "ENOENT") {
      throw new HttpError(404, "File not found");
    }

    throw error;
  }
}

function assertContentSize(content: string, maxFileSizeBytes: number): void {
  const contentSize = Buffer.byteLength(content, "utf8");
  if (contentSize > maxFileSizeBytes) {
    throw new HttpError(400, "File too large");
  }
}

export async function writeTextFileAtomic(filePath: string, content: string): Promise<void> {
  const tempPath = path.join(
    path.dirname(filePath),
    `.tmp-${path.basename(filePath)}-${process.pid}-${randomUUID()}`,
  );

  await fs.writeFile(tempPath, content, "utf8");

  try {
    await fs.rename(tempPath, filePath);
  } catch (error) {
    await fs.rm(tempPath, { force: true });
    throw error;
  }
}

async function removeIfExists(filePath: string): Promise<void> {
  try {
    await fs.rm(filePath, { force: true });
  } catch (error) {
    if (isErrnoException(error) && error.code === "ENOENT") {
      return;
    }

    throw error;
  }
}

async function readMetadata(metaPath: string): Promise<FileMetadata> {
  try {
    const rawContent = await fs.readFile(metaPath, "utf8");
    const parsed = JSON.parse(rawContent) as unknown;

    if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
      throw new HttpError(500, "Invalid metadata file");
    }

    return validateMetadata(parsed as FileMetadata, "stored metadata");
  } catch (error) {
    if (isErrnoException(error) && error.code === "ENOENT") {
      return {};
    }

    if (error instanceof SyntaxError) {
      throw new HttpError(500, "Invalid metadata file");
    }

    throw error;
  }
}

async function writeMetadata(metaPath: string, metadata: FileMetadata): Promise<void> {
  if (!hasMetadata(metadata)) {
    await removeIfExists(metaPath);
    return;
  }

  const serialized = `${JSON.stringify(cloneMetadata(metadata), null, 2)}\n`;
  await writeTextFileAtomic(metaPath, serialized);
}

function compareStrings(left: string, right: string): number {
  return left.localeCompare(right, "en", { sensitivity: "base", numeric: true });
}

async function collectFolderPaths(rootPath: string, currentPath = rootPath): Promise<string[]> {
  const entries = await fs.readdir(currentPath, { withFileTypes: true });
  const directories = entries
    .filter((entry) => entry.isDirectory())
    .sort((left, right) => compareStrings(left.name, right.name));

  const folders: string[] = [];
  for (const directory of directories) {
    const absolutePath = path.join(currentPath, directory.name);
    const relativePath = path.relative(rootPath, absolutePath).split(path.sep).join("/");
    folders.push(relativePath);
    folders.push(...(await collectFolderPaths(rootPath, absolutePath)));
  }

  return folders;
}

type SortableFileSummary = FileSummary & { sortOrder?: number };

export function sortFileSummaries<T extends SortableFileSummary>(
  files: T[],
  sort: FileSort,
  order: SortOrder,
): T[] {
  const direction = order === "asc" ? 1 : -1;

  return [...files].sort((left, right) => {
    let comparison = 0;

    if (sort === "name") {
      comparison = compareStrings(left.name, right.name);
    } else if (sort === "updatedAt") {
      comparison = new Date(left.updatedAt).getTime() - new Date(right.updatedAt).getTime();
    } else if (sort === "size") {
      comparison = left.size - right.size;
    } else {
      const leftOrder = left.sortOrder;
      const rightOrder = right.sortOrder;

      if (leftOrder !== undefined && rightOrder !== undefined) {
        comparison = leftOrder - rightOrder;
      } else if (leftOrder !== undefined) {
        comparison = -1;
      } else if (rightOrder !== undefined) {
        comparison = 1;
      } else {
        comparison = compareStrings(left.name, right.name);
      }
    }

    if (comparison === 0) {
      comparison = compareStrings(left.name, right.name);
    }

    if (comparison === 0) {
      comparison = compareStrings(left.id, right.id);
    }

    return comparison * direction;
  });
}

function toPublicSummary(record: StoredFileRecord): FileSummary {
  return {
    id: record.id,
    name: record.name,
    ...(record.title !== undefined ? { title: record.title } : {}),
    folder: record.folder,
    updatedAt: record.updatedAt,
    size: record.size,
  };
}

async function buildStoredFileRecord(
  filePath: string,
  metaPath: string,
  folder: string,
  name: string,
): Promise<StoredFileRecord> {
  const [fileStats, metadata] = await Promise.all([fs.stat(filePath), readMetadata(metaPath)]);
  const metaExists = await pathExists(metaPath);
  const metaStats = metaExists ? await fs.stat(metaPath) : undefined;
  const updatedAtMs = Math.max(fileStats.mtimeMs, metaStats?.mtimeMs ?? fileStats.mtimeMs);

  return {
    id: buildFileId(folder, name),
    name,
    ...(metadata.title !== undefined ? { title: metadata.title } : {}),
    ...(metadata.tags !== undefined ? { tags: metadata.tags } : {}),
    ...(metadata.sortOrder !== undefined ? { sortOrder: metadata.sortOrder } : {}),
    folder,
    updatedAt: new Date(updatedAtMs).toISOString(),
    size: fileStats.size,
    filePath,
    metaPath,
    updatedAtMs,
  };
}

async function moveManagedFiles(
  sourceFilePath: string,
  sourceMetaPath: string,
  targetFilePath: string,
  targetMetaPath: string,
): Promise<void> {
  if (targetFilePath !== sourceFilePath && (await pathExists(targetFilePath))) {
    throw new HttpError(400, "Target file already exists");
  }

  if (targetMetaPath !== sourceMetaPath && (await pathExists(targetMetaPath))) {
    throw new HttpError(400, "Target file already exists");
  }

  const sourceMetaExists = await pathExists(sourceMetaPath);
  await fs.rename(sourceFilePath, targetFilePath);

  try {
    if (sourceMetaExists && targetMetaPath !== sourceMetaPath) {
      await fs.rename(sourceMetaPath, targetMetaPath);
    }
  } catch (error) {
    await fs.rename(targetFilePath, sourceFilePath).catch(() => undefined);

    if (sourceMetaExists && targetMetaPath !== sourceMetaPath) {
      await fs.rename(targetMetaPath, sourceMetaPath).catch(() => undefined);
    }

    throw error;
  }
}

export function createStorage(config: AppConfig) {
  async function ensureDataRoot(): Promise<void> {
    await fs.mkdir(config.dataPath, { recursive: true });
  }

  async function listFolders(): Promise<string[]> {
    await ensureDataRoot();
    return collectFolderPaths(config.dataPath);
  }

  async function createFolder(name: string): Promise<string> {
    await ensureDataRoot();

    const { folder, folderPath } = resolveFolderPath(config.dataPath, name);
    await ensureDirectoryExists(path.dirname(folderPath));

    try {
      await fs.mkdir(folderPath);
    } catch (error) {
      if (isErrnoException(error) && error.code === "EEXIST") {
        throw new HttpError(400, "Folder already exists");
      }
      if (isErrnoException(error) && error.code === "ENOENT") {
        throw new HttpError(404, "Folder not found");
      }

      throw error;
    }

    return folder;
  }

  async function collectFolderFiles(folder: string): Promise<StoredFileRecord[]> {
    const { folderPath } = resolveFolderPath(config.dataPath, folder);
    await ensureDirectoryExists(folderPath);

    const entries = await fs.readdir(folderPath, { withFileTypes: true });
    const files = entries
      .filter((entry) => entry.isFile() && isSupportedFileName(entry.name))
      .map((entry) => {
        const resolved = resolveFilePaths(config.dataPath, folder, entry.name);
        return buildStoredFileRecord(
          resolved.filePath,
          resolved.metaPath,
          resolved.folder,
          resolved.name,
        );
      });

    return Promise.all(files);
  }

  async function listFiles(options: ListFilesOptions): Promise<FileSummary[]> {
    const folders =
      options.folder !== undefined
        ? [resolveFolderPath(config.dataPath, options.folder).folder]
        : await listFolders();

    const files = (await Promise.all(folders.map((folder) => collectFolderFiles(folder)))).flat();
    return sortFileSummaries(files, options.sort, options.order).map(toPublicSummary);
  }

  async function getFile(id: string): Promise<FileDetails> {
    const resolved = parseFileId(config.dataPath, id);
    await ensureFileExists(resolved.filePath);

    const [record, content] = await Promise.all([
      buildStoredFileRecord(resolved.filePath, resolved.metaPath, resolved.folder, resolved.name),
      fs.readFile(resolved.filePath, "utf8"),
    ]);

    return {
      ...toPublicSummary(record),
      content,
      ...(record.tags !== undefined ? { tags: [...record.tags] } : {}),
      ...(record.sortOrder !== undefined ? { sortOrder: record.sortOrder } : {}),
    };
  }

  async function createFile(input: CreateFileInput): Promise<FileDetails> {
    const resolved = resolveFilePaths(config.dataPath, input.folder, input.name);
    const metadata = validateMetadata(input, "request body");

    await ensureDataRoot();
    await ensureDirectoryExists(path.dirname(resolved.filePath));
    assertContentSize(input.content, config.maxFileSizeBytes);

    if (await pathExists(resolved.filePath)) {
      throw new HttpError(400, "File already exists");
    }

    await writeTextFileAtomic(resolved.filePath, input.content);
    await writeMetadata(resolved.metaPath, metadata);

    return getFile(resolved.id);
  }

  async function updateFile(id: string, input: UpdateFileInput): Promise<FileDetails> {
    const resolved = parseFileId(config.dataPath, id);
    await ensureFileExists(resolved.filePath);
    assertContentSize(input.content, config.maxFileSizeBytes);

    const existingMetadata = await readMetadata(resolved.metaPath);
    const nextMetadata = validateMetadata(
      {
        ...existingMetadata,
        ...input,
      },
      "request body",
    );

    await writeTextFileAtomic(resolved.filePath, input.content);
    await writeMetadata(resolved.metaPath, nextMetadata);

    return getFile(resolved.id);
  }

  async function deleteFile(id: string): Promise<void> {
    const resolved = parseFileId(config.dataPath, id);
    await ensureFileExists(resolved.filePath);

    await fs.rm(resolved.filePath);
    await removeIfExists(resolved.metaPath);
  }

  async function renameFile(id: string, newName: string): Promise<FileDetails> {
    const source = parseFileId(config.dataPath, id);
    await ensureFileExists(source.filePath);

    const target = resolveFilePaths(config.dataPath, source.folder, newName);
    if (target.name === source.name) {
      return getFile(source.id);
    }

    await moveManagedFiles(source.filePath, source.metaPath, target.filePath, target.metaPath);

    return getFile(target.id);
  }

  async function moveFile(id: string, targetFolder: string): Promise<FileDetails> {
    const source = parseFileId(config.dataPath, id);
    await ensureFileExists(source.filePath);

    const target = resolveFilePaths(config.dataPath, targetFolder, source.name);
    await ensureDirectoryExists(path.dirname(target.filePath));

    if (target.folder === source.folder) {
      return getFile(source.id);
    }

    await moveManagedFiles(source.filePath, source.metaPath, target.filePath, target.metaPath);

    return getFile(target.id);
  }

  async function fileExists(id: string): Promise<boolean> {
    return pathExists(parseFileId(config.dataPath, id).filePath);
  }

  // Creates `folder` with its missing parents; false when it was already there.
  async function ensureFolder(folder: string): Promise<boolean> {
    await ensureDataRoot();
    const { folderPath } = resolveFolderPath(config.dataPath, folder);
    if (await pathExists(folderPath)) {
      await ensureDirectoryExists(folderPath);
      return false;
    }

    await fs.mkdir(folderPath, { recursive: true });
    return true;
  }

  // Moves and renames at once, creating the target folder: `id` and `targetId` are full ids.
  async function relocateFile(id: string, targetId: string): Promise<FileDetails> {
    const source = parseFileId(config.dataPath, id);
    await ensureFileExists(source.filePath);

    const target = parseFileId(config.dataPath, targetId);
    if (target.id === source.id) {
      return getFile(source.id);
    }

    await ensureFolder(target.folder);
    await moveManagedFiles(source.filePath, source.metaPath, target.filePath, target.metaPath);

    return getFile(target.id);
  }

  return {
    ensureDataRoot,
    listFolders,
    createFolder,
    listFiles,
    getFile,
    createFile,
    updateFile,
    deleteFile,
    renameFile,
    moveFile,
    fileExists,
    ensureFolder,
    relocateFile,
  };
}
