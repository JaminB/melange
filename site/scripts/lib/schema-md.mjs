// schema-md: renders a JSON Schema (draft 2020-12 subset) as Markdown field-reference tables.
//
// SHARED MODULE: a copy of this file lives in both sites (melange/site/scripts/lib/schema-md.mjs and
// melange-plugins/site/scripts/lib/schema-md.mjs). Keep the two copies identical; change one, copy it to the other.
// Plain ESM, no dependencies.
//
// renderSchema(schema, opts) returns Markdown (no frontmatter, no H1):
//   - the schema's description as an intro paragraph (opts.intro !== false);
//   - "Top level": a table (Property | Type | Required | Default | Description) for the root object;
//   - one section per nested inline object, array-of-objects item and oneOf/anyOf object variant, titled by its path
//     (`weapons[]`, `weapons[].text`, `objects[]` with `type` = "crate");
//   - "Definitions": one section per `$defs` entry; `$ref`s link to them, also across files.
// opts:
//   fileName     this schema's file name (resolves "<file>#/$defs/x" refs that point back at it)
//   schemas      { "<file name>": schema } for refs into other files
//   pageUrl(f)   URL of the rendered page for schema file f (links for cross-file refs); null/undefined = no link
//   headingLevel level of the top sections (default 2; nested definition sections get one more)

// Heading ids as github-slugger (what Astro uses) produces them, for the ASCII headings this module writes.
export function slugify(text) {
  return text
    .toLowerCase()
    .replace(/[^\p{L}\p{N}\p{M}\p{Pc} -]/gu, '')
    .replace(/ /g, '-');
}

