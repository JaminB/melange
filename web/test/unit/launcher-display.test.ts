import assert from "node:assert/strict";
import { test } from "node:test";
import type { DisplayState } from "../../src/launcher/api";
import { defaultWindowed, displayOf, displaySizeOptions, sizeFromKey, sizeKey } from "../../src/launcher/api";
import { DISPLAY_FS_REMOVED, DISPLAY_NO_MELANGE, displayFullscreenHelp, displayNote, hotkeyText, windowSizeHelp } from "../../src/launcher/copy";

const wire = {
  monitor: { w: 1920, h: 1080 },
  modes: [{ w: 1920, h: 1080 }, { w: 1600, h: 900 }, { w: 1280, h: 720 }, { w: 1024, h: 768 }],
  windowed: { w: 1280, h: 720 }, source: "local", localCfg: true, exclusive: false, fullscreen: false, enabled: true,
  hotkey: "Alt+RETURN", melangeIni: true, running: false,
};
const state = (extra: Partial<DisplayState> = {}): DisplayState => ({ ...displayOf(wire), ...extra });

test("displayOf: garbage never throws and falls back to safe defaults", () => {
  const d = displayOf(null);
  assert.deepEqual(d.monitor, { w: 0, h: 0 });
  assert.deepEqual(d.modes, []);
  assert.equal(d.windowed, null);
  assert.equal(d.source, "none");
  assert.equal(d.fullscreen, false);
  assert.equal(d.enabled, true, "a missing Enabled reads as on, like the ini default");
  assert.equal(d.hotkey, "Alt+RETURN");
  assert.equal(d.refused, undefined);
  assert.equal(d.removedFs, undefined);
});

test("displayOf: round-trips the server's state and drops bad sizes", () => {
  const d = displayOf({ ...wire, modes: [...wire.modes, { w: -1, h: 5 }, { w: 1.5, h: 900 }, "1920x1080", null], refused: "Close Worms Ultimate Mayhem first.", removedFs: true });
  assert.equal(d.modes.length, 4);
  assert.deepEqual(d.windowed, { w: 1280, h: 720 });
  assert.equal(d.source, "local");
  assert.equal(d.refused, "Close Worms Ultimate Mayhem first.");
  assert.equal(d.removedFs, true);
  assert.equal(displayOf({ ...wire, source: "elsewhere" }).source, "none");
  assert.equal(displayOf({ ...wire, enabled: false }).enabled, false);
});

test("size keys round-trip and refuse anything else", () => {
  assert.equal(sizeKey({ w: 2560, h: 1440 }), "2560x1440");
  assert.deepEqual(sizeFromKey("2560x1440"), { w: 2560, h: 1440 });
  assert.equal(sizeFromKey("2560×1440"), undefined);
  assert.equal(sizeFromKey("x"), undefined);
  assert.equal(sizeFromKey("1e3x720"), undefined);
});

test("displaySizeOptions: the current size joins the list when it isn't offered, largest first, monitor marked", () => {
  const opts = displaySizeOptions(state({ windowed: { w: 1440, h: 900 } }));
  assert.deepEqual(opts.map((o) => o.key), ["1920x1080", "1600x900", "1440x900", "1280x720", "1024x768"]);
  assert.deepEqual(opts.filter((o) => o.native).map((o) => o.key), ["1920x1080"]);
  assert.equal(displaySizeOptions(state()).length, 4, "no duplicate when the current size is offered");
});

test("defaultWindowed: local.cfg's size, else the largest 16:9 below the monitor", () => {
  assert.deepEqual(defaultWindowed(state()), { w: 1280, h: 720 });
  assert.deepEqual(defaultWindowed(state({ windowed: null })), { w: 1600, h: 900 });
  assert.deepEqual(defaultWindowed(state({ windowed: null, modes: [{ w: 1920, h: 1080 }, { w: 1024, h: 768 }] })), { w: 1024, h: 768 });
  assert.equal(defaultWindowed(state({ windowed: null, modes: [] })), undefined);
});

test("hotkeyText: Melange.ini key names as people say them", () => {
  assert.equal(hotkeyText("Alt+RETURN"), "Alt+Enter");
  assert.equal(hotkeyText("Ctrl + Shift + GRAVE"), "Ctrl+Shift+`");
  assert.equal(hotkeyText("Ctrl+F11"), "Ctrl+F11");
});

test("displayFullscreenHelp: names the monitor and how to switch in game", () => {
  const t = displayFullscreenHelp(state());
  assert.ok(/1920 × 1080/.test(t));
  assert.ok(/Alt\+Enter/.test(t));
  assert.ok(/Alt\+Tab/.test(t));
  assert.ok(!/none/.test(displayFullscreenHelp(state({ hotkey: "none" }))));
});

test("displayNote: /FS, its removal, a missing Melange and a switched-off module", () => {
  assert.equal(displayNote(state()), undefined);
  assert.ok(/\/FS in local\.cfg/.test(displayNote(state({ exclusive: true })) ?? ""));
  assert.equal(displayNote(state({ exclusive: true, fullscreen: true })), undefined, "no warning once borderless is chosen");
  assert.equal(displayNote(state({ removedFs: true, fullscreen: true })), DISPLAY_FS_REMOVED);
  assert.equal(displayNote(state({ melangeIni: false })), DISPLAY_NO_MELANGE);
  assert.ok(/Display module is off/.test(displayNote(state({ fullscreen: true, enabled: false })) ?? ""));
});

test("windowSizeHelp: what the size means with fullscreen on and off", () => {
  assert.ok(/opens at/.test(windowSizeHelp(state())));
  assert.ok(/leave fullscreen/.test(windowSizeHelp(state({ fullscreen: true }))));
});
