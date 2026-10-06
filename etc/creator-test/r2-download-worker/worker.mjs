// Public, read-only downloads. Cloudflare supplies GAME_DATA through an R2 binding.
// Account tokens and S3 credentials must never be included in the desktop app.
export default {
  async fetch(request, env) {
    if (!['GET', 'HEAD'].includes(request.method)) {
      return new Response('Method not allowed', { status: 405, headers: { Allow: 'GET, HEAD' } });
    }
    const key = new URL(request.url).pathname.slice(1);
    if (!/^(dbc|maps|vmaps|mmaps)\/[A-Za-z0-9_-][A-Za-z0-9_.-]*$/.test(key) || key.includes('..')) {
      return new Response('Not found', { status: 404 });
    }
    try {
      let range;
      const rangeHeader = request.method === 'GET' && request.headers.get('Range');
      if (rangeHeader) {
        const metadata = await env.GAME_DATA.head(key);
        if (!metadata) return new Response('Not found', { status: 404 });
        const match = /^bytes=(\d*)-(\d*)$/.exec(rangeHeader);
        const invalid = () => new Response(null, {
          status: 416, headers: { 'Content-Range': `bytes */${metadata.size}` },
        });
        if (!match || (!match[1] && !match[2])) return invalid();
        let start, end;
        if (!match[1]) {
          const suffix = Number(match[2]);
          if (!Number.isSafeInteger(suffix) || suffix <= 0) return invalid();
          start = Math.max(0, metadata.size - suffix);
          end = metadata.size - 1;
        } else {
          start = Number(match[1]);
          end = match[2] ? Number(match[2]) : metadata.size - 1;
          if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end)) return invalid();
          end = Math.min(end, metadata.size - 1);
        }
        if (start >= metadata.size || start > end) return invalid();
        // A stale If-Range means return the complete object instead.
        if (!request.headers.has('If-Range') || request.headers.get('If-Range') === metadata.httpEtag) {
          range = { offset: start, length: end - start + 1 };
        }
      }
      const object = request.method === 'HEAD'
        ? await env.GAME_DATA.head(key)
        : await env.GAME_DATA.get(key, range ? { range } : undefined);
      if (!object) return new Response('Not found', { status: 404 });
      const headers = new Headers({
        'Content-Type': 'application/octet-stream',
        'Content-Length': String(range ? range.length : object.size),
        'Accept-Ranges': 'bytes',
        'ETag': object.httpEtag,
        'X-Content-Type-Options': 'nosniff',
        'Cache-Control': 'no-transform',
      });
      if (range) headers.set('Content-Range', `bytes ${range.offset}-${range.offset + range.length - 1}/${object.size}`);
      return new Response(request.method === 'HEAD' ? null : object.body, {
        status: range ? 206 : 200, headers,
      });
    } catch (error) {
      console.error('R2 download failed', key, error);
      return new Response('Storage temporarily unavailable', {
        status: 503, headers: { 'Retry-After': '5' },
      });
    }
  },
};
