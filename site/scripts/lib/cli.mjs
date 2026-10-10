// Shared helpers for the site's prebuild scripts (plain Node, no dependencies).
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

export const SITE_DIR = fileURLToPath(new URL('../../', import.meta.url));
export const REPO_DIR = path.resolve(SITE_DIR, '..');

// GitHub repository the site documents, used for edit and source links.
export const REPO_SLUG = 'JaminB/melange';
export const REPO_URL = `https://github.com/${REPO_SLUG}`;

// True when the module at `metaUrl` is the script node was started with (not imported by prebuild.mjs).
export function isMain(metaUrl) {
  return Boolean(process.argv[1]) && metaUrl === pathToFileURL(path.resolve(process.argv[1])).href;
}

// Runs a script's default export when it is invoked directly: errors print and exit 1.
export function runIfMain(metaUrl, fn) {
  if (!isMain(metaUrl)) return;
  fn().catch((err) => {
    console.error(err && err.message ? err.message : err);
    process.exitCode = 1;
  });
}

// Forward slashes, relative to the repo root, for messages.
export function rel(p) {
  return path.relative(REPO_DIR, p).split(path.sep).join('/');
}
