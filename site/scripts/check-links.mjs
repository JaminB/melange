// Link check over the built site (npm run check:links, after npm run build).
//
// Every href/src (and srcset entry) in dist/**/*.html that starts with the site base (/melange/) must resolve to a
// file in dist/: "/melange/foo/" -> dist/foo/index.html, "/melange/a.png" -> dist/a.png. Anchors are not checked.
// External links and relative links are ignored. Exits 1 and lists every broken link with the page it is on.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const SITE_DIR = fileURLToPath(new URL('../', import.meta.url));
const DIST = path.join(SITE_DIR, 'dist');
const BASE = '/melange/';

if (!fs.existsSync(DIST)) {
	console.error('check-links: dist/ not found; run npm run build first');
	process.exit(1);
}

const htmlFiles = fs
	.readdirSync(DIST, { recursive: true })
	.map(String)
	.filter((f) => f.endsWith('.html'));

const ATTR_RE = /\s(?:href|src|srcset)\s*=\s*("([^"]*)"|'([^']*)')/gi;

function decodeEntities(s) {
	return s
		.replace(/&amp;/g, '&')
		.replace(/&#x([0-9a-f]+);/gi, (_, h) => String.fromCodePoint(parseInt(h, 16)))
		.replace(/&#(\d+);/g, (_, d) => String.fromCodePoint(Number(d)));
}

function resolves(url) {
	let p = url.split('#')[0].split('?')[0];
	try {
		p = decodeURIComponent(p);
	} catch {
		/* keep as-is */
	}
	const rel = p.slice(BASE.length);
	const target = path.join(DIST, ...rel.split('/').filter(Boolean));
	if (p.endsWith('/')) return fs.existsSync(path.join(target, 'index.html'));
	if (fs.existsSync(target) && fs.statSync(target).isFile()) return true;
	return fs.existsSync(path.join(target, 'index.html'));
}

const failures = [];
let checked = 0;
for (const file of htmlFiles) {
	const html = fs.readFileSync(path.join(DIST, file), 'utf8');
	for (const m of html.matchAll(ATTR_RE)) {
		const raw = decodeEntities(m[2] ?? m[3] ?? '');
		const isSrcset = /^\s*srcset/i.test(m[0]);
		const urls = isSrcset ? raw.split(',').map((s) => s.trim().split(/\s+/)[0]) : [raw.trim()];
		for (const url of urls) {
			if (!url.startsWith(BASE) && url !== BASE.slice(0, -1)) continue;
			checked++;
			if (url === BASE.slice(0, -1) ? !resolves(BASE) : !resolves(url)) failures.push(`${file.replace(/\\/g, '/')}: ${url}`);
		}
	}
}

if (failures.length) {
	console.error(`check-links: ${failures.length} broken link(s) out of ${checked}:`);
	for (const f of [...new Set(failures)].sort()) console.error(`  ${f}`);
	process.exit(1);
}
console.log(`check-links: ${checked} links in ${htmlFiles.length} pages, all resolve`);
