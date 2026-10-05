FROM ubuntu:26.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install --yes --no-install-recommends \
        g++ libssl-dev python3-cryptography \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
