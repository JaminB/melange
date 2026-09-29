// The few Node built-ins the unit tests use (no @types/node in the lock).
declare module "node:test" {
  export function test(name: string, fn: () => void | Promise<void>): void;
}
declare module "node:assert/strict" {
  const assert: {
    (value: unknown, message?: string): void;
    equal(a: unknown, b: unknown, message?: string): void;
    deepEqual(a: unknown, b: unknown, message?: string): void;
    ok(value: unknown, message?: string): void;
    rejects(p: Promise<unknown>, check?: (e: unknown) => boolean): Promise<void>;
    throws(fn: () => unknown, error?: (new (...args: never[]) => Error) | ((e: unknown) => boolean), message?: string): void;
  };
  export default assert;
}
declare module "node:fs" {
  export function readFileSync(path: string | URL, encoding: "utf8"): string;
}
