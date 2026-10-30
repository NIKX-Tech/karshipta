<script lang="ts">
	import type { Snippet } from 'svelte';

	/**
	 * The one header every mobile bottom sheet in this app uses - EdgeSheet
	 * itself doesn't bake this in (not every EdgeSheet consumer wants a
	 * title/close row there, e.g. a sheet whose own peek snippet already is
	 * the header), but every sheet that does want one should look pixel-
	 * identical. Existed as N hand-rolled copies before this (left-rail.svelte's
	 * drawer, right-panel.svelte's ward sheet, fleet-map.svelte's layers/
	 * attribution sheets, ThemeToggle's/UnitsToggle's own sheets), each
	 * drifting slightly - different padding, different close-button size,
	 * one even built on a flat Dialog instead of a sheet at all (About) -
	 * confirmed live as a real, user-visible inconsistency, not just a code
	 * smell. px-3 matches every sheet body's own existing p-3 content
	 * padding (so the title and the content below it line up on the same
	 * left edge). py-1.5: requested directly after seeing it live - the
	 * row only needs enough vertical room for the h-8 w-8 close button
	 * itself (that height is what actually matters for a comfortable tap
	 * target, not extra padding around it), and a tighter row leaves more
	 * of a short sheet's own vertical space for real content/buttons
	 * instead of empty title-bar air. h-8 w-8 matches the close button to
	 * every other mobile icon button in this app (the map's own locate/
	 * measure/layers/weather buttons), not the old h-5 w-5 a mouse-driven
	 * desktop close button could get away with but a touch target
	 * genuinely couldn't. sticky top-0, not a plain in-flow row: requested
	 * directly - every sheet's own scrollable area is the whole panel
	 * (EdgeSheet's overflow-y-auto wraps header+content together), so
	 * without this, a tall sheet's content scrolling also dragged the
	 * title/close row out of view instead of leaving them reachable the
	 * whole time. shrink-0 keeps it from being squeezed by its flex
	 * column's own layout; bg-panel (explicit, not inherited) is what
	 * actually keeps scrolled content from showing through underneath it
	 * once it's pinned; z-10 keeps it above that scrolling content, same
	 * reasoning as every other overlap this app's own z-index ladder
	 * already resolves (well under EdgeSheet's own z-[45] catcher and
	 * fleet-map.svelte's z-[46] buttons, so neither is at risk here).
	 */
	interface Props {
		title: string;
		onclose: () => void;
		/** Extra content next to the title (e.g. left-rail.svelte's own
		 * connectivity dot + link label on the Gateway tab, or a ward
		 * count on Fleets) - optional, most sheets don't need this. */
		extra?: Snippet;
	}

	const { title, onclose, extra }: Props = $props();
</script>

<div
	class="sticky top-0 z-10 flex shrink-0 items-center justify-between gap-2 border-b border-edge bg-panel px-3 py-1"
>
	<div class="flex items-center gap-2">
		<p class="font-mono text-[10px] font-medium tracking-widest text-fg-muted uppercase">
			{title}
		</p>
		{#if extra}
			{@render extra()}
		{/if}
	</div>
	<button
		type="button"
		onclick={onclose}
		aria-label="Close"
		class="flex h-8 w-8 shrink-0 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
	>
		<svg
			width="16"
			height="16"
			viewBox="0 0 24 24"
			fill="none"
			stroke="currentColor"
			stroke-width="1.75"
			stroke-linecap="round"
			stroke-linejoin="round"
			aria-hidden="true"
		>
			<path d="M6 6l12 12M18 6 6 18" />
		</svg>
	</button>
</div>
