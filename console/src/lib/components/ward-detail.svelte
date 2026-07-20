<script lang="ts">
	import {
		flightModeToJSON,
		gpsFixTypeToJSON,
		wardClassToJSON,
		WardOrigin
	} from '$lib/gen/karshipta/v1/common';
	import { fleet } from '$lib/fleet-store.svelte';
	import CommandPanel from '$lib/components/command-panel.svelte';
	import MissionPanel from '$lib/components/mission-panel.svelte';

	interface Props {
		wardId: string;
		/**
		 * Per-instance override for whether command/mission controls render.
		 * Defaults to the store-wide `fleet.readonly` flag, which is all the
		 * single-operator OSS console ever needs: one gateway, one fleet,
		 * every ward in it is "yours". A multi-tenant app is different:
		 * the same session can show wards the signed-in account owns
		 * (commandable) alongside wards it can only view (a teammate's in
		 * an org, or someone else's on a shared map), which one global flag
		 * cannot express. Passing this prop lets the consuming app decide
		 * per ward; omitting it keeps the existing global behavior.
		 */
		readonly?: boolean;
	}

	const { wardId, readonly }: Props = $props();
	const effectiveReadonly = $derived(readonly ?? fleet.readonly);

	const FLIGHT_MODE_PREFIX = 'FLIGHT_MODE_';
	const GPS_FIX_PREFIX = 'GPS_FIX_TYPE_';
	const WARD_CLASS_PREFIX = 'WARD_CLASS_';

	const ward = $derived(fleet.wards[wardId]);
	const info = $derived(ward?.info);
	const state = $derived(ward?.state);
	// See ward-card.svelte: no autopilot behind this ward at all, not
	// SITL (a real autopilot binary flying simulated physics).
	const synthetic = $derived(info?.origin === WardOrigin.WARD_ORIGIN_SYNTHETIC);
	const wardClassLabel = $derived(
		info ? wardClassToJSON(info.wardClass).replace(WARD_CLASS_PREFIX, '').toLowerCase() : undefined
	);

	// Mode is a flight-autopilot concept - unset (undefined) for a ward with
	// no flight field, e.g. a livestock tag with no autopilot state machine.
	const modeLabel = $derived(
		state?.flight
			? flightModeToJSON(state.flight.flightMode).replace(FLIGHT_MODE_PREFIX, '')
			: undefined
	);
	const gpsLabel = $derived(
		state?.gps ? gpsFixTypeToJSON(state.gps.fixType).replace(GPS_FIX_PREFIX, '') : 'NO DATA'
	);
	const groundSpeed = $derived(
		state?.velocity ? Math.hypot(state.velocity.northMS, state.velocity.eastMS) : undefined
	);
</script>

<section
	class="flex h-full w-72 flex-col gap-3 overflow-y-auto border-l border-edge bg-panel p-3"
	aria-label="Ward detail {wardId}"
>
	<header class="flex items-center gap-2">
		<h2 class="font-mono text-sm font-semibold">{wardId}</h2>
		{#if synthetic}
			<span
				class="text-[10px] font-medium tracking-widest text-synthetic"
				title="No autopilot behind this ward; demo telemetry standing in for one"
			>
				SIM
			</span>
		{/if}
		{#if state?.flight?.armed}
			<span class="text-[10px] font-medium tracking-widest text-armed">ARMED</span>
		{/if}
		<button
			class="ml-auto rounded px-1 text-sm leading-none text-fg-muted hover:text-fg"
			aria-label="Close detail panel"
			onclick={() => fleet.select(undefined)}
		>
			&#x2715;
		</button>
	</header>

	{#if info}
		<p class="-mt-2 text-[10px] text-fg-muted">
			{info.autopilot}
			{info.firmwareVersion} &middot; {wardClassLabel}
		</p>
	{/if}

	{#if state}
		<dl class="grid grid-cols-2 gap-x-3 gap-y-1.5 text-xs">
			{#if modeLabel}
				<dt class="text-fg-muted">Mode</dt>
				<dd class="font-medium">{modeLabel}</dd>
			{/if}
			<dt class="text-fg-muted">Alt rel</dt>
			<dd class="font-mono tabular-nums">{state.position?.altitudeRelM.toFixed(1) ?? '?'} m</dd>
			<dt class="text-fg-muted">Alt MSL</dt>
			<dd class="font-mono tabular-nums">{state.position?.altitudeMslM.toFixed(0) ?? '?'} m</dd>
			<dt class="text-fg-muted">Speed</dt>
			<dd class="font-mono tabular-nums">
				{groundSpeed === undefined ? '?' : groundSpeed.toFixed(1)} m/s
			</dd>
			<dt class="text-fg-muted">Heading</dt>
			<dd class="font-mono tabular-nums">{state.headingDeg.toFixed(0)}&deg;</dd>
			<dt class="text-fg-muted">Battery</dt>
			<dd class="font-mono tabular-nums">
				{state.battery && state.battery.remainingPct >= 0
					? `${state.battery.remainingPct.toFixed(0)}% (${state.battery.voltageV.toFixed(1)} V)`
					: 'unknown'}
			</dd>
			<dt class="text-fg-muted">GPS</dt>
			<dd class="font-mono tabular-nums">
				{gpsLabel} &middot; {state.gps?.numSatellites ?? 0} sat
			</dd>
			<dt class="text-fg-muted">Position</dt>
			<dd class="font-mono text-[10px] tabular-nums">
				{state.position
					? `${state.position.latitudeDeg.toFixed(5)}, ${state.position.longitudeDeg.toFixed(5)}`
					: 'unknown'}
			</dd>
			{#if state.tags.length > 0}
				<dt class="text-fg-muted">Tags</dt>
				<dd class="font-mono text-[10px]">{state.tags.join(', ')}</dd>
			{/if}
			<dt class="text-fg-muted">Link</dt>
			<dd class={state.connected ? '' : 'text-critical'}>
				{state.connected ? 'connected' : 'lost'}
			</dd>
		</dl>
	{:else}
		<p class="text-xs text-fg-muted">Waiting for telemetry</p>
	{/if}

	{#if !effectiveReadonly && state?.flight}
		<CommandPanel {wardId} />
		<MissionPanel {wardId} />
	{/if}
</section>
