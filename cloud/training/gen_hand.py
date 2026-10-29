"""NumPy port of sjvasquez/handwriting-synthesis sampling (Graves-style attention RNN trained on IAM-OnDB).
Generates pen trajectories (dx, dy, pen_up) for arbitrary words in 13 writer styles.
The checkpoint comes from the repo cloned at $HS (default /job/hs, see Dockerfile); weights are cached as .npz."""
import os, sys, numpy as np
HS = os.environ.get('HS', '/job/hs')
sys.path.insert(0, HS)
ALPHABET = ['\x00', ' ', '!', '"', '#', "'", '(', ')', ',', '-', '.', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
            ':', ';', '?', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'R', 'S', 'T',
            'U', 'V', 'W', 'Y', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q',
            'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z']
A2N = {c: i for i, c in enumerate(ALPHABET)}
V = len(ALPHABET)

def load_weights():
    cache = os.path.join(os.environ.get('FS_DATA', '/data'), 'hand_weights.npz')
    if os.path.exists(cache):
        return dict(np.load(cache))
    import tensorflow as tf
    r = tf.train.load_checkpoint(f'{HS}/checkpoints/model-17900')
    p = 'rnn/LSTMAttentionCell/'
    Wt = {k: r.get_tensor(p + n) for k, n in [
        ('att_w', 'attention/weights'), ('att_b', 'attention/biases'),
        ('k1', 'lstm_cell/kernel'), ('b1', 'lstm_cell/bias'),
        ('k2', 'lstm_cell_1/kernel'), ('b2', 'lstm_cell_1/bias'),
        ('k3', 'lstm_cell_2/kernel'), ('b3', 'lstm_cell_2/bias')]}
    Wt['gmm_w'] = r.get_tensor('rnn/gmm/weights'); Wt['gmm_b'] = r.get_tensor('rnn/gmm/biases')
    try: np.savez(cache, **Wt)
    except OSError: pass
    return Wt

P = load_weights()
H = 400; K = 10; M = 20

def sig(x): return 1 / (1 + np.exp(-x))
def softplus(x): return np.logaddexp(0, x)

def lstm(x, h, c, k, b):
    z = np.concatenate([x, h], 1) @ k + b
    i, j, f, o = np.split(z, 4, 1)
    c = sig(f + 1.0) * c + sig(i) * np.tanh(j)
    return np.tanh(c) * sig(o), c

class State:
    def __init__(s, B, U):
        s.h1 = np.zeros((B, H), np.float32); s.c1 = s.h1.copy(); s.h2 = s.h1.copy(); s.c2 = s.h1.copy()
        s.h3 = s.h1.copy(); s.c3 = s.h1.copy(); s.kappa = np.zeros((B, K), np.float32)
        s.w = np.zeros((B, V), np.float32); s.phi = np.zeros((B, U), np.float32)

def step(x, s, onehot, mask):
    s1, s.c1 = lstm(np.concatenate([s.w, x], 1), s.h1, s.c1, P['k1'], P['b1']); s.h1 = s1
    ap = softplus(np.concatenate([s.w, x, s1], 1) @ P['att_w'] + P['att_b'])
    alpha, beta, kappa = np.split(ap, 3, 1)
    s.kappa = s.kappa + kappa / 25.0
    beta = np.clip(beta, .01, None)
    U = onehot.shape[1]
    u = np.arange(U, dtype=np.float32)[None, None, :]
    s.phi = (alpha[:, :, None] * np.exp(-np.square(s.kappa[:, :, None] - u) / beta[:, :, None])).sum(1)
    s.w = (s.phi[:, :, None] * onehot * mask[:, :, None]).sum(1)
    s2, s.c2 = lstm(np.concatenate([x, s1, s.w], 1), s.h2, s.c2, P['k2'], P['b2']); s.h2 = s2
    s3, s.c3 = lstm(np.concatenate([x, s2, s.w], 1), s.h3, s.c3, P['k3'], P['b3']); s.h3 = s3

