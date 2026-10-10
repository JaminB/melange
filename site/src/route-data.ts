// Starlight route middleware: one sidebar per docs version.
//
// Register it in astro.config.mjs:
//   starlight({ routeMiddleware: './src/route-data.ts', sidebar, ... })
// (Starlight imports the named `onRequest` export; the default export is the same function.)
//
// scripts/sidebar.mjs builds the latest sidebar followed by one collapsed group per versioned
// snapshot, `{label: 'v0.8.0', collapsed: true, items: [{autogenerate: {directory: 'v0.8.0'}}]}` (in route data: a group labelled 'v0.8.0'). Here:
// - on a latest page, the version groups are removed;
// - on a versioned page (id `v<x.y.z>/...`), the sidebar becomes the latest layout with every link
//   pointed at that version's copy of the page (labels unprefixed, the latest labels), entries the
//   version lacks dropped, and pages only the version has listed under "More".
// Prev/next links are recomputed from the new sidebar, unless the page sets `prev`/`next` itself.

import { defineRouteMiddleware, type StarlightRouteData } from '@astrojs/starlight/route-data';

type Entry = StarlightRouteData['sidebar'][number];
type Link = Extract<Entry, { type: 'link' }>;
type Group = Extract<Entry, { type: 'group' }>;

const VERSION_RE = /^v\d+\.\d+\.\d+$/;
const VERSION_ID_RE = /^(v\d+\.\d+\.\d+)(?:\/|$)/;
// Matches astro.config.mjs (`pagination: true`).
const PAGINATION = true;

const BASE = (import.meta.env.BASE_URL ?? '/').replace(/\/+$/, '');

function groupDir(e: Entry): string | undefined {
	if (e.type !== 'group') return undefined;
	const auto = (e as { autogenerate?: { directory: string } }).autogenerate;
	return auto?.directory ?? e.label;
}

function isVersionGroup(e: Entry): e is Group {
	return e.type === 'group' && VERSION_RE.test(groupDir(e) ?? '');
}

function flatten(entries: Entry[]): Link[] {
	return entries.flatMap((e) => (e.type === 'link' ? [e] : flatten(e.entries)));
}

/** "/melange/v0.8.0/install/" -> "install" (version prefix removed when given). */
function slugOf(href: string, version?: string): string {
	let p = href.split('#')[0];
	if (BASE && p.startsWith(BASE + '/')) p = p.slice(BASE.length);
	p = p.replace(/^\/+|\/+$/g, '');
	if (version && (p === version || p.startsWith(version + '/'))) p = p.slice(version.length).replace(/^\/+/, '');
	return p;
}

function versionSidebar(latest: Entry[], group: Group | undefined, version: string): Entry[] {
	const links = new Map(flatten(group?.entries ?? []).map((l) => [slugOf(l.href, version), l]));
	const used = new Set<string>();
	const map = (entries: Entry[]): Entry[] =>
		entries.flatMap((e): Entry[] => {
			if (e.type === 'link') {
				const s = slugOf(e.href);
				const v = links.get(s);
				if (!v) return [];
				used.add(s);
				return [{ ...v, label: e.label, badge: e.badge }];
			}
			const sub = map(e.entries);
			return sub.length ? [{ ...e, entries: sub } as Group] : [];
		});
	const out = map(latest);
	const rest = [...links].filter(([s]) => !used.has(s)).map(([, l]) => l);
	if (rest.length) out.push({ type: 'group', label: 'More', entries: rest, collapsed: false, badge: undefined });
	return out;
}

export const onRequest = defineRouteMiddleware((context) => {
	const route = context.locals.starlightRoute;
	const latest = route.sidebar.filter((e) => !isVersionGroup(e));
	const m = VERSION_ID_RE.exec(route.id);
	if (m) {
		const group = route.sidebar.filter(isVersionGroup).find((g) => groupDir(g) === m[1]);
		route.sidebar = versionSidebar(latest, group, m[1]);
	} else {
		route.sidebar = latest;
	}

	if (!PAGINATION) return;
	const data = route.entry.data as { prev?: unknown; next?: unknown };
	const flat = flatten(route.sidebar);
	const i = flat.findIndex((l) => l.isCurrent);
	if (data.prev === undefined) route.pagination.prev = i > 0 ? flat[i - 1] : undefined;
	if (data.next === undefined) route.pagination.next = i >= 0 && i + 1 < flat.length ? flat[i + 1] : undefined;
});

export default onRequest;
