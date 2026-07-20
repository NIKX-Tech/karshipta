# Multi-stage build for the gateway (BRIEF.md M6).
#
# Build context is the repo root, not gateway/: the CMake build needs the
# sibling proto/ directory (gateway/CMakeLists.txt, KARSHIPTA_PROTO_DIR).
#
#   docker build -f gateway/Dockerfile -t karshipta-gateway .
#
# ubuntu:22.04 for both stages, matching the exact recipe already proven in
# .github/workflows/ci.yml's gateway job (same MAVSDK release asset,
# libspdlog-dev, protobuf-compiler, libprotobuf-dev).

FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        curl \
        ca-certificates \
        protobuf-compiler \
        libprotobuf-dev \
        libspdlog-dev \
    && curl -fsSL -o /tmp/mavsdk.deb \
        https://github.com/mavlink/MAVSDK/releases/download/v3.17.1/libmavsdk-dev_3.17.1_ubuntu22.04_amd64.deb \
    && apt-get install -y --no-install-recommends /tmp/mavsdk.deb \
    && rm -rf /tmp/mavsdk.deb /var/lib/apt/lists/*

WORKDIR /src
COPY proto/ proto/
COPY gateway/CMakeLists.txt gateway/CMakeLists.txt
COPY gateway/src/ gateway/src/
COPY gateway/libs/ gateway/libs/

RUN cmake -S gateway -B gateway/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build gateway/build -j "$(nproc)"

FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        libprotobuf-dev \
        libspdlog-dev \
    && curl -fsSL -o /tmp/mavsdk.deb \
        https://github.com/mavlink/MAVSDK/releases/download/v3.17.1/libmavsdk-dev_3.17.1_ubuntu22.04_amd64.deb \
    && apt-get install -y --no-install-recommends /tmp/mavsdk.deb \
    && apt-get purge -y --auto-remove curl \
    && rm -rf /tmp/mavsdk.deb /var/lib/apt/lists/*

RUN useradd --system --create-home --home-dir /app --shell /usr/sbin/nologin karshipta
WORKDIR /app

COPY --from=builder /src/gateway/build/src/karshipta_gateway gateway/build/src/karshipta_gateway
COPY gateway/config/gateway.yaml gateway/config/gateway.yaml

RUN chown -R karshipta:karshipta /app
USER karshipta

EXPOSE 8765

CMD ["./gateway/build/src/karshipta_gateway"]
