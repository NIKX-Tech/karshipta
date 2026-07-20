# Multi-stage build for the console (ROADMAP.md C6).
#
# Static, not a Node server: this app has no +page.server.ts/+server.ts
# anywhere (a pure client-side WebGL map + WebSocket console), so
# adapter-static (vite.config.ts) is correct here and nginx just needs to
# serve the resulting files - no Node runtime in the final image at all.
#
# PUBLIC_* env vars (PUBLIC_GATEWAY_WS_URL, PUBLIC_READONLY,
# PUBLIC_OPENAIP_KEY) are read via $env/dynamic/public, which for a static
# build gets baked into build/_app/env.js at `vite build` time - there is no
# server left at container-start to read fresh values from. They must be
# passed as build args (`docker build --build-arg PUBLIC_GATEWAY_WS_URL=...`),
# not as `docker run -e ...`; the latter has no effect on an already-built
# static bundle. Verified directly: building with/without the var set
# produces a different build/_app/env.js each time.
#
#   docker build -f console/Dockerfile \
#     --build-arg PUBLIC_GATEWAY_WS_URL=ws://gateway:8765 \
#     -t karshipta-console .

FROM node:22-slim AS builder

WORKDIR /src
COPY console/package.json console/package-lock.json ./
RUN npm ci

COPY proto/ ../proto/
COPY console/ .

ARG PUBLIC_GATEWAY_WS_URL=""
ARG PUBLIC_READONLY=""
ARG PUBLIC_OPENAIP_KEY=""
ENV PUBLIC_GATEWAY_WS_URL=$PUBLIC_GATEWAY_WS_URL
ENV PUBLIC_READONLY=$PUBLIC_READONLY
ENV PUBLIC_OPENAIP_KEY=$PUBLIC_OPENAIP_KEY

RUN npm run proto:gen && npm run build

FROM nginx:1.27-alpine AS runtime

COPY deploy/console-nginx.conf /etc/nginx/conf.d/default.conf
COPY --from=builder /src/build /usr/share/nginx/html

EXPOSE 80
