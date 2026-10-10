# Melange documentation site

The source of <https://jaminb.github.io/melange/>: an [Astro Starlight](https://starlight.astro.build/) site with a
download-first landing page and the Melange docs, including the docs of every release from v0.8.0 on.

**npm runs here and nowhere else.** Melange itself never uses npm. This site is built only by
`.github/workflows/pages.yml` (on every push to `main`, at the end of `release.yml`, and as a build-only check on pull
requests), from `package-lock.json`, and nothing it installs is ever shipped in a release.

## Run it locally

```sh
cd site
npm ci
npm run dev          # generates the content, then serves http://localhost:4321/melange/
```

| Command | What it does |
|---|---|
| `npm run build` | `scripts/prebuild.mjs`, then `astro build` into `dist/` |
| `npm run check` | fails if a `wum.*` function registered in `../src/lua/` is missing from `../docs/lua-api.md` |
| `npm run check:links` | after a build: every `/melange/...` link in `dist/` resolves to a file |
| `npm run preview` | serves `dist/` |

## Where things come from

`../docs/*.md`, `../README.md` and `../CODE_SIGNING.md` are the source of truth. Edit them, never the generated pages.
`scripts/prebuild.mjs` writes everything below on each build; all of it is gitignored:

- `src/content/docs/*.md`: the latest docs (`sync-docs.mjs`; README is split into pages) and `changelog.md` (`releases.mjs`, from GitHub Releases)
- `src/content/docs/v<version>/`: the docs at each release tag (`versions.mjs`; CI checks out with full history for the tags)
- `src/content/docs/reference/schema/`: pages from `../docs/*.schema.json` (`schemas.mjs`)
- `src/data/`: `versions.json`, `releases.json`, `schemas.json`
- `public/images/`: `../docs/images/` (and `v<version>/` for each tag)

Hand-written parts: `astro.config.mjs`, `scripts/` (the sidebar layout is `scripts/sidebar.mjs`), `src/pages/index.astro`
(landing page), `src/components/` (Header, Footer, Hero, MobileMenuFooter overrides and the version picker),
`src/route-data.ts` (one sidebar per docs version), `src/styles/theme.css` (the theme shared with the plugins site; keep
the two copies identical) and `src/styles/site.css` (this site's layout glue), `src/assets/` and `public/favicon.svg`.
