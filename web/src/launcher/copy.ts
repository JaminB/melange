// The wizard's words (spec §5.3) as pure data: one function per family of states, each returning {title, body}.
// A unit test asserts every member of every union this file switches on produces non-empty title and body.
import type { Candidate, DisplaySize, DisplayState, DllInfo, GameCheck, LaaState, ImportContent, ImportJob, ImportSource, Importer, Plan, SetupProgress, SetupStatus, Store, UpdateApplied, UpdateStatus, VanillaGroup, Verdict } from "./api";
import { LauncherErrorCode, fingerprintShort, hashShort, sizeText, whenText } from "./api";

export interface Copy { title: string; body: string[]; }

export const WELCOME: Copy = {
  title: "Welcome to Melange",
  body: [
    "Melange adds fixes, mods and a plugin store to Worms Ultimate Mayhem. This will take about a minute: " +
    "we'll find your game, check it's the right version, install Melange and suggest a few plugins.",
    "Nothing is changed until you say so, and everything can be undone.",
  ],
};

export function findCopy(candidates: Candidate[]): Copy {
  const ok = candidates.filter((c) => c.check.verdict === "ok");
  if (ok.length > 1) return { title: "We found more than one copy", body: ["Pick the one you play."] };
  if (candidates.length) return { title: "We found your game", body: [] };
  return {
    title: "We couldn't find Worms Ultimate Mayhem",
    body: ["We looked in your Steam libraries and GOG. If it's installed somewhere else, choose the folder that contains WormsMayhem.exe."],
  };
}

export const CHILD_HINT = (folder: string): Copy => ({
  title: "That folder contains the game",
  body: [`That folder contains the game in ${folder}. Use that one?`],
});

const BUILD_SIZE = 5713408;
const BUILD_HASH = "041c8c6eb3b9f4fbaf367748f713ccb8f7bef68d13e825472c88c1ecf711ab7d";

export function checkCopy(check: GameCheck): Copy {
  switch (check.verdict) {
    case "ok":
      return { title: "Your game is ready for Melange", body: [`Worms Ultimate Mayhem, ${check.exe?.build || "Steam/GOG build #1077"}.`] };
    case "wrongBuild": {
      const expected = `Expected: size ${BUILD_SIZE.toLocaleString()} bytes, SHA-256 ${hashShort(BUILD_HASH)}`;
      // The found file's SHA-256 is only computed when its size and timestamp already matched a known build (no
      // point hashing an obviously different file); when it wasn't, the size alone still tells the user something.
      const found = check.exe ? `Found: size ${check.exe.size.toLocaleString()} bytes${check.exe.sha256 ? `, SHA-256 ${hashShort(check.exe.sha256)}` : ""}` : "";
      return {
        title: "This version of the game isn't supported",
        body: [
          "Melange only works with the Steam/GOG release, build #1077, and this WormsMayhem.exe is different. " +
          "Installing Melange on it could crash the game, so we won't.",
          "This usually happens when the game has been patched or modified, or when a beta branch is selected.",
          "On Steam: right-click the game › Properties › Installed Files › Verify integrity of game files, and make sure Betas is set to None. Then try again.",
          found ? `${expected}\n${found}` : expected,
        ],
      };
    }
    case "noExe":
      return { title: "WormsMayhem.exe isn't in this folder", body: ["Choose the folder that contains it."] };
    case "notFound":
      return { title: "That folder no longer exists", body: ["It may have been moved or deleted."] };
    case "unreadable":
      return { title: "We couldn't read WormsMayhem.exe", body: [`Windows said: ${check.error || "access was denied"}. Check that the file isn't blocked by antivirus, then try again.`] };
  }
}

export const CHECK_ACTION: Record<Verdict, { primary: string; secondary?: string }> = {
  ok: { primary: "Continue" },
  wrongBuild: { primary: "Check again", secondary: "Choose a different folder" },
  noExe: { primary: "Choose folder…" },
  notFound: { primary: "Find again" },
  unreadable: { primary: "Check again" },
};

