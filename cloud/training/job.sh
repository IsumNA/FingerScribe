#!/usr/bin/env bash
# One Nebius AI Cloud job, start to finish. Mount the recordings at /data (object storage or a shared filesystem).
set -euo pipefail
cd /job
echo "== 1/4 Nemotron Lightning writes the corpus (Token Factory)"
[ -f /data/corpus.txt ] || python3 make_corpus.py ${CORPUS_N:-20000} > /data/corpus.txt
echo "== 2/4 corpus -> pen paths (IAM-OnDB styles) -> simulated finger motion"
python3 synth_data.py /data/corpus.txt /data/synth_imu.npz
echo "== 3/4 train: pretrain on simulated + OnHW, fine-tune on the real recordings, held-out writer test"
python3 train_synth.py onhw ${EPOCHS:-6}
echo "== 4/4 export for the phone"
echo "weights: /data/train/synth_onhw.weights.h5  results: /data/train/results.json"
echo done
