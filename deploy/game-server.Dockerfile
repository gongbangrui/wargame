FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build qt6-base-dev qt6-websockets-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /source
COPY src /source/src
COPY cmake /source/cmake
COPY server /source/server
ARG WARGAME_VERSION=0.1.0
RUN cmake -S /source/server -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DWARGAME_VERSION=${WARGAME_VERSION} \
    && cmake --build /build --target wargame_server

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    libqt6core6 libqt6network6 libqt6websockets6 ca-certificates netcat-openbsd \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --home /opt/wargame wargame \
    && mkdir -p /opt/wargame /data \
    && chown -R wargame:wargame /opt/wargame /data
COPY --from=build /build/wargame_server /opt/wargame/wargame_server
USER wargame
WORKDIR /opt/wargame
ARG WARGAME_VERSION=0.1.0
LABEL org.opencontainers.image.title="wargame-server" \
      org.opencontainers.image.version="${WARGAME_VERSION}"
EXPOSE 8090
CMD ["/opt/wargame/wargame_server"]
