// Oasis wire protocol v1: one JSON object per WebSocket text message, discriminated by `t`.
export const PROTOCOL = 1;

export interface PanelInfo { id: string; title: string; url: string; }

export interface Welcome {
  proto: number;
  build: string;
  server: "game" | "standalone";
  game?: { exeBuild: number; melange: string };
  channels: string[];
  methods: string[];
  panels: PanelInfo[];
  limits?: { maxClients: number; maxMessageKB: number; maxQueueKB: number; maxQueuedCalls: number; readOnly: boolean };
}

export type ServerMessage =
  | ({ t: "welcome" } & Welcome)
  | { t: "bye"; reason: string; want?: number }
  | { t: "ev"; ch: string; seq: number; d?: unknown; b?: unknown[] }
  | { t: "drop"; ch: string; n: number; why: "queue" | "rate" }
  | { t: "res"; id: number; r: unknown }
  | { t: "err"; id: number | null; code: number; msg: string; data?: unknown }
  | { t: "bin"; ref: number; ch: string; len: number; meta: unknown };

export type ClientMessage =
  | { t: "hello"; proto: number; build: string; client: string }
  | { t: "sub"; ch: string; filter?: object; id?: number }
  | { t: "unsub"; ch: string; id?: number }
  | { t: "call"; id: number; m: string; p?: object };

export const ErrorCode = {
  Envelope: -32600,
  UnknownMethod: -32601,
  BadParams: -32602,
  Refused: -32000,
  NotInMatch: -32001,
  Busy: -32002,
  ReadOnly: -32003,
  Faulted: -32004,
  Timeout: -1,
  Disconnected: -2,
} as const;

export const CloseCode = { Kicked: 4000, Protocol: 4001, NoHello: 4002, Auth: 4003, Queue: 4008, TooMany: 4029 } as const;
