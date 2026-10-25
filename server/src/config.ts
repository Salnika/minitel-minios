import path from "node:path";

export interface AppConfig {
  apiToken: string;
  dataPath: string;
  port: number;
  maxFileSizeBytes: number;
  requestTimeoutMs: number;
  firmwarePath: string;
  firmwareMaxBytes: number;
}

function parsePositiveInteger(value: string | undefined, fallback: number, name: string): number {
  if (value === undefined) {
    return fallback;
  }

  const parsed = Number.parseInt(value, 10);
  if (!Number.isInteger(parsed) || parsed <= 0) {
    throw new Error(`${name} must be a positive integer`);
  }

  return parsed;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env, cwd = process.cwd()): AppConfig {
  const apiToken = env.API_TOKEN;
  if (!apiToken) {
    throw new Error("API_TOKEN is required");
  }

  const dataPath = path.resolve(env.DATA_PATH ?? path.join(cwd, "data/files"));

  return {
    apiToken,
    dataPath,
    port: parsePositiveInteger(env.PORT, 3000, "PORT"),
    maxFileSizeBytes: parsePositiveInteger(
      env.MAX_FILE_SIZE_BYTES,
      1_048_576,
      "MAX_FILE_SIZE_BYTES",
    ),
    requestTimeoutMs: parsePositiveInteger(env.REQUEST_TIMEOUT_MS, 10_000, "REQUEST_TIMEOUT_MS"),
    // Next to the files, on the same volume, but outside the text-file sandbox.
    firmwarePath: path.resolve(env.FIRMWARE_PATH ?? path.join(path.dirname(dataPath), "firmware")),
    firmwareMaxBytes: parsePositiveInteger(
      env.FIRMWARE_MAX_BYTES,
      4 * 1_048_576,
      "FIRMWARE_MAX_BYTES",
    ),
  };
}
