<script lang="ts">
	import type { Snippet } from 'svelte';

	interface Props {
		lat: number;
		lon: number;
		/** true while locateOrFallback is still resolving */
		locating: boolean;
		/** Overrides the default "Click the map..." status line - a consumer
		 * using a different placement gesture (e.g. panning the map under a
		 * fixed on-screen crosshair, rather than clicking a point) needs its
		 * own instruction, which this component has no way to guess on its
		 * own. Ignored while locating (that status always wins). */
		label?: string;
		oncancel: () => void;
		/** the app-specific confirm control (a plain button for a client-only
		 * caller, a form submit for a caller with a server action) - this
		 * component owns only the generic status/readout/cancel chrome */
		confirm: Snippet;
		/** This bar's own real rendered height in px - bindable so a caller
		 * can keep an on-screen crosshair (or anything else reasoning about
		 * "the visible map area") centered above it rather than centered on
		 * the full viewport including the space this bar covers. Same
		 * bind:panelHeight shape EdgeSheet already exposes for the same
		 * reason. */
		height?: number;
	}

	let { lat, lon, locating, label, oncancel, confirm, height = $bindable(0) }: Props = $props();
</script>

<!-- bottom-[calc(1rem+env(...))]: fixed-to-bottom-4, so without this it
     sits inside the home-indicator curve's own territory on a phone -
     same class of bug as fleet-map.svelte's legend panel. -->
<div
	role="status"
	bind:clientHeight={height}
	class="fixed bottom-[calc(1rem+env(safe-area-inset-bottom,0px))] left-1/2 z-40 flex -translate-x-1/2 items-center gap-3 rounded border border-edge bg-panel px-4 py-2.5 shadow-lg"
>
	<div class="text-xs">
		<p class="font-medium text-fg">
			{locating ? 'Finding your location...' : (label ?? 'Click the map to move the spawn point')}
		</p>
		<p class="font-mono text-[10px] text-fg-muted tabular-nums">
			{lat.toFixed(5)}, {lon.toFixed(5)}
		</p>
	</div>
	{@render confirm()}
	<button
		type="button"
		onclick={oncancel}
		class="rounded border border-edge px-3 py-1.5 text-xs text-fg-muted hover:text-fg"
	>
		Cancel
	</button>
</div>
