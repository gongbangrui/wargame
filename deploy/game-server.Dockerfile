FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
ARG WARGAME_ENABLE_FASTDDS=OFF
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build qt6-base-dev qt6-websockets-dev \
    && if [ "$WARGAME_ENABLE_FASTDDS" = "ON" ]; then \
        apt-get install -y --no-install-recommends libfastrtps-dev fastddsgen; \
    fi \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /source
COPY src /source/src
COPY cmake /source/cmake
COPY server /source/server
COPY design/vmf设计.docx /source/design/vmf设计.docx
COPY design/EncoderDecoder/README.txt /source/design/EncoderDecoder/README.txt
COPY design/EncoderDecoder/dic.xml /source/design/EncoderDecoder/dic.xml
COPY design/EncoderDecoder/dic_content.xml /source/design/EncoderDecoder/dic_content.xml
COPY design/EncoderDecoder/message_catalog.json /source/design/EncoderDecoder/message_catalog.json
COPY design/EncoderDecoder/msgStruct /source/design/EncoderDecoder/msgStruct
ARG WARGAME_VERSION=2.0.0
ARG WARGAME_SOURCE_DIGEST=dev
RUN cmake -S /source/server -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DWARGAME_VERSION=${WARGAME_VERSION} -DWARGAME_SOURCE_DIGEST=${WARGAME_SOURCE_DIGEST} -DWARGAME_ENABLE_FASTDDS=${WARGAME_ENABLE_FASTDDS} \
    && cmake --build /build --target wargame_server

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
ARG WARGAME_ENABLE_FASTDDS=OFF
RUN apt-get update && apt-get install -y --no-install-recommends \
    libqt6core6 libqt6network6 libqt6websockets6 ca-certificates netcat-openbsd \
    && if [ "$WARGAME_ENABLE_FASTDDS" = "ON" ]; then \
        apt-get install -y --no-install-recommends libfastrtps2.11t64; \
    fi \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --home /opt/wargame wargame \
    && mkdir -p /opt/wargame /data \
    && chown -R wargame:wargame /opt/wargame /data
COPY --from=build /build/wargame_server /opt/wargame/wargame_server
COPY --from=build /source/design/EncoderDecoder /opt/wargame/design/EncoderDecoder
COPY map/metadata.json /opt/wargame/map/metadata.json
ENV WARGAME_MAP_DIR=/opt/wargame/map
USER wargame
WORKDIR /opt/wargame
ARG WARGAME_VERSION=2.0.0
ARG WARGAME_SOURCE_DIGEST=dev
LABEL org.opencontainers.image.title="wargame-server" \
      org.opencontainers.image.version="${WARGAME_VERSION}" \
      org.opencontainers.image.revision="${WARGAME_SOURCE_DIGEST}"
EXPOSE 8090
CMD ["/opt/wargame/wargame_server"]
