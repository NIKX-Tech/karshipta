<script lang="ts">
	import type { Snippet } from 'svelte';

	interface Props {
		lat: number;
		lon: number;
		/** true while locateOrFallback is still resolving */
		locating: boolean;
		oncancel: () => void;
		/** the app-specific confirm control (a plain button for a client-only
		 * caller, a form submit for a caller with a server action) - this
		 * component owns only the generic status/readout/cancel chrome */
		confirm: Snippet;
	}

	const { lat, lon, locating, oncancel, confirm }: Props = $props();
</script>

<div
	role="status"
	class="fixed bottom-4 left-1/2 z-40 flex -translate-x-1/2 items-center gap-3 rounded border border-edge bg-panel px-4 py-2.5 shadow-lg"
>
	<div class="text-xs">
		<p class="font-medium text-fg">
			{locating ? 'Finding your location...' : 'Click the map to move the spawn point'}
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
