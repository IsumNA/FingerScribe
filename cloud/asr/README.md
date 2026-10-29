# Parakeet on a Nebius AI Cloud endpoint

NVIDIA Parakeet TDT 0.6B v3 (open weights, NVIDIA Open Model License) transcribes FingerScribe's voice notes.
It runs on Nebius AI Cloud as a serverless **Endpoint**, so the phone sends 8 kHz audio to Nebius and gets text back.

## Deploy
```
docker build -t cr.nebius.cloud/<your-registry>/fingerscribe-asr:1 .
docker push    cr.nebius.cloud/<your-registry>/fingerscribe-asr:1
```
Then in the Nebius console: Serverless AI → Endpoints → Create: that image, port 8080, one GPU (an L40S is plenty),
min replicas 0 (scale to zero). Copy the endpoint URL and its auth token.

## Wire it to the app
- Locally: set `PARAKEET_URL=https://<endpoint>/asr` and `PARAKEET_TOKEN=<token>` before running `app/serve.py`
  (or put them in `app/parakeet.txt` as two lines). `serve.py` forwards `/asr` to the endpoint.
- Vercel: `npx vercel env add PARAKEET_URL` and `PARAKEET_TOKEN`; `app/api/asr.js` forwards the same way.

Check: `curl -X POST https://<endpoint>/asr -H "Authorization: Bearer <token>" -d '{"wav_b64": "..."}'`
