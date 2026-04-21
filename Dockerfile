# ---------- builder ----------
FROM ubuntu:26.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc make \
        libc6-dev linux-libc-dev \
        liburing-dev libbpf-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile utils.h benchmark.c ./
COPY --parents v*/ ./
RUN make

# ---------- runtime ----------
FROM ubuntu:26.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        liburing2 libbpf1 \
        iproute2 ethtool && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build ./build

CMD ["bash"]
