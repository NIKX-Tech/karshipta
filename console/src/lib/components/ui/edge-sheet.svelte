<script lang="ts">
	import type { Snippet } from 'svelte';
	import { dialogOpenCount } from '$lib/dialog-open-count.svelte';

	interface Props {
		edge: 'left' | 'bottom';
		open: boolean;
		/** 'bottom' only: true = full content view, false = peek view. Always
		 * treated as true for 'left' - a drawer has no peek concept. */
		expanded?: boolean;
		/** aria-label for the panel while it traps focus (drawer, or an
		 * expanded sheet); ignored for a peeking sheet, which isn't modal. */
		label: string;
		/** Escape, or an unsuppressed outside tap, calls this - for 'left' it
		 * means "close the drawer"; for 'bottom' it means "collapse to
		 * peek" (the caller decides what collapsing all the way to fully
		 * closed/deselected means, this only ever asks to step back one
		 * level). */
		onclose: () => void;
		/** 'bottom' only: true while the active tab needs every map tap to
		 * reach the map untouched (Control/Mission/Zones place a waypoint or
		 * zone vertex by tapping the map itself) - suppresses the
		 * outside-tap-to-collapse catcher entirely rather than racing it
		 * against the tab's own map click handler. */
		suppressOutsideDismiss?: boolean;
		/** 'bottom' only, rendered instead of content while collapsed. A
		 * 'bottom' sheet omitting this falls through to rendering full
		 * `content` even while `!expanded` (no peek-specific semantics
		 * implied by this primitive on its own) - always pass this for a
		 * 'bottom' edge unless that fallback is genuinely wanted. */
		peek?: Snippet;
		content: Snippet;
	}

	const {
		edge,
		open,
		expanded = true,
		label,
		onclose,
		suppressOutsideDismiss = false,
		peek,
		content
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

	// Only fires for a click that lands on the backdrop itself, not one
	// that started inside the panel and bubbled up - same check Dialog
	// uses. For the bottom edge this guard is moot (its click-catcher and
	// panel are siblings, not nested, so a panel click never reaches the
	// catcher's own handler at all), but applying it uniformly means
	// neither edge's panel needs its own onclick handler just to stop
	// propagation - keeping the panel div free of a click handler is also
	// what avoids tripping the "non-interactive role needs a keyboard
	// handler" a11y lint rule on it.
	function onBackdropClick(event: MouseEvent) {
		if (event.target === event.currentTarget) onclose();
	}

	function onOutsideClick() {
		if (!suppressOutsideDismiss) onclose();
	}
</script>

<svelte:window {onkeydown} />

{#if open}
	{#if edge === 'left'}
		<!-- Full modal, dims and blocks the map behind it - there's no
		     reason for the map to stay interactive while browsing the fleet
		     list/connection panel/events, unlike the bottom sheet below. -->
		<div
			class="fixed inset-0 z-[45] flex bg-black/60"
			role="presentation"
			onclick={onBackdropClick}
		>
			<div
				bind:this={panelEl}
				role="dialog"
				aria-modal="true"
				aria-label={label}
				class="flex h-full w-80 max-w-[85vw] flex-col border-r border-edge bg-panel"
			>
				{@render content()}
			</div>
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
		<!-- max-h-[80vh]: a `position: fixed; bottom: 0` box with no top is
		     shrink-to-fit-on-content with no scroll fallback at all -
		     confirmed live as a real bug, a tall expanded tab (Configuration's
		     param list, a long mission/zone) pushed this panel's own header
		     and collapse button off the top of the screen with no way back,
		     exactly the "can't reach the controls" failure this whole fix
		     exists to prevent. Capped below full height (not 100vh) so the
		     map stays visible above it even while expanded, matching the
		     peek state's own intent; overflow-y-auto on this wrapper, not
		     just each tabpanel's own flex-1/overflow-y-auto (fleet-map.svelte
		     uses the same max-h/overflow-y-auto shape for its layers popup). -->
		<div
			bind:this={panelEl}
			role={expanded ? 'dialog' : 'region'}
			aria-modal={expanded ? 'true' : undefined}
			aria-label={label}
			class="fixed inset-x-0 bottom-0 z-[45] flex max-h-[80vh] flex-col overflow-y-auto rounded-t-lg border-t border-edge bg-panel"
		>
			{#if !expanded && peek}
				{@render peek()}
			{:else}
				{@render content()}
			{/if}
		</div>
	{/if}
{/if}
