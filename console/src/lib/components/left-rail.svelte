<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import AddWardMenu from '$lib/components/add-ward-menu.svelte';
	import WardCard from '$lib/components/ward-card.svelte';
	import Disclosure from '$lib/components/ui/disclosure.svelte';
	import FleetRow from '$lib/components/fleet-row.svelte';

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

	const UNASSIGNED_GROUP_ID = 'unassigned';

	// Per-group expanded state is local UI state, not persisted - default
	// expanded, matching every other Disclosure in this console. A Ward
	// belonging to more than one Fleet appears under each of them; a Ward
	// in none appears under the Unassigned group, which is hidden entirely
	// once nothing is left in it (rather than showing a permanent empty row).
	let groupExpanded = $state<Record<string, boolean>>({});
	function isGroupExpanded(id: string): boolean {
		return groupExpanded[id] ?? true;
	}

	const unassignedWardIds = $derived(fleetGroups.unassignedWardIds(fleet.wardIds));

	let createFleetOpen = $state(false);
	let newFleetName = $state('');
	let newFleetDescription = $state('');

	function submitCreateFleet(event: SubmitEvent) {
		event.preventDefault();
		const name = newFleetName.trim();
		if (!name) return;
		fleetGroups.requestCreateFleet(name, newFleetDescription.trim());
		newFleetName = '';
		newFleetDescription = '';
		createFleetOpen = false;
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
			class="flex w-80 flex-col gap-2 overflow-y-auto p-3"
			aria-label={fleetLabel}
		>
			<div class="flex gap-1.5">
				<AddWardMenu variant="compact" {onopenconnection} {onstartdemoplacement} />

				<div class="relative">
					<button
						type="button"
						class="rounded border border-edge px-2 py-1 font-mono text-xs text-fg-muted hover:border-accent hover:text-fg"
						aria-label="Create {fleetLabel}"
						aria-expanded={createFleetOpen}
						onclick={() => (createFleetOpen = !createFleetOpen)}
					>
						+ New {fleetLabel}
					</button>
					{#if createFleetOpen}
						<form
							class="absolute top-full left-0 z-30 mt-1 flex w-56 flex-col gap-1.5 rounded border border-edge bg-panel p-2"
							aria-label="New {fleetLabel}"
							onsubmit={submitCreateFleet}
						>
							<input
								type="text"
								bind:value={newFleetName}
								required
								placeholder="{fleetLabel} name"
								class="w-full rounded border border-edge bg-ink px-1.5 py-1 text-xs"
							/>
							<input
								type="text"
								bind:value={newFleetDescription}
								placeholder="Description (optional)"
								class="w-full rounded border border-edge bg-ink px-1.5 py-1 text-xs"
							/>
							<div class="flex gap-1.5">
								<button
									type="submit"
									class="rounded border border-accent/60 bg-accent/15 px-2 py-1 text-xs font-medium text-accent hover:bg-accent/25"
								>
									Create
								</button>
								<button
									type="button"
									onclick={() => (createFleetOpen = false)}
									class="rounded border border-edge px-2 py-1 text-xs text-fg-muted hover:text-fg"
								>
									Cancel
								</button>
							</div>
						</form>
					{/if}
				</div>
			</div>

			{#each fleetGroups.fleetIds as fleetId (fleetId)}
				<FleetRow {fleetId} {fleetLabel} />
			{/each}

			{#if unassignedWardIds.length > 0}
				<Disclosure
					id={UNASSIGNED_GROUP_ID}
					label="UNASSIGNED ({unassignedWardIds.length})"
					expanded={isGroupExpanded(UNASSIGNED_GROUP_ID)}
					onchange={(value) => (groupExpanded[UNASSIGNED_GROUP_ID] = value)}
				>
					{#each unassignedWardIds as wardId (wardId)}
						<WardCard {wardId} ward={fleet.wards[wardId]} />
					{/each}
				</Disclosure>
			{/if}
		</aside>
	{/if}
</div>
