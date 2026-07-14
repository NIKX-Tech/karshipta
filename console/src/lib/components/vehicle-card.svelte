<script lang="ts">
	import { flightModeToJSON } from '$lib/gen/karshipta/v1/common';
	import { fleet, isConfigTerminal, type Vehicle } from '$lib/fleet-store.svelte';

	interface Props {
		vehicleId: string;
		vehicle: Vehicle;
		/** See VehicleDetail: per-instance override of fleet.readonly, for a
		 * multi-tenant consumer showing owned and view-only vehicles in the
		 * same list. Omitting it keeps the existing store-wide behavior. */
		readonly?: boolean;
	}

	const { vehicleId, vehicle, readonly }: Props = $props();
	const effectiveReadonly = $derived(readonly ?? fleet.readonly);

	const FLIGHT_MODE_PREFIX = 'FLIGHT_MODE_';

	const state = $derived(vehicle.state);
	const modeLabel = $derived(
		state ? flightModeToJSON(state.flightMode).replace(FLIGHT_MODE_PREFIX, '') : 'UNKNOWN'
	);
	const batteryPct = $derived(state?.battery?.remainingPct);
	const connected = $derived(state?.connected ?? false);
	const selected = $derived(fleet.selectedVehicleId === vehicleId);
	const removable = $derived(!effectiveReadonly && !state?.armed && !state?.inAir);
	const removePending = $derived(
		fleet
			.configRequestsFor(vehicleId)
			.some((tracker) => tracker.kind === 'remove' && !isConfigTerminal(tracker.status))
	);

	function toggleSelect() {
		fleet.select(selected ? undefined : vehicleId);
	}

	function remove(event: Event) {
		event.stopPropagation();
		if (vehicle.source === 'demo') {
			fleet.removeDemoVehicle(vehicleId);
		} else {
			fleet.requestRemoveVehicle(vehicleId);
		}
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
		: 'border-edge bg-panel/90 hover:border-fg-muted'} {state && !connected
		? 'opacity-50 grayscale'
		: ''}"
>
	<div class="flex items-center gap-2">
		<h2 class="font-mono text-sm font-semibold">{vehicleId}</h2>
		{#if vehicle.source === 'demo'}
			<span class="text-fg-muted text-[9px] font-medium tracking-widest">DEMO</span>
		{/if}
		{#if state?.armed}
			<span class="text-armed text-[9px] font-medium tracking-widest">ARMED</span>
		{/if}
		{#if effectiveReadonly}
			<span class="text-fg-muted text-[9px] font-medium tracking-widest">VIEW ONLY</span>
		{/if}
		<span
			class="ml-auto inline-block h-2 w-2 rounded-full {connected
				? 'bg-accent animate-pulse'
				: 'bg-critical'}"
			role="status"
			aria-label={connected ? 'Link live' : 'Link lost'}
			title={connected ? 'Link live' : 'Link lost'}
		></span>
		{#if !effectiveReadonly}
			<button
				class="text-fg-muted hover:text-critical px-0.5 text-xs leading-none disabled:cursor-not-allowed disabled:opacity-30"
				aria-label="Remove {vehicleId}"
				title={removable ? 'Remove vehicle' : 'Land and disarm before removing'}
				disabled={!removable || removePending}
				onclick={remove}
			>
				&#x2715;
			</button>
		{/if}
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
