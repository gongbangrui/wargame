FROM python:3.13-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    DATA_DIR=/data

WORKDIR /app
COPY server/account/requirements.txt /tmp/requirements.txt
RUN python -m pip install --no-cache-dir -r /tmp/requirements.txt
COPY server/account /app
RUN mkdir -p /data && useradd --system --uid 10001 --home /app wargame && chown -R wargame:wargame /app /data
USER wargame

ARG WARGAME_VERSION=1.0.0
ARG WARGAME_SOURCE_DIGEST=dev
ENV WARGAME_VERSION=${WARGAME_VERSION} \
    WARGAME_SOURCE_DIGEST=${WARGAME_SOURCE_DIGEST}
LABEL org.opencontainers.image.title="wargame-account-web" \
      org.opencontainers.image.version="${WARGAME_VERSION}" \
      org.opencontainers.image.revision="${WARGAME_SOURCE_DIGEST}"

EXPOSE 8080
CMD ["python", "-m", "uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8080", "--proxy-headers"]
