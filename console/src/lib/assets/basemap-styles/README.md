# Basemap style layers

Vendored `layers` arrays from OpenFreeMap's hosted vector styles, fetched
2026-08-26. These are the "how to paint OpenFreeMap's vector tiles" part
of each style - the tiles themselves, sprite, and glyphs still load live
from OpenFreeMap's CDN as normal map traffic; only the layer _definitions_
are pinned here, so this app's rendering doesn't depend on their style
JSON never changing shape.

- `openfreemap-dark-layers.json` <- `https://tiles.openfreemap.org/styles/dark` (`layers`, 47 entries)
- `openfreemap-positron-layers.json` <- `https://tiles.openfreemap.org/styles/positron` (`layers`, 55 entries)
- `openfreemap-liberty-layers.json` <- `https://tiles.openfreemap.org/styles/liberty` (`layers`, 111 entries minus `natural_earth`)

`natural_earth` (liberty's low-zoom relief shading layer) was dropped: it's
the only layer across all three styles that references a second raster
source (`ne2_shaded`), and dropping it keeps this fix to exactly one new
vector source rather than two. A fast-follow could re-add it plus
`ne2_shaded` if the relief shading is wanted.

See `fleet-map.svelte`'s own comment at the CARTO-to-OpenFreeMap migration
for why these are vendored rather than fetched live. Loaded on demand, one
style at a time (not all three up front), by `load-basemap-layers.ts` in
this same directory.
