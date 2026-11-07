// node:zlib, for the PNG encoder (src/node.d.ts declares the other built-ins).
declare module "node:zlib" {
  export function deflateSync(data: Uint8Array, opts?: { level?: number }): Uint8Array;
}
