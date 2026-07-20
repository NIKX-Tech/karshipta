// This app has no +page.server.ts/+server.ts anywhere - a pure client-side
// WebGL map + WebSocket console, self-hosted by a single operator, with
// nothing to gain from SSR. Disabling it isn't just an optimization: with
// it on, a request rendered in Node (no window, no localStorage) always
// shows the empty-state onboarding wizard first, since the server has no
// way to know a demo fleet was persisted client-side - the browser paints
// that server response, then hydration replaces it, which is a real,
// reported flash on every reload, not a style nit. With ssr off there's
// only ever the one, correct, client-rendered paint.
export const ssr = false;
