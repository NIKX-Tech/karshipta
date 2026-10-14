// maplibre-gl's worker script imports a second chunk via a plain relative
// specifier ("./maplibre-gl-shared.mjs"), resolved against wherever the
// worker script itself is served from. Vite's per-import `?url` asset
// handling copies a file to a content-hashed path but does not rewrite (or
// even discover) imports inside an opaque asset, so the worker's own
// relative import 404s even once the worker file itself is served
// correctly. Copying both files as-is into static/ keeps them adjacent at a
// stable path, exactly like node_modules/maplibre-gl/dist/, side-stepping
// Vite's import graph entirely. See fleet-map.svelte's setWorkerUrl call.
//
// Same treatment for mapbox-gl-rtl-text: maplibre's setRTLTextPlugin takes a
// URL it fetches itself at runtime, not something Vite's import graph ever
// sees, so the file has to exist at a stable static path rather than being
// bundled. Self-hosted rather than pointed at unpkg to match this project's
// no-required-third-party-CDN stance (same reasoning as Fontsource for
// fonts) and to keep the console usable on an air-gapped deployment.
import { copyFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const consoleRoot = dirname(dirname(fileURLToPath(import.meta.url)));
const dest = join(consoleRoot, 'static/maplibre-gl');
mkdirSync(dest, { recursive: true });

const maplibreSrc = join(consoleRoot, 'node_modules/maplibre-gl/dist');
for (const name of [
	'maplibre-gl-worker.mjs',
	'maplibre-gl-worker.mjs.map',
	'maplibre-gl-shared.mjs',
	'maplibre-gl-shared.mjs.map'
]) {
	copyFileSync(join(maplibreSrc, name), join(dest, name));
}

copyFileSync(
	join(consoleRoot, 'node_modules/@mapbox/mapbox-gl-rtl-text/dist/mapbox-gl-rtl-text.js'),
	join(dest, 'mapbox-gl-rtl-text.js')
);
