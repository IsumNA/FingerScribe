# FingerScribe training as a Nebius AI Cloud serverless Job: corpus (Nemotron) -> pen paths -> simulated finger motion
# -> path model + word reader, exported for the phone. GPU image; per-second billing; no cluster to manage.
FROM tensorflow/tensorflow:2.16.1-gpu
WORKDIR /job
RUN pip install --no-cache-dir numpy wordfreq && git clone --depth 1 https://github.com/sjvasquez/handwriting-synthesis /job/hs
COPY *.py job.sh ./
ENV FS_DATA=/data
ENTRYPOINT ["bash", "job.sh"]
