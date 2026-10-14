# Changelog

Notable changes to Karshipta, most recent first. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning
follows [Semantic Versioning](https://semver.org/) from the first tagged
release onward.

## [0.2.0]

### Added

- console-core: an optional `onMapMove` callback on `FleetMap`, bound to
  MapLibre's `moveend` event (the settled position, not every animation
  frame). Nothing in this console consumes it yet; it exists so a
  consuming app can persist an operator's actual last map view across a
  page reload instead of only ever knowing the fixed center/zoom it
  passed in at mount.
- console-core: an optional `statusBadge` prop on `WardCard` (a label plus
  a muted/warning/critical tone, rendered on its own line rather than
  squeezed into the existing SIM/ARMED/VIEW ONLY badge row, since a
  caller-supplied label has no length guarantee the built-in single-word
  badges do), and a matching `statusBadgeFor` callback prop on `FleetRow`
  that forwards it to each ward's card - added after wiring the badge
  into a consuming app's fleet list showed that most real wards are
  grouped into a named fleet, not sitting in the unassigned bucket, so
  shipping the `WardCard` prop alone would leave the badge invisible for
  the common case. Both are inert extensibility hooks: nothing in this
  console passes either, so there is no visual change here.

### Fixed

- Gateway: three simultaneous MAVLink connections sharing one Mavsdk core
  could spiral into a reconnect storm. `connect_with_retry`'s own retry
  delay only applies between failed connection attempts; when a ward's
  link flapped disconnected immediately after a connect that itself
  resolved near-instantly (socket and `System` already established),
  `run_reconnect_loop` had no floor at all between the lost link and the
  next attempt. One flap became self-sustaining: the resulting log volume
  competed for the same thread pool processing heartbeats, producing more
  flaps - confirmed live at over 500,000 log lines in ten seconds, never
  recovering on its own. Fixed by adding a minimum delay between a lost
  link and the next reconnect attempt, the one place in the loop that was
  missing one.
- deploy: the three-SITL demo's GCS MAVLink stream all silently targeted
  the same remote port regardless of instance, contradicting the compose
  file's own comment that each instance uses a distinct target port - the
  entrypoint script set a distinct local port per instance but never
  passed the matching remote-port flag for the GCS stream, unlike the
  offboard stream one command below it which already did. Fixed to match
  that working pattern, and removed the now-unnecessary host port
  mappings, which were only creating a bind conflict between Docker's own
  port-forwarding proxy and a host-native gateway process.
- deploy: the demo SITL stack only worked on Mac/Windows Docker Desktop.
  It sent every instance's GCS and offboard MAVLink stream to
  `192.168.65.254`, Docker Desktop's fixed host-gateway address, which
  does not exist on native Linux Docker Engine - every packet SITL sent
  there was silently dropped, making the exact same compose file produce
  a persistent "link lost"/no-telemetry failure on Ubuntu while working
  fine on Mac. Switched the target to `host.docker.internal` with an
  `extra_hosts: host.docker.internal:host-gateway` entry so the name
  resolves on both platforms - except `px4-mavlink`'s own `-t` flag
  rejects a hostname outright ("invalid partner ip") and does not resolve
  one itself, confirmed immediately after the first fix shipped. Resolved
  by reading the real IP `extra_hosts` already wrote into `/etc/hosts` at
  container start and passing that literal IP to both `-t` flags instead
  of the hostname.
- console-core: a real ward's state envelope falls invalid/NaN GPS fields
  back to (0, 0) rather than dropping the frame; that used to crash the
  whole snapshot before it ever reached the map, so the fallback value
  went unnoticed until the crash itself was fixed. Once it stopped
  crashing, a ward with no real GPS fix plotted at "Null Island" in the
  middle of the ocean off the coast of Africa. The map marker sync loop
  now skips (and removes any existing marker for) that (0, 0) fallback
  specifically; the ward stays visible in the fleet list, only the map
  layer is affected.
