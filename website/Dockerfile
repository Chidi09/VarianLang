# Production container for the Varian website (a live Lumen/Zenith server).
# Generalizes to any Aurora app: swap the `vn lumen build` line + CMD.
#
#   docker build -f website/Dockerfile -t varian-site .
#   docker run -p 8080:8080 varian-site
#
# ── 1. Build the vn toolchain (hardened release) ─────────────────────────────
FROM ubuntu:22.04 AS toolchain
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential libffi-dev libcurl4-openssl-dev libpq-dev libssl-dev \
      libsqlite3-dev libhiredis-dev liburing-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make -j"$(nproc)" release

# ── 2. Compile the Lumen pages into one runnable app (baked to port 8080) ────
FROM toolchain AS site
WORKDIR /src
RUN ./vn lumen build website/pages website/site.vn 8080

# ── 3. Slim runtime image ────────────────────────────────────────────────────
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive LUMEN_QUIET=1
RUN apt-get update && apt-get install -y --no-install-recommends \
      libcurl4 libpq5 libssl3 libsqlite3-0 libhiredis0.14 liburing2 libffi8 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=site /src/vn /usr/local/bin/vn
COPY --from=site /src/vn_modules /app/vn_modules
COPY --from=site /src/website /app/website
EXPOSE 8080
# Production serve: no hot-reload, no LUMEN_DEV overlay (that's dev-only).
CMD ["vn", "run", "website/site.vn"]
