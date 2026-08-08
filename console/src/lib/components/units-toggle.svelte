<script lang="ts">
	import { onMount } from 'svelte';
	import { unitsStore, type UnitSystem } from '../units/units-store.svelte';

	const OPTIONS: { id: UnitSystem; label: string; hint: string }[] = [
		{ id: 'metric', label: 'Metric', hint: 'm, km, m/s, °C, mm' },
		{ id: 'imperial', label: 'Imperial', hint: 'ft, mi, mph, °F, in' }
	];

	let open = $state(false);
	let menuEl: HTMLDivElement | undefined = $state();

	function choose(system: UnitSystem): void {
		unitsStore.set(system);
		open = false;
	}

	// close the menu on an outside click or Escape - same convention as
	// ThemeToggle's own menu, which this now matches exactly (click to
	// open, not hover) rather than each control in this rail having its own
	// interaction style.
	$effect(() => {
		if (!open) return;
		const handlePointerDown = (event: PointerEvent) => {
			if (menuEl && !menuEl.contains(event.target as Node)) open = false;
		};
		const handleKeydown = (event: KeyboardEvent) => {
			if (event.key === 'Escape') open = false;
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

<div class="relative" bind:this={menuEl}>
	<button
		type="button"
		class="flex h-7 w-7 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
		aria-label="Units: {unitsStore.current}"
		aria-expanded={open}
		aria-haspopup="true"
		title="Units: {unitsStore.current}"
		onclick={() => (open = !open)}
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
		<div
			class="absolute right-0 bottom-full z-30 mb-1 w-36 rounded border border-edge bg-panel p-1"
			aria-label="Units menu"
		>
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
		</div>
	{/if}
</div>
