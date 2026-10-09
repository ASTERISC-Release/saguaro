const { Worker, isMainThread, parentPort, workerData } = require('worker_threads');
const crypto = require('crypto'), http = require('http'), zlib = require('zlib'), os = require('os');
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
if (!isMainThread) {
  let s = 0; for (let i = 0; i < 3e7; i++) s = (s + i * workerData) % 1000003;   // JIT-hot loop
  parentPort.postMessage({ id: workerData, s, fib: fib(27) });
} else {
  (async () => {
    const t0 = Date.now();
    // 1. JIT: hot recursion + typed arrays
    if (fib(30) !== 832040) throw new Error('fib');
    const a = new Float64Array(1e6).map((_, i) => Math.sqrt(i)); let sum = 0; for (const v of a) sum += v;
    if (Math.abs(sum - 666666166.4588) > 1) throw new Error('sum ' + sum);
    // 2. worker threads in parallel
    const res = await Promise.all([1, 2, 3, 4].map(id => new Promise((ok, bad) => {
      const w = new Worker(__filename, { workerData: id }); w.on('message', ok); w.on('error', bad);
    })));
    for (const r of res) if (r.fib !== 196418) throw new Error('worker fib');
    // 3. crypto + zlib
    const buf = crypto.randomBytes(1 << 20); const h = crypto.createHash('sha256').update(buf).digest('hex');
    const z = zlib.gunzipSync(zlib.gzipSync(buf)); if (crypto.createHash('sha256').update(z).digest('hex') !== h) throw new Error('zlib');
    // 4. http server + client, 200 requests
    const srv = http.createServer((q, r) => r.end('pong ' + q.url)); await new Promise(r => srv.listen(0, '127.0.0.1', r));
    const port = srv.address().port; let ok = 0;
    await Promise.all(Array.from({ length: 200 }, (_, i) => new Promise((res, rej) => {
      http.get({ host: '127.0.0.1', port, path: '/' + i }, r => { let d = ''; r.on('data', c => d += c); r.on('end', () => { if (d === 'pong /' + i) ok++; res(); }); }).on('error', rej);
    })));
    srv.close(); if (ok !== 200) throw new Error('http ' + ok);
    // 5. fs
    const fs = require('fs'); fs.writeFileSync('/tmp/heavy.txt', 'x'.repeat(100000)); if (fs.readFileSync('/tmp/heavy.txt').length !== 100000) throw new Error('fs');
    console.log('HEAVY OK', Date.now() - t0, 'ms');
  })().catch(e => { console.error('HEAVY FAIL', e); process.exit(1); });
}
