// Starlight sidebar for the Melange site.
//
// astro.config.mjs:  import { sidebar } from './scripts/sidebar.mjs';  starlight({ sidebar, ... })
//
// GROUPS is the information architecture for the latest docs (from ia-melange.md). Slugs are the
// files scripts/sync-docs.mjs writes into src/content/docs/<slug>.md (docs/<name>.md -> <name>,
// README.md's sections -> install, fullscreen, install-a-mod, uninstall, reporting-a-bug,
// CODE_SIGNING.md -> code-signing), plus changelog (releases.mjs) and reference/schema/* (schemas.mjs).
//
// The sidebar is built from what exists on disk when the config loads (run the prebuild first):
// an entry whose page is missing is skipped, and a generated page that no group lists lands in a
// "More" group so nothing is ever unreachable. One collapsed group per versioned snapshot
// (src/content/docs/v<ver>/, written by versions.mjs) follows; src/route-data.ts hides those on latest
// pages and, on a versioned page, swaps the whole sidebar for that version's pages in this layout.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const DOCS_DIR = fileURLToPath(new URL('../src/content/docs/', import.meta.url));
export const VERSION_DIR_RE = /^v\d+\.\d+\.\d+$/;

/** @type {{label: string, items: ({slug: string, label?: string} | {autogenerate: string, label: string})[]}[]} */
export const GROUPS = [
	{
		label: 'Get started',
		items: [
			{ slug: 'install', label: 'Install Melange' },
			{ slug: 'install-a-mod', label: 'Install a mod' },
			{ slug: 'multiplayer-fixes', label: 'Multiplayer fixes' },
			{ slug: 'fullscreen', label: 'Fullscreen' },
			{ slug: 'reporting-a-bug', label: 'Report a bug' },
			{ slug: 'uninstall', label: 'Uninstall' },
			{ slug: 'code-signing', label: 'Code signing' },
		],
	},
	{
		label: 'Make a plugin',
		items: [
			{ slug: 'creating-plugins', label: 'Creating a plugin' },
			{ slug: 'spice', label: 'Spice manifest' },
			{ slug: 'lua-api', label: 'Lua API (wum.*)' },
			{ slug: 'weapons', label: 'Weapon mods' },
			{ slug: 'importers', label: 'Content importers' },
		],
	},
	{
		label: 'Tools',
		items: [
			{ slug: 'oasis', label: 'Oasis (web app)' },
			{ slug: 'erg', label: 'Erg map editor' },
			{ slug: 'wormsign', label: 'Wormsign (replays, desyncs)' },
			{ slug: 'xomtool', label: 'Sieve (xomtool)' },
		],
	},
	{
		label: 'Developers',
		items: [
			{ slug: 'developer-guide', label: 'Developer guide' },
			{ slug: 'capture-format', label: 'GL capture format' },
		],
	},
	{
		label: 'Reference',
		items: [
			{ slug: 'changelog', label: 'Changelog' },
			{ autogenerate: 'reference/schema', label: 'Schemas' },
		],
	},
];

const exists = (slug) => fs.existsSync(path.join(DOCS_DIR, `${slug}.md`)) || fs.existsSync(path.join(DOCS_DIR, `${slug}.mdx`));
const dirHasPages = (dir) => {
	try {
		return fs.readdirSync(path.join(DOCS_DIR, dir), { recursive: true }).some((f) => /\.mdx?$/.test(String(f)));
	} catch {
		return false;
	}
};

/** Versioned snapshot folders, newest first. */
export function versionDirs() {
	let names = [];
	try {
		names = fs.readdirSync(DOCS_DIR, { withFileTypes: true }).filter((d) => d.isDirectory() && VERSION_DIR_RE.test(d.name)).map((d) => d.name);
	} catch {
		/* no generated docs yet */
	}
	const key = (v) => v.slice(1).split('.').map(Number);
	return names.sort((a, b) => {
		const [x, y] = [key(a), key(b)];
		return y[0] - x[0] || y[1] - x[1] || y[2] - x[2];
	});
}

/** The Starlight `sidebar` config array. */
export function buildSidebar({ includeVersions = true } = {}) {
	const listed = new Set();
	const sidebar = [];
	for (const g of GROUPS) {
		const items = [];
		for (const it of g.items) {
			if ('slug' in it) {
				listed.add(it.slug);
				if (exists(it.slug)) items.push(it.label ? { label: it.label, slug: it.slug } : { slug: it.slug });
			} else if (dirHasPages(it.autogenerate)) {
				items.push({ label: it.label, collapsed: false, items: [{ autogenerate: { directory: it.autogenerate } }] });
			}
		}
		if (items.length) sidebar.push({ label: g.label, items });
	}
	let extra = [];
	try {
		extra = fs
			.readdirSync(DOCS_DIR)
			.filter((f) => /\.mdx?$/.test(f) && f !== 'index.md' && f !== 'index.mdx')
			.map((f) => f.replace(/\.mdx?$/, ''))
			.filter((s) => !listed.has(s))
			.sort();
	} catch {
		/* no generated docs yet */
	}
	if (extra.length) sidebar.push({ label: 'More', items: extra.map((slug) => ({ slug })) });
	if (includeVersions) {
		for (const v of versionDirs()) sidebar.push({ label: v, collapsed: true, items: [{ autogenerate: { directory: v } }] });
	}
	return sidebar;
}

/** Slugs the sidebar lists, in order (used by versions.mjs for the version index pages). */
export function orderedSlugs() {
	return GROUPS.flatMap((g) => g.items.filter((it) => 'slug' in it).map((it) => it.slug));
}

export const sidebar = buildSidebar();
export default sidebar;
