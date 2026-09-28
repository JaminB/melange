// One multiplexed connection to the Oasis server, reconnecting with backoff 0.5 s -> 5 s.
import { CloseCode, ErrorCode, PROTOCOL, type ClientMessage, type ServerMessage, type Welcome } from "./protocol";

export type { Welcome, PanelInfo } from "./protocol";
export type ClientState = "connecting" | "open" | "closed" | "offline";

export interface Client {
  readonly state: ClientState;
  welcome(): Welcome | undefined;
  subscribe<T>(channel: string, filter: object | undefined, fn: (msg: T, seq: number) => void,
               onDrop?: (n: number) => void): () => void;
  call<T>(method: string, params?: object, timeoutMs?: number): Promise<T>;  // rejects with RpcError
  has(methodOrChannel: string): boolean;                                     // feature detection
  onState(fn: (s: Client["state"]) => void): () => void;
}

export class RpcError extends Error {
  constructor(readonly code: number, message: string, readonly data?: unknown) {
    super(message);
    this.name = "RpcError";
  }
}

interface Sub { channel: string; filter?: object; fn: (msg: unknown, seq: number) => void; onDrop?: (n: number) => void; }
interface Pending { resolve: (v: unknown) => void; reject: (e: RpcError) => void; timer: ReturnType<typeof setTimeout>; }

type SocketLike = Pick<WebSocket, "send" | "close" | "readyState"> & {
  onopen: ((ev: Event) => void) | null;
  onclose: ((ev: CloseEvent) => void) | null;
  onmessage: ((ev: MessageEvent) => void) | null;
  onerror: ((ev: Event) => void) | null;
};

export interface ClientOptions {
  url?: string;                                  // default: ws(s)://<page host>/ws
  build?: string;                                // the page's build id, sent in hello
  socket?: (url: string) => SocketLike;          // tests inject a fake WebSocket
  onStale?: (serverBuild: string) => void;       // the server runs a different build than this page
  minBackoffMs?: number;
  maxBackoffMs?: number;
}

export interface OasisClient extends Client {
  close(): void;
  lastClose(): { code: number; reason: string } | undefined;
}