- console-core: the aircraft overlay's server-side proxy to adsb.lol
  (added in 0.1.3) was returning 403 "User-Agent too generic; include
  valid contact info" in production even though an identical request from
  the host's own `curl` succeeded. Not a rate limit - adsb.lol's usage
  policy requires an identifying User-Agent with contact info, and
  Node's default fetch User-Agent doesn't qualify. Fixed by sending one,
  verified live against the real API with and without the fix to isolate
  the exact cause.
- console-core: CARTO's free legacy tile CDN now bakes an "API KEY
  REQUIRED" watermark directly into every basemap tile image while still
  returning HTTP 200, making the entire map background unusable in
  production with nothing to catch it as an error. Replaced with
  OpenFreeMap, which serves vector styles rather than CARTO's raster PNG
  tiles, so the swap was done additively - a new vector source plus a
  vendored, swappable layer set per style (dark/positron/liberty for the
  existing default-dark/default-light/roadmap options) rather than a full
  `map.setStyle()` call, which would tear down and rebuild every
  source/layer the map owns (ward markers, zones, obstacles, aircraft),
  not just the basemap. Terrain, Satellite, and Bathymetry, which were
  already separate raster sources, are unaffected. Style layer arrays are
  vendored at build time rather than fetched live, so rendering doesn't
  depend on OpenFreeMap's style JSON shape staying fixed.
- console-core: after the OpenFreeMap migration, Arabic/Hebrew and other
  RTL-script place-name labels rendered as disconnected, backwards
  characters instead of properly shaped, right-to-left text. CARTO's old
  raster tiles had label text baked into the tile image, so this never
  came up before; OpenFreeMap's vector styles label places via glyphs
  rendered at runtime, which needs MapLibre's RTL text plugin
  (`setRTLTextPlugin`) to shape and bidi-reorder the text. Fixed by
  vendoring the plugin and registering it, lazily, before map creation.
- console-core: three Lighthouse-driven fixes bundled together. ARIA:
  `aria-label`/`aria-labelledby` on elements with an implicit `generic` or
  `paragraph` role (bare divs/spans) is spec-prohibited; added the correct
  explicit role everywhere this occurred on the map and in dropdown menus.
  Performance: the three vendored OpenFreeMap style JSONs (about 109 KiB
  combined) were statically imported and evaluated on every map load even
  though only one style is ever active at a time; now loaded on demand per
  style and cached after first load. Best practices: Obstacles and
  Airports were fetched directly from the browser against OpenAIP, which
  sends no CORS headers - the same problem aircraft data already had and
  was already fixed for - so both now go through the same same-origin
  proxy pattern, and no longer need a client-side API key, so their layer
  toggles are unconditional like Aircraft's. `obstacleStore.configure()`
  and `airportStore.configure()` are removed as a result; any consumer
  still calling them needs updating. FleetMap also gained an optional
  `initialZoom` prop (defaults to the existing constant) for a
  karshipta-cloud follow-up that shows a country-scale view before a
  visitor grants geolocation, instead of a fixed close-in zoom.
- console-core: two accessibility bugs found live via axe-core, outside
  the original Lighthouse audit's scope. `Tabs` always set
  `aria-controls` to a fixed id regardless of whether a matching tabpanel
  element actually existed in the DOM, which broke in a collapsible panel
  layout where the tab buttons stay mounted but the panel content is
  fully unmounted while collapsed; fixed with an optional `panelsMounted`
  prop (default `true`, so every existing usage is unaffected) that omits
  `aria-controls` when the caller's panels genuinely aren't present. The
  map's "Loading airspace/aircraft data..." banner used an accent color
  on the panel background that, despite already being darkened once for
  light-theme contrast, still only reached 3.85:1 against white for 12px
  text, short of WCAG AA's 4.5:1; switched to the same foreground color
  already used by the map's cluster-marker button for the same reason.
