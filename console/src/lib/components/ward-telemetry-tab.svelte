<script lang="ts">
	import { wardClassToJSON } from '$lib/gen/karshipta/v1/common';
	import { fleet } from '$lib/fleet-store.svelte';
	import {
		formatBatteryLabel,
		formatGpsLabel,
		formatHeadingCompass,
		formatModeLabel,
		isStaleSample
	} from '$lib/ward-format';
	import { unitsStore } from '$lib/units/units-store.svelte';
	import { formatAltitude, formatSpeed } from '$lib/units/format';
	import { formatTimeAgo } from '$lib/time-format';
	import LogsPanel from '$lib/components/logs-panel.svelte';

	interface Props {
		wardId: string;
		/** See WardControlTab's own Props comment: per-instance override of
		 * fleet.readonly. Telemetry itself stays visible regardless (it's
		 * read-only by nature); this only gates the Flight logs trigger it
		 * hosts, which isn't. */
		readonly?: boolean;
	}

	const { wardId, readonly }: Props = $props();

	const WARD_CLASS_PREFIX = 'WARD_CLASS_';

	const ward = $derived(fleet.wards[wardId]);
	const info = $derived(ward?.info);
	const wardState = $derived(ward?.state);
	const wardClassLabel = $derived(
		info ? wardClassToJSON(info.wardClass).replace(WARD_CLASS_PREFIX, '').toLowerCase() : undefined
	);

	// karshipta#268: same ticking-clock reasoning as ward-card.svelte and
	// ward-status-strip.svelte - "updated Xs ago" only means something if it
	// keeps counting.
	let now = $state(Date.now());
	$effect(() => {
		const interval = setInterval(() => (now = Date.now()), 10_000);
		return () => clearInterval(interval);
	});
	const updatedAgoLabel = $derived(
		wardState && wardState.ingestionMs > 0 ? formatTimeAgo(wardState.ingestionMs, now) : undefined
	);
	const staleSample = $derived(isStaleSample(wardState));

	const gpsLabel = $derived(formatGpsLabel(wardState?.gps));
	const batteryLabel = $derived(formatBatteryLabel(wardState?.battery));
	const modeLabel = $derived(wardState?.flight ? formatModeLabel(wardState.flight) : undefined);
	const groundSpeed = $derived(
		wardState?.velocity
			? Math.hypot(wardState.velocity.northMS, wardState.velocity.eastMS)
			: undefined
	);
	// Up positive (the wire's own down_m_s is NED, down positive) - the sign
	// most operators read correctly on sight; the label spells out
	// climbing/descending/level too, since a bare "+1.2 m/s" is easy to
	// misread under pressure, especially for a ward that's currently armed
	// and airborne.
	const verticalSpeedMS = $derived(wardState?.velocity ? -wardState.velocity.downMS : undefined);
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
		<div class="flex items-center justify-between gap-2">
			<p class="text-[10px] text-fg-muted">
				{info.autopilot}
				{info.firmwareVersion} &middot; {wardClassLabel}
			</p>
			<!-- Only a flight ward ever has a MAVLink log to pull (MAVSDK's
			     LogFiles plugin, karshipta#249) - same wardState.flight gate the
			     Mode/Airborne rows below use. Placed right on this identity
			     line, not a separate section further down: it's the one
			     lightweight trigger for the dialog that holds the actual
			     list, not a growing block of its own. -->
			{#if wardState?.flight}
				<LogsPanel {wardId} {readonly} />
			{/if}
		</div>
	{/if}

	{#if wardState}
		<div class="flex flex-col gap-3">
			<section>
				<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">STATUS</h3>
				<dl class="mt-1.5 grid grid-cols-2 gap-x-3 gap-y-1.5 text-xs">
					<dt class="text-fg-muted">Health</dt>
					<dd class={wardState.healthOk ? '' : 'text-critical'}>
						{wardState.healthOk ? 'OK' : 'fault'}
					</dd>
					<dt class="text-fg-muted">Link</dt>
					<dd class={wardState.connected ? '' : 'text-critical'}>
						{wardState.connected ? 'connected' : 'lost'}
					</dd>
					{#if updatedAgoLabel}
						<dt class="text-fg-muted">Last update</dt>
						<dd class="font-mono tabular-nums">{updatedAgoLabel}</dd>
					{/if}
					{#if staleSample}
						<dt class="text-fg-muted">Sample freshness</dt>
						<!-- karshipta#268: independent of Link above - this flags a
						     reading that was already old when the gateway received
						     it (a Herald tracker buffering before it sends), which
						     can happen even while Link reads "connected". -->
						<dd
							class="text-accent"
							title="This reading was already old when the gateway received it"
						>
							stale on arrival
						</dd>
					{/if}
					<dt class="text-fg-muted">Battery</dt>
					<dd class="font-mono tabular-nums">{batteryLabel}</dd>
					{#if wardState.flight}
						<dt class="text-fg-muted">Mode</dt>
						<dd class="font-medium">{modeLabel}</dd>
						<dt class="text-fg-muted">Airborne</dt>
						<dd class="font-medium">{wardState.flight.inAir ? 'in air' : 'grounded'}</dd>
					{/if}
				</dl>
			</section>

			<section>
				<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">POSITION</h3>
				<dl class="mt-1.5 grid grid-cols-2 gap-x-3 gap-y-1.5 text-xs">
					<dt class="text-fg-muted">Alt rel</dt>
					<dd class="font-mono tabular-nums">
						{wardState.position !== undefined
							? formatAltitude(wardState.position.altitudeRelM, unitsStore.current)
							: '?'}
					</dd>
					<dt class="text-fg-muted">Alt MSL</dt>
					<dd class="font-mono tabular-nums">
						{wardState.position !== undefined
							? formatAltitude(wardState.position.altitudeMslM, unitsStore.current)
							: '?'}
					</dd>
					<dt class="text-fg-muted">GPS</dt>
					<dd class="font-mono tabular-nums">
						{gpsLabel} &middot; {wardState.gps?.numSatellites ?? 0} sat
					</dd>
					<dt class="text-fg-muted">GPS accuracy</dt>
					<dd class="font-mono tabular-nums">
						{wardState.gps !== undefined ? `HDOP ${wardState.gps.hdop.toFixed(1)}` : '?'}
					</dd>
					<dt class="text-fg-muted">Position</dt>
					<dd class="font-mono text-[10px] tabular-nums">
						{wardState.position
							? `${wardState.position.latitudeDeg.toFixed(5)}, ${wardState.position.longitudeDeg.toFixed(5)}`
							: 'unknown'}
					</dd>
				</dl>
			</section>

			<section>
				<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">MOTION</h3>
				<dl class="mt-1.5 grid grid-cols-2 gap-x-3 gap-y-1.5 text-xs">
					<dt class="text-fg-muted">Heading</dt>
					<dd class="font-mono tabular-nums">
						{wardState.headingDeg.toFixed(0)}&deg; ({formatHeadingCompass(wardState.headingDeg)})
					</dd>
					<dt class="text-fg-muted">Speed</dt>
					<dd class="font-mono tabular-nums">
						{groundSpeed === undefined ? '?' : formatSpeed(groundSpeed, unitsStore.current)}
					</dd>
					<dt class="text-fg-muted">Vertical</dt>
					<dd class="font-mono tabular-nums">{verticalSpeedLabel}</dd>
				</dl>
			</section>

			{#if wardState.tags.length > 0}
				<section>
					<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">TAGS</h3>
					<p class="mt-1.5 font-mono text-[10px]">{wardState.tags.join(', ')}</p>
				</section>
			{/if}
		</div>
	{:else}
		<p class="text-xs text-fg-muted">Waiting for telemetry</p>
	{/if}
</div>