export function loaderCopy(loader: SetupStatus["loader"], otherLoaders: DllInfo[]): Copy {
  if (loader.state === "none" && !otherLoaders.length)
    return { title: "Add Ultimate ASI Loader", body: ["Add Ultimate ASI Loader (dinput8.dll). It lets the game load Melange."] };
  if (loader.state === "none" && otherLoaders.length) {
    const other = otherLoaders[0];
    return { title: "Reuse the existing loader", body: [`An ASI loader is already installed as ${other.file}. We'll use it.`] };
  }
  if (loader.state === "ual")
    return { title: "Reuse Ultimate ASI Loader", body: [`Ultimate ASI Loader is already installed (version ${loader.dll?.version || "unknown"}). We'll use it.`] };
  const dll = loader.dll;
  if (dll?.kind === "microsoft")
    return { title: "Another program's dinput8.dll is in your game folder", body: ["It's a copy of Windows' own DirectInput library, which the game doesn't need there."] };
  if (dll?.kind === "reshade" || dll?.kind === "specialk")
    return {
      title: "Another program's dinput8.dll is in your game folder",
      body: [`It belongs to ${dll.description || dll.product || (dll.kind === "reshade" ? "ReShade" : "Special K")}. Melange needs Ultimate ASI Loader in its place.`,
        "We'll keep a copy of the current file so you can put it back with one click from Melange's Home page."],
    };
  return {
    title: "Another program's dinput8.dll is in your game folder",
    body: [dll ? `We can't tell what it is (no product information, ${sizeText(dll.size)}).` : "We can't tell what it is."],
  };
}

// The loader's own line in the Install step's checklist — shorter than loaderCopy's explanation, which stays in the
// warning box when a choice is needed.
export function loaderChecklistLine(loader: SetupStatus["loader"], otherLoaders: DllInfo[], replaceLoader: boolean): string {
  if (loader.state === "none" && !otherLoaders.length) return "Add Ultimate ASI Loader (dinput8.dll). It lets the game load Melange.";
  if (loader.state === "none") return `An ASI loader is already installed as ${otherLoaders[0].file}. We'll use it.`;
  if (loader.state === "ual") return `Ultimate ASI Loader is already installed (version ${loader.dll?.version || "unknown"}). We'll use it.`;
  const name = loader.dll?.description || loader.dll?.product || "the existing program";
  return replaceLoader ? `Replace ${name}'s dinput8.dll with Ultimate ASI Loader (a backup is kept).` : `Choose what to do with ${name}'s dinput8.dll below.`;
}

export function melangeCopy(status: SetupStatus, targetVersion: string): Copy {
  const m = status.melange;
  if (m.state === "missing") return { title: `Add Melange ${targetVersion}`, body: [] };
  if (m.state === "older") return { title: `Update Melange ${m.version} to ${targetVersion}`, body: [] };
  if (m.state === "newer") return { title: `Melange ${m.version} is already installed, newer than this one`, body: ["Keep it."] };
  if (m.state === "damaged") return { title: `Melange ${targetVersion} needs repairing`, body: ["Some files are missing or do not match. We'll repair it."] };
  if (m.state === "disabled") return { title: `Melange ${m.version} is disabled`, body: ["We'll re-enable it."] };
  return { title: `Melange ${targetVersion} is already installed`, body: ["We'll repair it."] };
}

// The calm, non-error line shown (and used as a tooltip) while a batch such as the recommended-plugins install
// holds the setup lock, in place of letting setup.*/plugins.setSettings fail with -32002.
export function busyNotice(busy: SetupProgress): string {
  const progress = busy.of > 0 ? ` (${busy.step} of ${busy.of})` : "";
  return `${busy.label || "Installing plugins — this finishes in a moment."}${progress}`;
}

