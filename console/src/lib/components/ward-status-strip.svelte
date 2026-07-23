<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { WardOrigin } from '$lib/gen/karshipta/v1/common';
	import {
		batteryHintLabel,
		batteryTier,
		formatBatteryPct,
		formatHeadingCompass,
		formatModeLabel,
		linkDotState
	} from '$lib/ward-format';

	interface Props {
		wardId: string;
	}

	const { wardId }: Props = $props();

	const ward = $derived(fleet.wards[wardId]);
	const state = $derived(ward?.state);

	const dotState = $derived(linkDotState(state));
	const dotLabel = $derived(
		dotState === 'ok' ? 'Connected' : dotState === 'fault' ? 'Connected, health fault' : 'Link lost'
	);
	const dotClass = $derived(
		dotState === 'ok'
			? 'bg-accent animate-pulse'
			: dotState === 'fault'
				? 'bg-critical'
				: 'border border-fg-muted bg-transparent'
	);

	const modeLabel = $derived(formatModeLabel(state?.flight));
	const tier = $derived(batteryTier(state?.battery));
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
				state?.flight?.armed ? { text: 'ARMED', class: 'text-armed' } : undefined,
				synthetic
					? {
							text: 'SIM',
							class: 'text-synthetic',
							title: 'No autopilot behind this ward; demo telemetry standing in for one'
						}
					: undefined,
				hintLabel
					? { text: hintLabel, class: tier === 'critical' ? 'text-critical' : 'text-accent' }
					: undefined
			] as const
		).filter((tag): tag is { text: string; class: string; title?: string } => tag !== undefined)
	);

	const batteryPctLabel = $derived(formatBatteryPct(state?.battery));
	const batteryClass = $derived(
		tier === 'critical' ? 'text-critical' : tier === 'caution' ? 'text-accent' : 'text-fg'
	);
	const headingLabel = $derived(formatHeadingCompass(state?.headingDeg));
	const headingDegValue = $derived(state?.headingDeg);
	const altLabel = $derived(
		state?.position ? `${state.position.altitudeRelM.toFixed(1)} m` : '? m'
	);
	const groundSpeed = $derived(
		state?.velocity ? Math.hypot(state.velocity.northMS, state.velocity.eastMS) : undefined
	);
	const speedLabel = $derived(
		groundSpeed === undefined ? '? m/s' : `${groundSpeed.toFixed(1)} m/s`
	);
</script>

<div
	class="flex flex-col gap-1 border-b border-edge px-3 py-2 text-xs"
	aria-label="Status for {wardId}"
>
	<div class="flex items-center justify-between gap-2">
		<div class="flex min-w-0 items-center gap-1.5">
			<span class="truncate font-mono text-sm font-semibold">{wardId}</span>
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
	</span>
</div>
