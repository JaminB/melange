import assert from "node:assert/strict";
import { test } from "node:test";
import {
  fingerprintShort, importEventOf, importJobOf, importMapOf, importMapsResultOf, importPreviewUrl, importerOf, importersOf,
  type ImportJob, type Importer,
} from "../../src/launcher/api";
import {
  disclosureCopy, importButtonLabel, importErrorText, importGateText, importProgressLine, importedHeaderLine, removeConfirmLine,
  reimportConfirmLine, resultCopy,
} from "../../src/launcher/copy";
import {
  bulkHideTargets, categoryOptions, countToHide, countToShow, DEFAULT_FILTERS, filterMaps, groupOptions, playBadge, sortedByTitle,
  timeOfDayLabel,
} from "../../src/launcher/import/model";

// -- api.ts: parsing is defensive, exactly like every other `*Of` in this file -------------------------------------

test("importerOf: garbage input never throws and falls back sanely", () => {
  assert.equal(importerOf(null), undefined);
  assert.equal(importerOf({}), undefined, "no plugin id");
  const i = importerOf({ plugin: "caravan", content: { title: "t" }, sources: [{ id: "mirror", fileName: "f.zip", size: 1, sha256: "a" }],
    status: "nonsense", job: { plugin: "caravan", phase: "nope" } });
  assert.equal(i?.name, "caravan", "name falls back to the plugin id");
  assert.equal(i?.status, "none", "an unknown status falls back to none");
  assert.equal(i?.sources[0].name, "mirror", "a source's name falls back to its id");
  assert.equal(i?.job?.phase, "idle", "an unknown phase falls back to idle");
  assert.equal(i?.gate, "");
});

test("importersOf: reads the {importers} shape and drops malformed entries", () => {
  const list = importersOf({ importers: [{ plugin: "caravan", content: {}, sources: [] }, { no: "id" }, null] });
  assert.equal(list.length, 1);
  assert.equal(list[0].plugin, "caravan");
  assert.deepEqual(importersOf(null), []);
});

test("importMapOf: unknown group/category/timeOfDay fall back, file is required", () => {
  assert.equal(importMapOf({}), undefined);
  const m = importMapOf({ file: "re_x", group: "nope", category: "nope", timeOfDay: "nope" });
  assert.equal(m?.group, "renewation");
  assert.equal(m?.category, "play");
  assert.equal(m?.timeOfDay, "DAY");
  assert.equal(m?.title, "re_x", "title falls back to the file name");
});

test("importJobOf and importEventOf: channel events carry either importers or a job, or both", () => {
  assert.equal(importJobOf({}), undefined, "a job needs a plugin");
  const j = importJobOf({ plugin: "caravan", phase: "building", step: 2, of: 5, result: { counts: { play: 1 }, packs: ["caravan-1"], fingerprint: "ab" } });
  assert.equal(j?.result?.fingerprint, "ab");
  assert.equal(j?.result?.counts.play, 1);

  const onlyJob = importEventOf({ job: { plugin: "caravan", phase: "downloading" } });
  assert.equal(onlyJob.importers, undefined);
  assert.equal(onlyJob.job?.phase, "downloading");
  const onlyImporters = importEventOf({ importers: [{ plugin: "caravan", content: {}, sources: [] }] });
  assert.equal(onlyImporters.job, undefined);
  assert.equal(onlyImporters.importers?.[0].plugin, "caravan");
});

test("importMapsResultOf, fingerprintShort, importPreviewUrl", () => {
  const r = importMapsResultOf({ maps: [{ file: "a" }], packs: [{ id: "caravan-1" }] });
  assert.equal(r.maps.length, 1);
  assert.equal(r.packs[0].id, "caravan-1");
  assert.equal(fingerprintShort("a1b2c3d4e5f6abcdef"), "a1b2c3d4e5f6");
  assert.equal(importPreviewUrl("caravan", "caravan_1_alpine"), "/import/previews/caravan/caravan_1_alpine.png");
});

// -- copy.ts: every piece of §12.2's text, parameterised by the RPC `Importer` -------------------------------------

function importer(partial: Partial<Importer> = {}): Importer {
  return {
    plugin: "caravan", name: "Caravan", recipe: "r", recipeVersion: "1.0.0", format: 1, supported: true,
    content: { title: "Fixture Mod", publisher: "fixture.example", termsUrl: "https://fixture.example/terms", credit: "Credit line." },
    sources: [{ id: "mirror", name: "fixture.example", host: "fixture.example", fileName: "Fixture.zip", size: 2 * 1048576, sha256: "a".repeat(64) }],
    expect: { maps: 12 }, status: "none", gate: "", ...partial,
  };
}

