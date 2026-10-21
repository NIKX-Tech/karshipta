<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { WardOrigin } from '$lib/gen/karshipta/v1/common';
	import {
		batteryHintLabel,
		batteryTier,
		formatBatteryPct,
		formatHeadingCompass,
		formatModeLabel,
		isStaleSample,
		linkDotState
	} from '$lib/ward-format';
	import { unitsStore } from '$lib/units/units-store.svelte';
	import { formatAltitude, formatSpeed } from '$lib/units/format';
	import { formatTimeAgo } from '$lib/time-format';

	interface Props {
		wardId: string;
		/** Same extension point as WardCard's own displayName prop - shown
		 * in place of the raw wardId when set, falling back to wardId when
		 * absent. See WardCard's Props comment for the full reasoning. */
		displayName?: string;
	}

	const { wardId, displayName }: Props = $props();

	const ward = $derived(fleet.wards[wardId]);
	const wardState = $derived(ward?.state);

	const dotState = $derived(linkDotState(wardState));
	// karshipta#268: ticks independently of connected, same reasoning as
	// ward-card.svelte's own clock - see that file for the full comment.
	let now = $state(Date.now());
	$effect(() => {
		const interval = setInterval(() => (now = Date.now()), 10_000);
		return () => clearInterval(interval);
	});
	const updatedAgoLabel = $derived(
		wardState && wardState.ingestionMs > 0 ? formatTimeAgo(wardState.ingestionMs, now) : undefined
	);
	const staleSample = $derived(isStaleSample(wardState));
	const dotLabel = $derived(
		dotState === 'ok'
			? 'Connected'
			: dotState === 'fault'
				? 'Connected, health fault'
				: updatedAgoLabel
					? `Link lost - last update ${updatedAgoLabel}`
					: 'Link lost'
	);
	const dotClass = $derived(
		dotState === 'ok'
			? 'bg-accent animate-pulse'
			: dotState === 'fault'
				? 'bg-critical'
				: 'border border-fg-muted bg-transparent'
	);

	const modeLabel = $derived(formatModeLabel(wardState?.flight));
	const tier = $derived(batteryTier(wardState?.battery));
	const hintLabel = $derived(batteryHintLabel(tier));
	// See ward-card.svelte: no autopilot behind this ward at all, not SITL
	// (a real autopilot binary flying simulated physics).
	const synthetic = $derived(ward?.info?.origin === WardOrigin.WARD_ORIGIN_SYNTHETIC);

	// Each present tag carries its own reserved color; only the separators
	// between them stay plain text color, so color always marks meaning.
	const tags = $derived(
		(
			[
				modeLabel ? { text: modeLabel, class: 'text-fg' } : undefined,
				// See ward-card.svelte's own comment on this same distinction:
				// armed/inAir freezes the instant the link drops, so a bare
				// "ARMED" here would keep reading as a current fact for a ward
				// that's been offline for hours. dotState (already computed
				// above from the same connected flag) is reused rather than a
				// second ad hoc check, same as this file's own tags already
				// lean on it for the connectivity dot.
				wardState?.flight?.armed
					? dotState === 'lost'
						? {
								text: 'ARMED (STALE)',
								class: 'text-armed',
								title: 'Last known state before the link dropped - may be stale'
							}
						: { text: 'ARMED', class: 'text-armed' }
					: undefined,
				synthetic
					? {
							text: 'SIM',
							class: 'text-synthetic',
							title: 'No autopilot behind this ward; demo telemetry standing in for one'
						}
					: undefined,
				// karshipta#268: independent of dotState - a connection can be
				// fully live while still delivering a sample that was already
				// old on arrival (a Herald tracker buffering before it sends).
				staleSample
					? {
							text: 'STALE SAMPLE',
							class: 'text-accent',
							title:
								"This reading was already old when the gateway received it - the link itself is fine, the sample isn't fresh"
						}
					: undefined,
				hintLabel
					? { text: hintLabel, class: tier === 'critical' ? 'text-critical' : 'text-accent' }
					: undefined
			] as const
		).filter((tag): tag is { text: string; class: string; title?: string } => tag !== undefined)
	);

	const batteryPctLabel = $derived(formatBatteryPct(wardState?.battery));
	const batteryClass = $derived(
		tier === 'critical' ? 'text-critical' : tier === 'caution' ? 'text-accent' : 'text-fg'
	);
	const headingLabel = $derived(formatHeadingCompass(wardState?.headingDeg));
	const headingDegValue = $derived(wardState?.headingDeg);
	const altLabel = $derived(
		wardState?.position
			? formatAltitude(wardState.position.altitudeRelM, unitsStore.current)
			: unitsStore.current === 'metric'
				? '? m'
				: '? ft'
	);
	const groundSpeed = $derived(
		wardState?.velocity
			? Math.hypot(wardState.velocity.northMS, wardState.velocity.eastMS)
			: undefined
	);
	const speedLabel = $derived(
		groundSpeed === undefined
			? unitsStore.current === 'metric'
				? '? m/s'
				: '? mph'
			: formatSpeed(groundSpeed, unitsStore.current)
	);
</script>

<div
	class="flex flex-col gap-1 border-b border-edge px-3 py-2 text-xs"
	aria-label="Status for {displayName ?? wardId}"
>
	<div class="flex items-center justify-between gap-2">
		<div class="flex min-w-0 items-center gap-1.5">
			<span class="truncate font-mono text-sm font-semibold">{displayName ?? wardId}</span>
			<span
				class="h-2 w-2 shrink-0 rounded-full {dotClass}"
				role="status"
				aria-label={dotLabel}
				title={dotLabel}
			></span>
		</div>
		<span class="shrink-0 font-mono text-sm font-bold tabular-nums {batteryClass}"
			>{batteryPctLabel}</span
		>
	</div>
	{#if tags.length > 0}
		<div class="flex flex-wrap items-center gap-x-1.5 gap-y-0.5">
			{#each tags as tag, i (tag.text)}
				{#if i > 0}
					<span class="text-fg-muted">&middot;</span>
				{/if}
				<span class="text-[10px] font-semibold tracking-wide {tag.class}" title={tag.title}
					>{tag.text}</span
				>
			{/each}
		</div>
	{/if}
	<span class="flex items-center gap-1 font-mono text-[10px] text-fg-muted tabular-nums">
		{#if headingDegValue !== undefined}
			<svg
				width="9"
				height="9"
				viewBox="0 0 34 34"
				class="shrink-0"
				style="rotate: {headingDegValue}deg"
				aria-hidden="true"
			>
				<path
					d="M17 3 L27 29 L17 22 L7 29 Z"
					fill="currentColor"
					stroke="currentColor"
					stroke-width="1.5"
					stroke-linejoin="round"
				/>
			</svg>
		{/if}
		{headingLabel} &middot; {altLabel} &middot; {speedLabel}
		<!-- Same "while disconnected only" reasoning as ward-card.svelte: while
		     connected this would just repeat "just now" on every tick. -->
		{#if dotState === 'lost' && updatedAgoLabel}
			&middot; {updatedAgoLabel}
		{/if}
	</span>
</div>
