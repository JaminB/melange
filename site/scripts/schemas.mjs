// schemas: renders every docs/*.schema.json as a field-reference page.
//   src/content/docs/reference/schema/<name>.md   (generated, gitignored; the folder is cleaned first)
//   src/data/schemas.json                         [{file, name, title, url, prose}] for the sync step's link map
//                                                  (e.g. a "Field reference" line on spice.md)
// The renderer is scripts/lib/schema-md.mjs, shared with the plugins site.
import fs from 'node:fs/promises';
import path from 'node:path';
import { REPO_DIR, REPO_URL, SITE_DIR, rel, runIfMain } from './lib/cli.mjs';
import { renderSchema } from './lib/schema-md.mjs';

const DOCS_DIR = path.join(REPO_DIR, 'docs');
const OUT_DIR = path.join(SITE_DIR, 'src', 'content', 'docs', 'reference', 'schema');
const DATA_FILE = path.join(SITE_DIR, 'src', 'data', 'schemas.json');
const BASE = '/melange';

const nameOf = (file) => file.replace(/\.schema\.json$/, '');
const pageUrl = (file) => `${BASE}/reference/schema/${nameOf(file)}/`;

function truncate(s, n) {
  const t = String(s).replace(/\s+/g, ' ').trim();
  if (t.length <= n) return t;
  const cut = t.slice(0, n - 3);
  const sp = cut.lastIndexOf(' ');
  return `${sp > n / 2 ? cut.slice(0, sp) : cut}...`;
}

// The prose page that explains a schema: spice-1 -> spice.md, erg-scene-2 -> erg.md.
async function proseFor(name) {
  const stem = name.replace(/-\d+$/, '');
  for (const cand of [stem, stem.split('-')[0]]) {
    try {
      await fs.access(path.join(DOCS_DIR, `${cand}.md`));
      return cand;
    } catch {
      /* try the next candidate */
    }
  }
  return null;
}

export default async function schemas() {
  const files = (await fs.readdir(DOCS_DIR)).filter((f) => f.endsWith('.schema.json')).sort();
  const parsed = {};
  for (const f of files) {
    try {
      parsed[f] = JSON.parse(await fs.readFile(path.join(DOCS_DIR, f), 'utf8'));
    } catch (err) {
      throw new Error(`schemas: docs/${f} is not valid JSON: ${err.message}`);
    }
  }

  await fs.rm(OUT_DIR, { recursive: true, force: true });
  await fs.mkdir(OUT_DIR, { recursive: true });

  const index = [];
  for (const f of files) {
    const schema = parsed[f];
    const name = nameOf(f);
    const title = schema.title || name;
    const prose = await proseFor(name);
    const description = truncate(schema.description || `Field reference for ${f}.`, 160);
    const front = [
      '---',
      `title: ${JSON.stringify(title)}`,
      `description: ${JSON.stringify(description)}`,
      `editUrl: ${JSON.stringify(`${REPO_URL}/edit/main/docs/${f}`)}`,
      '---',
      '',
    ].join('\n');
    const src = `Generated from [\`docs/${f}\`](${REPO_URL}/blob/main/docs/${f})${schema.$id ? ` (\`${schema.$id}\`)` : ''}.` +
      (prose ? ` The guide is [${prose}.md](${BASE}/${prose}/).` : '');
    const body = renderSchema(schema, { fileName: f, schemas: parsed, pageUrl, headingLevel: 2 });
    await fs.writeFile(path.join(OUT_DIR, `${name}.md`), `${front}\n${src}\n\n${body}`, 'utf8');
    index.push({ file: `docs/${f}`, name, title, url: pageUrl(f), prose: prose ? `${prose}.md` : null });
  }

  await fs.mkdir(path.dirname(DATA_FILE), { recursive: true });
  await fs.writeFile(DATA_FILE, `${JSON.stringify(index, null, 2)}\n`, 'utf8');
  console.log(`schemas: ${files.length} schema page(s) written to ${rel(OUT_DIR)}`);
  return index;
}

runIfMain(import.meta.url, schemas);
