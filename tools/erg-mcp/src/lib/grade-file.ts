// A grade by name or from a file, for the renders' `grade` (lib/grade.ts is the pure part).
import { existsSync, readFileSync } from "node:fs";
import { GRADES, parseGrade, type Grade } from "./grade";
import { ToolError } from "../registry";

export const GRADE_NAMES = Object.keys(GRADES).join(", ") || "none yet";

/** A grade by name (a built-in) or from the file game_material_shots {saveGrade} wrote. */
export function loadGrade(nameOrPath: string): { name: string; grade: Grade } {
  if (GRADES[nameOrPath]) return { name: nameOrPath, grade: GRADES[nameOrPath] };
  if (!existsSync(nameOrPath)) throw new ToolError(`grade: '${nameOrPath}' is neither a built-in grade (${GRADE_NAMES}) nor a file (game_material_shots {saveGrade: "<path>"} writes one in a match)`);
  try {
    return { name: nameOrPath, grade: parseGrade(readFileSync(nameOrPath, "utf8")) };
  } catch (e) {
    throw new ToolError(`grade: ${nameOrPath}: ${(e as Error).message}`);
  }
}
