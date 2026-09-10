// Adapted from https://github.com/CialloKing/ba-click-fx
// Copyright (c) 2026 CialloKing
// SPDX-License-Identifier: MIT; full notice in THIRD-PARTY-NOTICES.txt.

/**
 * Star 历史数据、GitHub API 与 SVG 生成契约测试。
 */

import assert from 'node:assert/strict';
import { mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  fetchCurrentStargazerDates,
  formatHistoryDate,
  parseStarHistoryCsv,
  reconstructStarHistory,
  renderStarHistorySvg,
  serializeStarHistoryCsv,
  updateStarHistory,
  upsertObservedStarCount,
} from '../tools/update-star-history.mjs';

function check(condition, message)
{
  assert.ok(condition, message);
  console.log(`  ✓ ${message}`);
}

function makeResponse(payload, status = 200)
{
  return new Response(JSON.stringify(payload),
    {
      headers: { 'content-type': 'application/json' },
      status,
    });
}

function makeRuntimeOptions(overrides = {})
{
  return {
    apiUrl: 'https://api.github.test',
    fetchImpl: async () => makeResponse([]),
    now: new Date('2026-09-02T19:17:00.000Z'),
    repository: 'CialloKing/ba-click-fx-desktop',
    token: 'test-token',
    ...overrides,
  };
}

console.log('\nStar API 分页与上海日期');
const requestedPages = [];
const firstPage = Array.from({ length: 100 }, (_, index) =>
  ({ starred_at: '2026-09-02T15:59:59.000Z', user: { id: index + 1 } }));
const secondPage = [
  { starred_at: '2026-09-02T16:00:00.000Z', user: { id: 101 } },
];
const pagedDates = await fetchCurrentStargazerDates(makeRuntimeOptions(
  {
    fetchImpl: async (url, init) =>
    {
      const page = Number(url.searchParams.get('page'));

      assert.equal(init.headers.Authorization, 'Bearer test-token');
      assert.equal(init.headers.Accept, 'application/vnd.github.star+json');
      assert.equal(init.headers['X-GitHub-Api-Version'], '2026-03-10');
      requestedPages.push(page);
      return makeResponse(page === 1 ? firstPage : secondPage);
    },
  }));

assert.deepEqual(requestedPages, [1, 2]);
assert.equal(pagedDates.length, 101);
check(
  formatHistoryDate(pagedDates[0]) === '2026-09-02' &&
    formatHistoryDate(pagedDates.at(-1)) === '2026-09-03',
  'Stargazers API 自动分页并按 Asia/Shanghai 划分日期',
);

console.log('\n回溯重建与每日观测');
const reconstructed = reconstructStarHistory(
  [
    '2026-08-30T15:59:59.000Z',
    '2026-08-30T16:00:00.000Z',
    '2026-09-01T16:00:00.000Z',
    '2026-09-02T18:00:00.000Z',
  ],
  '2026-09-03',
);

assert.deepEqual(reconstructed,
  [
    { date: '2026-08-30', stars: 1, source: 'reconstructed', observedAt: '' },
    { date: '2026-08-31', stars: 2, source: 'reconstructed', observedAt: '' },
    { date: '2026-09-01', stars: 2, source: 'reconstructed', observedAt: '' },
    { date: '2026-09-02', stars: 3, source: 'reconstructed', observedAt: '' },
  ],
);

const firstObservation = upsertObservedStarCount(reconstructed,
  {
    date: '2026-09-03',
    observedAt: '2026-09-02T19:17:00.000Z',
    stars: 55,
  });
const repeatedObservation = upsertObservedStarCount(firstObservation.rows,
  {
    date: '2026-09-03',
    observedAt: '2026-09-02T20:17:00.000Z',
    stars: 55,
  });

assert.equal(firstObservation.changed, true);
assert.equal(repeatedObservation.changed, false);
assert.equal(
  serializeStarHistoryCsv(repeatedObservation.rows),
  serializeStarHistoryCsv(firstObservation.rows),
);

const decreasedObservation = upsertObservedStarCount(firstObservation.rows,
  {
    date: '2026-09-04',
    observedAt: '2026-09-03T19:17:00.000Z',
    stars: 54,
  });

