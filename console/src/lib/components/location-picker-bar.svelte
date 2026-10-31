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
		/** A second, shorter line under the status line explaining what
		 * happens next - requested directly after confirming "placed it,
		 * then didn't understand why it immediately started moving" as a
		 * real point of confusion (the same one auto-selecting the new
		 * ward already addresses once it's deployed; this addresses it
		 * before that, while the operator is still deciding where to put
		 * it). Overridable the same way label is, for a caller whose demo
		 * ward behaves differently. */
		description?: string;
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

	let {
		lat,
		lon,
		locating,
		label,
		description,
		oncancel,
		confirm,
		height = $bindable(0)
	}: Props = $props();
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
		<!-- flex-col, confirm on its own full-width row below the text, not
		     squeezed beside it - requested directly, a bigger, easier
		     target for a thumb that just finished panning the map with the
		     same hand. pb-5 beyond EdgeSheet's own safe-area padding: extra
		     breathing room above the home-indicator gesture area, so the
		     button isn't sitting right at the edge either. -->
		<div class="flex flex-col gap-3 p-4 pb-5">
			<div class="text-xs">
				<p class="font-medium text-fg">
					{locating
						? 'Finding your location...'
						: (label ?? 'Click the map to move the spawn point')}
				</p>
				<p class="mt-1 text-fg-muted">
					{description ?? 'A simulated ward will spawn here and start flying automatically.'}
				</p>
				<p class="mt-1.5 font-mono text-[10px] text-fg-muted tabular-nums">
					{lat.toFixed(5)}, {lon.toFixed(5)}
				</p>
			</div>
			<div class="w-full">
				{@render confirm()}
			</div>
		</div>
	{/snippet}
</EdgeSheet>
