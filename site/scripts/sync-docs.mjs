// Prebuild step 1: the latest docs.
//
// ../docs/*.md (not docs/findings/**), ../README.md (split into pages, see README_SECTIONS in
// lib/docs.mjs) and ../CODE_SIGNING.md -> src/content/docs/<slug>.md, and ../docs/images/** ->
// public/images/. Links between docs become site URLs; unknown targets are left as-is and warned.
//
// Owns: the top-level *.md files in src/content/docs/ (except changelog.md, written by releases.mjs)
// and public/images/ (except the v<ver>/ folders, written by versions.mjs). Run: node scripts/sync-docs.mjs

import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { buildDocSet, renderPage, editUrl, worktreeSource, DOCS_OUT, IMAGES_OUT, REPO_ROOT, VERSION_DIR_RE } from './lib/docs.mjs';
import { orderedSlugs } from './sidebar.mjs';

const KEEP_TOP = new Set(['.gitkeep', 'changelog.md']);

	for (const f of ["README.md", "docs"]) if (!fs.existsSync(path.join(REPO_ROOT, f))) throw new Error(`${f} not found under ${REPO_ROOT}`);
export function syncDocs() {
	// Clean what this step owns.
	fs.mkdirSync(DOCS_OUT, { recursive: true });
	for (const d of fs.readdirSync(DOCS_OUT, { withFileTypes: true })) {
		if (d.isFile() && /\.mdx?$/.test(d.name) && !KEEP_TOP.has(d.name)) fs.rmSync(path.join(DOCS_OUT, d.name));
	}
	fs.mkdirSync(IMAGES_OUT, { recursive: true });
	for (const d of fs.readdirSync(IMAGES_OUT, { withFileTypes: true })) {
		if (!(d.isDirectory() && VERSION_DIR_RE.test(d.name))) fs.rmSync(path.join(IMAGES_OUT, d.name), { recursive: true, force: true });
	}

	const { pages, warnings } = buildDocSet(worktreeSource(), { latest: true });

	for (const p of pages) {
		if (KEEP_TOP.has(`${p.slug}.md`)) throw new Error(`${p.source}: slug "${p.slug}" collides with a page another step writes`);
		const fm = { editUrl: editUrl(p.source) };
		if (p.lastUpdated) fm.lastUpdated = p.lastUpdated;
		fs.writeFileSync(path.join(DOCS_OUT, `${p.slug}.md`), renderPage(p, fm));
	}

	// All of docs/images/ (the landing page uses some that no doc links).
	const imgSrc = path.join(REPO_ROOT, 'docs', 'images');
	let images = 0;
	if (fs.existsSync(imgSrc)) {
		for (const d of fs.readdirSync(imgSrc, { withFileTypes: true })) {
			if (d.isDirectory() && VERSION_DIR_RE.test(d.name)) throw new Error(`docs/images/${d.name} clashes with the versioned image folders`);
		}
		fs.cpSync(imgSrc, IMAGES_OUT, { recursive: true });
		images = fs.readdirSync(imgSrc, { recursive: true, withFileTypes: true }).filter((d) => d.isFile()).length;
	}

	const inSidebar = new Set(orderedSlugs());
	for (const p of pages) if (!inSidebar.has(p.slug)) warnings.push(`${p.source}: page "${p.slug}" is not in scripts/sidebar.mjs GROUPS (it goes under "More")`);

	return { pages: pages.length, images, warnings };
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
	try {
		const r = syncDocs();
		for (const w of r.warnings) console.warn(`  warning: ${w}`);
		console.log(`sync-docs: ${r.pages} pages, ${r.images} images, ${r.warnings.length} warnings`);
	} catch (e) {
		console.error(`sync-docs: FAILED: ${e.stack || e}`);
		process.exit(1);
	}
}
