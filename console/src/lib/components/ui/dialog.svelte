<script lang="ts">
	import type { Snippet } from 'svelte';

	interface Props {
		/** aria-label for the panel; also what a screen reader announces as the dialog's name */
		label: string;
		/** 'alertdialog' for a confirmation/warning that demands a response; 'dialog' otherwise */
		variant?: 'dialog' | 'alertdialog';
		/** false blocks Escape and backdrop-click while a request is pending, matching the
		 * disabled Cancel button callers already show in that state */
		dismissible?: boolean;
		onclose: () => void;
		children: Snippet;
	}

	const { label, variant = 'dialog', dismissible = true, onclose, children }: Props = $props();

	// WAI-ARIA APG's own list for a "reasonably complete" focus trap: link,
	// button, textarea/input/select (not disabled), and anything explicitly
	// tabbable. Recomputed on every Tab/mount rather than cached, since a
	// caller's own content can change which of its controls are disabled
	// (e.g. add-ward-dialog's fields while a request is pending).
	const FOCUSABLE_SELECTOR =
		'a[href], button:not([disabled]), textarea:not([disabled]), input:not([disabled]), select:not([disabled]), [tabindex]:not([tabindex="-1"])';

	let panelEl = $state<HTMLDivElement | undefined>(undefined);

	function focusableElements(): HTMLElement[] {
		if (!panelEl) return [];
		return Array.from(panelEl.querySelectorAll<HTMLElement>(FOCUSABLE_SELECTOR));
	}

	// Runs once the panel exists: moves focus onto [data-autofocus] (or the
	// first focusable element, if a caller didn't mark one) and remembers
	// what to give focus back to on close. Every existing dialog in this
	// codebase is mounted fresh per open ({#if ...}), so this effect's
	// single run per component instance is exactly "on open"/"on close".
	$effect(() => {
		if (!panelEl) return;
		const previouslyFocused = document.activeElement as HTMLElement | null;
		const initial =
			panelEl.querySelector<HTMLElement>('[data-autofocus]') ?? focusableElements()[0];
		initial?.focus();
		return () => {
			previouslyFocused?.focus();
		};
	});

	function onkeydown(event: KeyboardEvent) {
		if (event.key === 'Escape') {
			if (dismissible) onclose();
			return;
		}
		if (event.key !== 'Tab') return;
		// Boundary-only trap (APG's documented approach): let the browser's
		// normal Tab order run inside the panel, and only intervene at the
		// two edges, wrapping around instead of letting focus escape to the
		// page behind the dialog.
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

	// Only close on a click that lands on the backdrop itself, not one that
	// started inside the panel and bubbled up (e.g. a drag-select ending
	// outside the panel would otherwise misfire this).
	function onBackdropClick(event: MouseEvent) {
		if (dismissible && event.target === event.currentTarget) onclose();
	}
</script>

<svelte:window {onkeydown} />

<div
	class="fixed inset-0 z-50 flex items-center justify-center bg-black/60"
	role="presentation"
	onclick={onBackdropClick}
>
	<div
		bind:this={panelEl}
		role={variant}
		aria-modal="true"
		aria-label={label}
		class="w-80 rounded border border-edge bg-panel p-4"
	>
		{@render children()}
	</div>
</div>
