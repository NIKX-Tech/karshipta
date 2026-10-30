<script lang="ts">
	import Dialog from '$lib/components/ui/dialog.svelte';
	import EdgeSheet from '$lib/components/ui/edge-sheet.svelte';
	import SheetHeader from '$lib/components/ui/sheet-header.svelte';
	import { isMobile } from '$lib/is-mobile.svelte';
	import { mobileSurfaceUi } from '$lib/mobile-surface-ui.svelte';
	import logoMark from '$lib/assets/logo-mark.svg';

	// Trigger button + content both live here now, self-contained the same
	// shape as ThemeToggle/UnitsToggle (own open state, own mobileSurfaceUi
	// wiring) rather than left-rail.svelte owning an aboutOpen boolean and
	// mounting this purely as dialog content - confirmed live as a real
	// bug otherwise: with that split, this sheet never routed through
	// mobileSurfaceUi at all, so it opened behind/alongside whichever
	// other sheet (Theme, Units, the drawer) was already up instead of
	// replacing it, and never pushed the map out of its own way the way
	// every other sheet already did. Three components that are meant to
	// behave identically needed to actually share the same internal shape,
	// not just look similar on the surface.
	let open = $state(false);

	function setOpen(value: boolean) {
		open = value;
		if (!isMobile.current) return;
		if (value) mobileSurfaceUi.openAbout();
		else if (mobileSurfaceUi.open === 'about') mobileSurfaceUi.close();
	}

	// Mobile: something else became the open surface (Theme, Units, the
	// drawer) - close this one too, mirroring fleet-map.svelte's own
	// externalSheetOpen/onLocalSheetOpen coordination for the same reason.
	$effect(() => {
		if (isMobile.current && open && mobileSurfaceUi.open !== 'about') {
			open = false;
		}
	});
</script>

{#snippet aboutContent()}
	<div class="flex items-center gap-2">
		<img src={logoMark} alt="" class="h-5 w-auto" aria-hidden="true" />
		<h3 class="font-display text-sm font-semibold">Karshipta</h3>
	</div>
	<p class="mt-1 font-mono text-[11px] text-fg-muted tabular-nums">
		v{__PRODUCT_VERSION__} &middot; console v{__APP_VERSION__}
	</p>

	<p class="mt-3 text-xs text-fg-muted">
		Open-source, self-hosted command and control for unmanned vehicle and tracker fleets - from
		flight vehicles over MAVLink to any other tracked entity, GPS tags and generic trackers
		included.
	</p>
	<p class="mt-2 text-xs text-accent">
		Simulation-first. Not certified for real flight operations yet.
	</p>

	<dl class="mt-3 space-y-1 text-[11px] text-fg-muted">
		<div class="flex justify-between gap-2">
			<dt>License</dt>
			<dd>AGPL-3.0</dd>
		</div>
		<div class="flex justify-between gap-2">
			<dt>Copyright</dt>
			<dd>&copy; NIKX Technologies B.V.</dd>
		</div>
	</dl>

	<div class="mt-4">
		<!-- No Close button here - redundant with SheetHeader's own X (mobile)
		     and Dialog's backdrop/Escape (desktop); it also used to carry
		     data-autofocus, which is what caused the real bug this removal
		     fixes: EdgeSheet/Dialog both auto-focus either [data-autofocus]
		     or the first focusable element on open, and a browser always
		     scrolls a newly-focused element into view - a button way down
		     here, below content taller than the sheet's own capped height,
		     silently opened the sheet already scrolled to the bottom
		     instead of the top. Removing it (rather than keeping it but
		     dropping data-autofocus) means focus now lands on the first
		     real focusable element instead - SheetHeader's own close
		     button on mobile (already at the very top, so no scroll
		     happens at all) or this GitHub link on desktop (harmless
		     there, nothing to scroll in a dialog this short). -->
		<a
			href="https://github.com/NIKX-Tech/karshipta"
			target="_blank"
			rel="noreferrer"
			class="rounded border border-edge px-2 py-1 text-[11px] text-fg-muted hover:text-fg"
		>
			GitHub
		</a>
	</div>
{/snippet}

<!-- max-md:z-[46]: same reasoning as ThemeToggle's/UnitsToggle's own
     trigger - this component's own mobile EdgeSheet below renders its
     z-[45] outside-tap catcher as a DOM sibling of this trigger, inside
     the same left-rail icon column, so the column's own z-[46] does
     nothing to protect a sibling from it. -->
<button
	type="button"
	class="flex h-7 w-7 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg max-md:relative max-md:z-[46]"
	aria-label="About Karshipta"
	title="About Karshipta"
	onclick={() => setOpen(true)}
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
		<circle cx="12" cy="12" r="9" />
		<line x1="12" y1="11" x2="12" y2="16.5" />
		<circle cx="12" cy="7.5" r="1" fill="currentColor" stroke="none" />
	</svg>
</button>

{#if open && !isMobile.current}
	<Dialog label="About Karshipta" onclose={() => setOpen(false)}>
		{@render aboutContent()}
	</Dialog>
{/if}

<!-- requireExplicitClose: requested directly - every left-rail sheet
     (this, Theme, Units, the drawer) should only ever close via its own
     explicit close button, not an accidental outside tap or Escape. -->
<EdgeSheet
	edge="bottom"
	open={open && isMobile.current}
	label="About"
	onclose={() => setOpen(false)}
	requireExplicitClose
	bind:panelHeight={mobileSurfaceUi.aboutHeight}
>
	{#snippet content()}
		<div class="flex h-full flex-col">
			<SheetHeader title="About" onclose={() => setOpen(false)} />
			<div class="p-3">
				{@render aboutContent()}
			</div>
		</div>
	{/snippet}
</EdgeSheet>
