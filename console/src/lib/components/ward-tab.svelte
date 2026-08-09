<script lang="ts">
	import { wardClassToJSON } from '$lib/gen/karshipta/v1/common';
	import { fleet } from '$lib/fleet-store.svelte';
	import { formatBatteryLabel, formatGpsLabel } from '$lib/ward-format';
	import CommandPanel from '$lib/components/command-panel.svelte';
	import MissionPanel from '$lib/components/mission-panel.svelte';
	import { unitsStore } from '$lib/units/units-store.svelte';
	import { formatAltitude, formatSpeed } from '$lib/units/format';

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

	const WARD_CLASS_PREFIX = 'WARD_CLASS_';

	const ward = $derived(fleet.wards[wardId]);
	const info = $derived(ward?.info);
	const state = $derived(ward?.state);
	const wardClassLabel = $derived(
		info ? wardClassToJSON(info.wardClass).replace(WARD_CLASS_PREFIX, '').toLowerCase() : undefined
	);

	const gpsLabel = $derived(formatGpsLabel(state?.gps));
	const batteryLabel = $derived(formatBatteryLabel(state?.battery));
	const groundSpeed = $derived(
		state?.velocity ? Math.hypot(state.velocity.northMS, state.velocity.eastMS) : undefined
	);
</script>

<div class="flex flex-col gap-3">
	<!-- Identity, mode, ARMED, and battery percentage already live in the
	     always-visible ward-status-strip.svelte above this scrollable body -
	     repeating them here just cost vertical space for no new information.
	     Voltage and the raw connected/health_ok booleans still get their own
	     rows below since the strip only encodes those two through the link
	     dot's color, not as text. -->
	{#if info}
		<p class="text-[10px] text-fg-muted">
			{info.autopilot}
			{info.firmwareVersion} &middot; {wardClassLabel}
		</p>
	{/if}

	{#if state}
		<dl class="grid grid-cols-2 gap-x-3 gap-y-1.5 text-xs">
			<dt class="text-fg-muted">Health</dt>
			<dd class={state.healthOk ? '' : 'text-critical'}>{state.healthOk ? 'OK' : 'fault'}</dd>
			<dt class="text-fg-muted">Battery</dt>
			<dd class="font-mono tabular-nums">{batteryLabel}</dd>
			{#if state.flight}
				<dt class="text-fg-muted">Airborne</dt>
				<dd class="font-medium">{state.flight.inAir ? 'in air' : 'grounded'}</dd>
			{/if}
			<dt class="text-fg-muted">Alt rel</dt>
			<dd class="font-mono tabular-nums">
				{state.position !== undefined
					? formatAltitude(state.position.altitudeRelM, unitsStore.current)
					: '?'}
			</dd>
			<dt class="text-fg-muted">Alt MSL</dt>
			<dd class="font-mono tabular-nums">
				{state.position !== undefined
					? formatAltitude(state.position.altitudeMslM, unitsStore.current)
					: '?'}
			</dd>
			<dt class="text-fg-muted">Speed</dt>
			<dd class="font-mono tabular-nums">
				{groundSpeed === undefined ? '?' : formatSpeed(groundSpeed, unitsStore.current)}
			</dd>
			<dt class="text-fg-muted">Heading</dt>
			<dd class="font-mono tabular-nums">{state.headingDeg.toFixed(0)}&deg;</dd>
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
</div>
