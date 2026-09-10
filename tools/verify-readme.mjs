#!/usr/bin/env node

// Keep navigation and release-facing facts in sync without requiring a product build.
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const read = (path) => readFileSync(resolve(root, path), 'utf8');
const documents = ['README.md', 'README.en.md', 'docs/DEVELOPMENT.md', 'docs/DEVELOPMENT.en.md',
  'ARCHITECTURE.md', 'docs/adr/0009-identity-installer-scheme-c.md'];
const prose = (text) => text.replace(/^\s*```[^\n]*\n[\s\S]*?^\s*```\s*$/gm, '');

function anchors(text)
{
  const seen = new Map();
  return [...prose(text).matchAll(/^#{1,6}\s+(.+)$/gm)].map((match) =>
  {
    const base = match[1].trim().toLowerCase().replace(/[^\p{L}\p{M}\p{N}_\-\s]/gu, '').replace(/\s/g, '-');
    const count = seen.get(base) ?? 0;
    seen.set(base, count + 1);
    return count ? `${base}-${count}` : base;
  });
}

for (const file of documents)
{
  const body = prose(read(file));
  for (const match of body.matchAll(/\]\(([^\s)]+)\)/g))
  {
    const url = match[1];
    if (/^https?:\/\//.test(url))
    {
      new URL(url);
      continue;
    }
    const [path, fragment] = url.split('#');
    const target = path ? resolve(root, dirname(file), decodeURIComponent(path)) : resolve(root, file);
    assert.ok(existsSync(target), `${file}: missing link target ${url}`);
    if (fragment)
    {
      assert.ok(anchors(readFileSync(target, 'utf8')).includes(decodeURIComponent(fragment)),
        `${file}: missing anchor ${url}`);
    }
  }
}

const version = read('cmake/Version.cmake').match(/set\(BAFX_VERSION "([^"]+)"\)/)[1];
const presets = new Set(JSON.parse(read('CMakePresets.json')).workflowPresets.map((preset) => preset.name));
const readmes = documents.slice(0, 2).map(read);
for (const [index, body] of readmes.entries())
{
  assert.equal(body.split(version).length - 1, 1, `${documents[index]}: keep one current product version`);
  assert.match(body, new RegExp(`(?:当前产品版本|Current product version)[^\\n]*${version.replaceAll('.', '\\.')}`));
  for (const token of ['releases/latest', '*-Portable-windows-x64.zip', '*-setup-windows-x64.exe',
    '.sha256', 'Full', 'Slim', 'Not Run', 'FX-only', 'INSTALL-STATE.json', 'Premultiplied Alpha',
    '`Default`', '`Normal`'])
  {
    assert.ok(body.includes(token), `${documents[index]}: missing release guidance ${token}`);
  }
  for (const match of body.matchAll(/cmake --workflow --preset ([\w-]+)/g))
  {
    assert.ok(presets.has(match[1]), `Unknown workflow preset ${match[1]}`);
  }
}
assert.equal(readmes[0].match(/^## /gm).length, readmes[1].match(/^## /gm).length,
  'Keep the Chinese and English section structure aligned');
console.log('README checks passed: local links, anchors, version, release guidance, and build presets.');
