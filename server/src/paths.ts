import path from "node:path";

import { HttpError } from "./errors.js";
import { allowedExtensions, type FileExtension } from "./types.js";

const allowedExtensionSet = new Set<FileExtension>(allowedExtensions);
const folderSegmentPattern = /^[A-Za-z0-9][A-Za-z0-9_-]*$/;
const fileBasePattern = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;

export interface ResolvedFolder {
  folder: string;
  folderPath: string;
}

export interface ResolvedFile {
  id: string;
  folder: string;
  name: string;
  extension: FileExtension;
  filePath: string;
  metaPath: string;
}

function ensureNoUnsafePathSyntax(value: string, label: string): void {
  if (
    value.length === 0 ||
    value.startsWith("/") ||
    value.includes("\\") ||
    value.includes("\0") ||
    value.includes("..")
  ) {
    throw new HttpError(400, `Invalid ${label}`);
  }
}

function ensurePathWithinRoot(rootPath: string, candidatePath: string): void {
  const relativePath = path.relative(rootPath, candidatePath);
  if (relativePath.startsWith("..") || path.isAbsolute(relativePath)) {
    throw new HttpError(400, "Invalid path");
  }
}

export function validateFolderName(folder: string): string {
  const normalizedFolder = folder.trim();
  ensureNoUnsafePathSyntax(normalizedFolder, "folder name");

  const segments = normalizedFolder.split("/");
  if (segments.some((segment) => segment.length === 0 || !folderSegmentPattern.test(segment))) {
    throw new HttpError(400, "Invalid folder name");
  }

  return segments.join("/");
}

export function validateFileName(name: string): string {
  const normalizedName = name.trim();
  ensureNoUnsafePathSyntax(normalizedName, "file name");

  const extensionSeparatorIndex = normalizedName.lastIndexOf(".");
  if (extensionSeparatorIndex <= 0 || extensionSeparatorIndex === normalizedName.length - 1) {
    throw new HttpError(400, "Unsupported file extension");
  }

  const baseName = normalizedName.slice(0, extensionSeparatorIndex);
  const extension = normalizedName.slice(extensionSeparatorIndex + 1) as FileExtension;

  if (!fileBasePattern.test(baseName)) {
    throw new HttpError(400, "Invalid file name");
  }

  if (!allowedExtensionSet.has(extension)) {
    throw new HttpError(400, "Unsupported file extension");
  }

  return normalizedName;
}

export function buildFileId(folder: string, name: string): string {
  return `${folder}/${name}`;
}

export function resolveFolderPath(dataPath: string, folder: string): ResolvedFolder {
  const safeFolder = validateFolderName(folder);
  const folderPath = path.resolve(dataPath, safeFolder);
  ensurePathWithinRoot(dataPath, folderPath);

  return {
    folder: safeFolder,
    folderPath,
  };
}

export function resolveFilePaths(dataPath: string, folder: string, name: string): ResolvedFile {
  const resolvedFolder = resolveFolderPath(dataPath, folder);
  const safeName = validateFileName(name);
  const filePath = path.resolve(resolvedFolder.folderPath, safeName);
  ensurePathWithinRoot(dataPath, filePath);

  const baseName = safeName.slice(0, safeName.lastIndexOf("."));
  const metaPath = path.resolve(resolvedFolder.folderPath, `${baseName}.meta.json`);
  ensurePathWithinRoot(dataPath, metaPath);

  return {
    id: buildFileId(resolvedFolder.folder, safeName),
    folder: resolvedFolder.folder,
    name: safeName,
    extension: safeName.slice(safeName.lastIndexOf(".") + 1) as FileExtension,
    filePath,
    metaPath,
  };
}

export function parseFileId(dataPath: string, id: string): ResolvedFile {
  const normalizedId = id.trim();
  ensureNoUnsafePathSyntax(normalizedId, "file id");

  const separatorIndex = normalizedId.lastIndexOf("/");
  if (separatorIndex <= 0 || separatorIndex === normalizedId.length - 1) {
    throw new HttpError(400, "Invalid file id");
  }

  const folder = normalizedId.slice(0, separatorIndex);
  const name = normalizedId.slice(separatorIndex + 1);

  return resolveFilePaths(dataPath, folder, name);
}

export function isSupportedFileName(name: string): boolean {
  try {
    validateFileName(name);
    return true;
  } catch {
    return false;
  }
}
