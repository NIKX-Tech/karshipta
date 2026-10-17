<script lang="ts">
	import { flightModeToJSON, WardOrigin } from '$lib/gen/karshipta/v1/common';
	import { fleet, isConfigTerminal, type Ward } from '$lib/fleet-store.svelte';
	import { unitsStore } from '$lib/units/units-store.svelte';
	import { formatAltitude } from '$lib/units/format';
	import ConfirmDialog from '$lib/components/confirm-dialog.svelte';

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

	// Named wardState, not state: Svelte 5 resolves a bare $state(...) call
	// as legacy store auto-subscription to a binding literally named
	// `state` if one is in scope, even one declared with $derived - this
	// component now also needs the $state rune itself (confirmRemoveOpen
	// below), which fails to compile under that name collision.
	const wardState = $derived(ward.state);
	// Mode is a flight-autopilot concept - unset (undefined) for a ward with
	// no flight field, e.g. a livestock tag with no autopilot state machine.
	const modeLabel = $derived(
		wardState?.flight
			? flightModeToJSON(wardState.flight.flightMode).replace(FLIGHT_MODE_PREFIX, '')
			: undefined
	);
	const batteryPct = $derived(wardState?.battery?.remainingPct);
	const connected = $derived(wardState?.connected ?? false);
	// No autopilot behind this ward at all (demo math standing in for
	// one), distinct from SITL (a real autopilot binary, just not attached
	// to hardware): a violet accent + "SIM" label, not the same fade used
	// for a lost link below - this needs to read as "a working demo," not
	// "something's wrong with it".
	const synthetic = $derived(ward.info?.origin === WardOrigin.WARD_ORIGIN_SYNTHETIC);
	const selected = $derived(fleet.selectedWardId === wardId);
	// Gated on connected too, not just armed/inAir: those two fields are
	// the last state this ward ever reported, frozen the instant the link
	// drops - a ward that has been offline for hours can carry a
	// permanently stale armed:true with nothing left to refresh it. Once
	// disconnected there is no live link to actually be unsafe about
	// removing, so the block only makes sense while connected (confirmed
	// live: an already-dead ward's remove button stayed disabled forever).
	const removable = $derived(
		!effectiveReadonly && !(connected && (wardState?.flight?.armed || wardState?.flight?.inAir))
	);
	const removePending = $derived(
		fleet
			.configRequestsFor(wardId)
			.some((tracker) => tracker.kind === 'remove' && !isConfigTerminal(tracker.status))
	);

	function toggleSelect() {
		fleet.select(selected ? undefined : wardId);
	}

	// Same two-step confirm fleet-row.svelte already uses for deleting a
	// fleet - removing a ward is just as irreversible (worse for a real
	// one, see performRemove's own branch below), and an instant click
	// here had no way back, confirmed live as exactly the kind of
	// accidental action this library's own command-panel confirms (land/
	// RTL/force-disarm) already exist to prevent.
	let confirmRemoveOpen = $state(false);

	function remove(event: Event) {
		event.stopPropagation();
		confirmRemoveOpen = true;
	}

	function performRemove() {
		confirmRemoveOpen = false;
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
			: 'border-edge bg-panel/90 hover:border-fg-muted'} {wardState && !connected
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
		{#if wardState?.flight?.armed}
			<!-- Distinct label while disconnected, not just the card's own
			     opacity-50 grayscale fade below: a bare "ARMED" reads as a
			     current fact, and the fade alone is easy to miss at a glance -
			     this is the last state the ward ever reported, possibly hours
			     stale, not something currently true (see removable's own
			     comment on the same distinction for the remove button). -->
			<span
				class="shrink-0 text-[9px] font-medium tracking-widest text-armed"
				title={connected ? undefined : 'Last known state before the link dropped - may be stale'}
			>
				{connected ? 'ARMED' : 'ARMED (STALE)'}
			</span>
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
	{#if wardState}
		<p class="mt-1 truncate font-mono text-[10px] text-fg-muted tabular-nums">
			{#if modeLabel}{modeLabel} &middot;
			{/if}{wardState.position !== undefined
				? formatAltitude(wardState.position.altitudeRelM, unitsStore.current)
				: '?'} &middot; {batteryPct === undefined || batteryPct < 0
				? '?'
				: `${batteryPct.toFixed(0)}%`}
		</p>
	{:else}
		<p class="mt-1 text-[10px] text-fg-muted">Waiting for telemetry</p>
	{/if}
</div>

{#if confirmRemoveOpen}
	<ConfirmDialog
		title="Remove {wardId}"
		body={ward.source === 'demo'
			? 'This removes the demo ward. You can add another any time.'
			: 'This removes the ward from your fleet. If a gateway is still reporting it, this does not disconnect the gateway itself.'}
		confirmLabel="Remove"
		onconfirm={performRemove}
		oncancel={() => (confirmRemoveOpen = false)}
	/>
{/if}
