import tailwindcss from '@tailwindcss/vite';
import adapter from '@sveltejs/adapter-static';
import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig } from 'vite';

export default defineConfig({
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
