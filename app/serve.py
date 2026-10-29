"""FingerScribe local server: serves the app and passes Nemotron requests to Nebius Token Factory.

Run:  py serve.py        (start.bat does this)  ->  open http://localhost:8000
Why a server: browsers block web pages from calling the Nebius API directly (CORS), and it keeps your API key
out of the web page. The key is read from NEBIUS_API_KEY, or from nebius_key.txt in this folder (gitignored).
Only built-in Python modules are used."""
import http.server, json, os, socketserver, sys, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = "https://api.tokenfactory.nebius.com/v1"
PORT = int(os.environ.get("PORT", "8000"))

def load_key():
    if os.environ.get("NEBIUS_API_KEY"): return os.environ["NEBIUS_API_KEY"].strip()
    f = os.path.join(HERE, "nebius_key.txt")
    return open(f).read().strip() if os.path.exists(f) else ""
KEY = load_key()
WEIGHTS = ("model.json", "letters.json", "words_reader.json", "touch.json")   # trained on-phone models, not in the public repo

def load_parakeet():   # NVIDIA Parakeet speech-to-text on a Nebius AI Cloud endpoint: URL + token, from env or app/parakeet.txt
    url, tok = os.environ.get("PARAKEET_URL", ""), os.environ.get("PARAKEET_TOKEN", "")
    f = os.path.join(HERE, "parakeet.txt")
    if not url and os.path.exists(f):
        lines = [l.strip() for l in open(f) if l.strip()]; url = lines[0] if lines else ""; tok = lines[1] if len(lines) > 1 else ""
    return url.strip(), tok.strip()
PARAKEET_URL, PARAKEET_TOKEN = load_parakeet()

def load_tavily():     # Tavily key: env TAVILY_API_KEY or app/tavily_key.txt
    if os.environ.get("TAVILY_API_KEY"): return os.environ["TAVILY_API_KEY"].strip()
    f = os.path.join(HERE, "tavily_key.txt")
    return open(f).read().strip() if os.path.exists(f) else ""
TAVILY_KEY = load_tavily()

def tavily(body):
    b = json.loads(body or b"{}")
    clean = {"query": str(b.get("query", ""))[:400], "search_depth": "basic", "max_results": min(int(b.get("max_results", 4)), 5), "include_answer": False}
    req = urllib.request.Request("https://api.tavily.com/search", data=json.dumps(clean).encode(), method="POST",
                                 headers={"Authorization": "Bearer " + TAVILY_KEY, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.status, r.read()

def parakeet(body):
    req = urllib.request.Request(PARAKEET_URL, data=body, method="POST",
                                 headers={"Authorization": "Bearer " + PARAKEET_TOKEN, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.status, r.read()

def nebius(path, body=None):
    req = urllib.request.Request(BASE + path, data=body, method="POST" if body is not None else "GET",
                                 headers={"Authorization": "Bearer " + KEY, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.status, r.read()

class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=HERE, **k)
    def log_message(self, fmt, *args):
        if "/nebius/" in (self.path or ""): sys.stderr.write("nemotron %s\n" % (fmt % args))
    def _send(self, code, data, ctype="application/json"):
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
    def _proxy(self, body=None):
        if self.path == "/nebius/health": return self._send(200 if KEY else 503, json.dumps({"key": bool(KEY)}).encode())
        if not KEY: return self._send(503, b'{"error":"no nebius_key.txt next to serve.py"}')
        try: code, data = nebius(self.path[len("/nebius"):], body); self._send(code, data)
        except urllib.error.HTTPError as e: self._send(e.code, e.read())
        except Exception as e: self._send(502, json.dumps({"error": str(e)}).encode())
    def do_GET(self):
        if self.path.startswith("/nebius/"): return self._proxy()
        return super().do_GET()
    def do_POST(self):
        if self.path == "/tavily/search":
            if not TAVILY_KEY: return self._send(404, b'{"error":"no tavily_key.txt next to serve.py"}')
            n = int(self.headers.get("Content-Length") or 0)
            try: code, data = tavily(self.rfile.read(n)); return self._send(code, data)
            except urllib.error.HTTPError as e: return self._send(e.code, e.read())
            except Exception as e: return self._send(502, json.dumps({"error": str(e)}).encode())
        if self.path == "/asr":
            if not PARAKEET_URL: return self._send(404, b'{"error":"no Parakeet endpoint set (app/parakeet.txt)"}')
            n = int(self.headers.get("Content-Length") or 0)
            try: code, data = parakeet(self.rfile.read(n)); return self._send(code, data)
            except urllib.error.HTTPError as e: return self._send(e.code, e.read())
            except Exception as e: return self._send(502, json.dumps({"error": str(e)}).encode())
        if not self.path.startswith("/nebius/"): return self._send(404, b"{}")
        n = int(self.headers.get("Content-Length") or 0); self._proxy(self.rfile.read(n))
    def end_headers(self):
        if not self.path.startswith("/nebius/"): self.send_header("Cache-Control", "no-cache")
        super().end_headers()

if __name__ == "__main__":
    print("FingerScribe -> http://localhost:%d" % PORT)
    missing = [w for w in WEIGHTS if not os.path.exists(os.path.join(HERE, w))]
    if missing: print("Reader weights: missing", ", ".join(missing), "- handwriting won't be read locally (see README > Weights).")
    print("Tavily:", "set" if TAVILY_KEY else "not set (put tavily_key.txt next to serve.py)")
    print("Parakeet (Nebius endpoint):", "set" if PARAKEET_URL else "not set; voice notes use Nemotron Omni if available")
    if not KEY: print("Nemotron: no key found (put nebius_key.txt next to serve.py). The app works, without AI correction.")
    else:
        try:
            _, d = nebius("/models"); ids = [m["id"] for m in json.loads(d)["data"]]
            nem = [i for i in ids if "nemotron" in i.lower()]
            print("Nemotron on Nebius Token Factory: OK -", ", ".join(nem[:6]) or "(no Nemotron models listed!)")
        except Exception as e: print("Nemotron check FAILED:", e)
    class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True; allow_reuse_address = True
    S(("127.0.0.1", PORT), H).serve_forever()
