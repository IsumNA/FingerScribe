"""Shared pieces for the FingerScribe reader: data loading, orientation fix, the CTC model, batching, decoding.
Paths come from $FS_DATA (default /data): recordings at $FS_DATA/data (labels.csv + csv per word),
OnHW at $FS_DATA/onhw/words500_f0_6ch.npz, outputs under $FS_DATA/train."""
import csv, os, numpy as np, tensorflow as tf
ROOT = os.environ.get('FS_DATA', '/data')
TRAIN = os.path.join(ROOT, 'train'); os.makedirs(TRAIN, exist_ok=True)
ORIENT = os.environ.get('ORIENT', '0') == '1'
TAG = '_orient' if ORIENT else ''

def orient(x):
    """Rotate a recording so its average gravity points along -z, then remove gravity.
    Pen and finger hold the sensor at different angles; this puts both in the same frame
    (up to a turn about the vertical, which augmentation covers)."""
    g = x[:, :3].mean(0)
    n = np.linalg.norm(g)
    if n < 1e-6: return x
    a = g / n; b = np.array([0, 0, -1.0])
    v = np.cross(a, b); c = float(a @ b)
    if np.linalg.norm(v) < 1e-8:
        R = np.eye(3) if c > 0 else np.diag([1, -1, -1])
    else:
        vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
        R = np.eye(3) + vx + vx @ vx * (1 / (1 + c))
    acc = x[:, :3] @ R.T - b * n
    gyr = x[:, 3:6] @ R.T
    return np.concatenate([acc, gyr], 1).astype(np.float32)

def yaw(x, ang):
    c, s = np.cos(ang), np.sin(ang)
    R = np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]], np.float32)
    return np.concatenate([x[:, :3] @ R.T, x[:, 3:6] @ R.T], 1)

# OnHW label alphabet: 0-25 A-Z, 26-51 a-z, 52-58 German extras, 59 = pad/blank
BLANK = 59
NCLASS = 60

def enc(word):
    out = []
    for ch in word:
        if 'A' <= ch <= 'Z': out.append(ord(ch) - 65)
        elif 'a' <= ch <= 'z': out.append(26 + ord(ch) - 97)
    return out

def dec(ids):
    s = []
    for i in ids:
        if 0 <= i < 26: s.append(chr(65 + i))
        elif 26 <= i < 52: s.append(chr(97 + i - 26))
        elif i < 59: s.append('?')
    return ''.join(s)

def load_onhw(path, max_len=1200):
    d = np.load(path)
    out = {}
    for split in ('train', 'val'):
        x, lens, y = d[f'{split}_x'].astype(np.float32), d[f'{split}_len'], d[f'{split}_y']
        starts = np.concatenate([[0], np.cumsum(lens)[:-1]])
        X, Y = [], []
        for s, l, lab in zip(starts, lens, y):
            if l < 20 or l > max_len: continue
            lab = [int(v) for v in lab if v != BLANK]
            if not lab or len(lab) * 4 + 4 > l: continue
            seg = x[s:s + l]
            X.append(orient(seg) if ORIENT else seg); Y.append(lab)
        out[split] = (X, Y)
    return out

def load_isum(root):
    rows = list(csv.DictReader(open(os.path.join(root, 'labels.csv'))))
    X, W, R = [], [], []
    for r in rows:
        f = os.path.join(root, r['file'].replace('\\', '/'))
        a = np.loadtxt(f, delimiter=',', skiprows=1, dtype=np.float32)[:, 1:7]
        X.append(orient(a) if ORIENT else a); W.append(r['word']); R.append(int(r['rep']))
    return X, W, np.array(R)

MEAN = np.array([-0.17, 0.81, -0.47, -2.5, -7.4, 2.8], np.float32)
STD = np.array([0.26, 0.14, 0.20, 37.1, 48.8, 38.5], np.float32)
if ORIENT:
    MEAN = np.zeros(6, np.float32)
    STD = np.array([0.2, 0.2, 0.2, 40, 40, 40], np.float32)

def norm(x):
    return (x - MEAN) / STD

def build_model(width=128):
    inp = tf.keras.Input((None, 6))
    h = inp
    for ch, st in ((64, 1), (96, 2), (128, 2)):
        h = tf.keras.layers.Conv1D(ch, 5, strides=st, padding='same')(h)
        h = tf.keras.layers.BatchNormalization()(h)
        h = tf.keras.layers.Activation('gelu')(h)
        h = tf.keras.layers.Dropout(0.1)(h)
    h = tf.keras.layers.Bidirectional(tf.keras.layers.LSTM(width, return_sequences=True, dropout=0.2))(h)
    h = tf.keras.layers.Bidirectional(tf.keras.layers.LSTM(width, return_sequences=True, dropout=0.2))(h)
    out = tf.keras.layers.Dense(NCLASS)(h)
    return tf.keras.Model(inp, out)