export function errorCopy(code: number, data?: unknown): Copy {
  const d = (data && typeof data === "object" ? data : {}) as Record<string, unknown>;
  switch (code) {
    case LauncherErrorCode.Refused:
      if (typeof d.message === "string") return { title: "Close Worms Ultimate Mayhem first", body: [d.message] };
      return { title: "Close Worms Ultimate Mayhem first", body: ["Melange can't change game files while it's running."] };
    case LauncherErrorCode.Busy:
      return { title: "Another change is already running", body: ["Wait for it to finish, then try again."] };
    case LauncherErrorCode.BadParams:
      return { title: "That setting isn't valid", body: [typeof d.why === "string" ? d.why : "Check the value and try again."] };
    case LauncherErrorCode.AccessDenied:
      return { title: "Windows didn't let us write to the game folder", body: ["You can restart Melange as administrator to finish."] };
    case LauncherErrorCode.PayloadMissing:
      return {
        title: "Some Melange files are missing",
        body: ["Extract the whole zip you downloaded and run Melange.exe from that folder.",
          Array.isArray(d.missing) ? `Missing: ${(d.missing as string[]).join(", ")}` : ""].filter(Boolean),
      };
    case LauncherErrorCode.Failed:
      return {
        title: "Installation didn't complete, and nothing was changed",
        body: [`${typeof d.path === "string" ? `${d.path}: ` : ""}${typeof d.message === "string" ? d.message : "Windows reported an error."}`],
      };
    case LauncherErrorCode.PlanChanged:
      return { title: "What will change has changed", body: ["Something changed since you last checked. Review the plan again."] };
    default:
      return { title: "Something went wrong", body: ["Try again, or check the logs on the Help page."] };
  }
}

export function planLine(step: Plan["steps"][number]): string {
  const verb = { add: "Add", replace: "Replace", remove: "Remove", backup: "Back up", merge: "Merge", keep: "Keep" }[step.op];
  return `${verb} ${step.path}${step.detail ? ` — ${step.detail}` : ""}`;
}

export const READY = (path: string, line: string): Copy => ({ title: "You're all set", body: [`Melange is installed in ${path}.${line ? ` ${line}` : ""}`] });

export const RECOMMENDED_NOTE = "These become your defaults. Change them in Settings › Defaults.";
export const OFFLINE_STORE: Copy = {
  title: "We can't reach the plugin store right now",
  body: ["You can install plugins later from the Store page."],
};

// -- Local content importer (spec §12.2); `{}` values come from the recipe, via `Importer`, so this page serves any
// importer, not just Caravan -----------------------------------------------------------------------------------
export function importButtonLabel(status: Importer["status"]): string {
  switch (status) {
    case "none": return "Import maps";
    case "stale": return "Re-import";
    case "damaged": return "Repair";
    case "unsupported": return "Needs a newer Melange";
    default: return "Maps";
  }
}

export const importInstalledToast = (pluginName: string): string => `${pluginName} installed. Open it on the Plugins page to import the maps.`;

export interface DisclosureCopy { heading: string; intro: string; bullets: string[]; }
export function disclosureCopy(imp: Importer, source: ImportSource | undefined): DisclosureCopy {
  const c = imp.content;
  const size = source ? sizeText(source.size) : "";
  const host = source?.host || c.publisher;
  const fileName = source?.fileName ?? "";
  return {
    heading: `Bring the ${c.title} maps to Melange`,
    intro: `${imp.name} imports the maps from ${c.title}, a fan mod published on ${c.publisher}, into Melange on this PC. ${imp.name} itself contains no maps.`,
    bullets: [
      `Downloaded only when you ask. Melange fetches ${fileName} (${size}) from ${host}, under that site's terms. You can also use a copy you already have.`,
      "Checked before use. The file must match a known checksum before anything is read from it.",
      "Stays on your PC. Imported maps are saved in your game's Mods folder. Melange never uploads or shares them.",
      `Maps only. ${c.title}'s textures, scripts and game changes are not used. Maps use the game's standard textures, and maps made for special modes play as plain deathmatch.`,
      `Not affiliated. Melange and ${imp.name} are not made by or affiliated with ${c.publisher} or Team17. ${c.credit}`,
    ],
  };
}
export const disclosureCheckboxLabel = (publisher: string): string => `I understand these maps come from ${publisher} under its terms.`;
export const termsLinkLabel = (publisher: string): string => `Read ${publisher}'s terms`;

