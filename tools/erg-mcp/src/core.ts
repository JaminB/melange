// The editor's model, imported straight from the web app's sources so erg-mcp edits a level exactly as the Erg panel
// does (same commands, same patch). Every module here is free of three.js, Preact and the DOM: import single modules,
// never a barrel that also exports components (terrain/index.ts exports TerrainTools). build.ps1 refuses a bundle that
// still imports three or preact.
export * from "../../../web/src/sdk/erg/scene";
export * from "../../../web/src/sdk/erg/commands";
export * from "../../../web/src/sdk/erg/diff";
export { createErgSession, ErgError, type ErgSession, type ScriptProblem, type TimeOfDay } from "../../../web/src/sdk/erg/session";
export { createLevelService, type BaseInfo, type LevelService, type LevelList, type PaletteEntry as ServerPaletteEntry, type ProjectInfo, type Themes } from "../../../web/src/sdk/erg/levels";
export { createClient, RpcError, type OasisClient } from "../../../web/src/sdk/client";
export { ErrorCode } from "../../../web/src/sdk/protocol";

export { EditorStore, type Loaded } from "../../../web/src/panels/erg/model/store";
export { loadBase, openProject, withPatch } from "../../../web/src/panels/erg/model/loader";
export { Frames, frameIndex, round, roundVec, snap, snapVec, translationOnlyFrames } from "../../../web/src/panels/erg/model/geometry";
export { cornerOffset, dropPoint, rayTerrain, rayVoxels } from "../../../web/src/panels/erg/model/ground";
export * from "../../../web/src/panels/erg/model/placing";
export { Duplicate, Group, setMany } from "../../../web/src/panels/erg/model/edits";
export { AREA_KNOTS, KNOTS, PLACED_OBJECTS, TEAM_KNOTS, checkScene, type Issue } from "../../../web/src/panels/erg/model/checks";

export * from "../../../web/src/panels/erg/terrain/voxel";
export * from "../../../web/src/panels/erg/terrain/blend";
export * from "../../../web/src/panels/erg/terrain/brush";
export * from "../../../web/src/panels/erg/terrain/blocks";
export * from "../../../web/src/panels/erg/terrain/sculpt";
export * from "../../../web/src/panels/erg/terrain/surround";
export { pick, type Hit } from "../../../web/src/panels/erg/terrain/pick";
