<script lang="ts">
	import { flightModeToJSON } from '$lib/gen/karshipta/v1/common';
	import type { Vehicle } from '$lib/fleet-store.svelte';

	interface Props {
		vehicleId: string;
		vehicle: Vehicle;
	}

	const { vehicleId, vehicle }: Props = $props();

	const FLIGHT_MODE_PREFIX = 'FLIGHT_MODE_';

	const state = $derived(vehicle.state);
	const modeLabel = $derived(
		state ? flightModeToJSON(state.flightMode).replace(FLIGHT_MODE_PREFIX, '') : 'UNKNOWN'
	);
	const batteryPct = $derived(state?.battery?.remainingPct);
	const connected = $derived(state?.connected ?? false);
</script>

<article class="border-edge bg-panel/90 rounded border p-3">
	<header class="flex items-center gap-2">
		<h2 class="font-mono text-sm font-semibold">{vehicleId}</h2>
		{#if state?.armed}
			<span class="text-armed text-[10px] font-medium tracking-widest">ARMED</span>
		{/if}
		<span
			class="ml-auto inline-block h-2 w-2 rounded-full {connected
				? 'bg-accent animate-pulse'
				: 'bg-critical'}"
			role="status"
			aria-label={connected ? 'Link live' : 'Link lost'}
			title={connected ? 'Link live' : 'Link lost'}
		></span>
	</header>
	{#if state}
		<dl class="mt-2 grid grid-cols-3 gap-x-3 gap-y-1 text-xs">
			<dt class="text-fg-muted">Mode</dt>
			<dd class="col-span-2 font-medium">{modeLabel}</dd>
			<dt class="text-fg-muted">Alt rel</dt>
			<dd class="col-span-2 font-mono tabular-nums">
				{state.position?.altitudeRelM.toFixed(1) ?? '?'} m
			</dd>
			<dt class="text-fg-muted">Battery</dt>
			<dd class="col-span-2 font-mono tabular-nums">
				{batteryPct === undefined || batteryPct < 0 ? 'unknown' : `${batteryPct.toFixed(0)}%`}
			</dd>
		</dl>
	{:else}
		<p class="text-fg-muted mt-2 text-xs">Waiting for telemetry</p>
	{/if}
</article>