export const sourceDownloadLabel = (source: ImportSource): string => `Download from ${source.name} (${sizeText(source.size)})`;
export const SOURCE_LOCAL_LABEL = "Use a zip I already have";
export const CHOOSE_FILE_LABEL = "Choose file…";
export const localFileChosenLine = (name: string, size: number): string => `${name}, ${sizeText(size)}`;
export const localFileHint = (source: ImportSource): string => `It must be ${source.fileName}, ${source.size.toLocaleString()} bytes.`;
export const keepZipLabel = (size: number): string => `Keep the zip so I can re-import later without downloading (${sizeText(size)})`;

export const IMPORT_BUTTON_LABEL = "Import maps";
// `gate` is one of the `import.*` refusal reasons (RPC §10.3); anything else is shown as given so a future reason
// still says something rather than nothing.
export function importGateText(gate: string): string {
  if (gate === "gameRunning") return "Close Worms Ultimate Mayhem to import maps.";
  if (gate === "noGame") return "Choose your game folder on the Home page first.";
  return gate;
}

export function importProgressLine(job: ImportJob): string {
  switch (job.phase) {
    case "downloading": return `Downloading… ${sizeText(job.bytes)} of ${sizeText(job.total)}`;
    case "copying": return "Copying your zip…";
    case "verifying": return "Checking the file…";
    case "reading": return "Reading the maps…";
    case "building": return `Building map packs… ${job.step} of ${job.of}`;
    case "placing": return "Installing the packs…";
    case "cancelled": return "Import cancelled. Nothing was changed.";
    default: return "";
  }
}

export function importErrorText(job: ImportJob, imp: Importer): string {
  const message = job.message || "";
  switch (job.reason) {
    case "hash":
      return `This isn't the expected ${imp.content.title} zip: its size or checksum doesn't match. Nothing was read ` +
        `from it. If you downloaded it yourself, get it again from ${imp.content.publisher}.`;
    case "network":
      return `The download failed: ${message}. Check your connection and try again, or use a zip you already have.`;
    case "space":
      return `There isn't enough free space on the drive with your game.${message ? ` The import needs about ${message}.` : ""}`;
    case "zip":
      return `The zip is damaged or contains something unexpected${message ? ` (${message})` : ""}. Nothing was installed.`;
    case "recipe":
      return `The zip didn't contain what ${imp.name} expected${message ? ` (${message})` : ""}. Nothing was installed.`;
    case "vanilla":
      return "Some of your game's own map files differ from the expected version, so the maps based on them can't be " +
        "imported the same way as for other players. Verify the game files in Steam, then try again.";
    case "occupied":
      return `A folder named ${message || "it"} is already in Mods and wasn't made by ${imp.name}. Move or remove it, then try again.`;
    default:
      return `The import failed${message ? `: ${message}` : ""}. Your existing maps were left as they were.`;
  }
}

export function resultCopy(result: NonNullable<ImportJob["result"]>, content: ImportContent): { heading: string; lines: string[] } {
  const lines = [
    `${result.counts.play} play as designed · ${result.counts.dm} play as deathmatch · ${result.counts.mode} made for ` +
    "special modes play as deathmatch and are hidden until you show them",
    `${result.packs.length} map pack${result.packs.length === 1 ? "" : "s"}, ${sizeText(result.bytes)}, in your game's Mods folder.`,
  ];
  if (result.skipped > 0) lines.push(`${result.skipped} entries in ${content.title} weren't imported: their files are missing or they replace standard maps.`);
  lines.push(`Fingerprint ${fingerprintShort(result.fingerprint)}. Players with the same fingerprint can play these maps together online.`);
  return { heading: `${result.maps} maps imported`, lines };
}

export function importedHeaderLine(imp: Importer): string {
  const im = imp.imported;
  if (!im) return "";
  return `${im.maps} maps · imported ${whenText(im.importedAt)} · fingerprint ${fingerprintShort(im.fingerprint)}`;
}
export const staleBanner = (name: string): string =>
  `${name} was updated. Re-import to get the changes. Players need the same version to play these maps together online.`;
export const DAMAGED_BANNER = "Some imported packs are missing or changed. Re-import to repair them.";

export const PACKS_NOTE = "To play online, everyone needs the same packs turned on. Hiding a map below only takes it " +
  "out of your own map list and random picks; it still loads if a host picks it.";