assert.equal(decreasedObservation.rows.at(-1).stars, 54);

const changedSameDay = upsertObservedStarCount(firstObservation.rows,
  { date: '2026-09-03', stars: 53, observedAt: '2026-09-03T02:00:00.000Z' });
assert.equal(changedSameDay.rows.length, firstObservation.rows.length);
assert.equal(changedSameDay.rows.at(-1).stars, 53);
const afterMissedDay = upsertObservedStarCount(firstObservation.rows,
  { date: '2026-09-05', stars: 56, observedAt: '2026-09-04T19:17:00.000Z' });
assert.equal(afterMissedDay.rows.some((row) => row.date === '2026-09-04'), false);

console.log('\nCSV 与 SVG 合同');
const csv = serializeStarHistoryCsv(decreasedObservation.rows);

assert.deepEqual(parseStarHistoryCsv(csv), decreasedObservation.rows);
for (const invalidCsv of [
  'date,stars\n2026-09-03,55\n',
  'date,stars,source,observed_at\n2026-02-30,1,reconstructed,\n',
  'date,stars,source,observed_at\n2026-09-03,-1,reconstructed,\n',
  'date,stars,source,observed_at\n2026-09-03,1,estimated,\n',
  'date,stars,source,observed_at\n2026-09-03,1,reconstructed,2026-09-02T19:17:00.000Z\n',
  'date,stars,source,observed_at\n2026-09-03,1,observed,\n',
  'date,stars,source,observed_at\n2026-09-01,2,reconstructed,\n2026-09-02,1,reconstructed,\n',
  'date,stars,source,observed_at\n2026-09-01,1,reconstructed,\n2026-09-03,2,reconstructed,\n',
])
{
  assert.throws(() => parseStarHistoryCsv(invalidCsv));
}

const firstSvg = renderStarHistorySvg(decreasedObservation.rows, 'CialloKing/ba-click-fx-desktop');
const secondSvg = renderStarHistorySvg(decreasedObservation.rows, 'CialloKing/ba-click-fx-desktop');

assert.equal(firstSvg, secondSvg);
assert.match(firstSvg, /width="960" height="480" viewBox="0 0 720 360"/);
const historyPaths = firstSvg.match(/<path data-series="stars" d="([^"]+)"/g) ?? [];

assert.equal(historyPaths.length, 1);
assert.equal(historyPaths[0].match(/\bL\b/g)?.length, decreasedObservation.rows.length - 1);
assert.doesNotMatch(firstSvg, /stroke-dasharray|data-source=|reconstruct|observ/i);
assert.match(firstSvg, /text-anchor="start"[^>]*>2026-08-30<\/text>/);
assert.match(firstSvg, /text-anchor="end"[^>]*>2026-09-04<\/text>/);

const singlePointSvg = renderStarHistorySvg(
  [{ date: '2026-09-02', stars: 1, source: 'reconstructed', observedAt: '' }],
  'CialloKing/ba-click-fx-desktop',
);

assert.match(singlePointSvg, /<circle data-series="stars"/);

console.log('\n文件级生成与幂等');
const generatedDirectory = mkdtempSync(join(tmpdir(), 'ba-click-fx-star-generated-'));

