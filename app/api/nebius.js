// Passes the app's Nemotron requests to Nebius Token Factory. The key lives in Vercel's NEBIUS_API_KEY setting,
// never in the web page. Only the two calls the app uses are allowed, only Nemotron models, with a reply-length cap,
// so a stranger who finds the link can't use your key for anything else.
const BASE = 'https://api.tokenfactory.nebius.com/v1';
module.exports = async (req, res) => {
  const key = (process.env.NEBIUS_API_KEY || '').trim();
  const path = '/' + [].concat(req.query.path || []).join('/');
  res.setHeader('Cache-Control', 'no-store');
  if (path === '/health') return res.status(key ? 200 : 503).json({ key: !!key });
  if (!key) return res.status(503).json({ error: 'NEBIUS_API_KEY is not set in Vercel' });
  if (!['/models', '/chat/completions'].includes(path)) return res.status(404).json({ error: 'not allowed' });
  let body;
  if (req.method === 'POST') {
    body = typeof req.body === 'string' ? JSON.parse(req.body || '{}') : (req.body || {});
    if (!/nemotron/i.test(String(body.model || ''))) return res.status(403).json({ error: 'Nemotron models only' });
    body.max_tokens = Math.min(Number(body.max_tokens) || 1200, 2000);
  }
  try {
    const r = await fetch(BASE + path, { method: body ? 'POST' : 'GET', headers: { Authorization: 'Bearer ' + key, 'Content-Type': 'application/json' }, body: body ? JSON.stringify(body) : undefined });
    res.status(r.status).setHeader('Content-Type', 'application/json');
    res.send(await r.text());
  } catch (e) { res.status(502).json({ error: String(e) }); }
};
