<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { formatBatteryLabel, formatModeLabel } from '$lib/ward-format';

	interface Props {
		wardId: string;
	}

	const { wardId }: Props = $props();

	const state = $derived(fleet.wards[wardId]?.state);
	const modeLabel = $derived(formatModeLabel(state?.flight));
	const batteryLabel = $derived(formatBatteryLabel(state?.battery));
	const connected = $derived(state?.connected ?? false);
</script>

<div
	class="flex items-center gap-2 border-b border-edge px-3 py-2 text-xs"
	aria-label="Status for {wardId}"
>
	<span class="truncate font-mono font-semibold">{wardId}</span>
	{#if modeLabel}
		<span class="text-fg-muted">{modeLabel}</span>
	{/if}
	{#if state?.flight?.armed}
		<span class="text-[10px] font-medium tracking-widest text-armed">ARMED</span>
	{/if}
	<span
		class="h-2 w-2 shrink-0 rounded-full {connected ? 'animate-pulse bg-accent' : 'bg-critical'}"
		role="status"
		aria-label={connected ? 'Link live' : 'Link lost'}
		title={connected ? 'Link live' : 'Link lost'}
	></span>
	<span class="ml-auto font-mono text-[10px] text-fg-muted tabular-nums">{batteryLabel}</span>
</div>
