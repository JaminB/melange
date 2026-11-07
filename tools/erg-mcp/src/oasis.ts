// The Oasis connection: where the server is, and one open SDK client to it. The client is the web app's own
// (web/src/sdk/client.ts) over Node's WebSocket, which sends no Origin header, so the server checks the launch token
// (?k=) instead (src/oasis/auth.cpp CheckUpgrade).
//   1. connect {url}, or --url <url> on the command line
//   2. ERG_MCP_URL
//   3. Documents\Melange\oasis_url.txt (the game writes it while its Oasis runs)
//   4. Melange.exe --serve --game <dir>, started here: connect {gameDir}, or MELANGE_GAME_DIR (MELANGE_EXE overrides
//      <dir>\Melange.exe); the server lives as long as erg-mcp
import { spawn, type ChildProcess } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { createClient, type OasisClient } from "./core";

export interface ConnectOptions { url?: string; gameDir?: string; exe?: string; }
export interface Connection { client: OasisClient; url: string; via: string; server: string; methods: string[]; }

const OPEN_TIMEOUT = 10000;
const SPAWN_TIMEOUT = 30000;

/** A launch URL (http://127.0.0.1:<port>/?k=<token>) or a ws:// URL as the socket URL ws://<host>/ws?k=<token>. */
export function socketUrl(u: string): string {
  const url = new URL(u.trim());
  const k = url.searchParams.get("k");
  if (!k) throw new Error(`${u}: the URL carries no ?k= launch token`);
  if (url.protocol !== "http:" && url.protocol !== "ws:") throw new Error(`${u}: expected an http:// launch URL or a ws:// URL`);
  return `ws://${url.host}/ws?k=${encodeURIComponent(k)}`;
}

export function documentsDir(): string | null {
  for (const base of [process.env.USERPROFILE, process.env.OneDrive]) {
    if (!base) continue;
    const d = join(base, "Documents");
    if (existsSync(join(d, "Melange"))) return d;
  }
  return null;
}

export function urlFile(): string | null {
  const d = documentsDir();
  const f = d && join(d, "Melange", "oasis_url.txt");
  return f && existsSync(f) ? f : null;
}

function argUrl(): string | undefined {
  const i = process.argv.indexOf("--url");
  return i > 0 ? process.argv[i + 1] : undefined;
}

let served: { proc: ChildProcess; url: string; gameDir: string } | undefined;
process.on("exit", () => served?.proc.kill());

/** Starts Melange.exe --serve for a game folder (once; a later call for the same folder reuses it). */
export function serve(gameDir: string, exe?: string): Promise<string> {
  if (served && served.proc.exitCode === null && served.gameDir === gameDir) return Promise.resolve(served.url);
  served?.proc.kill();
  served = undefined;
  const bin = exe || process.env.MELANGE_EXE || join(gameDir, "Melange.exe");
  if (!existsSync(bin)) return Promise.reject(new Error(`${bin} does not exist`));
  return new Promise((resolve, reject) => {
    const proc = spawn(bin, ["--serve", "--game", gameDir], { cwd: gameDir, stdio: ["ignore", "pipe", "pipe"], windowsHide: true });
    let out = "";
    const timer = setTimeout(() => {
      proc.kill();
      reject(new Error(`${bin} --serve printed no URL within ${SPAWN_TIMEOUT / 1000} s`));
    }, SPAWN_TIMEOUT);
    proc.on("error", (e) => {
      clearTimeout(timer);
      reject(e);
    });
    proc.on("exit", (code) => {
      clearTimeout(timer);
      if (served?.proc === proc) served = undefined;
      reject(new Error(`${bin} --serve exited (${code}) before it listened${out ? `: ${out.trim().slice(-300)}` : ""}`));
    });
    proc.stderr?.setEncoding("utf8");
    proc.stderr?.on("data", (s) => process.stderr.write(`[melange] ${s}`));
    proc.stdout?.setEncoding("utf8");
    proc.stdout?.on("data", (s) => {
      out += s;
      const m = /melange: listening, (\S+)/.exec(out);
      if (!m) return;
      clearTimeout(timer);
      served = { proc, url: m[1], gameDir };
      resolve(m[1]);
    });
  });
}

/** The server's PID when erg-mcp started it, for the status of connect. */
export const servedPid = () => served?.proc.pid;

/** Opens the SDK client and waits for the welcome. Rejects (and closes) when the server refuses or does not answer. */
export function open(url: string, via: string): Promise<Connection> {
  const ws = socketUrl(url);
  const client = createClient({ url: ws, build: "dev", minBackoffMs: 500, maxBackoffMs: 5000 });
  return new Promise((resolve, reject) => {
    const done = (err?: Error) => {
      clearTimeout(timer);
      off();
      if (err) {
        client.close();
        return reject(err);
      }
      const w = client.welcome()!;
      resolve({ client, url, via, server: w.server, methods: w.methods });
    };
    const timer = setTimeout(() => done(new Error(`no welcome from ${ws.replace(/k=[^&]+/, "k=...")} within ${OPEN_TIMEOUT / 1000} s`)), OPEN_TIMEOUT);
    const off = client.onState((s) => {
      if (s === "open") done();
      // The first attempt failed: nothing listens there, or the upgrade was refused (a wrong or old token).
      else if (s === "closed" || s === "offline")
        done(new Error(`could not connect to ${ws.replace(/k=[^&]+/, "k=...")}: nothing listens there or the token is wrong (${client.lastClose()?.code ?? "?"})`));
    });
    if (client.state === "open") done();
  });
}

/** Connects by the first source that is set (see the top of this file); explicit options try only themselves. */
export async function connect(o: ConnectOptions = {}): Promise<Connection> {
  if (o.url) return open(o.url, "url");
  if (o.gameDir) return open(await serve(o.gameDir, o.exe), `Melange.exe --serve (${o.gameDir})`);
  const tried: string[] = [];
  const cli = argUrl();
  if (cli) return open(cli, "--url");
  if (process.env.ERG_MCP_URL) return open(process.env.ERG_MCP_URL, "ERG_MCP_URL");
  const file = urlFile();
  if (file) {
    try {
      return await open(readFileSync(file, "utf8"), file);
    } catch (e) {
      tried.push(`${file}: ${(e as Error).message}`);   // left behind by a game that has closed
    }
  }
  const game = process.env.MELANGE_GAME_DIR;
  if (game) return open(await serve(game, o.exe), `Melange.exe --serve (${game})`);
  throw new Error(["no Oasis server: pass connect {url} or {gameDir}, or set ERG_MCP_URL or MELANGE_GAME_DIR", ...tried].join("; "));
}
