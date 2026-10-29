// Forwards FingerScribe's evening questions to Tavily search. Key in Vercel's TAVILY_API_KEY; only search, small result count.
module.exports = async (req, res) => {
  const key = (process.env.TAVILY_API_KEY || '').trim();
  res.setHeader('Cache-Control', 'no-store');
  if (!key) return res.status(404).json({ error: 'TAVILY_API_KEY is not set in Vercel' });
  if (req.method !== 'POST') return res.status(405).json({ error: 'POST only' });
  const b = typeof req.body === 'string' ? JSON.parse(req.body || '{}') : (req.body || {});
  const body = { query: String(b.query || '').slice(0, 400), search_depth: 'basic', max_results: Math.min(Number(b.max_results) || 4, 5), include_answer: false };
  try {
    const r = await fetch('https://api.tavily.com/search', { method: 'POST', headers: { Authorization: 'Bearer ' + key, 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    res.status(r.status).setHeader('Content-Type', 'application/json'); res.send(await r.text());
  } catch (e) { res.status(502).json({ error: String(e) }); }
};
