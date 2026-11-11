// erg-mcp: an MCP server (stdio) in front of Erg, the Oasis map editor. See docs/erg-mcp.md.
//   node dist/erg-mcp.mjs [--url <launch url>]        (erg-mcp.cmd runs it with the pinned Node)
import { GROUPS } from "./tools/groups.gen";
import { McpServer } from "./mcp";
import { Registry } from "./registry";
import { Session } from "./session";

export const VERSION = "0.1.0";

// stdout carries the protocol only.
console.log = console.info = console.debug = (...a: unknown[]) => void process.stderr.write(`${a.map(String).join(" ")}\n`);

const registry = new Registry();
for (const [group, mod] of GROUPS) registry.add(group, mod.tools);

if (process.argv.includes("--list-tools")) {
  for (const t of registry.list()) process.stdout.write(`${registry.groupOf(t.name)}\t${t.name}\n`);
  process.exit(0);
}

const session = new Session();
const server = new McpServer(registry, session, {
  name: "erg-mcp",
  version: VERSION,
  instructions: "Edits Worms Ultimate Mayhem levels through Erg (Melange's map editor). Start with list_bases, new_project or " +
    "open_project, then describe_level. Positions are map units (1 voxel) unless a field says world (map x 20). Edits are " +
    "undoable and stay in memory until save.",
});
server.listen(() => {
  session.conn?.client.close();
  process.exit(0);
});
