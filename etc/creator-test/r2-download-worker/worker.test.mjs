import test from 'node:test';
import assert from 'node:assert/strict';
import worker from './worker.mjs';

const content = 'abcdefghij';
const metadata = { size: content.length, httpEtag: '"test"' };
const env = { GAME_DATA: {
  async head(key) { return key === 'dbc/test.dbc' ? metadata : null; },
  async get(key, options) {
    if (key !== 'dbc/test.dbc') return null;
    const range = options?.range;
    return { ...metadata, body: range ? content.slice(range.offset, range.offset + range.length) : content };
  },
} };
const fetch = (path = 'dbc/test.dbc', options) => worker.fetch(new Request(`https://example.workers.dev/${path}`, options), env);

test('downloads bytes with integrity-related headers', async () => {
  const response = await fetch();
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('Content-Length'), '10');
  assert.equal(await response.text(), content);
});
test('HEAD returns metadata without a body', async () => {
  const response = await fetch(undefined, { method: 'HEAD' });
  assert.equal(response.status, 200);
  assert.equal(await response.text(), '');
});
test('supports bounded, open-ended and suffix ranges', async () => {
  for (const [range, expected, header] of [
    ['bytes=2-4', 'cde', 'bytes 2-4/10'],
    ['bytes=7-', 'hij', 'bytes 7-9/10'],
    ['bytes=-2', 'ij', 'bytes 8-9/10'],
  ]) {
    const response = await fetch(undefined, { headers: { Range: range } });
    assert.equal(response.status, 206);
    assert.equal(response.headers.get('Content-Range'), header);
    assert.equal(await response.text(), expected);
  }
});
test('rejects invalid ranges and honors stale If-Range', async () => {
  for (const range of ['bytes=10-', 'bytes=-0', 'bytes=5-2', 'bytes=0-1,3-4']) {
    assert.equal((await fetch(undefined, { headers: { Range: range } })).status, 416);
  }
  const response = await fetch(undefined, { headers: { Range: 'bytes=2-4', 'If-Range': '"old"' } });
  assert.equal(response.status, 200);
  assert.equal(await response.text(), content);
});
test('rejects writes, listing and unrelated objects', async () => {
  assert.equal((await fetch(undefined, { method: 'PUT', body: 'overwrite' })).status, 405);
  for (const path of ['', 'secret.txt', 'dbc/subfolder/file', 'dbc/a..b', 'dbc/missing.dbc']) {
    assert.equal((await fetch(path)).status, 404);
  }
});
test('storage failure is retryable without disclosing exception details', async () => {
  const response = await worker.fetch(new Request('https://example.workers.dev/dbc/test.dbc'), {
    GAME_DATA: { async get() { throw new Error('test storage outage'); } },
  });
  assert.equal(response.status, 503);
  assert.equal(response.headers.get('Retry-After'), '5');
  assert.equal(await response.text(), 'Storage temporarily unavailable');
});