test("importButtonLabel: every status has a label", () => {
  assert.equal(importButtonLabel("none"), "Import maps");
  assert.equal(importButtonLabel("imported"), "Maps");
  assert.equal(importButtonLabel("stale"), "Re-import");
  assert.equal(importButtonLabel("damaged"), "Repair");
  assert.equal(importButtonLabel("unsupported"), "Needs a newer Melange");
});

test("disclosureCopy: names the plugin and content, and includes the source in the download bullet", () => {
  const imp = importer();
  const d = disclosureCopy(imp, imp.sources[0]);
  assert.ok(d.heading.includes("Fixture Mod"));
  assert.ok(d.intro.includes("Caravan"));
  assert.ok(d.intro.includes("fixture.example"));
  assert.equal(d.bullets.length, 5);
  assert.ok(d.bullets[0].includes("Fixture.zip"));
  assert.ok(d.bullets[0].includes("2.0 MiB"));
  assert.ok(d.bullets.at(-1)!.includes("Credit line."));
});

test("importGateText: known reasons get the exact §12.2 copy, others pass through", () => {
  assert.equal(importGateText("gameRunning"), "Close Worms Ultimate Mayhem to import maps.");
  assert.equal(importGateText("noGame"), "Choose your game folder on the Home page first.");
  assert.equal(importGateText("somethingElse"), "somethingElse");
});

function job(partial: Partial<ImportJob> = {}): ImportJob {
  return { plugin: "caravan", phase: "idle", bytes: 0, total: 0, step: 0, of: 0, ...partial };
}

test("importProgressLine: every building phase renders non-empty text", () => {
  for (const phase of ["downloading", "copying", "verifying", "reading", "building", "placing", "cancelled"] as const) {
    const line = importProgressLine(job({ phase, bytes: 512, total: 1024, step: 2, of: 5 }));
    assert.ok(line.length > 0, phase);
  }
  assert.ok(/512 B of 1 KiB/.test(importProgressLine(job({ phase: "downloading", bytes: 512, total: 1024 }))));
  assert.ok(/2 of 5/.test(importProgressLine(job({ phase: "building", step: 2, of: 5 }))));
});

test("importErrorText: every documented reason has non-empty, on-topic text", () => {
  const imp = importer();
  const reasons = ["hash", "network", "space", "zip", "recipe", "vanilla", "occupied", "write", "internal", undefined] as const;
  for (const reason of reasons) {
    const text = importErrorText(job({ phase: "error", reason, message: "disk full" }), imp);
    assert.ok(text.length > 0, String(reason));
  }
  assert.ok(importErrorText(job({ phase: "error", reason: "hash" }), imp).includes("fixture.example"));
  assert.ok(importErrorText(job({ phase: "error", reason: "network", message: "timed out" }), imp).includes("timed out"));
  assert.ok(importErrorText(job({ phase: "error", reason: "occupied", message: "caravan-2" }), imp).includes("caravan-2"));
});

