<script lang="ts">
	import type { Snippet } from 'svelte';
	import { dialogOpenCount } from '$lib/dialog-open-count.svelte';

	// --mobile-left-rail-inset/--mobile-right-rail-inset (read directly in
	// the markup below, not props): how far this panel insets from the
	// left/right edge to clear whichever icon rail is actually visible
	// right now. Used to be two props (leftRailInset/rightRailInset) every
	// consumer computed and hardcoded its own copy of - confirmed live as
	// a repeated, real bug: none of those copies were reactive to the rail
	// actually toggling visible/hidden while a sheet stayed open, so a
	// sheet opened before the rail appeared got painted over once it did,
	// and a sheet left open after the rail was hidden again kept reserving
	// space for a rail no longer there. One consuming app's own page shell
	// (this console's +page.svelte) now sets both vars in exactly one
	// place, inherited by every EdgeSheet (and, via the same vars,
	// fleet-map.svelte's own hand-rolled sheets) without any per-consumer
	// wiring at all - the only way to guarantee they can't drift apart
	// again. Both default to 0px if a consuming app never sets them (no
	// rail concept at all).
	interface Props {
		edge: 'left' | 'bottom';
		open: boolean;
		/** 'bottom' only: true = full content view, false = peek view. Always
		 * treated as true for 'left' - a drawer has no peek concept. */
		expanded?: boolean;
		/** aria-label for the panel while it traps focus (drawer, or an
		 * expanded sheet); ignored for a peeking sheet, which isn't modal. */
		label: string;
		/** An unsuppressed Escape or outside tap calls this - for 'left' it
		 * means "close the drawer"; for 'bottom' it means "collapse to
		 * peek" (the caller decides what collapsing all the way to fully
		 * closed/deselected means, this only ever asks to step back one
		 * level). */
		onclose: () => void;
		/** 'bottom' only: true while the active tab needs every map tap to
		 * reach the map untouched (Control/Mission/Zones place a waypoint or
		 * zone vertex by tapping the map itself) - removes the outside-tap
		 * catcher ENTIRELY (not just makes it inert) so taps pass straight
		 * through to the map underneath instead of racing the tab's own map
		 * click handler. NOT what requireExplicitClose below is for - that
		 * one keeps the background blocked, this one needs it gone. */
		suppressOutsideDismiss?: boolean;
		/** True to make the explicit close/collapse button in the caller's
		 * own content the ONLY way to close this sheet: Escape is ignored,
		 * and the outside-tap catcher stays rendered (background taps still
		 * can't reach whatever's underneath, same as always) but no longer
		 * closes anything when tapped - distinct from suppressOutsideDismiss
		 * above, which removes that catcher so taps pass through. Requested
		 * directly for every left-rail sheet (the Fleets/Gateway/Events
		 * drawer, Theme, Units, About): skimming one of these with a stray
		 * tap or Escape shouldn't dismiss it, but the map underneath still
		 * shouldn't receive clicks meant for the sheet either. */
		requireExplicitClose?: boolean;
		/** 'bottom' only, rendered instead of content while collapsed. A
		 * 'bottom' sheet omitting this falls through to rendering full
		 * `content` even while `!expanded` (no peek-specific semantics
		 * implied by this primitive on its own) - always pass this for a
		 * 'bottom' edge unless that fallback is genuinely wanted. */
		peek?: Snippet;
		content: Snippet;
		/** The panel's own real rendered height in px (its actual visible
		 * size - capped by max-h-[80vh] and shrunk-to-fit below that, not
		 * its content's full/scrollable height, which can be taller than
		 * what's actually on screen). Bindable so a 'bottom' caller can
		 * shrink a consuming app's own map viewport by exactly this much
		 * while the sheet is open, pushing it up clear of the sheet
		 * instead of letting the sheet just cover the bottom of it -
		 * confirmed live as a real bug otherwise (measuring the content
		 * snippet's own height directly, from outside this component,
		 * doesn't account for the cap or the collapsed/peek state the way
		 * this binding does for free). 0 while closed, same as the panel
		 * itself not existing then. */
		panelHeight?: number;
	}

	let {
		edge,
		open,
		expanded = true,
		label,
		onclose,
		suppressOutsideDismiss = false,
		requireExplicitClose = false,
		peek,
		content,
		panelHeight = $bindable(0)
	}: Props = $props();

	// WAI-ARIA APG's own list for a "reasonably complete" focus trap -
	// same set Dialog uses, recomputed per Tab press rather than cached
	// since a caller's own content can change which controls are disabled.
	const FOCUSABLE_SELECTOR =
		'a[href], button:not([disabled]), textarea:not([disabled]), input:not([disabled]), select:not([disabled]), [tabindex]:not([tabindex="-1"])';

	let panelEl = $state<HTMLDivElement | undefined>(undefined);

	function focusableElements(): HTMLElement[] {
		if (!panelEl) return [];
		return Array.from(panelEl.querySelectorAll<HTMLElement>(FOCUSABLE_SELECTOR));
	}

	// A drawer is always modal; a sheet only earns a focus trap once
	// expanded - its peek state is a glanceable summary, not something a
	// keyboard user should get stuck inside.
	const trapsFocus = $derived(edge === 'left' || expanded);

	// bind:clientHeight below only fires while the panel is actually
	// mounted - closing it (open turns false) unmounts the bound element
	// without Svelte resetting the value, so without this, panelHeight
	// would keep reporting its last-open height forever after, which is
	// wrong for a caller using it to reserve space only while this is
	// genuinely open (see the prop's own comment).
	$effect(() => {
		if (!open) panelHeight = 0;
	});

	// panelEl becomes undefined again whenever the panel unmounts (open
	// turns false, or peek<->expanded swaps content away from a trapping
	// state), which reruns this effect's cleanup the same way Dialog's own
	// per-open-instance effect does - no separate close handling needed.
	$effect(() => {
		if (!trapsFocus || !panelEl) return;
		const previouslyFocused = document.activeElement as HTMLElement | null;
		const initial =
			panelEl.querySelector<HTMLElement>('[data-autofocus]') ?? focusableElements()[0];
		initial?.focus();
		return () => {
			previouslyFocused?.focus();
		};
	});

	function onkeydown(event: KeyboardEvent) {
		if (!open) return;
		if (event.key === 'Escape') {
			// A Dialog (e.g. a disarm/land/RTL ConfirmDialog opened from
			// inside this sheet's own content) owns Escape while it's up -
			// its own <svelte:window onkeydown> and this one are two
			// independent listeners on the same event, unlike a real click,
			// which z-index already resolves correctly (Dialog's backdrop
			// is z-50, above this component's z-[45], so the browser never
			// even delivers an outside click to EdgeSheet's own catcher
			// while a Dialog is up). Escape has no such protection, so it
			// needs this explicit check instead.
			if (dialogOpenCount.current > 0) return;
			// requireExplicitClose, not suppressOutsideDismiss: Control/
			// Mission/Zones (suppressOutsideDismiss) have no reason to
			// block Escape - it doesn't compete with a map tap the way an
			// outside click does - but requireExplicitClose's whole point
			// is "only the explicit button works", which has to mean
			// Escape too, not just a stray tap.
			if (requireExplicitClose) return;
			onclose();
			return;
		}
		if (!trapsFocus || event.key !== 'Tab') return;
		const focusable = focusableElements();
		if (focusable.length === 0) {
			event.preventDefault();
			return;
		}
		const first = focusable[0];
		const last = focusable[focusable.length - 1];
		if (event.shiftKey && document.activeElement === first) {
			event.preventDefault();
			last.focus();
		} else if (!event.shiftKey && document.activeElement === last) {
			event.preventDefault();
			first.focus();
		}
	}

	function onOutsideClick() {
		// suppressOutsideDismiss already means this catcher isn't rendered
		// at all for 'bottom' (see its own render condition below), so this
		// check only actually matters for 'left', whose backdrop always
		// renders. requireExplicitClose keeps the catcher/backdrop
		// rendered (still blocking the background) but makes it inert.
		if (suppressOutsideDismiss || requireExplicitClose) return;
		onclose();
	}
