// Forwards FingerScribe voice notes to NVIDIA Parakeet running on a Nebius AI Cloud endpoint (PARAKEET_URL, PARAKEET_TOKEN in Vercel).
module.exports = async (req, res) => {
  const url = (process.env.PARAKEET_URL || '').trim(), tok = (process.env.PARAKEET_TOKEN || '').trim();
  res.setHeader('Cache-Control', 'no-store');
  if (!url) return res.status(404).json({ error: 'PARAKEET_URL is not set in Vercel' });
  if (req.method !== 'POST') return res.status(405).json({ error: 'POST only' });
  const body = typeof req.body === 'string' ? req.body : JSON.stringify(req.body || {});
  try {
    const r = await fetch(url, { method: 'POST', headers: { Authorization: 'Bearer ' + tok, 'Content-Type': 'application/json' }, body });
    res.status(r.status).setHeader('Content-Type', 'application/json'); res.send(await r.text());
  } catch (e) { res.status(502).json({ error: String(e) }); }
};