- console-core: an intermittent blank map on remount, confirmed via a
  real browser crash capture ("Cannot read properties of undefined
  (reading 'off')" deep inside MapLibre's own compiled internals). The
  `ScaleControl` used for the map's legend-embedded scale bar is created
  without `addControl()` (its DOM node is inserted into the app's own
  legend panel rather than MapLibre's corner box), so it needs a manual
  `onRemove()` call instead of the usual automatic cleanup - that manual
  call existed in two separate places and both fired on every unmount,
  so `onRemove()` ran twice on the same control; MapLibre's own
  `onRemove()` clears the control's internal map reference, so the second
  call crashed inside it. Fixed by giving the control exactly one owner
  for that cleanup call. A related, separately real bug surfaced and was
  fixed on the same branch: `setRTLTextPlugin` was being called
  unconditionally from the component's instance script, which reruns on
  every single mount, violating MapLibre's own documented "once per page"
  contract for that API and corrupting an in-flight worker registration
  on repeat visits; moved to a `<script module>` block that runs exactly
  once per page load regardless of how many times the component mounts,
  with `setWorkerUrl` kept in its original relative order ahead of it to
  avoid introducing a new ordering bug while fixing this one.
- console-core: a build-time crash in any consumer app that server-renders
  (a Node adapter, not static). The module-level `setWorkerUrl`/
  `setRTLTextPlugin` calls added by the remount fix above run the instant
  the module is first imported, including during SvelteKit's SSR/build
  analysis pass in Node, where `document` does not exist - a page that
  only statically imports `FleetMap` pulled this code in during that
  analysis regardless of whether the component ever actually renders
  there. Fixed by guarding the module-level calls behind a
  `typeof document !== 'undefined'` check, matching the same guard this
  app's own entry page already used for its instance script.
- console-core: the events feed redesigned from a single cramped row per
  event (a dot, timestamp, ward id, message) that got hard to read once
  the ward id was a full UUID, into a card layout: the event message with
  a relative "time ago" on the same line, the ward id below as plain
  descriptive text, and severity shown as a left-border accent instead of
  a leading dot.
- console-core: the map's attribution control came up expanded by default
  instead of staying collapsed. MapLibre's own compact-attribution control
  starts out empty (no attribution text loaded yet), so its internal
  guard skips collapsing it at that point; once real attribution content
  arrives moments later, MapLibre's own update handler clears that empty
  state and expands the control fresh, after any one-time "collapse it"
  call had already run - so it ended up open regardless. A `MutationObserver`
  watching the control's class attribute was tried first and worked, but
  measured as expensive enough on a busy telemetry map (`sourcedata` can
  fire many times a second) to risk stalling the render thread. Replaced
  with a plain class removal re-run on the same three events MapLibre's
  own code reacts to, registered after its own listeners so it always
  runs after them and wins.

## [0.1.3]

### Fixed

- Gateway: `karshipta_gateway.exe` failed to start on a real Windows
  machine ("libssl-3-x64.dll was not found") because cpp-httplib (used
  only by the Herald HTTP ingestion listener, which never needs HTTPS)
  silently linked against whatever OpenSSL happened to be on the build
  machine - present on GitHub's Windows CI runner, absent on a real
  customer's. Removed the accidental dependency at its source.
- console-core: the aircraft feed's upstream (api.airplanes.live) shut
  down third-party API access entirely; its documented replacement
  (api.adsb.one) turned out to be Cloudflare-bot-challenging every
  request on top of its own infrastructure problems. Aircraft data now
  goes through a same-origin server-side proxy (a consuming app's own
  `/api/aircraft/point/[lat]/[lon]/[radius]` route, backed by
  adsb.lol) instead of a direct browser-to-third-party fetch, since
  none of the direct options send the CORS headers a browser needs.
- console-core: aircraft altitude scaling washed low-altitude markers
  out to near-invisible against the dark basemap, and lifted real
  aircraft (which fly kilometers higher than any drone) too close to
  ward altitude range on a pitched camera. Color intensity now varies
  by lightness only (hue/saturation stay fixed per category, so
  markers never wash out); the altitude-to-lift curve widened
  aircraft/ward separation across the whole realistic altitude range.
- console-core: `ScaleControl.setUnit` could be called before MapLibre
  had finished mounting the control, crashing inside MapLibre's own
  code in a consuming app with enough surrounding components to shift
  effect scheduling.
- console-core: `WardCard`'s header wrapped to a second line instead of
  truncating the ward id; the "+ New Fleet" popover got clipped inside
  a scrolling panel once the ward list was long enough to scroll.

### Added

- console-core: aircraft markers lift above their ground anchor by
  altitude on a pitched camera, the same treatment ward markers
  already had.
- console-core: `FleetRow` and `Disclosure` exported from the
  library's public surface, with `FleetRow` gaining an optional
  `wardIds` prop for a multi-tenant consumer whose ward store includes
  wards it doesn't own.

## [0.1.2]

### Added

- console-core: aircraft markers rebuilt as DOM `maplibregl.Marker`
  elements with real inline SVG instead of a canvas-rasterized MapLibre
  symbol layer - the rasterized icons lost fine detail (a thin fuselage
  vanishing at typical marker size) that native SVG doesn't. Real
  top-down airliner silhouettes per category replace the old hand-drawn
  shape; military fixed-wing aircraft get a genuinely distinct fighter
  silhouette instead of a recolored civilian one, military rotorcraft
  keep the existing helicopter shape. Marker position and heading now
  animate between fixes instead of snapping, and the refresh interval
  drops from 16s to 7s.
- console-core: Obstacles, Airports, and Aircraft map layers default on;
  Obstacles gained zoom-based thinning to support that. Richer popup
  data: airport private/PPR access flags and METAR flight category,
  earthquake USGS PAGER alert level and tsunami warnings with a link to
  the event page, wildfire day/night detection.
- console-core: new "center on my location" map control, using the
  browser's geolocation.
- console-core: export the Fleet/Zone/FleetMission message codecs from
  the `/wire` subpath, not just the package root - the server-side-safe
  entrypoint a consuming backend actually needs to encode/decode those
  envelopes without pulling in browser-only Svelte component CSS.

### Fixed

- console-core: `WardMissionStatus` (the enum `WardMissionState` and
  `WardMissionPlan` reference) was missing from both the package root
  and `/wire` exports, leaving no way to reference ACTIVE/STOPPED/
  REJECTED by name.
- console-core: aircraft viewport tiling collapsed into small dense
  clusters instead of spreading real coverage across a wide zoomed-out
  view; fixed to a grid sized proportionally to how much bigger the
  viewport is than one tile's coverage. Also fixed the loading banner
  blinking on ordinary background refreshes.

## [0.1.1]

### Added

- console-core: publish the Zones (operator-drawn keep-in/keep-out
  geofences), Fleet-groups (named ward groupings within a session), and
  Fleet-mission (per-ward mission wizard, list, live map routes) wire
  types, stores, and display components from the library's export
  surface - built earlier but never exposed to a consuming app until now.

### Fixed

- Gateway: the disarm RPC issued when removing or stopping a ward had no
  timeout of its own, so an autopilot that never acknowledged the command
  could hang the call indefinitely. Now bounded to 5 seconds; a timeout is
  treated as a failed disarm instead of left to hang.
- Gateway: real Windows relay builds. `KARSHIPTA_GATEWAY_ENABLE_RELAY=ON`
  was a hard error on Windows since the relay transport landed; once
  relayly shipped Windows SDK support, three more real problems surfaced
  the first time this was actually built there - a missing mbedTLS
  dependency, a missing `bcrypt` link for its entropy source, and a
  Windows-DLL-export gap in ixwebsocket itself (several `static const`
  class members used as default-argument values, none exported - MSVC
  does not auto-export data symbols from a DLL the way it does
  functions). All three fixed; Windows now builds and links with relay
  enabled like every other platform. The `herald-only` release flavor no
  longer links relayly at all, on any platform - Herald ingestion is
  plain HTTP, it never had the NAT-traversal problem relay solves.

## [0.1.0] - first public release

The first public release: gateway milestones M1 to M10, console milestones
C1 to C8 (see [ROADMAP.md](ROADMAP.md) for the full list). Summarized here
retroactively, since no changelog existed before this file did.

### Added

**Gateway**

- Multi-ward MAVLink connectivity via MAVSDK (PX4, ArduPilot SITL), one
  shared Mavsdk core per process.
- Live telemetry publish at 2 to 10 Hz per ward: position, battery, flight
  mode, armed/in-air state, link status.
- Commands - arm, disarm (including forced), takeoff, land, RTL, goto -
  with a full accepted/executing/success/rejected/timeout ack lifecycle.
- Missions: click-to-plan-equivalent upload, start/pause, repeat count,
  live progress, QGC `.plan` and Mission Planner `.wpl` file import.
- Ward add/remove with persisted fleet state, reconnect handling, and a
  read-only viewer client mode.
- Fleet: named, persistent ward groups, many-to-many membership.
- Zone: named keep-in/keep-out polygons, advisory (warns before upload,
  does not block).
- Fleet Mission: per-ward independently-planned routes submitted as one
  trackable unit, with per-ward and aggregate status through upload,
  active flight, and stop (RTL/Hold/Land, operator's choice); remove and
  edit are safety-gated on every ward having actually stopped. Replaces an
  earlier design that broadcast one shared route to every selected ward
  simultaneously - see
  [docs/fleet-mission-model.md](docs/fleet-mission-model.md).
- Herald ingestion for trackers with no autopilot: native HTTP, a
  declarative YAML field-mapping HTTP path for arbitrary vendor JSON, and
  raw GT06 TCP - see
  [gateway/docs/herald-ingest.md](gateway/docs/herald-ingest.md).
- Optional E2E-encrypted relay transport (relayly) for remote operations
  without a reachable inbound port, verified end to end against a real
  relay server.
- SQLite persistence for Fleet, Zone, and Fleet Mission; YAML persistence
  for ward connection state.
- Two build flavors from one codebase: `karshipta-gateway` (MAVLink +
  Herald) and `karshipta-herald` (Herald only, no MAVSDK dependency).
- Concurrency and crash-safety hardening across the ward/fleet managers
  (two-level locking, transactional persistence writes).

**Console**

- Live fleet map (MapLibre): ward markers, telemetry, trails, mission
  routes, zones, worldwide airspace overlay (OpenAIP, opt-in).
- Command panel with confirm dialogs on flight-critical actions, and an
  events feed with severity.
- Mission planning: click-to-plan waypoints, upload, start/pause, looping,
  live progress.
- Fleet and Zone management UI, and a Fleet Mission list/wizard: pick a
  Fleet or ad-hoc wards, plan an independent route per ward, track status,
  stop/remove/edit per mission.
- Onboarding: empty-state console, add demo/simulated/real wards from the
  UI, gateway connection panel (direct WebSocket or relay pairing).
- Read-only viewer mode.
- Published as an embeddable npm package (`@nikx-tech/karshipta-console-core`).

**Other**

- `proto/karshipta/v1/` protobuf schema as the single source of truth for
  the wire contract, generating both the gateway's C++ types and the
  console's TypeScript types.
- One-command demo: `docker compose up` brings up 3 PX4 SITL wards, the
  gateway, and the console together.
- Ward rename: the core entity renamed from Vehicle to Ward across the
  whole stack, generalized to represent non-flight tracked entities
  (livestock GPS tags, generic trackers) alongside flight vehicles.
- Real release automation (`release-gateway.yml`): builds and publishes
  gateway/herald binaries for macOS (arm64), Linux, and Windows on every
  `v*.*.*` tag.
