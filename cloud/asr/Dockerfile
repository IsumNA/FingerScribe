# NVIDIA Parakeet TDT 0.6B v3 speech-to-text for FingerScribe, as a Nebius AI Cloud endpoint.
FROM nvcr.io/nvidia/nemo:25.07
WORKDIR /app
RUN pip install --no-cache-dir fastapi uvicorn soundfile
COPY server.py .
ENV ASR_MODEL=nvidia/parakeet-tdt-0.6b-v3
EXPOSE 8080
CMD ["uvicorn", "server:app", "--host", "0.0.0.0", "--port", "8080"]