export const MAP_SEARCH_PLACEHOLDER = "Search maps";
export const MAP_SHOWN_OPTIONS: { value: "all" | "shown" | "hidden"; label: string }[] = [
  { value: "all", label: "All" }, { value: "shown", label: "Shown" }, { value: "hidden", label: "Hidden" },
];
export const MAP_BROWSER_EMPTY = "No maps match these filters.";
export const showAllLabel = (n: number): string => `Show all ${n}`;
export const hideAllLabel = (n: number): string => `Hide all ${n}`;

export const reimportConfirmLine = (content: ImportContent): string =>
  `Re-import ${content.title}? The packs are rebuilt from the zip. Your shown and hidden maps and pack choices are kept.`;
export const removeConfirmLine = (pluginName: string, packs: number): string =>
  `Remove all ${pluginName} maps? This deletes ${packs} map pack${packs === 1 ? "" : "s"} from your Mods folder.`;
export const deleteZipCheckboxLabel = (size: number): string => `Also delete the downloaded zip (${sizeText(size)})`;
export const deleteZipActionLabel = (size: number): string => `Delete downloaded zip (${sizeText(size)})`;
export const REMOVED_TOAST = "Imported maps removed.";

export const storeImportsLine = (pluginName: string, title: string, size: number, host: string): string =>
  `${pluginName} can download ${title} (${sizeText(size)}) from ${host} when you ask it to. The maps are imported on your PC; the plugin contains none of them.`;
export const storeRemoveCascadeLine = (n: number): string => `This also removes the ${n} map${n === 1 ? "" : "s"} it imported.`;

// -- Melange updating itself (the global banner, Home and Settings) -----------------------------------------------
export const UPDATE_GAME_RUNNING = "Close the game to update";
export const UPDATE_ACTION = "Restart to update";
export const WHATS_NEW = "What's new";

// The banner over every page: a quiet progress line while a download runs, the one-click restart once it is ready,
// nothing otherwise. `blocked` says why the button is disabled (the game is running).
export function updateBanner(u: UpdateStatus, gameRunning: boolean): { text: string; action?: string; blocked?: string } | undefined {
  const v = u.latest ?? "";
  if (u.phase === "downloading") {
    const p = u.progress;
    const pct = p && p.total > 0 ? ` ${Math.min(100, Math.floor((p.got / p.total) * 100))}%` : "";
    return { text: `Downloading Melange ${v}…${pct}` };
  }
  if (u.phase === "ready") return { text: `Melange ${v} is ready`, action: UPDATE_ACTION, blocked: gameRunning ? UPDATE_GAME_RUNNING : undefined };
  return undefined;
}

// What the last start's update did, shown once by the launcher that started after it.
export function updateAppliedLine(a: UpdateApplied): string {
  if (a.ok) return `Updated to Melange ${a.version}.${a.warnings.length ? ` ${a.warnings.join(" ")}` : ""}`;
  return `Melange wasn't updated to ${a.version || "the new version"}. ${a.message || "Something went wrong; the logs on the Help page say more."}`;
}

// Settings › Updates, beside "Check for updates".
export function updateCheckLine(u: UpdateStatus): string {
  switch (u.phase) {
    case "checking": return "Checking for updates…";
    case "downloading": return `Downloading Melange ${u.latest ?? ""}…`;
    case "ready": return `Melange ${u.latest ?? ""} is ready. Restart to update.`;
    case "current": return `You're up to date (Melange ${u.current}).`;
    case "error": return `Couldn't check for updates. ${u.error ?? ""}`.trim();
    default: return u.lastCheck ? `Last checked ${whenText(u.lastCheck)}.`
      : u.auto === false ? "Automatic checks are off." : "Melange checks for updates each time it starts.";
  }
}

// -- Restore vanilla ------------------------------------------------------------------------------------------------
const plural = (n: number, one: string, many = `${one}s`) => `${n} ${n === 1 ? one : many}`;

// What the warning names: "Melange, Renewation HD 0.2A2, WUMPatch, 3 ASI plugins and 12 other files not part of
// the game". Groups come in the server's display order, "other" last.
export function vanillaFoundText(groups: VanillaGroup[]): string {
  const parts = groups.map((g) => g.id === "asi" ? plural(g.files, "ASI plugin")
    : g.id === "other" ? `${plural(g.files, "other file")} not part of the game` : g.label);
  if (parts.length <= 1) return parts[0] ?? "";
  return `${parts.slice(0, -1).join(", ")} and ${parts[parts.length - 1]}`;
}

