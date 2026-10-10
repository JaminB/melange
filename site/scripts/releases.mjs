// releases: reads Melange's GitHub releases for the landing page's download button and the changelog.
//   src/data/releases.json            {latest: {version, tag, url, size, sha256, publishedAt, notes, htmlUrl} | null,
//                                      all: [{version, tag, publishedAt, notes, url, prerelease}]}
//   src/content/docs/changelog.md     one H2 per release, newest first, notes verbatim (generated, gitignored)
// Uses GITHUB_TOKEN when set (CI passes github.token; unauthenticated calls are rate-limited to 60/hour).
// Network trouble is a WARNING, not an error: releases.json becomes {latest: null, all: []} and the landing page
// falls back to a plain "Download from GitHub Releases" button. MELANGE_SITE_OFFLINE=1 simulates that.
import fs from 'node:fs/promises';
import path from 'node:path';
import { REPO_SLUG, REPO_URL, SITE_DIR, rel, runIfMain } from './lib/cli.mjs';

const API = `https://api.github.com/repos/${REPO_SLUG}/releases?per_page=100`;
const RELEASES_PAGE = `${REPO_URL}/releases`;
const DATA_FILE = path.join(SITE_DIR, 'src', 'data', 'releases.json');
const CHANGELOG = path.join(SITE_DIR, 'src', 'content', 'docs', 'changelog.md');
const TIMEOUT_MS = 20000;

const warn = (msg) => console.warn(`WARN releases: ${msg}`);

async function getJson(url, { auth }) {
  if (process.env.MELANGE_SITE_OFFLINE === '1') throw new Error('offline (MELANGE_SITE_OFFLINE=1)');
  const headers = { Accept: 'application/vnd.github+json', 'User-Agent': 'melange-site-build' };
  if (auth && process.env.GITHUB_TOKEN) {
    headers.Authorization = `Bearer ${process.env.GITHUB_TOKEN}`;
    headers['X-GitHub-Api-Version'] = '2022-11-28';
  }
  const res = await fetch(url, { headers, signal: AbortSignal.timeout(TIMEOUT_MS), redirect: 'follow' });
  if (!res.ok) throw new Error(`GET ${url}: HTTP ${res.status} ${res.statusText}`);
  return res.json();
}

const versionOf = (tag) => String(tag).replace(/^v/, '');

function normaliseNotes(body) {
  return String(body ?? '').replace(/\r\n?/g, '\n').trim();
}

// The zip's SHA-256: from the melange-<v>.json manifest asset, else the "SHA-256 of melange-x.zip: `...`" body line.
async function sha256For(release, zip) {
  const manifest = release.assets.find((a) => a.name === zip.name.replace(/\.zip$/, '.json'));
  if (manifest) {
    try {
      const m = await getJson(manifest.browser_download_url, { auth: false });
      if (m && /^[0-9a-f]{64}$/i.test(m.sha256 ?? '') && (!m.zip || m.zip === zip.name)) return m.sha256.toLowerCase();
      warn(`${manifest.name} has no usable sha256 for ${zip.name}; using the release notes`);
    } catch (err) {
      warn(`cannot read ${manifest.name} (${err.message}); using the release notes`);
    }
  }
  const esc = zip.name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const m = new RegExp(`SHA-256 of ${esc}:\\s*\`?([0-9a-f]{64})`, 'i').exec(release.body ?? '');
  if (m) return m[1].toLowerCase();
  warn(`no SHA-256 found for ${zip.name}`);
  return null;
}

// Release notes go under an H2 per release, so their own H1/H2 headings move down one level.
function demoteHeadings(md) {
  let fence = null;
  return md
    .split('\n')
    .map((line) => {
      const f = /^\s*(```+|~~~+)/.exec(line);
      if (f) {
        if (!fence) fence = f[1][0];
        else if (f[1][0] === fence) fence = null;
        return line;
      }
      if (fence) return line;
      return line.replace(/^(#{1,5})(\s)/, '#$1$2');
    })
    .join('\n');
}

function changelogMarkdown(data, offline) {
  const front = [
    '---',
    'title: Changelog',
    `description: ${JSON.stringify('Every Melange release, newest first, with its release notes.')}`,
    'editUrl: false',
    '---',
    '',
  ].join('\n');
  if (!data.all.length) {
    return (
      `${front}\n` +
      `${offline ? 'The release list could not be loaded when this site was built.' : 'No releases were found when this site was built.'} ` +
      `See [GitHub Releases](${RELEASES_PAGE}) for every Melange version and its notes.\n`
    );
  }
  const parts = [
    front.trimEnd(),
    `Every Melange release, newest first. Downloads and checksums are also on [GitHub Releases](${RELEASES_PAGE}).`,
  ];
  for (const r of data.all) {
    const date = r.publishedAt ? r.publishedAt.slice(0, 10) : 'unreleased';
    const tags = r.prerelease ? ' (pre-release)' : '';
    parts.push(`## ${r.version}${tags}`);
    parts.push(`Released ${date}. [Release page](${r.url})`);
    if (r.notes) parts.push(demoteHeadings(r.notes));
  }
  return parts.join('\n\n') + '\n';
}

export default async function releases() {
  let data = { latest: null, all: [] };
  let offline = false;
  try {
    const list = await getJson(API, { auth: true });
    if (!Array.isArray(list)) throw new Error('unexpected response from the releases API');
    const published = list
      .filter((r) => !r.draft)
      .sort((a, b) => String(b.published_at ?? '').localeCompare(String(a.published_at ?? '')));
    data.all = published.map((r) => ({
      version: versionOf(r.tag_name),
      tag: r.tag_name,
      publishedAt: r.published_at,
      notes: normaliseNotes(r.body),
      url: r.html_url,
      prerelease: Boolean(r.prerelease),
    }));
    const stable = published.find((r) => !r.prerelease);
    if (!stable) {
      warn('no published (non-draft, non-prerelease) release found');
    } else {
      const zip = stable.assets.find((a) => /^melange-.+\.zip$/.test(a.name));
      if (!zip) {
        warn(`release ${stable.tag_name} has no melange-<version>.zip asset; the landing page will link to GitHub Releases`);
      } else {
        data.latest = {
          version: versionOf(stable.tag_name),
          tag: stable.tag_name,
          url: zip.browser_download_url,
          size: zip.size,
          sha256: await sha256For(stable, zip),
          publishedAt: stable.published_at,
          notes: normaliseNotes(stable.body),
          htmlUrl: stable.html_url,
        };
      }
    }
  } catch (err) {
    offline = true;
    data = { latest: null, all: [] };
    warn(`cannot read GitHub releases (${err.message}); writing {latest: null, all: []}`);
  }

  await fs.mkdir(path.dirname(DATA_FILE), { recursive: true });
  await fs.writeFile(DATA_FILE, `${JSON.stringify(data, null, 2)}\n`, 'utf8');
  await fs.mkdir(path.dirname(CHANGELOG), { recursive: true });
  await fs.writeFile(CHANGELOG, changelogMarkdown(data, offline), 'utf8');

  const l = data.latest;
  console.log(
    l
      ? `releases: latest ${l.version} (${l.size} bytes, sha256 ${l.sha256 ? l.sha256.slice(0, 12) + '...' : 'unknown'}), ${data.all.length} release(s) -> ${rel(DATA_FILE)}, ${rel(CHANGELOG)}`
      : `releases: no release data${offline ? ' (offline)' : ''}; wrote fallback ${rel(DATA_FILE)}, ${rel(CHANGELOG)}`,
  );
  return data;
}

runIfMain(import.meta.url, releases);
