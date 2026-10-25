FROM node:24-bookworm-slim AS build

WORKDIR /app
RUN corepack enable

COPY package.json pnpm-lock.yaml tsconfig.json tsconfig.build.json vite.config.ts ./
RUN pnpm install --frozen-lockfile --ignore-scripts

COPY src ./src
COPY SPECS.md ./SPECS.md

RUN ./node_modules/.bin/vp test
RUN ./node_modules/.bin/vp run build

FROM node:24-bookworm-slim AS runtime

WORKDIR /app
ENV NODE_ENV=production
ENV DATA_PATH=/data/files
ENV PORT=3000

RUN corepack enable

COPY package.json pnpm-lock.yaml ./
RUN pnpm install --frozen-lockfile --prod --ignore-scripts

COPY --from=build /app/dist ./dist

VOLUME ["/data"]
EXPOSE 3000

# The slim image has no curl/wget: check /health with Node itself.
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
  CMD node -e "fetch('http://127.0.0.1:' + (process.env.PORT || 3000) + '/health').then((r) => process.exit(r.ok ? 0 : 1)).catch(() => process.exit(1))"

CMD ["node", "dist/server.js"]
