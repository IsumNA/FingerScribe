"""Step 1 of the Nebius training job: NVIDIA Nemotron 3.5 Lightning (on Nebius Token Factory) writes the training corpus.
Short, jotted, journal-style fragments with names, places and cursive-prone words: the kind of thing people
actually write on their leg. These become pen paths (gen_hand.py), then finger motion (sim_imu.py), then training data.
Usage: NEBIUS_API_KEY=... python3 make_corpus.py 20000 > corpus.txt"""
import os, sys, json, random, urllib.request
N = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
KEY = os.environ["NEBIUS_API_KEY"]; MODEL = os.environ.get("CORPUS_MODEL", "nvidia/Nemotron-3_5-Lightning")
BASE = "https://api.tokenfactory.nebius.com/v1/chat/completions"
THEMES = ["a train journey", "a walk by a river", "a lecture", "a difficult conversation", "a side project", "cooking",
          "a friend who seemed off", "money worries", "an idea for a product", "a badminton match", "the weather",
          "a book", "a dream", "the gym", "a phone call home", "a bus stop", "a late night", "a market"]
def ask(theme, k=40):
    body = {"model": MODEL, "temperature": 1.0, "max_tokens": 1200, "messages": [{"role": "user", "content":
        f"Write {k} short fragments someone might jot in a pocket journal, about {theme}. 2 to 8 words each, lowercase, "
        f"no numbers or punctuation except apostrophes, varied vocabulary, include first names and place names sometimes. "
        f"One per line, nothing else."}]}
    req = urllib.request.Request(BASE, data=json.dumps(body).encode(), headers={"Authorization": "Bearer " + KEY, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r: out = json.load(r)["choices"][0]["message"]["content"]
    return [l.strip().lower() for l in out.splitlines() if 1 <= len(l.split()) <= 8 and all(c.isalpha() or c in " '" for c in l.strip())]
seen = set()
while len(seen) < N:
    for f in ask(random.choice(THEMES)):
        if f not in seen: seen.add(f); print(f, flush=True)
        if len(seen) >= N: break
