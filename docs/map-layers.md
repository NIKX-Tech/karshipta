# Map layers and reference data

Every optional overlay the console's map can show, beyond the live ward
fleet itself: what it is, where the data comes from, whether it needs a
key, and its current implementation status. Ward types this is written
for aren't drone-only - flight, ground, marine, and anything else a
gateway or Herald device reports - so "useful for X" below calls out which
ward types actually benefit from a given layer.

All optional layers default off except Cities (see each entry). An
operator opts in per layer from the map's layers menu (the icon next to
the zoom controls).

## Implemented

### No-fly zones (airspace)

- **Status:** Live.
- **Source:** OpenAIP `/api/airspaces`, requires `PUBLIC_OPENAIP_KEY`.
- **Purpose:** Real controlled/restricted/prohibited airspace polygons.
  Prohibited (red), Restricted (amber), Other (blue).
- **Useful for:** Flight wards.
- **Files:** `src/lib/geozones/`

### Obstacles

- **Status:** Live.
- **Source:** OpenAIP `/api/obstacles`, same key as No-fly zones.
- **Purpose:** Towers, masts, wind turbines - real collision hazards for
  low-altitude flight. Popup shows height, elevation, country, and a
  Wikipedia link when OpenAIP's own OSM import carries one.
- **Useful for:** Flight wards, low-altitude ground ops near structures.
- **Files:** `src/lib/obstacles/`

### Airports

- **Status:** Live.
- **Source:** OpenAIP `/api/airports`, same key as No-fly zones.
- **Purpose:** Airfields and heliports. Popup shows ICAO code, elevation,
  country.
- **Useful for:** Flight wards (traffic/airspace awareness near strips).
- **Files:** `src/lib/airports/`

### Cities

