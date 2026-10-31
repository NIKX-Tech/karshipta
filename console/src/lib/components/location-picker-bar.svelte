<script lang="ts">
	import type { Snippet } from 'svelte';
	import EdgeSheet from './ui/edge-sheet.svelte';
	import SheetHeader from './ui/sheet-header.svelte';

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
		 * the full viewport including the space this bar covers. */
		height?: number;
	}

	let { lat, lon, locating, label, oncancel, confirm, height = $bindable(0) }: Props = $props();
</script>

<!-- EdgeSheet, not a small floating centered card - confirmed live on a
     real device as a real bug, the old centered card sat half behind the
     map's own right-side zoom/measure controls and generally read as
     cramped. suppressOutsideDismiss: the whole point of this flow is
     panning the map underneath to choose a spot - EdgeSheet's own
     outside-tap catcher would otherwise sit on top of the map and both
     dismiss the sheet AND eat the drag gesture a pan needs, confirmed
     live as the real "map doesn't move any more" bug this fixes.
     expanded (always true, no peek): this has no lesser "glanceable
     summary" state the way a ward detail sheet's peek does - it's either
     up, doing its one job, or not shown at all. requireExplicitClose
     deliberately left off: unlike the other left-rail sheets (skimmed
     content, a stray dismiss would lose nothing), this is an active task
     - Escape canceling it (same as the header's own X) is expected, not
     a footgun. No separate Cancel button next to confirm any more
     (requested directly): the header X already is Cancel, a second one
     right below it was redundant. -->
<EdgeSheet
	edge="bottom"
	open={true}
	expanded
	suppressOutsideDismiss
	label="Place demo ward"
	onclose={oncancel}
	bind:panelHeight={height}
>
	{#snippet content()}
		<SheetHeader title="Place demo ward" onclose={oncancel} />
		<div class="flex items-center justify-between gap-3 p-3">
			<div class="min-w-0 text-xs">
				<p class="font-medium text-fg">
					{locating
						? 'Finding your location...'
						: (label ?? 'Click the map to move the spawn point')}
				</p>
				<p class="font-mono text-[10px] text-fg-muted tabular-nums">
					{lat.toFixed(5)}, {lon.toFixed(5)}
				</p>
			</div>
			<div class="shrink-0">
				{@render confirm()}
			</div>
		</div>
	{/snippet}
</EdgeSheet>
