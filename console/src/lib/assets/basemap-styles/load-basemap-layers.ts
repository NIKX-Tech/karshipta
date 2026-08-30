import type * as maplibregl from 'maplibre-gl';

// 'default'/'roadmap' naming matches MapStyle in fleet-map.svelte - kept
// separate from it (this only covers the two vector-backed defaults plus
// roadmap, not terrain/satellite/bathymetry) since those three raster
// styles have no vendored layer JSON of their own to load here. 'roadmap'
// (OpenFreeMap's Liberty style) is offered only alongside the light theme
// - see fleet-map.svelte's own mapStyle comment for why.
export type VectorBasemap = 'dark' | 'light' | 'roadmap';

// Loaded on demand, one style at a time, instead of the three vendored
// JSON files (~109 KiB combined) all being parsed/evaluated up front even
// though only one is ever active - a visitor who never switches basemaps
// used to pay for all three regardless. Results are cached per style below
// so switching back to an already-seen style is instant. A plain module
// Map, not SvelteMap: this is internal memoization never read reactively,
// not component state.
const BASEMAP_LAYER_LOADERS: Record<VectorBasemap, () => Promise<{ default: unknown }>> = {
	dark: () => import('./openfreemap-dark-layers.json'),
	light: () => import('./openfreemap-positron-layers.json'),
	roadmap: () => import('./openfreemap-liberty-layers.json')
};
const basemapLayerCache = new Map<VectorBasemap, maplibregl.LayerSpecification[]>();

export async function loadBasemapLayers(
	style: VectorBasemap
): Promise<maplibregl.LayerSpecification[]> {
	const cached = basemapLayerCache.get(style);
	if (cached) return cached;
	const module = await BASEMAP_LAYER_LOADERS[style]();
	// Vendored JSON's own type inference doesn't structurally match
	// MapLibre's tagged LayerSpecification union (paint expression arrays
	// come back widened) - safe to assert since these are pinned verbatim
	// from OpenFreeMap's own validated style output, never hand-authored.
	const layers = module.default as maplibregl.LayerSpecification[];
	basemapLayerCache.set(style, layers);
	return layers;
}