export function createClient(opts: ClientOptions = {}): OasisClient {
  const url = opts.url ?? `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`;
  const build = opts.build ?? "dev";
  const open = opts.socket ?? ((u: string) => new WebSocket(u) as unknown as SocketLike);
  const minBackoff = opts.minBackoffMs ?? 500, maxBackoff = opts.maxBackoffMs ?? 5000;

  let state: ClientState = "connecting";
  let ws: SocketLike | undefined;
  let welcome: Welcome | undefined;
  let backoff = minBackoff;
  let retry: ReturnType<typeof setTimeout> | undefined;
  let stopped = false;
  let nextId = 1;
  let last: { code: number; reason: string } | undefined;
  const subs = new Set<Sub>();
  const pending = new Map<number, Pending>();
  const listeners = new Set<(s: ClientState) => void>();

  const setState = (s: ClientState) => {
    if (s === state) return;
    state = s;
    for (const fn of [...listeners]) fn(s);
  };
  const send = (m: ClientMessage) => {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(m));
  };
  const channels = () => new Map([...subs].map((s) => [s.channel, s.filter] as const));

  const failAll = (code: number, msg: string) => {
    for (const [id, p] of pending) {
      clearTimeout(p.timer);
      p.reject(new RpcError(code, msg));
      pending.delete(id);
    }
  };

  const handle = (m: ServerMessage) => {
    switch (m.t) {
      case "welcome": {
        const w: Welcome = { proto: m.proto, build: m.build, server: m.server, game: m.game, channels: m.channels ?? [],
          methods: m.methods ?? [], panels: m.panels ?? [], limits: m.limits };
        welcome = w;
        backoff = minBackoff;
        if (w.build !== build && build !== "dev" && opts.onStale) opts.onStale(w.build);
        for (const [ch, filter] of channels()) send({ t: "sub", ch, filter });
        setState("open");
        break;
      }
      case "bye":
        last = { code: CloseCode.Protocol, reason: m.reason };
        break;
      case "ev": {
        const deliver = (v: unknown, seq: number) => {
          for (const s of [...subs]) if (s.channel === m.ch) s.fn(v, seq);
        };
        if (m.b) m.b.forEach((v, i) => deliver(v, m.seq + i));
        else deliver(m.d, m.seq);
        break;
      }
      case "drop":
        for (const s of [...subs]) if (s.channel === m.ch) s.onDrop?.(m.n);
        break;
      case "res":
      case "err": {
        if (m.id === null) break;
        const p = pending.get(m.id);
        if (!p) break;
        pending.delete(m.id);
        clearTimeout(p.timer);
        if (m.t === "res") p.resolve(m.r);
        else p.reject(new RpcError(m.code, m.msg, m.data));
        break;
      }
    }
  };

  const connect = () => {
    retry = undefined;
    if (stopped) return;
    let sock: SocketLike;
    try {
      sock = open(url);
    } catch {
      schedule();
      return;
    }
    ws = sock;
    sock.onopen = () => send({ t: "hello", proto: PROTOCOL, build, client: "oasis-web" });
    sock.onmessage = (ev) => {
      let m: ServerMessage;
      try {
        m = JSON.parse(typeof ev.data === "string" ? ev.data : "");
      } catch {
        return;
      }
      if (m && typeof m === "object" && typeof m.t === "string") handle(m);
    };
    sock.onerror = () => {};
    sock.onclose = (ev) => {
      if (ws !== sock) return;
      ws = undefined;
      last = { code: ev.code, reason: ev.reason };
      failAll(ErrorCode.Disconnected, "disconnected");
      if (stopped || ev.code === CloseCode.Protocol || ev.code === CloseCode.Auth) {
        setState("offline");
        return;
      }
      setState("closed");
      schedule();
    };
  };

  const schedule = () => {
    if (stopped || retry) return;
    retry = setTimeout(connect, backoff);
    backoff = Math.min(maxBackoff, backoff * 2);
  };

  connect();

  return {
    get state() { return state; },
    welcome: () => welcome,
    lastClose: () => last,
    has: (name) => !!welcome && (welcome.methods.includes(name) || welcome.channels.includes(name)),
    onState(fn) {
      listeners.add(fn);
      return () => listeners.delete(fn);
    },
    subscribe<T>(channel: string, filter: object | undefined, fn: (msg: T, seq: number) => void, onDrop?: (n: number) => void) {
      const s: Sub = { channel, filter, fn: fn as Sub["fn"], onDrop };
      const already = [...subs].some((x) => x.channel === channel);
      subs.add(s);
      if (state === "open" && (!already || filter)) send({ t: "sub", ch: channel, filter });
      return () => {
        if (!subs.delete(s)) return;
        if (state === "open" && ![...subs].some((x) => x.channel === channel)) send({ t: "unsub", ch: channel });
      };
    },
    call<T>(method: string, params: object = {}, timeoutMs = 10000) {
      return new Promise<T>((resolve, reject) => {
        if (state !== "open") {
          reject(new RpcError(ErrorCode.Disconnected, "not connected"));
          return;
        }
        const id = nextId++;
        const timer = setTimeout(() => {
          pending.delete(id);
          reject(new RpcError(ErrorCode.Timeout, `${method}: no answer within ${timeoutMs} ms`));
        }, timeoutMs);
        pending.set(id, { resolve: resolve as (v: unknown) => void, reject, timer });
        send({ t: "call", id, m: method, p: params });
      });
    },
    close() {
      stopped = true;
      if (retry) clearTimeout(retry);
      failAll(ErrorCode.Disconnected, "closed");
      const s = ws;
      ws = undefined;
      s?.close(1000, "closed");
      setState("offline");
    },
  };
}
