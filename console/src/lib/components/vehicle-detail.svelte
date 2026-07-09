<script lang="ts">
	import { flightModeToJSON, gpsFixTypeToJSON } from '$lib/gen/karshipta/v1/common';
	import { fleet } from '$lib/fleet-store.svelte';
	import CommandPanel from '$lib/components/command-panel.svelte';

	interface Props {
		vehicleId: string;
	}

	const { vehicleId }: Props = $props();

	const FLIGHT_MODE_PREFIX = 'FLIGHT_MODE_';
	const GPS_FIX_PREFIX = 'GPS_FIX_TYPE_';

	const vehicle = $derived(fleet.vehicles[vehicleId]);
	const info = $derived(vehicle?.info);
	const state = $derived(vehicle?.state);

	const modeLabel = $derived(
		state ? flightModeToJSON(state.flightMode).replace(FLIGHT_MODE_PREFIX, '') : 'UNKNOWN'
	);
	const gpsLabel = $derived(
		state?.gps ? gpsFixTypeToJSON(state.gps.fixType).replace(GPS_FIX_PREFIX, '') : 'NO DATA'
	);
	const groundSpeed = $derived(
		state?.velocity ? Math.hypot(state.velocity.northMS, state.velocity.eastMS) : undefined
	);
</script>

<section
	class="border-edge bg-panel flex h-full w-72 flex-col gap-3 overflow-y-auto border-l p-3"
	aria-label="Vehicle detail {vehicleId}"
>
	<header class="flex items-center gap-2">
		<h2 class="font-mono text-sm font-semibold">{vehicleId}</h2>
		{#if state?.armed}
			<span class="text-armed text-[10px] font-medium tracking-widest">ARMED</span>
		{/if}
		<button
			class="text-fg-muted hover:text-fg ml-auto rounded px-1 text-sm leading-none"
			aria-label="Close detail panel"
			onclick={() => fleet.select(undefined)}
		>
			&#x2715;
		</button>
	</header>

	{#if info}
		<p class="text-fg-muted -mt-2 text-[10px]">
			{info.autopilot}
			{info.firmwareVersion} &middot; multirotor
		</p>
	{/if}

	{#if state}
		<dl class="grid grid-cols-2 gap-x-3 gap-y-1.5 text-xs">
			<dt class="text-fg-muted">Mode</dt>
			<dd class="font-medium">{modeLabel}</dd>
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
			<dt class="text-fg-muted">Link</dt>
			<dd class={state.connected ? '' : 'text-critical'}>
				{state.connected ? 'connected' : 'lost'}
			</dd>
		</dl>
	{:else}
		<p class="text-fg-muted text-xs">Waiting for telemetry</p>
	{/if}

	<CommandPanel {vehicleId} />
</section>
