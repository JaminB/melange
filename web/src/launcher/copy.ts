// The wizard's words (spec §5.3) as pure data: one function per family of states, each returning {title, body}.
// A unit test asserts every member of every union this file switches on produces non-empty title and body.
import type { Candidate, DllInfo, GameCheck, Plan, SetupStatus, Verdict } from "./api";
import { LauncherErrorCode, sizeText } from "./api";

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
    case "wrongBuild":
      return {
        title: "This version of the game isn't supported",
        body: [
          "Melange only works with the Steam/GOG release, build #1077, and this WormsMayhem.exe is different. " +
          "Installing Melange on it could crash the game, so we won't.",
          "This usually happens when the game has been patched or modified, or when a beta branch is selected.",
          "On Steam: right-click the game › Properties › Installed Files › Verify integrity of game files, and make sure Betas is set to None. Then try again.",
          `The Steam/GOG release is: size ${BUILD_SIZE.toLocaleString()} bytes, SHA-256 ${BUILD_HASH.slice(0, 6)}…`,
        ],
      };
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