try
{
  const bootstrapResult = await updateStarHistory(makeRuntimeOptions(
    {
      bootstrap: true,
      dataDir: generatedDirectory,
      fetchImpl: async (url) => makeResponse(url.pathname.endsWith('/stargazers') ?
        [
          { starred_at: '2026-09-01T10:00:00Z', user: { id: 1 } },
          { starred_at: '2026-09-02T15:00:00Z', user: { id: 2 } },
        ] : { stargazers_count: 2 }),
    }));

  assert.equal(bootstrapResult.changed, true);
  assert.deepEqual(bootstrapResult.changedFiles.sort(),
    ['README.md', 'star-history.svg', 'stars.csv']);
  assert.match(readFileSync(join(generatedDirectory, 'README.md'), 'utf8'), /cannot recover removed Stars/);
  assert.deepEqual(bootstrapResult.rows.at(-1),
    { date: '2026-09-03', stars: 2, source: 'observed', observedAt: '2026-09-02T19:17:00.000Z' });
  await assert.rejects(updateStarHistory(makeRuntimeOptions({ bootstrap: true, dataDir: generatedDirectory })),
    /refuses to overwrite/);

  const observeOptions = makeRuntimeOptions(
    {
      dataDir: generatedDirectory,
      fetchImpl: async () => makeResponse({ stargazers_count: 55 }),
    });
  const observeResult = await updateStarHistory(observeOptions);
  const generatedSnapshot = new Map(
    ['README.md', 'stars.csv', 'star-history.svg'].map((name) =>
      [name, readFileSync(join(generatedDirectory, name), 'utf8')]),
  );
  const repeatedResult = await updateStarHistory(
    {
      ...observeOptions,
      now: new Date('2026-09-02T20:17:00.000Z'),
    });

  assert.equal(observeResult.changed, true);
  assert.equal(repeatedResult.changed, false);

  for (const [name, content] of generatedSnapshot)
  {
    assert.equal(readFileSync(join(generatedDirectory, name), 'utf8'), content);
  }
}
finally
{
  rmSync(generatedDirectory, { force: true, recursive: true });
}

console.log('\n失败前不改写数据');
const temporaryDirectory = mkdtempSync(join(tmpdir(), 'ba-click-fx-star-history-'));

try
{
  const csvPath = join(temporaryDirectory, 'stars.csv');
  const svgPath = join(temporaryDirectory, 'star-history.svg');

  writeFileSync(csvPath, 'invalid CSV sentinel\n', 'utf8');
  writeFileSync(svgPath, 'SVG sentinel\n', 'utf8');
  await assert.rejects(
    updateStarHistory(makeRuntimeOptions({ dataDir: temporaryDirectory })),
  );
  assert.equal(readFileSync(csvPath, 'utf8'), 'invalid CSV sentinel\n');
  assert.equal(readFileSync(svgPath, 'utf8'), 'SVG sentinel\n');

  writeFileSync(csvPath, serializeStarHistoryCsv(firstObservation.rows), 'utf8');
  const beforeApiFailureCsv = readFileSync(csvPath, 'utf8');
  await assert.rejects(
    updateStarHistory(makeRuntimeOptions(
      {
        dataDir: temporaryDirectory,
        fetchImpl: async () => makeResponse({ message: 'failure' }, 503),
      })),
  );
  assert.equal(readFileSync(csvPath, 'utf8'), beforeApiFailureCsv);
  assert.equal(readFileSync(svgPath, 'utf8'), 'SVG sentinel\n');
}
finally
{
  rmSync(temporaryDirectory, { force: true, recursive: true });
}

const emptyDirectory = mkdtempSync(join(tmpdir(), 'bafx-star-bootstrap-'));
try
{
  // An interrupted bootstrap must not leave a reconstructed-only data branch.
  await assert.rejects(updateStarHistory(makeRuntimeOptions(
    {
      bootstrap: true,
      dataDir: emptyDirectory,
      fetchImpl: async (url) => url.pathname.endsWith('/stargazers')
        ? makeResponse([]) : makeResponse({ message: 'rate limited' }, 429),
    })));
  assert.deepEqual(readdirSync(emptyDirectory), []);
  const emptyResult = await updateStarHistory(makeRuntimeOptions(
    {
      bootstrap: true,
      dataDir: emptyDirectory,
      fetchImpl: async (url) => makeResponse(url.pathname.endsWith('/stargazers') ? [] : { stargazers_count: 0 }),
    }));
  assert.equal(emptyResult.rows.length, 1);
  assert.equal(emptyResult.latestStars, 0);
  assert.doesNotMatch(readFileSync(join(emptyDirectory, 'star-history.svg'), 'utf8'), /NaN|Infinity/);
  for (const count of [-1, 0.5, null, '15'])
  {
    await assert.rejects(updateStarHistory(makeRuntimeOptions(
      { dataDir: emptyDirectory, fetchImpl: async () => makeResponse({ stargazers_count: count }) })));
  }
}
finally
{
  rmSync(emptyDirectory, { force: true, recursive: true });
}

console.log('\nStar history checks passed.');
