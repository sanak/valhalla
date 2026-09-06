import { createServer } from 'node:http';
import { openSync, readSync, statSync, closeSync, readFileSync } from 'node:fs';
import { extname, join, normalize } from 'node:path';

const TYPES = {
  '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript',
  '.wasm': 'application/wasm', '.tar': 'application/x-tar',
};

export function serve(root, port = 0) {
  const server = createServer((req, res) => {
    const path = join(root, normalize(decodeURIComponent(req.url.split('?')[0])));
    let stat;
    try {
      stat = statSync(path);
    } catch {
      res.writeHead(404).end();
      return;
    }
    // readFileSync on a directory throws EISDIR out of the handler and takes the process with it
    if (!stat.isFile()) {
      res.writeHead(404).end();
      return;
    }
    const type = TYPES[extname(path)] ?? 'application/octet-stream';
    const range = /^bytes=(\d+)-(\d+)$/.exec(req.headers.range ?? '');
    if (!range) {
      res.writeHead(200, { 'content-type': type, 'content-length': stat.size });
      res.end(readFileSync(path));
      return;
    }
    const start = Number(range[1]);
    const end = Math.min(Number(range[2]), stat.size - 1);
    const buf = Buffer.alloc(end - start + 1);
    const fd = openSync(path, 'r');
    readSync(fd, buf, 0, buf.length, start);
    closeSync(fd);
    res.writeHead(206, {
      'content-type': type,
      'content-length': buf.length,
      'content-range': `bytes ${start}-${end}/${stat.size}`,
    });
    res.end(buf);
  });
  return new Promise((resolve) => server.listen(port, () => resolve(server)));
}
