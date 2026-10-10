// Shared Markdown transformation for sync-docs.mjs (latest docs) and versions.mjs (tagged docs).
//
// Input: a "source" abstraction over the repository (the working tree, or a git tag), so the same
// code turns ../docs/*.md, ../README.md and ../CODE_SIGNING.md into Starlight pages either way.
// Plain Node, no dependencies.

import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

export const REPO = 'JaminB/melange';
export const BASE = '/melange';
export const SITE_DIR = fileURLToPath(new URL('../../', import.meta.url));
export const REPO_ROOT = path.resolve(SITE_DIR, '..');
export const DOCS_OUT = path.join(SITE_DIR, 'src', 'content', 'docs');
export const DATA_OUT = path.join(SITE_DIR, 'src', 'data');
export const IMAGES_OUT = path.join(SITE_DIR, 'public', 'images');
export const VERSION_DIR_RE = /^v\d+\.\d+\.\d+$/;

// README.md is split into one page per H2 section (see ia-melange.md / scripts/sidebar.mjs).
// key = GitHub anchor of the H2; value = page slug, or null to drop the section from the site
// (the landing page and the footer cover it). The text before the first H2 goes to `install`.
// An H2 that is not listed becomes its own page (slug = its anchor) and is reported as a warning.
export const README_SECTIONS = {
	'install-melange': 'install',
	fullscreen: 'fullscreen',
	'install-a-mod': 'install-a-mod',
	uninstall: 'uninstall',
	'reporting-a-bug': 'reporting-a-bug',
	'for-developers': null, // landing page "For modders" strip + sidebar
	license: null, // site footer
};
export const README_INTRO_SLUG = 'install';

// Other top-level files that become pages.
export const EXTRA_FILES = { 'CODE_SIGNING.md': 'code-signing' };

// Extra lines added after a page's first paragraph (the "link map": prose page -> generated reference).
// Only for the latest docs; `needs` must exist in the source for the line to be added.
export const EXTRA_LINES = {
	spice: {
		needs: 'docs/spice-1.schema.json',
		text: `**Field reference:** every \`spice.json\` field with its type, default and allowed values is in the [spice.json schema reference](${BASE}/reference/schema/spice-1/).`,
	},
};

// Raw HTML the docs may use on purpose; anything else that looks like a tag (`<game>`, `<mod name>`)
// is escaped so Markdown does not swallow it.
const HTML_OK = /^\/?(?:br|kbd|sup|sub|details|summary|img|a|b|i|em|strong|code|span|div|p)\b/i;

// ---------------------------------------------------------------------------------------------
// Sources

/** The working tree. */
export function worktreeSource() {
	return {
		ref: 'main',
		version: null,
		read(p) {
			try {
				return fs.readFileSync(path.join(REPO_ROOT, p), 'utf8');
			} catch {
				return null;
			}
		},
		exists(p) {
			return fs.existsSync(path.join(REPO_ROOT, p));
		},
		listDocs() {
			return fs
				.readdirSync(path.join(REPO_ROOT, 'docs'), { withFileTypes: true })
				.filter((d) => d.isFile() && d.name.endsWith('.md'))
				.map((d) => 'docs/' + d.name)
				.sort();
		},
		lastUpdated(p) {
			return gitDate(['log', '-1', '--format=%cI', '--', p]);
		},
	};
}

/** A git tag (e.g. v0.8.0). */
export function tagSource(tag) {
	const all = git(['ls-tree', '-r', '--name-only', tag]).split('\n').filter(Boolean);
	const files = new Set(all);
	const dirs = new Set();
	for (const f of all) {
		let d = path.posix.dirname(f);
		while (d && d !== '.') {
			dirs.add(d);
			d = path.posix.dirname(d);
		}
	}
	return {
		ref: tag,
		version: tag.replace(/^v/, ''),
		files: all,
		read(p) {
			return files.has(p) ? git(['show', `${tag}:${p}`]) : null;
		},
		readBuffer(p) {
			return execFileSync('git', ['show', `${tag}:${p}`], { cwd: REPO_ROOT, maxBuffer: 256 << 20 });
		},
		exists(p) {
			return files.has(p) || dirs.has(p);
		},
		listDocs() {
			return all.filter((f) => /^docs\/[^/]+\.md$/.test(f)).sort();
		},
		lastUpdated(p) {
			return gitDate(['log', '-1', '--format=%cI', tag, '--', p]);
		},
	};
}