- **Status:** Live. **Default on** (the one layer that is - see below).
- **Source:** Bundled dataset, not a live API - GeoNames' `cities15000`
  dump (CC BY 4.0), filtered to population >= 100,000 (6,234 of the
  original ~34,000). No key, no network call after the one-time load
  (dynamically imported, ~260KB, code-split into its own chunk so it
  doesn't bloat the initial bundle). Thinned by population as zoom
  decreases so a whole-continent view doesn't show all 6,000+ pins at
  once.
- **Purpose:** General map orientation/reference, not operational data -
  why it defaults on where every other layer here defaults off.
- **Useful for:** All ward types (it's just a map reference layer).
- **Files:** `src/lib/cities/`

### Aircraft

- **Status:** Live.
- **Source:** airplanes.live `/v2/point/[lat]/[lon]/[radius]` - no key, no
  signup, documented limit 1 request/second. Originally built against
  OpenSky Network's anonymous REST API, but that sends a fixed
  Access-Control-Allow-Origin locked to opensky-network.org itself, so it
  can never be called from any other browser origin (confirmed live via a
  CORS error, not an assumption) - swapped out for airplanes.live, which
  sends a wildcard CORS header and actually works client-side.
- **Purpose:** Real crewed-aircraft ADS-B positions - actual traffic
  sharing the airspace, not just its boundaries (complements No-fly
  zones). Category-shaped, heading-rotated icons (plane/rotorcraft/
  glider/UAV/ground vehicle) and short movement trails.
- **Military:** A distinct marker color (not a separate icon shape - the
  feed's `category` and military status are independent, so a military
  transport is still shaped like any other `heavy` aircraft, a military
  helicopter like any other `rotorcraft`) driven by the real `dbFlags`
  bitmask airplanes.live itself returns, confirmed live against a Royal
  Netherlands Air Force Apache. `ownOp` often already names the
  operating branch directly for military aircraft (it did for that
  Apache: "Royal Netherlands Air Force"). The adsbdb enrichment below is
  civil-registry-only, confirmed live to return "unknown aircraft" /
  "unknown callsign" for the same Apache - it contributes nothing extra
  for military traffic, which is expected, not a bug.
- **Emergency:** Same real-field treatment - a distinct marker color plus
  a decoded label (general/medical/minimum fuel/radio failure/unlawful
  interference/downed) from the feed's own `emergency` status, not a
  guess from squawk code alone.
- **Popup enrichment:** Operator, route, registered country, and
  manufacturer via adsbdb.com (free, no key) when airplanes.live's own
  fields are missing - civil aircraft only, per the Military note above.
- **Viewport handling:** The `/point` endpoint's radius is capped at
  250nm regardless of zoom, so a single query over a wide (zoomed-out)
  viewport only ever covered a small, misleadingly dense fraction of it.
  Fixed two ways: the viewport splits into up to 4 tiled point queries
  when it's wider than one tile's coverage, and categories are thinned by
  zoom (only `heavy` aircraft show below zoom 6, matching the same
  tiered-by-zoom pattern Cities uses for population).
- **Useful for:** Flight wards.
- **Files:** `src/lib/aircraft/`

### Earthquakes

- **Status:** Live.
- **Source:** USGS `fdsnws/event` feed, fully open, no key. 30-day
  lookback, magnitude >= 2.5.
- **Purpose:** Recent seismic activity. Marker size scales with magnitude.
- **Useful for:** Ground and marine ops in seismically active regions;
  lower general utility than the other layers.
- **Files:** `src/lib/earthquakes/`

### Wildfires

- **Status:** Live.
- **Source:** NASA FIRMS `area/csv` API (VIIRS SNPP NRT), needs a free
  `MAP_KEY` (`PUBLIC_FIRMS_KEY`) - registered at
  firms.modaps.eosdis.nasa.gov, not a shared/demo key. Documented limit:
  5,000 transactions/10 minutes, generous. Returns CSV, not JSON - the
  only layer here that does.
- **Purpose:** Near-real-time satellite fire/hotspot detection.
- **Useful for:** Any outdoor fleet - flight or ground - avoiding active
  fire zones.
- **Files:** `src/lib/wildfires/`

### Map style picker

- **Status:** Live.
- **Source:** CARTO (dark/light/Voyager roadmap tiles) and OpenTopoMap
  (terrain), both free/no-key.
- **Purpose:** Map/Roadmap/Terrain/Satellite. Roadmap only appears
  alongside the light theme (falls back to Map if the theme changes out
  from under it) - the basemap deliberately follows the app's own
  dark/light toggle rather than exposing an independent choice, to avoid
  a light-map-under-dark-chrome mismatch.
- **Files:** `src/lib/components/fleet-map.svelte` (`MAP_STYLE_OPTIONS`)

### Theme (Light / Dark / System)

- **Status:** Live.
- **Purpose:** Not a map layer, but lives in the same right-rail control
  cluster. `themeStore` only knows light/dark; System is this app's own
  addition on top, tracking the OS preference live while selected.
- **Files:** `src/lib/theme.svelte.ts`, `src/lib/components/theme-toggle.svelte`

### Units (Metric / Imperial)

- **Status:** Live.
- **Purpose:** Also not a map layer. Affects read-only formatting only
  (altitude, speed, distance readouts, the measure tool, obstacle/airport
  elevations, weather) - never command inputs (takeoff altitude, waypoint
  altitude) or the wire protocol, which stay in meters/m-per-second as the
  schema defines them. Vehicle-scale speeds (aircraft ground speed, wind)
  format as km/h or mph via `formatVehicleSpeed`, not raw m/s - the
  general-public convention for speeds at that magnitude, distinct from
  ward/drone telemetry speed (`formatSpeed`, m/s), which stays natural at
  drone scale.
- **Files:** `src/lib/units/`, `src/lib/components/units-toggle.svelte`

### Bathymetry (GEBCO)

- **Status:** Live.
- **Source:** GEBCO WMS (`wms.gebco.net`), free, no key.
- **Purpose:** Real ocean depth data instead of a flat blue ocean - a Map
  Style picker option (`bathymetry`) alongside Map/Roadmap/Terrain/
  Satellite, not a togglable overlay.
- **Useful for:** Marine and submarine wards specifically.
- **Files:** `src/lib/components/fleet-map.svelte` (`MAP_STYLE_OPTIONS`)

### METAR weather (airport popups)

- **Status:** Live.
- **Source:** NOAA Aviation Weather Center
  (`aviationweather.gov/api/data/metar`), free, no key.
- **Purpose:** Real airport surface weather (wind, temperature,
  visibility), fetched on hover for that specific airport - an
  enhancement to the existing Airport popup, not a separate layer, since
  METAR data is inherently per-airport.
- **Useful for:** Flight wards.
- **Files:** `src/lib/components/fleet-map.svelte` (`fetchMetarHtml`)

### Weather widget (Open-Meteo)

- **Status:** Live. **Default on** (needs no key, same reasoning as
  Cities).
- **Source:** Open-Meteo, free, no key, no meaningful rate limit.
- **Purpose:** Not a map layer - a small always-on widget for the current
  map center: condition icon (from the real WMO `weather_code` + day/
  night), temperature (+ feels-like when meaningfully different), wind
  and gusts, cloud cover, humidity, pressure, a rain/showers/snowfall
  breakdown, and visibility.
- **Useful for:** All ward types (wind matters for flight, wind and
  conditions matter for marine, general conditions for ground).
- **Files:** `src/lib/weather/`

### NDBC buoys

- **Effort:** Medium-high.
- **Source:** NOAA NDBC. Confirmed genuinely feasible but not a single
  bbox query like every other layer here: real-time conditions are
  per-station (`data/realtime2/{station_id}.txt`, plain text not JSON),
  and there's no live "buoys near this bbox" endpoint. Needs a bundled
  station-location dataset first (NDBC publishes one, ~1,900 stations,
  same shape as the Cities dataset) to know which buoys exist and where,
  then a live per-station text fetch on demand (e.g. on click) for actual
  conditions.
- **Purpose:** Wave height, sea state, marine conditions.
- **Useful for:** Marine wards specifically.

### AISStream (vessel AIS)

- **Effort:** High - the largest lift on this list.
- **Source:** AISStream.io, confirmed real and free, but needs a
  registered API key *and* is WebSocket-based (persistent connection,
  reconnect/backoff logic), unlike every other layer here's simple
  request/response shape.
- **Purpose:** Real-time vessel positions - the same "real traffic, not
  just boundaries" value airplanes.live already provides for aircraft.
- **Useful for:** Marine wards specifically.

## Shared infrastructure

`src/lib/openaip/request-gate.ts` - a request coordinator shared by
No-fly zones, Obstacles, and Airports (the three OpenAIP-backed layers),
since all three read from the same rate-limited key. Serializes and
spaces out requests across all three so enabling more than one at once
doesn't trip OpenAIP's shared volume limit, even though each layer alone
stays well under it. Aircraft (airplanes.live), Earthquakes (USGS), and
Wildfires (FIRMS) each have their own independent service and budget, so
they run their own debounce/cache/retry rather than sharing this gate.

`src/lib/openaip/tile-bounds.ts` - splits an oversized viewport into up
to 2 side-by-side queries for the three OpenAIP layers, since each one's
own source module still clamps to OpenAIP's confirmed 5-degree bbox
limit. Smaller than Aircraft's own up-to-4-tile response to the same
class of problem, on purpose - OpenAIP's shared key is far tighter than
airplanes.live's, so this stays conservative about the extra request
volume.