def out_len(l):
    return (((l + 1) // 2) + 1) // 2

def batches(X, Y, bs, shuffle=True, augment=False):
    idx = np.argsort([len(x) for x in X])
    chunks = [idx[i:i + bs] for i in range(0, len(idx), bs)]
    if shuffle: np.random.shuffle(chunks)
    for c in chunks:
        L = max(len(X[i]) for i in c)
        xb = np.zeros((len(c), L, 6), np.float32)
        lens = np.zeros(len(c), np.int32)
        for j, i in enumerate(c):
            x = norm(X[i])
            if augment:
                if ORIENT: x = yaw(x, np.random.uniform(0, 2 * np.pi)).astype(np.float32)
                x = x * np.random.uniform(0.85, 1.15, (1, 6)).astype(np.float32) + np.random.normal(0, 0.03, x.shape).astype(np.float32)
            xb[j, :len(x)] = x; lens[j] = len(x)
        ys = [Y[i] for i in c]
        yl = np.array([len(y) for y in ys], np.int32)
        yb = np.full((len(c), max(yl)), 0, np.int32)
        for j, y in enumerate(ys): yb[j, :len(y)] = y
        yield xb, lens, yb, yl, c

def make_step(model, opt):
    @tf.function(reduce_retracing=True)
    def step(xb, olen, yb, yl):
        with tf.GradientTape() as t:
            logits = model(xb, training=True)
            loss = tf.reduce_mean(tf.nn.ctc_loss(yb, logits, yl, olen, logits_time_major=False, blank_index=BLANK))
        g = t.gradient(loss, model.trainable_variables)
        g, _ = tf.clip_by_global_norm(g, 5.0)
        opt.apply_gradients(zip(g, model.trainable_variables))
        return loss
    return step

def greedy(logits, olen):
    best = np.argmax(logits, -1)
    res = []
    for b, l in zip(best, olen):
        seq, prev = [], -1
        for t in b[:l]:
            if t != prev and t != BLANK: seq.append(int(t))
            prev = t
        res.append(dec(seq))
    return res

def lev(a, b):
    d = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        p, d[0] = d[0], i
        for j, cb in enumerate(b, 1):
            p, d[j] = d[j], min(d[j] + 1, d[j - 1] + 1, p + (ca != cb))
    return d[-1]

def evaluate(model, X, W, lexicon):
    """Open-vocabulary greedy decoding, plus closed-vocabulary (pick the lexicon word with lowest CTC loss)."""
    Y = [enc(w) for w in W]
    preds, lex_preds = [None] * len(X), [None] * len(X)
    lex_enc = [enc(w) for w in lexicon]
    for xb, lens, yb, yl, c in batches(X, Y, 32, shuffle=False):
        logits = model(xb, training=False).numpy()
        olen = out_len(lens)
        for j, p in zip(c, greedy(logits, olen)): preds[j] = p
        lp = tf.nn.log_softmax(logits).numpy()
        for k, j in enumerate(c):
            T = olen[k]
            one = np.repeat(lp[k:k + 1, :T], len(lexicon), 0)
            L = max(len(e) for e in lex_enc)
            lab = np.zeros((len(lexicon), L), np.int32)
            for n, e in enumerate(lex_enc): lab[n, :len(e)] = e
            losses = tf.nn.ctc_loss(lab, one, [len(e) for e in lex_enc], [T] * len(lexicon),
                                    logits_time_major=False, blank_index=BLANK).numpy()
            lex_preds[j] = lexicon[int(np.argmin(losses))]
    cer = sum(lev(p, w) for p, w in zip(preds, W)) / sum(len(w) for w in W)
    wacc = np.mean([p == w for p, w in zip(preds, W)])
    lacc = np.mean([p == w for p, w in zip(lex_preds, W)])
    return cer, wacc, lacc, preds, lex_preds

def dict_decode(model, X, W, n_cand=150, lm_weight=0.5):
    """Free-writing decode with a 20k-word English dictionary: shortlist words close to the
    raw guess, then pick the one the model finds most likely (plus a small word-frequency prior)."""
    from wordfreq import top_n_list, word_frequency
    words = [w for w in top_n_list('en', 20000) if w.isalpha() and w.isascii()]
    logf = np.log(np.array([word_frequency(w, 'en') for w in words]) + 1e-9)
    Y = [enc(w) for w in W]
    out = [None] * len(X)
    for xb, lens, yb, yl, c in batches(X, Y, 32, shuffle=False):
        logits = model(xb, training=False).numpy()
        olen = out_len(lens)
        guesses = greedy(logits, olen)
        lp = tf.nn.log_softmax(logits).numpy()
        for k, j in enumerate(c):
            g = guesses[k].lower()
            d = np.array([lev(g, w) if abs(len(w) - len(g)) <= 3 else 99 for w in words])
            cand = np.argsort(d)[:n_cand]
            T = int(olen[k])
            cw = [words[i] for i in cand if 2 * len(words[i]) <= T]
            if not cw: out[j] = g; continue
            ce = [enc(w) for w in cw]
            L = max(len(e) for e in ce)
            lab = np.zeros((len(cw), L), np.int32)
            for n, e in enumerate(ce): lab[n, :len(e)] = e
            loss = tf.nn.ctc_loss(lab, np.repeat(lp[k:k + 1, :T], len(cw), 0), [len(e) for e in ce], [T] * len(cw),
                                  logits_time_major=False, blank_index=BLANK).numpy()
            score = -loss + lm_weight * np.array([logf[words.index(w)] for w in cw])
            out[j] = cw[int(np.argmax(score))]
    return float(np.mean([o == w for o, w in zip(out, W)])), out
