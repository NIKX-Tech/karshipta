import { readFileSync } from 'node:fs';
import tailwindcss from '@tailwindcss/vite';
import adapter from '@sveltejs/adapter-static';
import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig } from 'vite';

// Surfaced in the About dialog (about-dialog.svelte); read once at build
// time since there's no server left at runtime to ask package.json for it
// (same reasoning as PUBLIC_OPENAIP_KEY/PUBLIC_GATEWAY_WS_URL in the
// Dockerfile - a static build has nothing to read a file from later).
const { version: appVersion } = JSON.parse(readFileSync('./package.json', 'utf-8'));

export default defineConfig({
	define: {
		__APP_VERSION__: JSON.stringify(appVersion)
	},
	plugins: [
		tailwindcss(),
		sveltekit({
			compilerOptions: {
				// Force runes mode for the project, except for libraries. Can be removed in svelte 6.
				runes: ({ filename }) =>
					filename.split(/[/\\]/).includes('node_modules') ? undefined : true
			},

			// Static, not adapter-auto: this app has no +page.server.ts/+server.ts
			// anywhere (a pure client-side WebGL map + WebSocket console), so
			// there's no server logic to run at all - a static file server (see
			// Dockerfile) is correct and simpler than a Node runtime. fallback
			// covers a future second route the same single-page shell serves;
			// today there's exactly one route, so it's also just the build output.
			adapter: adapter({ fallback: 'index.html' })
		})
	]
});