test("resultCopy: the skipped line only appears when something was skipped", () => {
  const content = importer().content;
  const clean = resultCopy({ maps: 12, counts: { play: 8, dm: 2, mode: 2, skipped: 0 }, packs: ["caravan-1", "caravan-2"], bytes: 2345678, fingerprint: "a1b2c3d4e5f6abcd", skipped: 0 }, content);
  assert.equal(clean.heading, "12 maps imported");
  assert.ok(!clean.lines.some((l) => /weren't imported/.test(l)));
  assert.ok(clean.lines.some((l) => /a1b2c3d4e5f6/.test(l)));
  const dirty = resultCopy({ maps: 10, counts: { play: 8, dm: 2, mode: 0, skipped: 0 }, packs: ["caravan-1"], bytes: 1, fingerprint: "abc", skipped: 3 }, content);
  assert.ok(dirty.lines.some((l) => /3 entries in Fixture Mod weren't imported/.test(l)));
});

test("importedHeaderLine, reimportConfirmLine, removeConfirmLine", () => {
  const imp = importer({ imported: { recipeVersion: "1.0.0", fingerprint: "a1b2c3d4e5f6abcd", importedAt: "2026-10-01T12:00:00Z", maps: 12,
    counts: { play: 8, dm: 2, mode: 2, skipped: 0 }, packs: [], bytes: 1 } });
  assert.ok(importedHeaderLine(imp).includes("12 maps"));
  assert.ok(importedHeaderLine(imp).includes("a1b2c3d4e5f6"));
  assert.ok(reimportConfirmLine(imp.content).includes("Fixture Mod"));
  assert.equal(removeConfirmLine("Caravan", 1), "Remove all Caravan maps? This deletes 1 map pack from your Mods folder.");
  assert.equal(removeConfirmLine("Caravan", 2), "Remove all Caravan maps? This deletes 2 map packs from your Mods folder.");
});

// -- import/model.ts: the map browser's pure filtering ---------------------------------------------------------

const MAPS = [
  { file: "Alpine", stem: "alpine", pack: "caravan-1", title: "Alpine", author: "mapper1", group: "mmp" as const, groupLabel: "Mega Map Pack",
    category: "play" as const, categoryLabel: "Plays as designed", theme: "ARCTIC", timeOfDay: "DAY" as const, survivor: true, hidden: false, preview: true },
  { file: "re_Harbour", stem: "re_harbour", pack: "caravan-1", title: "Harbour", group: "renewation" as const, groupLabel: "Fixture Mod",
    category: "dm" as const, categoryLabel: "Deathmatch only", theme: "PIRATE", timeOfDay: "DAY" as const, survivor: true, hidden: false, preview: true },
  { file: "re_Jungle_PRO", stem: "re_jungle_pro", pack: "caravan-2", title: "Jungle", mode: "Pro", group: "renewation" as const, groupLabel: "Fixture Mod",
    category: "mode" as const, categoryLabel: "Mode not supported", theme: "PREHISTORIC", timeOfDay: "NIGHT" as const, survivor: true, hidden: true, preview: false },
];

test("filterMaps: query, group, category and shown all narrow the set", () => {
  assert.equal(filterMaps(MAPS, DEFAULT_FILTERS).length, 3);
  assert.equal(filterMaps(MAPS, { ...DEFAULT_FILTERS, query: "harbour" }).length, 1);
  assert.equal(filterMaps(MAPS, { ...DEFAULT_FILTERS, query: "mapper1" }).length, 1, "author matches too");
  assert.equal(filterMaps(MAPS, { ...DEFAULT_FILTERS, group: "renewation" }).length, 2);
  assert.equal(filterMaps(MAPS, { ...DEFAULT_FILTERS, category: "mode" }).length, 1);
  assert.equal(filterMaps(MAPS, { ...DEFAULT_FILTERS, shown: "hidden" }).length, 1);
  assert.equal(filterMaps(MAPS, { ...DEFAULT_FILTERS, shown: "shown" }).length, 2);
});

test("groupOptions and categoryOptions: first-seen order, deduplicated", () => {
  assert.deepEqual(groupOptions(MAPS), [{ value: "mmp", label: "Mega Map Pack" }, { value: "renewation", label: "Fixture Mod" }]);
  assert.deepEqual(categoryOptions(MAPS), [
    { value: "play", label: "Plays as designed" }, { value: "dm", label: "Deathmatch only" }, { value: "mode", label: "Mode not supported" },
  ]);
});

test("bulkHideTargets, countToShow/countToHide: act on file name, not stem", () => {
  assert.deepEqual(bulkHideTargets(MAPS, true).sort(), ["Alpine", "re_Harbour"]);
  assert.deepEqual(bulkHideTargets(MAPS, false), ["re_Jungle_PRO"]);
  assert.equal(countToShow(MAPS), 1);
  assert.equal(countToHide(MAPS), 2);
});

test("sortedByTitle, timeOfDayLabel, playBadge", () => {
  assert.deepEqual(sortedByTitle(MAPS).map((m) => m.title), ["Alpine", "Harbour", "Jungle"]);
  assert.equal(timeOfDayLabel("DAY"), "Day");
  assert.equal(timeOfDayLabel("EVENING"), "Evening");
  assert.equal(timeOfDayLabel("NIGHT"), "Night");
  assert.equal(playBadge(MAPS[0]), undefined, "a play-as-designed map gets no badge");
  assert.equal(playBadge(MAPS[1]), "Deathmatch only");
  assert.equal(playBadge(MAPS[2]), "Pro: mode not supported");
});
