"""Pretrain the reader on OnHW-Words500 (pen data, writer-independent fold 0, 6 channels)."""
import sys, time, numpy as np, tensorflow as tf
from common import *

np.random.seed(0); tf.random.set_seed(0)
d = load_onhw(os.path.join(ROOT, 'onhw', 'words500_f0_6ch.npz'))
Xtr, Ytr = d['train']; Xva, Yva = d['val']
print('onhw train', len(Xtr), 'val', len(Xva), flush=True)

model = build_model()
opt = tf.keras.optimizers.Adam(1e-3)
step = make_step(model, opt)
epochs = int(sys.argv[1]) if len(sys.argv) > 1 else 8
best = 9
for ep in range(epochs):
    t0 = time.time(); tot = n = 0
    opt.learning_rate.assign(1e-3 * (0.5 ** max(0, ep - 3)))
    for xb, lens, yb, yl, c in batches(Xtr, Ytr, 32, augment=True):
        tot += float(step(xb, out_len(lens), yb, yl)); n += 1
    sub = np.random.RandomState(1).choice(len(Xva), 600, replace=False)
    Xs = [Xva[i] for i in sub]; Ws = [dec(Yva[i]) for i in sub]
    errs = chars = 0
    for xb, lens, yb, yl, c in batches(Xs, [Yva[i] for i in sub], 64, shuffle=False):
        for j, p in zip(c, greedy(model(xb, training=False).numpy(), out_len(lens))):
            errs += lev(p, Ws[j]); chars += len(Ws[j])
    cer = errs / chars
    print(f'ep {ep} loss {tot/n:.3f} val CER {cer:.3f} ({time.time()-t0:.0f}s)', flush=True)
    if cer < best:
        best = cer; model.save_weights(os.path.join(TRAIN, f'onhw{TAG}.weights.h5'))
print('best val CER', best)