export function git(args) {
	return execFileSync('git', args, { cwd: REPO_ROOT, encoding: 'utf8', maxBuffer: 256 << 20 });
}

function gitDate(args) {
	try {
		const out = git(args).trim();
		return out || null;
	} catch {
		return null;
	}
}

// ---------------------------------------------------------------------------------------------
// Markdown helpers

/** GitHub-style heading anchor (close to github-slugger). */
export function anchor(text) {
	return plain(text)
		.toLowerCase()
		.replace(/[^\p{L}\p{M}\p{N}\p{Pc}\- ]/gu, '')
		.replace(/ /g, '-');
}

/** Strip inline Markdown to plain text. */
export function plain(md) {
	return md
		.replace(/!\[([^\]]*)\]\([^)]*\)/g, '$1')
		.replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')
		.replace(/`+([^`]*?)`+/g, '$1')
		.replace(/(\*\*|__)(.*?)\1/g, '$2')
		.replace(/(^|[^\w*])[*_]([^*_]+)[*_](?=$|[^\w*])/g, '$1$2')
		.replace(/<\/?([A-Za-z]+)\b[^>]*>/g, (tag, name) => (HTML_OK.test(name) ? '' : tag))
		.replace(/\s+/g, ' ')
		.trim();
}

/** Split Markdown into lines tagged as fenced code or not. */
function splitFences(md) {
	const lines = md.replace(/\r\n?/g, '\n').split('\n');
	let fence = null;
	return lines.map((line) => {
		const m = /^\s{0,3}(`{3,}|~{3,})/.exec(line);
		if (fence) {
			const close = /^\s{0,3}(`{3,}|~{3,})\s*$/.exec(line);
			if (close && close[1][0] === fence[0] && close[1].length >= fence.length) {
				fence = null;
			}
			return { line, code: true };
		}
		if (m) {
			fence = m[1];
			return { line, code: true };
		}
		return { line, code: false };
	});
}

/** Apply fn to the parts of a non-code line that are outside inline code spans. */
function mapOutsideInlineCode(line, fn) {
	let out = '';
	let last = 0;
	const re = /(`+)([\s\S]*?[^`])\1(?!`)/g;
	let m;
	while ((m = re.exec(line))) {
		out += fn(line.slice(last, m.index)) + m[0];
		last = m.index + m[0].length;
	}
	return out + fn(line.slice(last));
}

/** Headings outside code: [{level, text, anchor, index}] with GitHub-style duplicate suffixes. */
function headingsOf(lines) {
	const seen = new Map();
	const out = [];
	lines.forEach((l, index) => {
		if (l.code) return;
		const m = /^(#{1,6})\s+(.*?)\s*#*\s*$/.exec(l.line);
		if (!m) return;
		let a = anchor(m[2]);
		const n = seen.get(a) ?? 0;
		seen.set(a, n + 1);
		if (n) a = `${a}-${n}`;
		out.push({ level: m[1].length, text: m[2], anchor: a, index });
	});
	return out;
}

function description(lines) {
	let para = [];
	for (const l of lines) {
		if (l.code) {
			if (para.length) break;
			continue;
		}
		const t = l.line.trim();
		if (!t) {
			if (para.length) break;
			continue;
		}
		if (!para.length && (/^(#|\||!\[|<|>|[-*+] |\d+\. |---|\*\*\*)/.test(t))) continue;
		para.push(t);
	}
	let d = plain(para.join(' '));
	if (d.length > 160) {
		d = d.slice(0, 159);
		const sp = d.lastIndexOf(' ');
		if (sp > 100) d = d.slice(0, sp);
		d = d.replace(/[\s,;:.(-]+$/, '') + '…';
	}
	return d;
}

// ---------------------------------------------------------------------------------------------
// Building a doc set

/**
 * Turn a source into pages.
 * @returns {{pages: {slug, source, title, description, body, lastUpdated}[], images: Set<string>, warnings: string[]}}
 */
export function buildDocSet(src, { latest = true } = {}) {
	const warnings = [];
	const warn = (m) => warnings.push(m);
	const units = []; // {slug, source, lines (tagged), title, sectionAnchors}

	// docs/*.md and extra files: one page each.
	const fileSlug = new Map();
	for (const p of src.listDocs()) fileSlug.set(p, path.posix.basename(p, '.md'));
	for (const [p, slug] of Object.entries(EXTRA_FILES)) if (src.exists(p)) fileSlug.set(p, slug);
	for (const [p, slug] of fileSlug) {
		const lines = splitFences(src.read(p) ?? '');
		const hs = headingsOf(lines);
		const h1 = hs.find((h) => h.level === 1);
		let title = slug;
		if (h1) {
			title = plain(h1.text);
			lines.splice(h1.index, 1);
		} else warn(`${p}: no H1, using "${slug}" as the title`);
		units.push({ slug, source: p, lines, title });
	}

	// README.md: split by H2.
	const readmeAnchors = new Map(); // anchor -> {slug, isPage}
	const readme = src.read('README.md');
	if (readme != null) {
		const lines = splitFences(readme);
		const hs = headingsOf(lines);
		for (const h of hs) if (h.level === 1) lines[h.index] = { line: '', code: false };
		const h2s = hs.filter((h) => h.level === 2);
		const intro = lines.slice(0, h2s.length ? h2s[0].index : lines.length);
		const bySlug = new Map();
		const add = (slug, title, part) => {
			if (!bySlug.has(slug)) bySlug.set(slug, { slug, source: 'README.md', lines: [], title });
			const u = bySlug.get(slug);
			if (!u.title && title) u.title = title;
			u.lines.push(...part, { line: '', code: false });
		};
		h2s.forEach((h, i) => {
			let slug = README_SECTIONS[h.anchor];
			if (slug === null) return;
			if (slug === undefined) {
				slug = h.anchor;
				warn(`README.md: section "${h.text}" has no page in README_SECTIONS; publishing it as /${slug}/`);
			}
			const end = i + 1 < h2s.length ? h2s[i + 1].index : lines.length;
			const body = lines.slice(h.index + 1, end).map((l) =>
				// The H2 becomes the page title, so lower headings move up one level.
				l.code ? l : { ...l, line: l.line.replace(/^(#{3,6})(\s)/, (_, h, s) => h.slice(1) + s) },
			);
			readmeAnchors.set(h.anchor, { slug, isPage: true });
			for (const sub of hs) if (sub.index > h.index && sub.index < end) readmeAnchors.set(sub.anchor, { slug, isPage: false });
			if (slug === README_INTRO_SLUG && !bySlug.has(slug)) add(slug, plain(h.text), intro);
			add(slug, plain(h.text), body);
		});
		if (!bySlug.has(README_INTRO_SLUG)) warn(`README.md: no "Install Melange" section; the intro is not published`);
		units.push(...bySlug.values());
	}

	const slugBySource = new Map([...fileSlug]); // docs/x.md -> x
	const allSlugs = new Set(units.map((u) => u.slug));
	const anchorsBySlug = new Map(units.map((u) => [u.slug, new Set(headingsOf(u.lines).map((h) => h.anchor))]));
	const prefix = latest ? '' : `v${src.version}/`;
	const images = new Set();

	const pageUrl = (slug, hash) => `${BASE}/${prefix}${slug}/${hash ? '#' + hash : ''}`;
	const blobUrl = (p, hash) => `https://github.com/${REPO}/blob/${src.ref}/${p}${hash ? '#' + hash : ''}`;

	function resolve(target, unit) {
		if (/^[a-z][a-z0-9+.-]*:/i.test(target) || target.startsWith('//') || target.startsWith('/')) return target;
		const hashAt = target.indexOf('#');
		const p = hashAt < 0 ? target : target.slice(0, hashAt);
		const hash = hashAt < 0 ? '' : target.slice(hashAt + 1);
		if (!p) {
			if (unit.source === 'README.md') {
				const a = readmeAnchors.get(hash);
				if (!a) {
					warn(`README.md: unknown anchor #${hash}`);
					return target;
				}
				return a.slug === unit.slug ? target : pageUrl(a.slug, a.isPage ? '' : hash);
			}
			if (hash && !anchorsBySlug.get(unit.slug)?.has(hash)) warn(`${unit.source}: unknown anchor #${hash}`);
			return target;
		}
		const repoPath = path.posix.normalize(path.posix.join(path.posix.dirname(unit.source), decodeURI(p)));
		if (repoPath.startsWith('..')) {
			warn(`${unit.source}: link outside the repository: ${target}`);
			return target;
		}
		if (repoPath === 'README.md') {
			if (!hash) return latest ? `${BASE}/` : `${BASE}/${prefix}`;
			const a = readmeAnchors.get(hash);
			if (!a) {
				// A released tag's docs can't be fixed any more: link the README as it was at that tag.
				if (!latest) return blobUrl('README.md', hash);
				warn(`${unit.source}: README.md has no section #${hash} (link left as-is: ${target})`);
				return target;
			}
			return pageUrl(a.slug, a.isPage ? '' : hash);
		}
		if (slugBySource.has(repoPath)) {
			const slug = slugBySource.get(repoPath);
			if (hash && !anchorsBySlug.get(slug)?.has(hash)) warn(`${unit.source}: ${repoPath} has no heading #${hash}`);
			return pageUrl(slug, hash);
		}
		if (repoPath.startsWith('docs/images/')) {
			if (!src.exists(repoPath)) warn(`${unit.source}: missing image ${repoPath}`);
			images.add(repoPath);
			return `${BASE}/images/${prefix}${repoPath.slice('docs/images/'.length)}`;
		}
		const schema = /^docs\/([^/]+)\.schema\.json$/.exec(repoPath);
		if (schema && src.exists(repoPath)) {
			return latest ? `${BASE}/reference/schema/${schema[1]}/` : blobUrl(repoPath, hash);
		}
		if (src.exists(repoPath)) return blobUrl(repoPath, hash);
		warn(`${unit.source}: unknown link target ${target}`);
		return target;
	}

	function rewriteText(text, unit) {
		return text
			.replace(/\]\(\s*<?([^)\s>]+)>?(\s+"[^"]*")?\s*\)/g, (_, t, title = '') => `](${resolve(t, unit)}${title})`)
			.replace(/<(?=\/?[A-Za-z][^>\n]*>)/g, (lt, offset, s) => {
				const rest = s.slice(offset + 1);
				if (HTML_OK.test(rest) || /^(https?:|mailto:)/i.test(rest)) return lt;
				return '&lt;';
			});
	}

	const pages = units.map((u) => {
		let lines = u.lines.map((l) => (l.code ? l.line : mapOutsideInlineCode(l.line, (t) => rewriteText(t, u))));
		// Trim leading/trailing blank lines and collapse runs of blank lines.
		let body = lines.join('\n').replace(/^\s*\n/, '').replace(/\n{3,}/g, '\n\n').trim() + '\n';
		const extra = latest && EXTRA_LINES[u.slug];
		if (extra && src.exists(extra.needs)) {
			const paras = body.split(/\n\n/);
			const at = paras.findIndex((p) => !/^(#|\||!\[|```)/.test(p.trim()));
			paras.splice(at < 0 ? 0 : at + 1, 0, extra.text);
			body = paras.join('\n\n');
		}
		return {
			slug: u.slug,
			source: u.source,
			title: u.title.replace(/`/g, ''),
			description: description(u.lines),
			body,
			lastUpdated: src.lastUpdated(u.source),
		};
	});

	return { pages, images, warnings, slugs: allSlugs };
}

// ---------------------------------------------------------------------------------------------
// Output

function yamlValue(v, indent = '') {
	if (v === false || v === true || typeof v === 'number') return String(v);
	// An unquoted ISO 8601 timestamp is a YAML date, which Starlight's `lastUpdated` needs (a quoted one is a string).
	if (v instanceof Date) return v.toISOString();
	if (typeof v === 'string' && /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})$/.test(v)) return v;
	if (v && typeof v === 'object') {
		return (
			'\n' +
			Object.entries(v)
				.filter(([, x]) => x !== undefined && x !== null)
				.map(([k, x]) => `${indent}  ${k}: ${yamlValue(x, indent + '  ')}`)
				.join('\n')
		);
	}
	return JSON.stringify(String(v));
}

/** A page as a Markdown file with frontmatter. */
export function renderPage(page, frontmatter = {}) {
	const fm = { title: page.title, description: page.description || undefined, ...frontmatter };
	const head = Object.entries(fm)
		.filter(([, v]) => v !== undefined && v !== null)
		.map(([k, v]) => `${k}: ${yamlValue(v)}`)
		.join('\n');
	return `---\n${head}\n---\n\n<!-- Generated from ${page.source} by site/scripts. Edit the source, not this file. -->\n\n${page.body}`;
}

export function editUrl(source) {
	return `https://github.com/${REPO}/edit/main/${source}`;
}