</script>

<svelte:window {onkeydown} />

{#if open}
	{#if edge === 'left'}
		<!-- Full modal, dims and blocks the map behind it - there's no
		     reason for the map to stay interactive while browsing the fleet
		     list/connection panel/events, unlike the bottom sheet below.
		     The panel's own pt-/pb-safe-area keep its header and bottom
		     controls clear of a notch/status bar and the home-indicator
		     gesture area; bg-panel still paints behind them since padding
		     is inside the border/background box. Backdrop and panel are
		     separate siblings, not nested (same shape as the bottom edge
		     below) - a click reaching the backdrop is then definitionally
		     outside the panel, no target-vs-currentTarget check needed, and
		     the panel needs no onclick handler of its own just to stop
		     propagation. requireExplicitClose applies here too, same as
		     the bottom edge's own catcher below - "only the explicit
		     close button works" should mean that regardless of which
		     edge (suppressOutsideDismiss has no meaning for 'left': there
		     is no map underneath this edge's own opaque backdrop for a
		     tap to need to reach). -->
		<div
			class="fixed inset-0 z-[45] bg-black/60"
			role="presentation"
			onclick={onOutsideClick}
		></div>
		<div
			bind:this={panelEl}
			bind:clientHeight={panelHeight}
			role="dialog"
			aria-modal="true"
			aria-label={label}
			class="fixed inset-y-0 z-[45] flex w-80 max-w-[85vw] flex-col border-r border-edge bg-panel pt-[env(safe-area-inset-top,0px)] pb-[env(safe-area-inset-bottom,0px)]"
			style="left: var(--mobile-left-rail-inset, 0px)"
		>
			{@render content()}
		</div>
	{:else}
		<!-- Bottom sheet: never dims the map, which must stay visible
		     underneath at all times. An invisible click-catcher behind the
		     panel (not a backdrop - the map keeps its own colors) lets a tap
		     on the still-visible map collapse the sheet back to peek, except
		     while suppressOutsideDismiss is set. Only while expanded: a
		     peeking sheet is non-modal glanceable chrome, not something that
		     should block clicks across the entire page the moment it exists -
		     confirmed live as a real bug, every click anywhere (including
		     RightPanel's own always-visible icon rail, nothing to do with
		     this sheet) was getting silently swallowed by a full-viewport
		     catcher that existed the instant a ward was merely selected. -->
		{#if expanded && !suppressOutsideDismiss}
			<div class="fixed inset-0 z-[45]" role="presentation" onclick={onOutsideClick}></div>
		{/if}
		<!-- max-height via --mobile-sheet-max-height (set by the consuming
		     app's own page shell, falls back to 80vh if unset): a
		     `position: fixed; bottom: 0` box with no top is shrink-to-fit-
		     on-content with no scroll fallback at all - confirmed live as a
		     real bug, a tall expanded tab (Configuration's param list, a
		     long mission/zone) pushed this panel's own header and collapse
		     button off the top of the screen with no way back, exactly the
		     "can't reach the controls" failure this whole fix exists to
		     prevent. The cap itself, not just a smaller map box, is what
		     keeps the map visible underneath - a fixed element paints over
		     the map regardless of the map's own computed height, so the map
		     staying "big enough" on paper doesn't help if the sheet still
		     grows tall enough to cover it; overflow-y-auto on this wrapper,
		     not just each tabpanel's own flex-1/overflow-y-auto
		     (fleet-map.svelte uses the same shape for its layers/
		     attribution sheets, driven by the same CSS var). -->
		<div
			bind:this={panelEl}
			bind:clientHeight={panelHeight}
			role={expanded ? 'dialog' : 'region'}
			aria-modal={expanded ? 'true' : undefined}
			aria-label={label}
			class="fixed inset-x-0 bottom-0 z-[45] flex flex-col overflow-y-auto rounded-t-lg border-t border-edge bg-panel pb-[env(safe-area-inset-bottom,0px)]"
			style="left: var(--mobile-left-rail-inset, 0px); right: var(--mobile-right-rail-inset, 0px); max-height: var(--mobile-sheet-max-height, 80vh)"
		>
			{#if !expanded && peek}
				{@render peek()}
			{:else}
				{@render content()}
			{/if}
		</div>
	{/if}
{/if}
