import assert from "node:assert/strict";
import { test } from "node:test";
import type { DllInfo, GameCheck, MelangeState, SetupStatus, Verdict } from "../../src/launcher/api";
import { LauncherErrorCode } from "../../src/launcher/api";
import { checkCopy, errorCopy, findCopy, loaderChecklistLine, loaderCopy, melangeCopy, planLine } from "../../src/launcher/copy";

const VERDICTS: Verdict[] = ["ok", "wrongBuild", "noExe", "notFound", "unreadable"];
const check = (verdict: Verdict, extra: Partial<GameCheck> = {}): GameCheck =>
  ({ path: "C:\\Games\\WUM", verdict, store: "steam", running: false, writable: true, ...extra });

test("checkCopy: every verdict has a non-empty title and body", () => {
  for (const v of VERDICTS) {
    const c = checkCopy(check(v, v === "unreadable" ? { error: "Access is denied." } : {}));
    assert.ok(c.title.length > 0, v);
    assert.ok(c.body.length > 0, v);
  }
});

test("checkCopy: wrongBuild names the build and shows the expected hash", () => {
  const c = checkCopy(check("wrongBuild", { exe: { size: 123, timestamp: 1, sha256: "abcdef0123456789" } }));
  assert.ok(/#1077/.test(c.body.join(" ")));
  assert.ok(/041c8c/.test(c.body.join(" ")));
});

test("findCopy: none, one and several candidates", () => {
  assert.ok(/couldn't find/.test(findCopy([]).title));
  assert.ok(/found your game/.test(findCopy([{ path: "a", source: "steam", check: check("ok", { path: "a" }) }]).title));
  const several = findCopy([
    { path: "a", source: "steam", check: check("ok", { path: "a" }) },
    { path: "b", source: "manual", check: check("ok", { path: "b" }) },
  ]);
  assert.ok(/more than one/.test(several.title));
});

function dll(kind: DllInfo["kind"], extra: Partial<DllInfo> = {}): DllInfo {
  return { file: "dinput8.dll", sha256: "x".repeat(64), size: 139264, kind, known: true, ours: false, ...extra };
}

test("loaderCopy: none with no other loader asks to add UAL", () => {
  const c = loaderCopy({ state: "none" }, []);
  assert.ok(c.title && c.body.length);
  assert.ok(/Ultimate ASI Loader/.test(c.body[0]));
});

test("loaderCopy: none with a UAL under another name reuses it", () => {
  const c = loaderCopy({ state: "none" }, [dll("ual", { file: "version.dll" })]);
  assert.ok(/version\.dll/.test(c.body[0]));
});

test("loaderCopy: ual at dinput8.dll is reused", () => {
  const c = loaderCopy({ state: "ual", dll: dll("ual", { version: "9.7.4" }) }, []);
  assert.ok(/already installed/.test(c.body[0]));
});

test("loaderCopy: every other dll kind has a non-empty title and body", () => {
  for (const kind of ["microsoft", "reshade", "specialk", "other"] as DllInfo["kind"][]) {
    const c = loaderCopy({ state: "other", dll: dll(kind, { description: "Something 1.0" }) }, []);
    assert.ok(c.title.length > 0, kind);
    assert.ok(c.body.length > 0, kind);
  }
});

function status(partial: Partial<SetupStatus["melange"]> = {}): SetupStatus {
  return {
    game: null, running: false, melangeLoaded: false, loader: { state: "none" }, otherLoaders: [],
    melange: { state: "missing", duplicates: [], ...partial }, ini: { present: false, missingKeys: 0 }, legacy: [],
    payload: { ok: true, version: "0.4.0", missing: [], fromGameFolder: false }, backups: [],
  };
}

test("loaderChecklistLine: stays short and reflects the replace choice", () => {
  assert.ok(/Add Ultimate ASI Loader/.test(loaderChecklistLine({ state: "none" }, [], false)));
  assert.ok(/version\.dll/.test(loaderChecklistLine({ state: "none" }, [dll("ual", { file: "version.dll" })], false)));
  assert.ok(/already installed/.test(loaderChecklistLine({ state: "ual", dll: dll("ual", { version: "9.7.4" }) }, [], false)));
  const other = { state: "other" as const, dll: dll("reshade", { description: "ReShade 6.3.0" }) };
  assert.ok(/Choose what to do/.test(loaderChecklistLine(other, [], false)));
  assert.ok(/Replace ReShade 6\.3\.0/.test(loaderChecklistLine(other, [], true)));
});

test("melangeCopy: every state has a non-empty title", () => {
  const states: MelangeState[] = ["missing", "installed", "disabled", "older", "newer", "damaged"];
  for (const s of states) {
    const c = melangeCopy(status({ state: s, version: "0.3.1" }), "0.4.0");
    assert.ok(c.title.length > 0, s);
  }
});

test("errorCopy: every documented code has a non-empty title and body", () => {
  for (const code of Object.values(LauncherErrorCode)) {
    const c = errorCopy(code, { message: "detail", why: "detail", missing: ["melange.asi"], path: "dinput8.dll" });
    assert.ok(c.title.length > 0, String(code));
    assert.ok(c.body.length > 0, String(code));
  }
  const unknown = errorCopy(-99999);
  assert.ok(unknown.title.length > 0);
});

test("planLine: every op renders a readable verb", () => {
  for (const op of ["add", "replace", "remove", "backup", "merge", "keep"] as const) {
    const line = planLine({ op, path: "melange.asi", detail: "0.4.0" });
    assert.ok(/melange\.asi/.test(line));
    assert.ok(line.length > "melange.asi".length);
  }
});
