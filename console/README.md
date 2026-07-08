# Karshipta console

SvelteKit web dashboard for the Karshipta fleet: live map (MapLibre GL), vehicle status, and, in later milestones, commands and missions. See [CLAUDE.md](CLAUDE.md) for the conventions and `../docs/architecture.md` for the big picture.

## Develop

```sh
npm install
npm run proto:gen   # generate TypeScript types from ../proto (ts-proto via buf)
npm run dev -- --open
```

With no gateway configured, the console runs a fake fleet: three simulated multirotors circling the PX4 SITL default home position. To connect to a real gateway instead, set the WebSocket URL:

```sh
PUBLIC_GATEWAY_WS_URL=ws://localhost:8765 npm run dev
```

## Verify

```sh
npm run lint    # prettier + eslint
npm run check   # svelte-check, TypeScript strict
npm run build
```

Generated code in `src/lib/gen/` is never edited by hand; rerun `npm run proto:gen` after any schema change.
