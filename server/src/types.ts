export const allowedExtensions = ["txt", "md"] as const;

export type FileExtension = (typeof allowedExtensions)[number];
export type FileSort = "name" | "updatedAt" | "size" | "manual";
export type SortOrder = "asc" | "desc";

export interface FileMetadata {
  title?: string;
  tags?: string[];
  sortOrder?: number;
}

export interface FileSummary {
  id: string;
  name: string;
  title?: string;
  folder: string;
  updatedAt: string;
  size: number;
}

export interface FileDetails extends FileSummary {
  content: string;
  tags?: string[];
  sortOrder?: number;
}

export interface CreateFileInput extends FileMetadata {
  folder: string;
  name: string;
  content: string;
}

export interface UpdateFileInput extends FileMetadata {
  content: string;
}

export interface ListFilesOptions {
  folder?: string;
  sort: FileSort;
  order: SortOrder;
}

export interface Task {
  file: string;
  line: number;
  project: string;
  text: string;
  done: boolean;
  due?: string;
  time?: string;
  priority: number;
}

export type TaskStatus = "open" | "done" | "all";
export type TaskDueFilter = "today" | "overdue";

export interface ListTasksOptions {
  project?: string;
  status: TaskStatus;
  due?: TaskDueFilter;
  today: string;
  limit: number;
}

export interface AddTaskInput {
  project: string;
  text: string;
  due?: string;
  time?: string;
  priority?: number;
}

export interface ProjectSummary {
  name: string;
  title: string;
  status?: string;
  goal?: string;
  next?: string;
  openTasks: number;
  overdueTasks: number;
  weekMinutes: number;
  updatedAt: string;
}

export interface SearchHit {
  id: string;
  line: number;
  snippet: string;
}

export interface FirmwareManifest {
  version: string;
  size: number;
  md5: string;
  publishedAt: string;
}
