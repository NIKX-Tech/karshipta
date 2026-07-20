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
	class="border-edge bg-panel fixed bottom-4 left-1/2 z-40 flex -translate-x-1/2 items-center gap-3 rounded border px-4 py-2.5 shadow-lg"
>
	<div class="text-xs">
		<p class="text-fg font-medium">
			{locating ? 'Finding your location...' : 'Click the map to move the spawn point'}
		</p>
		<p class="text-fg-muted font-mono text-[10px] tabular-nums">
			{lat.toFixed(5)}, {lon.toFixed(5)}
		</p>
	</div>
	{@render confirm()}
	<button
		type="button"
		onclick={oncancel}
		class="border-edge text-fg-muted hover:text-fg rounded border px-3 py-1.5 text-xs"
	>
		Cancel
	</button>
</div>
