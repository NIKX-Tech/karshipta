<script lang="ts">
	import { flightModeToJSON } from '$lib/gen/karshipta/v1/common';
	import { fleet, type Vehicle } from '$lib/fleet-store.svelte';

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
	const selected = $derived(fleet.selectedVehicleId === vehicleId);

	function toggleSelect() {
		fleet.select(selected ? undefined : vehicleId);
	}
</script>

<div
	role="button"
	tabindex="0"
	aria-pressed={selected}
	onclick={toggleSelect}
	onkeydown={(event) => {
		if (event.key === 'Enter' || event.key === ' ') {
			event.preventDefault();
			toggleSelect();
		}
	}}
	class="cursor-pointer rounded border px-3 py-2 {selected
		? 'border-selected bg-panel'
		: 'border-edge bg-panel/90 hover:border-fg-muted'}"
>
	<div class="flex items-center gap-2">
		<h2 class="font-mono text-sm font-semibold">{vehicleId}</h2>
		{#if state?.armed}
			<span class="text-armed text-[9px] font-medium tracking-widest">ARMED</span>
		{/if}
		<span
			class="ml-auto inline-block h-2 w-2 rounded-full {connected
				? 'bg-accent animate-pulse'
				: 'bg-critical'}"
			role="status"
			aria-label={connected ? 'Link live' : 'Link lost'}
			title={connected ? 'Link live' : 'Link lost'}
		></span>
	</div>
	{#if state}
		<p class="text-fg-muted mt-1 truncate font-mono text-[10px] tabular-nums">
			{modeLabel}
			&middot; {state.position?.altitudeRelM.toFixed(0) ?? '?'} m &middot; {batteryPct ===
				undefined || batteryPct < 0
				? '?'
				: `${batteryPct.toFixed(0)}%`}
		</p>
	{:else}
		<p class="text-fg-muted mt-1 text-[10px]">Waiting for telemetry</p>
	{/if}
</div>
