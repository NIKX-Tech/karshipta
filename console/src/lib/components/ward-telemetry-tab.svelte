<script lang="ts">
	import { wardClassToJSON } from '$lib/gen/karshipta/v1/common';
	import { fleet } from '$lib/fleet-store.svelte';
	import {
		formatBatteryLabel,
		formatGpsLabel,
		formatHeadingCompass,
		formatModeLabel
	} from '$lib/ward-format';
	import { unitsStore } from '$lib/units/units-store.svelte';
	import { formatAltitude, formatSpeed } from '$lib/units/format';

	interface Props {
		wardId: string;
	}

	const { wardId }: Props = $props();

	const WARD_CLASS_PREFIX = 'WARD_CLASS_';

	const ward = $derived(fleet.wards[wardId]);
	const info = $derived(ward?.info);
	const state = $derived(ward?.state);
	const wardClassLabel = $derived(
		info ? wardClassToJSON(info.wardClass).replace(WARD_CLASS_PREFIX, '').toLowerCase() : undefined
	);

	const gpsLabel = $derived(formatGpsLabel(state?.gps));
	const batteryLabel = $derived(formatBatteryLabel(state?.battery));
	const modeLabel = $derived(state?.flight ? formatModeLabel(state.flight) : undefined);
	const groundSpeed = $derived(
		state?.velocity ? Math.hypot(state.velocity.northMS, state.velocity.eastMS) : undefined
	);
	// Up positive (the wire's own down_m_s is NED, down positive) - the sign
	// most operators read correctly on sight; the label spells out
	// climbing/descending/level too, since a bare "+1.2 m/s" is easy to
	// misread under pressure, especially for a ward that's currently armed
	// and airborne.
	const verticalSpeedMS = $derived(state?.velocity ? -state.velocity.downMS : undefined);
	const verticalSpeedLabel = $derived.by(() => {
		if (verticalSpeedMS === undefined) return '?';
		const formatted = formatSpeed(Math.abs(verticalSpeedMS), unitsStore.current);
		if (Math.abs(verticalSpeedMS) < 0.1) return `level (${formatted})`;
		return verticalSpeedMS > 0 ? `climbing ${formatted}` : `descending ${formatted}`;
	});
</script>

<div class="flex flex-col gap-3">
	<!-- Identity, mode, ARMED, battery percentage, heading, altitude (rel),
	     and speed also appear in the always-visible ward-status-strip.svelte
	     above this tab, compact and at a glance - repeated here on purpose
	     now that Telemetry has its own dedicated tab: a client reading this
	     tab wants the full, labeled picture, not just what fits in a
	     header strip. -->
	{#if info}
		<p class="text-[10px] text-fg-muted">
			{info.autopilot}
			{info.firmwareVersion} &middot; {wardClassLabel}
		</p>
	{/if}

	{#if state}
		<div class="flex flex-col gap-3">
			<section>
				<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">STATUS</h3>
				<dl class="mt-1.5 grid grid-cols-2 gap-x-3 gap-y-1.5 text-xs">
					<dt class="text-fg-muted">Health</dt>
					<dd class={state.healthOk ? '' : 'text-critical'}>{state.healthOk ? 'OK' : 'fault'}</dd>
					<dt class="text-fg-muted">Link</dt>
					<dd class={state.connected ? '' : 'text-critical'}>
						{state.connected ? 'connected' : 'lost'}
					</dd>
					<dt class="text-fg-muted">Battery</dt>
					<dd class="font-mono tabular-nums">{batteryLabel}</dd>
					{#if state.flight}
						<dt class="text-fg-muted">Mode</dt>
						<dd class="font-medium">{modeLabel}</dd>
						<dt class="text-fg-muted">Airborne</dt>
						<dd class="font-medium">{state.flight.inAir ? 'in air' : 'grounded'}</dd>
					{/if}
				</dl>
			</section>

			<section>
				<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">POSITION</h3>
				<dl class="mt-1.5 grid grid-cols-2 gap-x-3 gap-y-1.5 text-xs">
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
					<dt class="text-fg-muted">GPS</dt>
					<dd class="font-mono tabular-nums">
						{gpsLabel} &middot; {state.gps?.numSatellites ?? 0} sat
					</dd>
					<dt class="text-fg-muted">GPS accuracy</dt>
					<dd class="font-mono tabular-nums">
						{state.gps !== undefined ? `HDOP ${state.gps.hdop.toFixed(1)}` : '?'}
					</dd>
					<dt class="text-fg-muted">Position</dt>
					<dd class="font-mono text-[10px] tabular-nums">
						{state.position
							? `${state.position.latitudeDeg.toFixed(5)}, ${state.position.longitudeDeg.toFixed(5)}`
							: 'unknown'}
					</dd>
				</dl>
			</section>

			<section>
				<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">MOTION</h3>
				<dl class="mt-1.5 grid grid-cols-2 gap-x-3 gap-y-1.5 text-xs">
					<dt class="text-fg-muted">Heading</dt>
					<dd class="font-mono tabular-nums">
						{state.headingDeg.toFixed(0)}&deg; ({formatHeadingCompass(state.headingDeg)})
					</dd>
					<dt class="text-fg-muted">Speed</dt>
					<dd class="font-mono tabular-nums">
						{groundSpeed === undefined ? '?' : formatSpeed(groundSpeed, unitsStore.current)}
					</dd>
					<dt class="text-fg-muted">Vertical</dt>
					<dd class="font-mono tabular-nums">{verticalSpeedLabel}</dd>
				</dl>
			</section>

			{#if state.tags.length > 0}
				<section>
					<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">TAGS</h3>
					<p class="mt-1.5 font-mono text-[10px]">{state.tags.join(', ')}</p>
				</section>
			{/if}
		</div>
	{:else}
		<p class="text-xs text-fg-muted">Waiting for telemetry</p>
	{/if}
</div>
