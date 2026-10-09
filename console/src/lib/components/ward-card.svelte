<script lang="ts">
	import { flightModeToJSON, WardOrigin } from '$lib/gen/karshipta/v1/common';
	import { fleet, isConfigTerminal, type Ward } from '$lib/fleet-store.svelte';
	import { unitsStore } from '$lib/units/units-store.svelte';
	import { formatAltitude } from '$lib/units/format';

	interface StatusBadge {
		label: string;
		tone: 'muted' | 'warning' | 'critical';
	}

	interface Props {
		wardId: string;
		ward: Ward;
		/** See WardTab: per-instance override of fleet.readonly, for a
		 * multi-tenant consumer showing owned and view-only wards in the
		 * same list. Omitting it keeps the existing store-wide behavior. */
		readonly?: boolean;
		/** Generic extension point for a consumer app with its own
		 * per-ward status concept this library has no notion of. Absent by
		 * default; this library never sets it itself. */
		statusBadge?: StatusBadge;
	}

	const { wardId, ward, readonly, statusBadge }: Props = $props();
	const statusBadgeToneClass: Record<StatusBadge['tone'], string> = {
		muted: 'text-fg-muted',
		warning: 'text-accent',
		critical: 'text-critical'
	};
	const effectiveReadonly = $derived(readonly ?? fleet.readonly);

	const FLIGHT_MODE_PREFIX = 'FLIGHT_MODE_';

	const state = $derived(ward.state);
	// Mode is a flight-autopilot concept - unset (undefined) for a ward with
	// no flight field, e.g. a livestock tag with no autopilot state machine.
	const modeLabel = $derived(
		state?.flight
			? flightModeToJSON(state.flight.flightMode).replace(FLIGHT_MODE_PREFIX, '')
			: undefined
	);
	const batteryPct = $derived(state?.battery?.remainingPct);
	const connected = $derived(state?.connected ?? false);
	// No autopilot behind this ward at all (demo math standing in for
	// one), distinct from SITL (a real autopilot binary, just not attached
	// to hardware): a violet accent + "SIM" label, not the same fade used
	// for a lost link below - this needs to read as "a working demo," not
	// "something's wrong with it".
	const synthetic = $derived(ward.info?.origin === WardOrigin.WARD_ORIGIN_SYNTHETIC);
	const selected = $derived(fleet.selectedWardId === wardId);
	const removable = $derived(!effectiveReadonly && !state?.flight?.armed && !state?.flight?.inAir);
	const removePending = $derived(
		fleet
			.configRequestsFor(wardId)
			.some((tracker) => tracker.kind === 'remove' && !isConfigTerminal(tracker.status))
	);

	function toggleSelect() {
		fleet.select(selected ? undefined : wardId);
	}

	function remove(event: Event) {
		event.stopPropagation();
		if (ward.source === 'demo') {
			fleet.removeDemoWard(wardId);
		} else {
			fleet.requestRemoveWard(wardId);
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
		: synthetic
			? 'border-synthetic/40 bg-panel/90 hover:border-synthetic'
			: 'border-edge bg-panel/90 hover:border-fg-muted'} {state && !connected
		? 'opacity-50 grayscale'
		: ''}"
>
	<div class="flex items-center gap-2">
		<!-- min-w-0 lets a long wardId truncate instead of the row wrapping -
		     the dot and remove button must never drop to a second line, or a
		     card in a narrow rail can render with an apparently-missing
		     remove button that's actually just wrapped out of view below. -->
		<h2 class="min-w-0 flex-1 truncate font-mono text-sm font-semibold">{wardId}</h2>
		{#if synthetic}
			<span
				class="shrink-0 text-[9px] font-medium tracking-widest text-synthetic"
				title="No autopilot behind this ward; demo telemetry standing in for one"
			>
				SIM
			</span>
		{/if}
		{#if state?.flight?.armed}
			<span class="shrink-0 text-[9px] font-medium tracking-widest text-armed">ARMED</span>
		{/if}
		{#if effectiveReadonly}
			<span class="shrink-0 text-[9px] font-medium tracking-widest text-fg-muted">VIEW ONLY</span>
		{/if}
		<span
			class="inline-block h-2 w-2 shrink-0 rounded-full {connected
				? 'animate-pulse bg-accent'
				: 'bg-critical'}"
			role="status"
			aria-label={connected ? 'Link live' : 'Link lost'}
			title={connected ? 'Link live' : 'Link lost'}
		></span>
		{#if !effectiveReadonly}
			<button
				class="shrink-0 px-0.5 text-xs leading-none text-fg-muted hover:text-critical disabled:cursor-not-allowed disabled:opacity-30"
				aria-label="Remove {wardId}"
				title={removable ? 'Remove ward' : 'Land and disarm before removing'}
				disabled={!removable || removePending}
				onclick={remove}
			>
				&#x2715;
			</button>
		{/if}
	</div>
	{#if statusBadge}
		<!-- Its own line, not squeezed into the header row with the wardId,
		     dot, and remove button: unlike the single-word built-in badges
		     (SIM, ARMED, VIEW ONLY), a consumer-supplied label has no length
		     guarantee, and cramming it in there pushed the wardId down to a
		     one- or two-character truncation in a narrow rail. -->
		<p class="mt-1 truncate text-[10px] font-medium {statusBadgeToneClass[statusBadge.tone]}">
			{statusBadge.label}
		</p>
	{/if}
	{#if state}
		<p class="mt-1 truncate font-mono text-[10px] text-fg-muted tabular-nums">
			{#if modeLabel}{modeLabel} &middot;
			{/if}{state.position !== undefined
				? formatAltitude(state.position.altitudeRelM, unitsStore.current)
				: '?'} &middot; {batteryPct === undefined || batteryPct < 0
				? '?'
				: `${batteryPct.toFixed(0)}%`}
		</p>
	{:else}
		<p class="mt-1 text-[10px] text-fg-muted">Waiting for telemetry</p>
	{/if}
</div>