def sample_out(s, bias, rng):
    z = s.h3 @ P['gmm_w'] + P['gmm_b']
    pis, sigmas, rhos, mus, es = np.split(z, [M, 3 * M, 4 * M, 6 * M], 1)
    pis = pis * (1 + bias[:, None]); sigmas = sigmas - bias[:, None]
    pis = np.exp(pis - pis.max(1, keepdims=True)); pis /= pis.sum(1, keepdims=True)
    pis = np.where(pis < .01, 0, pis); pis /= pis.sum(1, keepdims=True)
    sigmas = np.clip(np.exp(sigmas), 1e-4, None); rhos = np.clip(np.tanh(rhos), -1 + 1e-8, 1 - 1e-8)
    es = np.clip(sig(es), 1e-8, 1 - 1e-8); es = np.where(es < .01, 0, es)
    B = len(z)
    idx = np.array([rng.choice(M, p=p) for p in pis])
    mu1, mu2 = mus[np.arange(B), idx], mus[np.arange(B), M + idx]
    s1, s2 = sigmas[np.arange(B), idx], sigmas[np.arange(B), M + idx]
    rho = rhos[np.arange(B), idx]
    n1, n2 = rng.standard_normal(B), rng.standard_normal(B)
    x1 = mu1 + s1 * n1
    x2 = mu2 + s2 * (rho * n1 + np.sqrt(1 - rho ** 2) * n2)
    e = (rng.random(B) < es[:, 0]).astype(np.float32)
    return np.stack([x1, x2, e], 1).astype(np.float32)

def generate(words, style=None, bias=0.75, seed=0, max_steps_per_char=40):
    rng = np.random.default_rng(seed)
    B = len(words)
    prefix = ''
    if style is not None:
        xp = np.load(f'{HS}/styles/style-{style}-strokes.npy')
        prefix = np.load(f'{HS}/styles/style-{style}-chars.npy').tobytes().decode('utf-8') + ' '
    texts = [prefix + w + '  ' for w in words]
    U = max(len(t) for t in texts)
    onehot = np.zeros((B, U, V), np.float32); mask = np.zeros((B, U), np.float32); L = np.zeros(B, int)
    for b, t in enumerate(texts):
        for u, ch in enumerate(t): onehot[b, u, A2N.get(ch, 0)] = 1
        mask[b, :len(t)] = 1; L[b] = len(t)
    s = State(B, U)
    biasv = np.full(B, bias, np.float32)
    if style is not None:
        for x in xp:
            step(np.repeat(x[None].astype(np.float32), B, 0), s, onehot, mask)
        x = sample_out(s, biasv, rng)
    else:
        x = np.tile(np.array([[0, 0, 1]], np.float32), (B, 1))
    out = [[] for _ in range(B)]; done = np.zeros(B, bool)
    for t in range(max_steps_per_char * max(len(w) for w in words) + 20):
        step(x, s, onehot, mask)
        x = sample_out(s, biasv, rng)
        ci = s.phi.argmax(1)
        newly = (ci >= L) | ((ci >= L - 1) & (x[:, 2] == 1))
        for b in range(B):
            if not done[b]: out[b].append(x[b].copy())
        done |= newly
        if done.all(): break
    return [np.array(o) for o in out]

def to_coords(offsets):
    """offsets (dx, dy, eos) -> absolute (x, y, pen_up_after)"""
    c = offsets.copy(); c[:, :2] = np.cumsum(offsets[:, :2], 0); return c

def clean(offsets):
    """Drop stray trailing marks (the model sometimes starts the next word after the double space)."""
    c = to_coords(offsets)
    ends = list(np.where(c[:, 2] == 1)[0])
    if not ends or ends[-1] != len(c) - 1: ends.append(len(c) - 1)
    strokes, s = [], 0
    for e in ends:
        strokes.append((s, e + 1)); s = e + 1
    while len(strokes) > 1:
        a, b = strokes[-1]
        prev_xmax = c[:strokes[-1][0], 0].max()
        if (b - a) < 10 and c[a:b, 0].min() > prev_xmax + 2: strokes.pop()
        else: break
    return offsets[:strokes[-1][1]]
