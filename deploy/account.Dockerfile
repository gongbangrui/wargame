FROM python:3.13-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    DATA_DIR=/data

WORKDIR /app
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3-fastapi python3-uvicorn python3-argon2 \
    && rm -rf /var/lib/apt/lists/*
COPY server/account /app
RUN mkdir -p /data && useradd --system --uid 10001 --home /app wargame && chown -R wargame:wargame /app /data
USER wargame

EXPOSE 8080
CMD ["/usr/bin/python3", "-m", "uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8080", "--proxy-headers"]
