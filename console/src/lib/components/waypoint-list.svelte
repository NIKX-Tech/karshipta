<script lang="ts">
	import type { DraftWaypoint } from '$lib/fleet-store.svelte';

	interface Props {
		waypoints: DraftWaypoint[];
		onRemove: (index: number) => void;
	}

	const { waypoints, onRemove }: Props = $props();
</script>

{#if waypoints.length > 0}
	<ol class="mt-2 space-y-1" aria-label="Waypoints">
		{#each waypoints as waypoint, index (index)}
			<li class="flex items-center gap-2 text-[10px]">
				<span class="w-5 font-mono text-selected">{index + 1}</span>
				<span class="font-mono text-fg-muted tabular-nums">
					{waypoint.latitudeDeg.toFixed(4)}, {waypoint.longitudeDeg.toFixed(4)}
				</span>
				<input
					type="number"
					min="2"
					max="120"
					aria-label="Waypoint {index + 1} altitude"
					bind:value={waypoint.altitudeRelM}
					class="ml-auto w-12 rounded border border-edge bg-ink px-1 py-0.5 font-mono text-[10px] tabular-nums"
				/>
				<span class="text-fg-muted">m</span>
				<button
					class="px-0.5 text-fg-muted hover:text-critical"
					aria-label="Remove waypoint {index + 1}"
					onclick={() => onRemove(index)}
				>
					&#x2715;
				</button>
			</li>
		{/each}
	</ol>
{/if}
