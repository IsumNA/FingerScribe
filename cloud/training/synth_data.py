"""corpus.txt -> synth_imu.npz : pen paths from the IAM-OnDB handwriting generator (13 styles), then simulated
finger motion from sim_imu.simulate, with domain randomisation. Output matches load format in train_synth.py."""
import sys, numpy as np
from gen_hand import generate, clean
try:
    from sim_imu import simulate          # the finger-motion simulator is not published (kept private)
except ImportError:
    raise SystemExit('sim_imu.py (the finger-motion simulator) is not included in the public repo. '
                     'Provide sim_imu.simulate(pen_offsets, n_chars, rng) -> (T, 6) accel+gyro at 100 Hz to run this step.')
src, dst = sys.argv[1], sys.argv[2]
words = [w for w in (l.strip() for l in open(src)) if w]
rng = np.random.default_rng(0)
X, L, W = [], [], []
B = 32
for i in range(0, len(words), B):
    batch = words[i:i + B]
    style = int(rng.integers(0, 13))
    try: paths = generate(batch, style=style, bias=float(rng.uniform(0.5, 1.0)), seed=i)
    except Exception as e: print('skip batch', i, e, flush=True); continue
    for w, p in zip(batch, paths):
        p = clean(p)
        if len(p) < 10: continue
        x = simulate(p, len(w.replace(' ', '')), rng)
        X.append(x); L.append(len(x)); W.append(w)
    if (i // B) % 20 == 0: print(f'{len(W)} samples', flush=True)
np.savez_compressed(dst, x=np.concatenate(X), lens=np.array(L), words=np.array(W))
print('wrote', dst, len(W), 'samples')
