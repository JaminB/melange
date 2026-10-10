// Prebuild step 2: versioned docs from git tags.
//
// For every tag v<x.y.z> >= 0.8.0 (pre-release tags are skipped): the docs/*.md, README.md and
// CODE_SIGNING.md that exist at the tag, transformed like the latest docs, into
// src/content/docs/v<ver>/<slug>.md (with a "you are reading an old version" banner, no search
// indexing), the tag's docs/images/** into public/images/v<ver>/, a version index page
// (src/content/docs/v<ver>/index.md), and src/data/versions.json.
// Needs the tags: CI checks out with fetch-depth: 0.
//
// Owns: src/content/docs/v*/, public/images/v*/, src/data/versions.json. Run: node scripts/versions.mjs

import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { buildDocSet, renderPage, tagSource, worktreeSource, git, BASE, DOCS_OUT, IMAGES_OUT, DATA_OUT, VERSION_DIR_RE } from './lib/docs.mjs';
import { GROUPS } from './sidebar.mjs';

export const MIN_VERSION = [0, 8, 0];

const parse = (tag) => {
	const m = /^v(\d+)\.(\d+)\.(\d+)$/.exec(tag);
	return m ? m.slice(1).map(Number) : null;
};
const cmp = (a, b) => a[0] - b[0] || a[1] - b[1] || a[2] - b[2];

export function versionTags() {
	return git(['tag', '--list', 'v*'])
		.split('\n')
		.map((t) => t.trim())
		.filter(Boolean)
		.map((tag) => ({ tag, v: parse(tag) }))
		.filter((t) => t.v && cmp(t.v, MIN_VERSION) >= 0)
		.sort((a, b) => cmp(b.v, a.v))
		.map((t) => t.tag);
}

function indexPage(ver, pages, latestSlugs) {
	const bySlug = new Map(pages.map((p) => [p.slug, p]));
	const used = new Set();
	const sections = [];
	for (const g of GROUPS) {
		const links = g.items
			.filter((it) => 'slug' in it && bySlug.has(it.slug))
			.map((it) => {
				used.add(it.slug);
				return `- [${it.label ?? bySlug.get(it.slug).title}](${BASE}/v${ver}/${it.slug}/)`;
			});
		if (links.length) sections.push(`## ${g.label}\n\n${links.join('\n')}`);
	}
	const rest = pages.filter((p) => !used.has(p.slug)).map((p) => `- [${p.title}](${BASE}/v${ver}/${p.slug}/)`);
	if (rest.length) sections.push(`## More\n\n${rest.join('\n')}`);
	return {
		slug: `v${ver}`,
		source: `the v${ver} tag`,
		title: `Melange ${ver} documentation`,
		description: `The Melange documentation as it was released in version ${ver}.`,
		body: `These pages are the documentation that shipped with Melange ${ver}. For the current version, see the [latest docs](${BASE}/install/).\n\n${sections.join('\n\n')}\n`,
	};
}

export function buildVersions() {
	// Clean what this step owns.
	for (const dir of [DOCS_OUT, IMAGES_OUT]) {
		fs.mkdirSync(dir, { recursive: true });
		for (const d of fs.readdirSync(dir, { withFileTypes: true })) {
			if (d.isDirectory() && VERSION_DIR_RE.test(d.name)) fs.rmSync(path.join(dir, d.name), { recursive: true, force: true });
		}
	}
	fs.mkdirSync(DATA_OUT, { recursive: true });

	const tags = versionTags();
	const latestSlugs = buildDocSet(worktreeSource(), { latest: true }).slugs;
	const warnings = [];
	let pageCount = 0;
	let imageCount = 0;

	for (const tag of tags) {
		const src = tagSource(tag);
		const ver = src.version;
		const outDir = path.join(DOCS_OUT, `v${ver}`);
		fs.mkdirSync(outDir, { recursive: true });
		const { pages, warnings: w } = buildDocSet(src, { latest: false });
		warnings.push(...w.map((x) => `${tag}: ${x}`));

		for (const p of pages) {
			const latestHref = latestSlugs.has(p.slug) ? `${BASE}/${p.slug}/` : `${BASE}/`;
			const fm = {
				// Astro would slugify "v0.8.0" to "v080"; an explicit slug keeps the dots in the URL.
				slug: `v${ver}/${p.slug}`,
				editUrl: false,
				lastUpdated: p.lastUpdated ?? undefined,
				pagefind: false,
				banner: { content: `You are reading the docs for Melange ${ver}. <a href="${latestHref}">Read the latest.</a>` },
			};
			fs.writeFileSync(path.join(outDir, `${p.slug}.md`), renderPage(p, fm));
			pageCount++;
		}
		const idx = indexPage(ver, pages, latestSlugs);
		fs.writeFileSync(
			path.join(outDir, 'index.md'),
			renderPage(idx, {
				slug: idx.slug,
				editUrl: false,
				pagefind: false,
				sidebar: { hidden: true },
				banner: { content: `You are reading the docs for Melange ${ver}. <a href="${BASE}/">Read the latest.</a>` },
			}),
		);

		// All of docs/images/ at the tag.
		const imgDir = path.join(IMAGES_OUT, `v${ver}`);
		for (const f of src.files.filter((f) => f.startsWith('docs/images/'))) {
			const out = path.join(imgDir, ...f.slice('docs/images/'.length).split('/'));
			fs.mkdirSync(path.dirname(out), { recursive: true });
			fs.writeFileSync(out, src.readBuffer(f));
			imageCount++;
		}
	}

	const versions = tags.map((t) => t.replace(/^v/, ''));
	const data = { latest: 'main', released: tags[0] ?? null, versions };
	fs.writeFileSync(path.join(DATA_OUT, 'versions.json'), JSON.stringify(data, null, '\t') + '\n');
	if (!tags.length) warnings.push('no tags >= v0.8.0 found (shallow clone? CI needs fetch-depth: 0); no versioned docs');
	return { tags, pages: pageCount, images: imageCount, warnings };
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
	try {
		const r = buildVersions();
		for (const w of r.warnings) console.warn(`  warning: ${w}`);
		console.log(`versions: ${r.tags.length} versions (${r.tags.join(', ') || 'none'}), ${r.pages} pages, ${r.images} images, ${r.warnings.length} warnings`);
	} catch (e) {
		console.error(`versions: FAILED: ${e.stack || e}`);
		process.exit(1);
	}
}