function codeSpan(s) {
  const str = String(s);
  const runs = str.match(/`+/g);
  const n = runs ? Math.max(...runs.map((r) => r.length)) + 1 : 1;
  const fence = '`'.repeat(n);
  const pad = n > 1 || str.startsWith('`') || str.endsWith('`') ? ' ' : '';
  return `${fence}${pad}${str}${pad}${fence}`;
}

// Escapes prose for Markdown: `<` (would start raw HTML), `*` (emphasis); code spans are kept as they are.
function prose(s) {
  return String(s)
    .split(/(`+[^`]*?`+)/)
    .map((part, i) => (i % 2 ? part : part.replace(/</g, '&lt;').replace(/([*_])/g, '\\$1')))
    .join('');
}

// A table cell: one line, `|` escaped (GFM honours `\|` inside code spans too).
function cell(s) {
  return String(s ?? '').replace(/\r?\n/g, ' ').replace(/\|/g, '\\|');
}

function sentence(s) {
  const t = String(s).trim();
  if (!t) return '';
  return /[.!?:)]$|`$/.test(t) ? t : `${t}.`;
}

function json(v) {
  return codeSpan(JSON.stringify(v));
}

function jsonType(v) {
  if (v === null) return 'null';
  if (Array.isArray(v)) return 'array';
  if (Number.isInteger(v)) return 'integer';
  return typeof v;
}

function range(lo, hi, unit) {
  const u = (n) => (n === 1 ? unit.replace(/s$/, '') : unit);
  if (lo !== undefined && hi !== undefined) return lo === hi ? `Exactly ${lo} ${u(lo)}` : `${lo} to ${hi} ${unit}`;
  if (lo !== undefined) return `At least ${lo} ${u(lo)}`;
  if (hi !== undefined) return `At most ${hi} ${u(hi)}`;
  return null;
}

export function renderSchema(rootSchema, opts = {}) {
  const fileName = opts.fileName ?? '';
  const schemas = opts.schemas ?? {};
  const pageUrl = opts.pageUrl ?? (() => null);
  const H = opts.headingLevel ?? 2;

  const used = new Map();
  const takeSlug = (title) => {
    const base = slugify(title);
    const n = used.get(base) ?? 0;
    used.set(base, n + 1);
    return n ? `${base}-${n}` : base;
  };

  const pathSections = []; // {title, schema, doc, slug}
  const defTitle = (name) => `Definition: \`${name}\``;
  const defSlug = (name) => slugify(defTitle(name));

  const docOf = (file) => (file && file !== fileName ? schemas[file] : rootSchema);

  function resolve(ref, doc) {
    const [file, frag = ''] = ref.split('#');
    const target = file ? docOf(file) : doc;
    if (!target) return { schema: {}, doc, file, name: ref, missing: true };
    let node = target;
    for (const raw of frag.split('/').filter(Boolean)) {
      const key = raw.replace(/~1/g, '/').replace(/~0/g, '~');
      node = node?.[key];
    }
    const parts = frag.split('/').filter(Boolean);
    const isDef = parts.length === 2 && parts[0] === '$defs';
    return {
      schema: node ?? {},
      doc: target,
      file: file && file !== fileName ? file : null,
      name: isDef ? parts[1] : ref,
      isDef,
      missing: node === undefined,
    };
  }

  function refLink(ref, doc) {
    const r = resolve(ref, doc);
    if (r.missing || !r.isDef) return codeSpan(ref);
    if (r.file) {
      const url = pageUrl(r.file);
      return url ? `[${r.name}](${url}#${defSlug(r.name)})` : `${codeSpan(r.name)} (${codeSpan(r.file)})`;
    }
    return `[${r.name}](#${defSlug(r.name)})`;
  }

  const isObjectSchema = (s) => s && typeof s === 'object' && (s.type === 'object' || (!s.type && s.properties)) && s.properties;

  function variantLabel(v, i, doc) {
    const s = v.$ref ? resolve(v.$ref, doc).schema : v;
    for (const [k, p] of Object.entries(s.properties ?? {})) {
      if (p && p.const !== undefined) return `with \`${k}\` = ${JSON.stringify(p.const)}`;
    }
    return `variant ${i + 1}`;
  }

  function enqueue(title, schema, doc) {
    const slug = takeSlug(title);
    pathSections.push({ title, schema, doc, slug });
    return slug;
  }

  // Constraint notes for one schema node (not following $ref).
  function notes(s) {
    const out = [];
    if (s.const !== undefined) out.push(`Must be ${json(s.const)}.`);
    if (Array.isArray(s.enum)) out.push(`One of: ${s.enum.map(json).join(', ')}.`);
    if (s.pattern) out.push(`Pattern: ${codeSpan(s.pattern)}.`);
    if (s.format) out.push(`Format: ${s.format}.`);
    const len = range(s.minLength, s.maxLength, 'characters');
    if (len) out.push(`${len}.`);
    const lo = s.minimum ?? s.exclusiveMinimum;
    const hi = s.maximum ?? s.exclusiveMaximum;
    if (lo !== undefined || hi !== undefined) {
      const l = s.exclusiveMinimum !== undefined ? `more than ${lo}` : `${lo}`;
      const h = s.exclusiveMaximum !== undefined ? `less than ${hi}` : `${hi}`;
      if (lo !== undefined && hi !== undefined) out.push(`From ${l} to ${h}.`);
      else if (lo !== undefined) out.push(s.exclusiveMinimum !== undefined ? `More than ${lo}.` : `At least ${lo}.`);
      else out.push(s.exclusiveMaximum !== undefined ? `Less than ${hi}.` : `At most ${hi}.`);
    }
    if (s.multipleOf !== undefined) out.push(`A multiple of ${s.multipleOf}.`);
    const items = range(s.minItems, s.maxItems, 'items');
    if (items) out.push(`${items}.`);
    if (s.uniqueItems) out.push('Items are unique.');
    const props = range(s.minProperties, s.maxProperties, 'properties');
    if (props) out.push(`${props}.`);
    if (s.not) {
      if (s.not.const !== undefined) out.push(`Not ${json(s.not.const)}.`);
      else if (Array.isArray(s.not.enum)) out.push(`Not one of: ${s.not.enum.map(json).join(', ')}.`);
      else out.push('Must not match a sub-schema (see the source).');
    }
    return out;
  }

  // Describes a schema for a table row: {type, notes[], desc}. `label` is the path used for child sections.
  function describe(s, doc, label) {
    if (!s || typeof s !== 'object' || Object.keys(s).filter((k) => k !== 'description' && k !== 'default').length === 0)
      return { type: 'any', notes: [], desc: s?.description ?? null };
    const own = notes(s);
    const desc = s.description ?? null;

    if (s.$ref) {
      const r = resolve(s.$ref, doc);
      return { type: refLink(s.$ref, doc), notes: own, desc: desc ?? r.schema.description ?? null };
    }

    const alts = s.oneOf ?? s.anyOf;
    if (Array.isArray(alts)) {
      const allObjects = alts.every((v) => isObjectSchema(v.$ref ? resolve(v.$ref, doc).schema : v) && !v.$ref);
      if (allObjects && label) {
        const links = alts.map((v, i) => {
          const title = `${codeSpan(label)} ${variantLabel(v, i, doc)}`;
          const slug = enqueue(title, v, doc);
          return `[${variantLabel(v, i, doc).replace(/^with /, '')}](#${slug})`;
        });
        return { type: `one of: ${links.join(', ')}`, notes: own, desc };
      }
      const parts = alts.map((v, i) => compact(v, doc, label ? `${label}` : null, i));
      return { type: [...new Set(parts)].join(' or '), notes: own, desc };
    }

    if (s.const !== undefined && !s.type) return { type: jsonType(s.const), notes: own, desc };
    if (Array.isArray(s.enum) && !s.type) {
      return { type: [...new Set(s.enum.map(jsonType))].join(' or '), notes: own, desc };
    }

    const types = Array.isArray(s.type) ? s.type : [s.type ?? (s.properties ? 'object' : s.items || s.prefixItems ? 'array' : undefined)];
    if (types.length > 1) return { type: types.join(' or '), notes: own, desc };
    const t = types[0];

    if (t === 'object') {
      if (s.properties && label) {
        const slug = enqueue(codeSpan(label), s, doc);
        return { type: `[object](#${slug})`, notes: own, desc };
      }
      if (s.additionalProperties && typeof s.additionalProperties === 'object') {
        const inner = describe(s.additionalProperties, doc, label ? `${label}.*` : null);
        const valueNotes = inner.notes.length ? [`Values: ${inner.notes.join(' ')}`] : [];
        return { type: `object of ${inner.type}`, notes: [...own, ...valueNotes], desc };
      }
      return { type: 'object', notes: own, desc };
    }

    if (t === 'array') {
      if (Array.isArray(s.prefixItems)) {
        const parts = s.prefixItems.map((p) => describe(p, doc, null));
        const tuple = `[${parts.map((p) => p.type).join(', ')}]`;
        const pos = parts.map((p, i) => {
          const bits = [p.desc ? sentence(prose(p.desc)) : '', ...p.notes].filter(Boolean).join(' ');
          return bits ? `Item ${i + 1}: ${bits}` : '';
        });
        return { type: `array ${tuple}`, notes: [...own, ...pos.filter(Boolean)], desc };
      }
      if (s.items && typeof s.items === 'object') {
        const inner = describe(s.items, doc, label ? `${label}[]` : null);
        const itemNotes = inner.notes.length ? [`Items: ${inner.notes.join(' ')}`] : [];
        const itemDesc = inner.desc && inner.desc !== desc && s.items.$ref ? [`Items: ${sentence(prose(inner.desc))}`] : [];
        return { type: `array of ${inner.type}`, notes: [...own, ...itemNotes, ...itemDesc], desc };
      }
      return { type: 'array', notes: own, desc };
    }

    return { type: t ?? 'any', notes: own, desc };
  }

  // A short inline form for one anyOf/oneOf alternative.
  function compact(v, doc, label, i) {
    if (v.$ref) return refLink(v.$ref, doc);
    if (v.const !== undefined) return json(v.const);
    if (v.type === 'null') return 'null';
    const d = describe(v, doc, label ? `${label} (${i + 1})` : null);
    return d.notes.length ? `${d.type} (${d.notes.join(' ').replace(/\.$/, '')})` : d.type;
  }

  function table(s, doc, label) {
    const props = s.properties ?? {};
    const req = new Set(s.required ?? []);
    const rows = ['| Property | Type | Required | Default | Description |', '|---|---|---|---|---|'];
    for (const [name, p] of Object.entries(props)) {
      const childLabel = label ? `${label}.${name}` : name;
      const d = describe(p, doc, childLabel);
      const text = [d.desc ? sentence(prose(d.desc)) : '', ...d.notes].filter(Boolean).join(' ');
      const def = p && p.default !== undefined ? json(p.default) : '';
      rows.push(`| ${codeSpan(name)} | ${cell(d.type)} | ${req.has(name) ? 'Yes' : ''} | ${cell(def)} | ${cell(text)} |`);
    }
    if (s.additionalProperties && typeof s.additionalProperties === 'object') {
      const d = describe(s.additionalProperties, doc, label ? `${label}.*` : '*');
      const text = [d.desc ? sentence(prose(d.desc)) : '', ...d.notes].filter(Boolean).join(' ');
      rows.push(`| *(any other name)* | ${cell(d.type)} |  |  | ${cell(text)} |`);
    }
    return rows.join('\n');
  }

  function objectBody(s, doc, label, { withDesc = true } = {}) {
    const out = [];
    if (withDesc && s.description) out.push(sentence(prose(s.description)));
    const extra = notes(s);
    if (extra.length) out.push(extra.join(' '));
    if (Object.keys(s.properties ?? {}).length) out.push(table(s, doc, label));
    else out.push('No properties.');
    return out.join('\n\n');
  }

  const h = (lvl) => '#'.repeat(lvl);
  const out = [];
  if (opts.intro !== false && rootSchema.description) out.push(prose(rootSchema.description));

  // Reserve the fixed headings first so path sections never take their slugs.
  const topTitle = 'Top level';
  const defsTitle = 'Definitions';
  takeSlug(topTitle);
  const defs = rootSchema.$defs ?? {};
  if (Object.keys(defs).length) takeSlug(defsTitle);
  for (const name of Object.keys(defs)) takeSlug(defTitle(name));

  if (isObjectSchema(rootSchema)) {
    out.push(`${h(H)} ${topTitle}`, objectBody(rootSchema, rootSchema, null, { withDesc: false }));
  } else {
    const d = describe(rootSchema, rootSchema, '(root)');
    out.push(`${h(H)} ${topTitle}`, `Type: ${d.type}. ${d.notes.join(' ')}`.trim());
  }

  // Path sections (the queue grows while it is drained). Definitions add their own nested sections too.
  const drain = (lvl) => {
    while (pathSections.length) {
      const sec = pathSections.shift();
      const title = sec.title;
      const labelMatch = /^`([^`]+)`/.exec(title);
      out.push(`${h(lvl)} ${title}`, objectBody(sec.schema, sec.doc, labelMatch ? labelMatch[1] : null));
    }
  };
  drain(H);

  if (Object.keys(defs).length) {
    out.push(`${h(H)} ${defsTitle}`);
    for (const [name, d] of Object.entries(defs)) {
      out.push(`${h(H + 1)} ${defTitle(name)}`);
      const alts = d.oneOf ?? d.anyOf;
      if (isObjectSchema(d)) {
        out.push(objectBody(d, rootSchema, name));
      } else if (Array.isArray(alts) && alts.every((v) => isObjectSchema(v))) {
        const x = describe(d, rootSchema, name);
        out.push([x.desc ? sentence(prose(x.desc)) : '', `One of: ${x.type.replace(/^one of: /, '')}.`, ...x.notes].filter(Boolean).join(' '));
      } else {
        const x = describe(d, rootSchema, name);
        out.push([x.desc ? sentence(prose(x.desc)) : '', `Type: ${x.type}.`, ...x.notes].filter(Boolean).join(' '));
      }
      drain(H + 1);
    }
  }

  return out.join('\n\n') + '\n';
}
