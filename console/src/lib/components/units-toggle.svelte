<script lang="ts">
	import { onMount } from 'svelte';
	import { unitsStore, type UnitSystem } from '../units/units-store.svelte';
	import { isMobile } from '../is-mobile.svelte';
	import { mobileSurfaceUi } from '../mobile-surface-ui.svelte';
	import EdgeSheet from './ui/edge-sheet.svelte';
	import SheetHeader from './ui/sheet-header.svelte';

	const OPTIONS: { id: UnitSystem; label: string; hint: string }[] = [
		{ id: 'metric', label: 'Metric', hint: 'm, km, m/s, °C, mm' },
		{ id: 'imperial', label: 'Imperial', hint: 'ft, mi, mph, °F, in' }
	];

	let open = $state(false);
	let menuEl: HTMLDivElement | undefined = $state();

	// Routes mobile open/close through mobileSurfaceUi too, so this sheet
	// and Theme's own close each other (and the Fleets/Gateway/Events
	// drawer, which already reacts to mobileSurfaceUi.open changing out
	// from under it) instead of stacking - both live in the same left-rail
	// icon column, directly adjacent triggers, a real collision otherwise.
	// Desktop's own popup stays untouched by any of this (no mobile-surface
	// concept applies there).
	function setOpen(value: boolean) {
		open = value;
		if (!isMobile.current) return;
		if (value) mobileSurfaceUi.openUnits();
		else if (mobileSurfaceUi.open === 'units') mobileSurfaceUi.close();
	}

	function choose(system: UnitSystem): void {
		unitsStore.set(system);
		setOpen(false);
	}

	// Mobile: something else became the open surface (Theme, the drawer) -
	// close this one too, mirroring fleet-map.svelte's own
	// externalSheetOpen/onLocalSheetOpen coordination for the same reason.
	$effect(() => {
		if (isMobile.current && open && mobileSurfaceUi.open !== 'units') {
			open = false;
		}
	});

	// close the menu on an outside click or Escape - desktop popup only;
	// the mobile EdgeSheet below owns its own outside-tap/Escape handling.
	$effect(() => {
		if (!open || isMobile.current) return;
		const handlePointerDown = (event: PointerEvent) => {
			if (menuEl && !menuEl.contains(event.target as Node)) setOpen(false);
		};
		const handleKeydown = (event: KeyboardEvent) => {
			if (event.key === 'Escape') setOpen(false);
		};
		window.addEventListener('pointerdown', handlePointerDown);
		window.addEventListener('keydown', handleKeydown);
		return () => {
			window.removeEventListener('pointerdown', handlePointerDown);
			window.removeEventListener('keydown', handleKeydown);
		};
	});

	onMount(() => {
		unitsStore.init();
	});
</script>

{#snippet optionsList()}
	{#each OPTIONS as option (option.id)}
		<button
			type="button"
			class="flex w-full items-center justify-between rounded px-2 py-1.5 text-left hover:bg-white/5 {unitsStore.current ===
			option.id
				? 'text-accent'
				: 'text-fg'}"
			onclick={() => choose(option.id)}
		>
			<span class="flex flex-col items-start">
				<span class="text-xs">{option.label}</span>
				<span class="text-[10px] text-fg-muted">{option.hint}</span>
			</span>
			{#if unitsStore.current === option.id}
				<svg
					width="12"
					height="12"
					viewBox="0 0 24 24"
					fill="none"
					stroke="currentColor"
					stroke-width="2"
					stroke-linecap="round"
					stroke-linejoin="round"
					aria-hidden="true"
				>
					<path d="M20 6 9 17l-5-5" />
				</svg>
			{/if}
		</button>
	{/each}
{/snippet}

<!-- max-md:z-[46]: this component's own mobile EdgeSheet below renders
     its z-[45] outside-tap catcher as a DOM sibling of this trigger,
     inside the SAME left-rail icon column that wraps both - unlike the
     Fleets/Gateway/Events drawer's own EdgeSheet (left-rail.svelte,
     rendered at that file's top level, a true sibling of the icon column
     rather than nested inside it), so the icon column's own z-[46] does
     nothing to protect this trigger from a catcher that's its own
     sibling, not an outside element. Confirmed live as a real bug: with
     Units' sheet open, the adjacent Theme button became unclickable -
     same underlying cause the button column in fleet-map.svelte already
     needed this exact fix for. -->
<div class="relative max-md:z-[46]" bind:this={menuEl}>
	<button
		type="button"
		class="flex h-7 w-7 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
		aria-label="Units: {unitsStore.current}"
		aria-expanded={open}
		aria-haspopup="true"
		title="Units: {unitsStore.current}"
		onclick={() => setOpen(!open)}
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
			<rect x="2.5" y="7" width="19" height="10" rx="1" />
			<path d="M6 7v3M10 7v3M14 7v3M18 7v3" />
		</svg>
	</button>
	{#if open}
		<!-- left-0, not right-0: this toggle lives in left-rail.svelte's
		     narrow w-10 icon column at the far left edge of the screen -
		     right-0 (grow leftward from the trigger, the right-rail era's
		     own positioning) pushed the menu off the left edge of the
		     viewport entirely, confirmed live as a real bug (it opened,
		     aria-expanded flipped, but nothing was ever visible on screen).
		     max-md:hidden: below that, the EdgeSheet below replaces it -
		     same reasoning fleet-map.svelte's own layers/attribution popups
		     give for their own bottom-sheet alternative. -->
		<div
			role="menu"
			class="absolute bottom-full left-0 z-30 mb-1 w-36 rounded border border-edge bg-panel p-1 max-md:hidden"
			aria-label="Units menu"
		>
			{@render optionsList()}
		</div>
	{/if}
</div>

<!-- requireExplicitClose: requested directly - every left-rail sheet
     (this, Theme, About, the drawer) should only ever close via its own
     explicit close button, not an accidental outside tap or Escape. -->
<EdgeSheet
	edge="bottom"
	open={open && isMobile.current}
	label="Units"
	onclose={() => setOpen(false)}
	requireExplicitClose
	bind:panelHeight={mobileSurfaceUi.unitsHeight}
>
	{#snippet content()}
		<div class="flex h-full flex-col">
			<SheetHeader title="Units" onclose={() => setOpen(false)} />
			<div class="flex flex-col gap-0.5 p-3">
				{@render optionsList()}
			</div>
		</div>
	{/snippet}
</EdgeSheet>
