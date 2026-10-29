"""Fine-tune on the real sleeve recordings, testing on held-out repetitions.
Usage: python3 train_finetune.py <mode> [folds]   mode = pretrained | scratch | zeroshot"""
import sys, json, time, numpy as np, tensorflow as tf
from common import *

mode = sys.argv[1]
folds = [int(f) for f in sys.argv[2].split(',')] if len(sys.argv) > 2 else [1, 2, 3, 4, 5]
X, W, R = load_isum(os.path.join(ROOT, 'data'))
lexicon = sorted(set(W))
results = []

for test_rep in folds:
    np.random.seed(test_rep); tf.random.set_seed(test_rep)
    tr = [i for i in range(len(X)) if R[i] != test_rep]
    te = [i for i in range(len(X)) if R[i] == test_rep] if mode != 'zeroshot' else list(range(len(X)))
    model = build_model()
    if mode in ('pretrained', 'zeroshot'):
        model.load_weights(os.path.join(TRAIN, f'onhw{TAG}.weights.h5'))
    if mode != 'zeroshot':
        opt = tf.keras.optimizers.Adam(5e-4 if mode == 'pretrained' else 1e-3)
        step = make_step(model, opt)
        Xtr = [X[i] for i in tr]; Ytr = [enc(W[i]) for i in tr]
        epochs = 40 if mode == 'pretrained' else 80
        t0 = time.time()
        for ep in range(epochs):
            for xb, lens, yb, yl, c in batches(Xtr, Ytr, 16, augment=True):
                step(xb, out_len(lens), yb, yl)
        print(f'  trained {epochs} epochs in {time.time()-t0:.0f}s', flush=True)
    cer, wacc, lacc, preds, lex = evaluate(model, [X[i] for i in te], [W[i] for i in te], lexicon)
    dacc, dpred = dict_decode(model, [X[i] for i in te], [W[i] for i in te])
    print(f'{mode} test rep {test_rep}: CER {cer:.3f}  open-vocab word acc {wacc:.3f}  20k-dict acc {dacc:.3f}  lexicon acc {lacc:.3f}', flush=True)
    results.append(dict(rep=test_rep, cer=cer, wacc=wacc, lacc=lacc, dacc=dacc,
                        preds=[(W[i], p, l) for i, p, l in zip(te, preds, lex)]))
    if mode == 'zeroshot': break

json.dump(results, open(os.path.join(TRAIN, f'results_{mode}{TAG}.json'), 'w'))
print(mode, 'MEAN  CER %.3f  open %.3f  20k-dict %.3f  lexicon %.3f' % tuple(np.mean([[r[k] for r in results] for k in ('cer', 'wacc', 'dacc', 'lacc')], 1)))
