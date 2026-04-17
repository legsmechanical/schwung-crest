FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    build-essential \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    python3 \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
