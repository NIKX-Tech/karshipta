# CLAUDE.md: karshipta/console

You are assisting development of the Karshipta console: a SvelteKit web dashboard showing live ward fleets on a map and sending commands and missions to the gateway.

## Stack (locked)

- Svelte 5 (runes syntax: $state, $derived, $effect) + SvelteKit, TypeScript strict mode.
- Tailwind CSS + shadcn-svelte for UI components.
- MapLibre GL JS for the map. No React, no deck.gl unless explicitly requested.
- Protobuf types generated with ts-proto into `src/lib/gen/`. Regenerate, never hand-edit.
- Vite. Static adapter not required here (console may need a node adapter later); default adapter-auto.

## Non-negotiable rules

1. The protobuf schema in `../proto/karshipta/v1/` is the single source of truth. Every WebSocket frame is one binary Envelope, decoded with the generated types. No JSON payloads, no untyped message handling, no magic strings for message kinds: switch on the Envelope oneof.
2. `"strict": true` in tsconfig. No `any`, no non-null assertions without a comment justifying them, no @ts-ignore.
3. State architecture: one `FleetStore` module (Svelte 5 runes in a .svelte.ts file) owns all ward state, keyed by ward_id. Components subscribe to the store; nothing else touches the WebSocket.
4. The transport lives in one module (`src/lib/transport/`), with connect/reconnect/backoff logic isolated there. Components never see raw WebSocket objects.
5. Every user action that sends a Command generates a uuid command_id and tracks the CommandAck; surface rejections to the user with the reason string. No fire-and-forget.
6. Accessibility and keyboard basics on interactive controls. Confirmation dialog on destructive or flight-critical actions (disarm in air, land, RTL).

## Style

- Components small and dumb; logic in `src/lib/`. File names kebab-case, components PascalCase.
- Prettier + eslint + svelte-check must pass; CI fails otherwise.
- Comments explain why, not what.

## Hygiene

- Plain ASCII punctuation only: hyphens, colons, parentheses, no em dashes or curly quotes, for consistent rendering across terminals, diffs, and editors.
- Conventional commits (`feat(console): ...`). Never add AI attribution to commits.

## When asked to write code

Write complete, runnable code including config changes. After any feature, state exactly how to verify it in the browser (commands, URL, expected behavior). Do not scaffold beyond the current milestone.
