import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import vm from 'node:vm';

const library = await readFile(new URL('../index.js', import.meta.url), 'utf8');
const downloadScript = await readFile(new URL('../../manager/app/src/main/assets/webview/download.js', import.meta.url), 'utf8');
let importId = 0;
async function loadLibrary(bridge) {
  if (bridge) globalThis.ksu = bridge;
  else delete globalThis.ksu;
  return import(`data:text/javascript;base64,${Buffer.from(library).toString('base64')}#${importId++}`);
}

test('old or absent managers expose unavailable capabilities without breaking imports', async () => {
  for (const bridge of [undefined, {}]) {
    const api = await loadLibrary(bridge);
    assert.equal(api.io.support, false);
    assert.equal(api.io.version, 0);
    assert.equal(api.pm.support, false);
    assert.deepEqual(api.pm.listPackages('all'), []);
    assert.deepEqual(api.pm.getPackagesInfo(['missing']), []);
  }
});

test('package facade preserves multiple users and missing package errors', async () => {
  const infos = [
    { packageName: 'sample', uid: 12345, userId: 0 },
    { packageName: 'sample', uid: 1012345, userId: 10 },
    { packageName: 'missing', error: 'Package not found or inaccessible' },
  ];
  const api = await loadLibrary({ pm: () => ({
    version: () => 1,
    listPackages: () => '["sample"]',
    getPackagesInfo: (packages) => {
      assert.deepEqual(JSON.parse(packages), ['sample', 'missing']);
      return JSON.stringify(infos);
    },
  }) });
  assert.equal(api.pm.version, 1);
  assert.deepEqual(api.pm.listPackages('all'), ['sample']);
  assert.deepEqual(api.pm.getPackagesInfo(['sample', 'missing']), infos);
});

test('file facade preserves stream handles, optional arguments and malformed listings', async () => {
  const calls = [];
  const api = await loadLibrary({ io: () => ({
    version: () => 1,
    File: path => ({
      getPath: () => path,
      list: () => 'invalid',
      listFiles: () => '["/data/report"]',
      newOutputStream: (...args) => { calls.push(['new', ...args]); return 'writer'; },
    }),
    FileOutputStream: () => ({
      open: (...args) => { calls.push(['open', ...args]); return 'writer'; },
      write: (...args) => { calls.push(['write', ...args]); return true; },
      close: id => id === 'writer',
    }),
    FileInputStream: () => ({ read: (...args) => { calls.push(['read', ...args]); return 'aGVsbG8='; } }),
  }) });
  const file = api.io.File('/data/report');
  assert.equal(file.getPath(), '/data/report');
  assert.deepEqual(file.list(), []);
  assert.deepEqual(file.listFiles(), ['/data/report']);
  const writer = api.io.FileOutputStream();
  assert.equal(file.newOutputStream(), 'writer');
  assert.equal(writer.open('/data/report', true), 'writer');
  assert.equal(writer.write('writer', 'aGVsbG8='), true);
  assert.equal(api.io.FileInputStream().read('reader', 64), 'aGVsbG8=');
  assert.equal(writer.close('writer'), true);
  assert.deepEqual(calls, [['new'], ['open', '/data/report', true], ['write', 'writer', 'aGVsbG8='], ['read', 'reader', 64]]);
});

function downloads(options = {}) {
  const calls = [];
  const errors = [];
  class TestURL extends URL {
    static createObjectURL() { return 'blob:https://mui.kernelsu.org/blob-id'; }
    static revokeObjectURL() {}
  }
  class Anchor {
    click() { calls.push(['originalClick']); }
    hasAttribute(name) { return name === 'download'; }
  }
  class FileReader {
    readAsDataURL(blob) {
      blob.arrayBuffer().then(bytes => {
        this.result = `data:${blob.type};base64,${Buffer.from(bytes).toString('base64')}`;
        this.onload();
      }).catch(error => { this.error = error; this.onerror(); });
    }
  }
  const context = vm.createContext({
    URL: TestURL, Blob, FileReader, HTMLAnchorElement: Anchor, Element: Anchor,
    document: { addEventListener() {} },
    location: { href: 'https://mui.kernelsu.org/index.html', origin: 'https://mui.kernelsu.org' },
    console: { error: (...args) => errors.push(args) },
    setTimeout,
    fetch: options.fetch || (async () => { throw Error('Unexpected fetch'); }),
    ksu_download: {
      startChunkedDownload: (...args) => { calls.push(['start', ...args]); return 'id'; },
      writeDownloadChunk: (id, data) => { calls.push(['chunk', id, Buffer.from(data, 'base64').length]); return !options.failChunk; },
      completeChunkedDownload: id => { calls.push(['complete', id]); return true; },
      cancelChunkedDownload: id => calls.push(['cancel', id]),
      save: (...args) => calls.push(['save', ...args]),
      download: (...args) => calls.push(['download', ...args]),
    },
  });
  context.window = context;
  vm.runInContext(downloadScript, context);
  return { context, calls, errors, blobUrl: blob => context.URL.createObjectURL(blob) };
}

test('blob downloads preserve bytes and MIME without relying on internal network hosts', async () => {
  const d = downloads();
  const url = d.blobUrl(new Blob(['hello'], { type: 'text/plain' }));
  await d.context.__ksu_download_url(url, 'report.txt');
  assert.deepEqual(d.calls, [['save', 'aGVsbG8=', 'report.txt', 'text/plain']]);
  assert.equal(d.errors.length, 0);
});

test('large blobs stream bounded chunks and complete once', async () => {
  const d = downloads();
  const size = 10 * 1024 * 1024 + 1;
  const url = d.blobUrl(new Blob([new Uint8Array(size)]));
  await d.context.__ksu_download_url(url, 'large.bin');
  const chunks = d.calls.filter(call => call[0] === 'chunk');
  assert.equal(chunks.reduce((sum, call) => sum + call[2], 0), size);
  assert.ok(chunks.every(call => call[2] <= 512 * 1024));
  assert.deepEqual(d.calls.at(-1), ['complete', 'id']);
  assert.equal(d.errors.length, 0);
});

test('a failed blob chunk cancels the temporary transfer', async () => {
  const d = downloads({ failChunk: true });
  const url = d.blobUrl(new Blob([new Uint8Array(10 * 1024 * 1024 + 1)]));
  await d.context.__ksu_download_url(url, 'large.bin');
  assert.deepEqual(d.calls.at(-1), ['cancel', 'id']);
  assert.equal(d.calls.some(call => call[0] === 'complete'), false);
  assert.equal(d.errors.length, 1);
});

test('external URLs use Android downloads while module fetch failures never save error pages', async () => {
  const d = downloads({ fetch: async () => ({ ok: false, status: 404 }) });
  await d.context.__ksu_download_url('https://example.com/report.txt', 'report.txt');
  assert.deepEqual(d.calls, [['download', 'https://example.com/report.txt', 'report.txt', null]]);
  await d.context.__ksu_download_url('https://mui.kernelsu.org/missing.txt', 'missing.txt');
  assert.equal(d.calls.length, 1);
  assert.equal(d.errors.length, 1);
});
