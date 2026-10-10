// Generates everything the site builds from (all of it gitignored), then `astro build` / `astro dev` runs.
// Steps run in order, each as its own process so each also works standalone (node scripts/<step>.mjs):
//   1. sync-docs     latest docs from ../docs, ../README.md, ../CODE_SIGNING.md
//   2. versions      docs snapshots from git tags >= v0.8.0, src/data/versions.json
//   3. wum-coverage  every registered wum.* function is documented in docs/lua-api.md (also `npm run check`)
//   4. schemas       reference pages from ../docs/*.schema.json
//   5. releases      src/data/releases.json and the changelog page from GitHub Releases
// A failing step stops the prebuild with a non-zero exit. A step whose script does not exist is
// reported and skipped (so the steps can land separately).

import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const STEPS = ['sync-docs', 'versions', 'wum-coverage', 'schemas', 'releases'];

let skipped = 0;
for (const step of STEPS) {
	const script = path.join(here, `${step}.mjs`);
	if (!fs.existsSync(script)) {
		console.warn(`prebuild: warning: scripts/${step}.mjs is missing, skipped`);
		skipped++;
		continue;
	}
	const r = spawnSync(process.execPath, [script], { stdio: 'inherit', cwd: path.dirname(here), env: process.env });
	if (r.status !== 0) {
		console.error(`prebuild: ${step} failed (exit ${r.status ?? r.signal})`);
		process.exit(r.status || 1);
	}
}
console.log(`prebuild: ${STEPS.length - skipped}/${STEPS.length} steps ran${skipped ? `, ${skipped} missing` : ''}`);
