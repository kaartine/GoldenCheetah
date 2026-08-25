FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        blender \
        ca-certificates \
        python3-numpy \
    && rm -rf /var/lib/apt/lists/*

ENTRYPOINT ["blender"]
