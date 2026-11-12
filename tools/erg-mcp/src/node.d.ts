// The few Node built-ins erg-mcp uses (no @types/node in the lock). The pinned Node (web/toolchain.lock.json) is 24, so
// WebSocket, fetch and structuredClone are the DOM lib's globals.
declare const process: {
  argv: string[];
  env: Record<string, string | undefined>;
  platform: string;
  pid: number;
  exitCode: number | undefined;
  stdin: { setEncoding(e: "utf8"): void; on(ev: "data", fn: (chunk: string) => void): void; on(ev: "end", fn: () => void): void };
  stdout: { write(s: string): boolean };
  stderr: { write(s: string): boolean };
  on(ev: "exit" | "SIGINT" | "SIGTERM", fn: () => void): void;
  exit(code?: number): never;
};
declare const Buffer: {
  from(data: ArrayBuffer | ArrayLike<number> | string, encoding?: "base64" | "utf8"): Uint8Array & { toString(encoding?: "base64" | "utf8"): string };
  concat(list: Uint8Array[]): Uint8Array;
};
declare module "node:fs" {
  export function readFileSync(path: string, encoding: "utf8"): string;
  export function readFileSync(path: string): Uint8Array;
  export function existsSync(path: string): boolean;
  export function appendFileSync(path: string, data: string): void;
  export function readdirSync(path: string): string[];
  export function statSync(path: string): { isDirectory(): boolean; mtimeMs: number; size: number };
  export function writeFileSync(path: string, data: string | Uint8Array): void;
  export function mkdirSync(path: string, opts?: { recursive?: boolean }): void;
  export function cpSync(from: string, to: string, opts?: { recursive?: boolean; force?: boolean; preserveTimestamps?: boolean }): void;
  export function rmSync(path: string, opts?: { recursive?: boolean; force?: boolean }): void;
  export function renameSync(from: string, to: string): void;
  export function mkdtempSync(prefix: string): string;
}
declare module "node:os" {
  export function tmpdir(): string;
}
declare module "node:zlib" {
  export function deflateSync(data: Uint8Array, opts?: { level?: number }): Uint8Array;
}
declare module "node:path" {
  export function join(...parts: string[]): string;
  export function dirname(p: string): string;
  export function resolve(...parts: string[]): string;
  export function basename(p: string): string;
}
declare module "node:crypto" {
  export interface Hash { update(data: string | Uint8Array): Hash; digest(enc: "hex"): string; }
  export function createHash(alg: "sha256"): Hash;
}
declare module "node:child_process" {
  export interface ChildProcess {
    pid?: number;
    exitCode: number | null;
    stdout: { setEncoding(e: "utf8"): void; on(ev: "data", fn: (chunk: string) => void): void } | null;
    stderr: { setEncoding(e: "utf8"): void; on(ev: "data", fn: (chunk: string) => void): void } | null;
    on(ev: "exit", fn: (code: number | null) => void): void;
    on(ev: "error", fn: (e: Error) => void): void;
    kill(): boolean;
  }
  export function spawn(cmd: string, args: string[], opts: { cwd?: string; stdio?: ("ignore" | "pipe")[]; windowsHide?: boolean }): ChildProcess;
  export function execFileSync(cmd: string, args: string[], opts: { encoding: "utf8"; windowsHide?: boolean; timeout?: number }): string;
}
