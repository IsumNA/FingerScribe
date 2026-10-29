"""Pretrain on simulated finger-IMU words (from Nemotron's corpus), then test on real recordings.
  stage 1: zero-shot on all real recordings (never trained on real data)
  stage 2: fine-tune with the real words (+ synthetic mixed in), test on 20 held-out words, 3 folds
Usage: python3 train_synth.py [onhw|scratch] [epochs]"""
import os, sys, json, time, numpy as np, tensorflow as tf
os.environ['ORIENT'] = '1'
from common import *

S = np.load(os.path.join(ROOT, 'synth_imu.npz'), allow_pickle=True)
starts = np.concatenate([[0], np.cumsum(S['lens'])[:-1]])
SX = [orient(S['x'][a:a + l]) for a, l in zip(starts, S['lens'])]
SY = [enc(w) for w in S['words']]
keep = [i for i in range(len(SX)) if SY[i] and len(SY[i]) * 4 + 4 <= len(SX[i]) <= 1500]
SX = [SX[i] for i in keep]; SY = [SY[i] for i in keep]
print('synthetic samples', len(SX), flush=True)

X, W, R = load_isum(os.path.join(ROOT, 'data'))
init = sys.argv[1] if len(sys.argv) > 1 else 'onhw'
epochs = int(sys.argv[2]) if len(sys.argv) > 2 else 4

np.random.seed(0); tf.random.set_seed(0)
m = build_model()
onhw_w = os.path.join(TRAIN, 'onhw_orient.weights.h5')
if init == 'onhw' and os.path.exists(onhw_w): m.load_weights(onhw_w)
opt = tf.keras.optimizers.Adam(1e-3 if init == 'scratch' else 5e-4); step = make_step(m, opt)
sub = np.random.RandomState(0).choice(len(X), min(120, len(X)), replace=False)
for ep in range(epochs):
    t0 = time.time()
    for xb, lens, yb, yl, c in batches(SX, SY, 32, augment=True):
        step(xb, out_len(lens), yb, yl)
    cer, wacc, _, preds, _ = evaluate(m, X, W, sorted(set(W)))
    print(f'pretrain ep {ep} ({time.time()-t0:.0f}s): ZERO-SHOT on real (no real training): CER {cer:.3f} raw {wacc:.3f}', flush=True)
m.save_weights(os.path.join(TRAIN, f'synth_{init}.weights.h5'))
dacc, _ = dict_decode(m, [X[i] for i in sub], [W[i] for i in sub])
print(f'zero-shot dictionary acc on {len(sub)} real samples: {dacc:.3f}', flush=True)

words = sorted(set(W)); rng0 = np.random.RandomState(0); rng0.shuffle(words)
res = []
for f in range(3):
    held = set(words[f * 20:(f + 1) * 20])
    tr = [i for i in range(len(X)) if W[i] not in held]; te = [i for i in range(len(X)) if W[i] in held]
    if not te: continue
    np.random.seed(f); tf.random.set_seed(f)
    mf = build_model(); mf.load_weights(os.path.join(TRAIN, f'synth_{init}.weights.h5'))
    o2 = tf.keras.optimizers.Adam(5e-4); st2 = make_step(mf, o2)
    Xtr = [X[i] for i in tr]; Ytr = [enc(W[i]) for i in tr]
    for ep in range(25):
        idx = np.random.choice(len(SX), min(len(Xtr), len(SX)), replace=False)   # keep synthetic in the mix
        for xb, lens, yb, yl, c in batches(Xtr + [SX[i] for i in idx], Ytr + [SY[i] for i in idx], 16, augment=True):
            st2(xb, out_len(lens), yb, yl)
    cer, wacc, _, preds, _ = evaluate(mf, [X[i] for i in te], [W[i] for i in te], sorted(held))
    dacc, dp = dict_decode(mf, [X[i] for i in te], [W[i] for i in te])
    print(f'fold {f}: unseen CER {cer:.3f} raw {wacc:.3f} dict {dacc:.3f}', list(zip([W[i] for i in te], preds, dp))[:8], flush=True)
    res.append((cer, wacc, dacc))
mean = np.mean(res, 0)
print('MEAN with synthetic: unseen CER %.3f raw %.3f dict %.3f   (18 Oct baseline without: 0.554 / 0.073 / 0.247)' % tuple(mean), flush=True)
json.dump({'folds': res, 'mean': dict(cer=float(mean[0]), raw=float(mean[1]), dict=float(mean[2])), 'synthetic_samples': len(SX), 'real_recordings': len(X)},
          open(os.path.join(TRAIN, 'results.json'), 'w'), indent=1)
