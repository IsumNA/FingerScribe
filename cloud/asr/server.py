"""FingerScribe speech-to-text: NVIDIA Parakeet TDT 0.6B v3 (open weights) behind a tiny HTTP API.
Deploy as a Nebius AI Cloud serverless endpoint (see Dockerfile and README.md in this folder).
POST /asr  with JSON {"wav_b64": "<base64 of a 16-bit mono WAV>"}  ->  {"text": "..."}
GET  /health                                                       ->  {"model": "...", "ok": true}
The sleeve records at 8 kHz; Parakeet expects 16 kHz, so audio is resampled here."""
import base64, io, os, tempfile
import numpy as np, soundfile as sf
from fastapi import FastAPI
from pydantic import BaseModel
import nemo.collections.asr as nemo_asr

MODEL = os.environ.get("ASR_MODEL", "nvidia/parakeet-tdt-0.6b-v3")
app = FastAPI()
asr = nemo_asr.models.ASRModel.from_pretrained(model_name=MODEL)

class Req(BaseModel):
    wav_b64: str

def to16k(x, sr):
    if sr == 16000: return x
    n = int(len(x) * 16000 / sr)
    return np.interp(np.linspace(0, len(x) - 1, n), np.arange(len(x)), x).astype(np.float32)

@app.get("/health")
def health(): return {"model": MODEL, "ok": True}

@app.post("/asr")
def transcribe(r: Req):
    x, sr = sf.read(io.BytesIO(base64.b64decode(r.wav_b64)), dtype="float32")
    if x.ndim > 1: x = x.mean(axis=1)
    x = to16k(x, sr)
    with tempfile.NamedTemporaryFile(suffix=".wav") as f:
        sf.write(f.name, x, 16000); out = asr.transcribe([f.name])
    hyp = out[0]; text = getattr(hyp, "text", hyp) if not isinstance(hyp, str) else hyp
    return {"text": str(text).strip()}
