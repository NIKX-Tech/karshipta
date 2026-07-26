// maplibre-gl's worker script imports a second chunk via a plain relative
// specifier ("./maplibre-gl-shared.mjs"), resolved against wherever the
// worker script itself is served from. Vite's per-import `?url` asset
// handling copies a file to a content-hashed path but does not rewrite (or
// even discover) imports inside an opaque asset, so the worker's own
// relative import 404s even once the worker file itself is served
// correctly. Copying both files as-is into static/ keeps them adjacent at a
// stable path, exactly like node_modules/maplibre-gl/dist/, side-stepping
// Vite's import graph entirely. See fleet-map.svelte's setWorkerUrl call.
import { copyFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const consoleRoot = dirname(dirname(fileURLToPath(import.meta.url)));
const src = join(consoleRoot, 'node_modules/maplibre-gl/dist');
const dest = join(consoleRoot, 'static/maplibre-gl');

mkdirSync(dest, { recursive: true });
for (const name of [
	'maplibre-gl-worker.mjs',
	'maplibre-gl-worker.mjs.map',
	'maplibre-gl-shared.mjs',
	'maplibre-gl-shared.mjs.map'
]) {
	copyFileSync(join(src, name), join(dest, name));
}
