<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import AddWardMenu from '$lib/components/add-ward-menu.svelte';
	import WardCard from '$lib/components/ward-card.svelte';

	interface Props {
		fleetLabel: string;
		onopenconnection: () => void;
		onstartdemoplacement: () => void;
	}

	const { fleetLabel, onopenconnection, onstartdemoplacement }: Props = $props();

	const COLLAPSED_STORAGE_KEY = 'karshipta.leftRailCollapsed';

	function readStoredCollapsed(): boolean {
		if (typeof localStorage === 'undefined') return false;
		return localStorage.getItem(COLLAPSED_STORAGE_KEY) === 'true';
	}

	// Starts expanded, unlike the right panel: the fleet list is core,
	// always-relevant content (it was a fixed-width, always-visible aside
	// before this restructure), not something that only becomes relevant
	// after a later action the way ward detail does.
	let collapsed = $state(readStoredCollapsed());

	function setCollapsed(value: boolean) {
		collapsed = value;
		if (typeof localStorage === 'undefined') return;
		localStorage.setItem(COLLAPSED_STORAGE_KEY, String(value));
	}
</script>

<div class="flex h-full border-r border-edge">
	<div class="flex w-10 shrink-0 flex-col items-center gap-1 border-r border-edge py-2">
		<button
			type="button"
			class="flex h-8 w-8 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
			aria-expanded={!collapsed}
			aria-controls="left-rail-content"
			aria-label={collapsed ? `Expand ${fleetLabel} list` : `Collapse ${fleetLabel} list`}
			title={collapsed ? `Expand ${fleetLabel} list` : `Collapse ${fleetLabel} list`}
			onclick={() => setCollapsed(!collapsed)}
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
				<path d="M4 6h16M4 12h16M4 18h10" />
			</svg>
		</button>
	</div>

	{#if !collapsed}
		<aside
			id="left-rail-content"
			class="flex w-56 flex-col gap-2 overflow-y-auto p-3"
			aria-label={fleetLabel}
		>
			<AddWardMenu variant="compact" {onopenconnection} {onstartdemoplacement} />
			{#each fleet.wardIds as wardId (wardId)}
				<WardCard {wardId} ward={fleet.wards[wardId]} />
			{/each}
		</aside>
	{/if}
</div>
