# Training on Nebius AI Cloud

One serverless **Job** does the whole loop:

1. **Nemotron 3.5 Lightning** (Token Factory) writes ~20,000 journal-style fragments (`make_corpus.py`).
2. A handwriting generator trained on IAM-OnDB turns them into pen paths in 13 writer styles (`gen_hand.py`),
   and a physics finger model with domain randomisation turns paths into accelerometer + gyro at 100 Hz (`sim_imu.py`).
3. The reader is pretrained on simulated + OnHW data, then fine-tuned on the real sleeve recordings and tested on
   words it never saw (`train_synth.py`, `common.py`).
4. Weights and the held-out numbers are written to `/data/train/`. The app runs the reader exported as `app/words_reader.json`; this job is how the next one is trained.

```
docker build -t cr.nebius.cloud/<registry>/fingerscribe-train:1 .
docker push    cr.nebius.cloud/<registry>/fingerscribe-train:1
```
Nebius console → Serverless AI → Jobs → Create: that image, 1 GPU, mount your recordings bucket at `/data`,
env `NEBIUS_API_KEY`, optional `CORPUS_N`, `EPOCHS`. The job log prints the held-out numbers; copy them into the
README results table.

Honest note: on 18 Oct a reader trained on simulated motion *alone* scored 0% on real words; mixed in, it was
within noise. Simulated data helps the **path** model (3.13 mm vs 3.68 mm with more data) more than the reader.
The job is the pipeline to keep improving that, not a claim that it is solved.

`common.py`, `train_pretrain.py`, `train_finetune.py`, `train_synth.py` and `gen_hand.py` are the scripts used for the
18 Oct results, with paths moved to `$FS_DATA`.

**Not included:** the finger-motion simulator (`sim_imu.py`), the sleeve recordings and the trained weights stay private
while we commercialise FingerScribe. `synth_data.py` stops with a clear message if `sim_imu.py` is missing; plug in any
`simulate(pen_offsets, n_chars, rng) -> (T, 6)` accel+gyro function at 100 Hz to run step 2, or skip it and train on
your own recordings (step 3).