// Mod frameworks other than Melange: the dialog says plainly that they go too.
export function vanillaOthers(groups: VanillaGroup[]): VanillaGroup[] {
  return groups.filter((g) => g.id !== "melange" && g.id !== "other");
}

// How the stock files a mod overwrote come back. `started`: Steam was asked to verify (after apply only).
export function vanillaVerifyText(store: Store, started?: boolean): string {
  if (store !== "steam") return "Verify the game files in GOG Galaxy or reinstall the game to get the original files back.";
  if (started === false) return "Steam didn't open. In Steam, right-click Worms Ultimate Mayhem › Properties › Installed Files › Verify integrity of game files.";
  return started ? "Steam is verifying the game files and will download the originals." : "Afterwards, Steam verifies the game files and downloads the originals.";
}

export const VANILLA_DONE = "Worms is back to stock. Melange will close.";

// -- Settings › Display -----------------------------------------------------------------------------------------------
// "Alt+RETURN" (Melange.ini's key names) as people say it.
export const hotkeyText = (k: string): string =>
  k.split("+").map((p) => p.trim()).map((p) => (/^(return|enter)$/i.test(p) ? "Enter" : /^grave$/i.test(p) ? "`" : p)).join("+");
export const displaySizeText = (s: DisplaySize): string => `${s.w} × ${s.h}`;
export const DISPLAY_RUNNING = "Close the game to change these: it reads them when it starts.";
export const DISPLAY_NO_MELANGE = "Install Melange to use fullscreen.";
export const DISPLAY_FS_REMOVED = "Removed the stock launcher's exclusive fullscreen (/FS) from local.cfg: Melange's borderless fullscreen replaces it.";

export function displayFullscreenHelp(d: DisplayState): string {
  const at = d.monitor.w > 0 ? ` (${displaySizeText(d.monitor)})` : "";
  const keys = /^(none|off)?$/i.test(d.hotkey.trim()) ? "View › Fullscreen" : `${hotkeyText(d.hotkey)} or View › Fullscreen in the overlay`;
  return `Fills the screen without a border, at your monitor's resolution${at}. Alt+Tab works as with any window. ` +
    `Switch in game with ${keys}; the game remembers your choice.`;
}

// A note under the switch, or undefined.
export function displayNote(d: DisplayState): string | undefined {
  if (d.removedFs) return DISPLAY_FS_REMOVED;
  if (!d.melangeIni) return DISPLAY_NO_MELANGE;
  if (d.exclusive && !d.fullscreen)
    return "The stock launcher's exclusive fullscreen is on (/FS in local.cfg). Turning on fullscreen here replaces it with Melange's borderless one: the two can't both be on.";
  if (d.fullscreen && !d.enabled) return "Melange's Display module is off in Melange.ini; turning fullscreen on again switches it back on.";
  return undefined;
}

export function windowSizeHelp(d: DisplayState): string {
  return d.fullscreen
    ? "The window's size when you leave fullscreen. Saved as /W and /H in the game's local.cfg."
    : "The size the game's window opens at. Saved as /W and /H in the game's local.cfg.";
}

// -- Settings › Memory -----------------------------------------------------------------------------------------------
export const LAA_NOTE = "Only changes this PC. Network games and other players are not affected.";
export const LAA_RUNNING = "Close the game to change this: it reads it when it starts.";
export const LAA_NO_MELANGE = "Install Melange first.";
export const laaLaunchFailed = (reason: string): string =>
  `Couldn't switch the game to 4 GB mode: ${reason.replace(/[.\s]+$/, "")}. The game started with 2 GB.`;
export const laaStatus =(s: LaaState): string =>
  s.enabled && s.active ? "On: the game can use up to 4 GB."
  : s.enabled ? "On. It takes effect the next time you start the game from Melange."
  : s.active ? "Off. The game file still has it from something other than Melange."
  : "Off: the game uses up to 2 GB.";
