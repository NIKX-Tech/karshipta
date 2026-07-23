<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import AddWardMenu from '$lib/components/add-ward-menu.svelte';
	import WardCard from '$lib/components/ward-card.svelte';
	import FleetRow from '$lib/components/fleet-row.svelte';
	import EventsFeed from '$lib/components/events-feed.svelte';
	import Tabs, { type TabItem } from '$lib/components/ui/tabs.svelte';

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

	const TABS: TabItem[] = [
		{ id: 'fleets', label: 'Fleets' },
		{ id: 'events', label: 'Events' }
	];
	let activeTab = $state<'fleets' | 'events'>('fleets');
	const activeLabel = $derived(TABS.find((tab) => tab.id === activeTab)?.label ?? '');

	// Same "click the active tab to close, click a different one to switch
	// and open" toggle the right panel's tabs use.
	function onTabChange(id: string) {
		if (id === activeTab && !collapsed) {
			setCollapsed(true);
			return;
		}
		activeTab = id as typeof activeTab;
		setCollapsed(false);
	}

	// A small badge on the rail's Events icon for events that arrived while
	// that tab wasn't the visible one - cleared the moment it actually is.
	let lastSeenEventCount = $state(0);
	const hasUnseenEvents = $derived(fleet.events.length > lastSeenEventCount);
	$effect(() => {
		if (activeTab === 'events' && !collapsed) {
			lastSeenEventCount = fleet.events.length;
		}
	});

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

<div class="flex h-full border-r border-edge bg-panel">
	<div class="flex w-10 shrink-0 flex-col items-center gap-1 border-r border-edge py-2">
		<Tabs
			tabs={TABS}
			activeId={activeTab}
			showSelection={!collapsed}
			onchange={onTabChange}
			orientation="vertical"
		>
			{#snippet children(tab)}
				{#if tab.id === 'fleets'}
					<svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
						<path d="M12 2 15 9 12 7.3 9 9Z" />
						<path d="M5 12 8 19 5 17.3 2 19Z" />
						<path d="M19 12 22 19 19 17.3 16 19Z" />
					</svg>
				{:else if tab.id === 'events'}
					<span class="relative flex items-center justify-center">
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
							<circle cx="12" cy="12" r="8" />
							<path d="M12 7v5l3.5 2" />
						</svg>
						{#if hasUnseenEvents}
							<span
								class="absolute top-0.5 right-0.5 h-1.5 w-1.5 rounded-full bg-accent"
								aria-hidden="true"
							></span>
						{/if}
					</span>
				{/if}
			{/snippet}
		</Tabs>
	</div>

	{#if !collapsed}
		<aside id="left-rail-content" class="flex w-80 flex-col" aria-label={activeLabel}>
			<div class="flex items-center justify-between gap-2 border-b border-edge px-3 py-2">
				<p class="font-mono text-[10px] font-medium tracking-widest text-fg-muted">
					{activeLabel.toUpperCase()}
				</p>
				<button
					type="button"
					class="flex h-5 w-5 shrink-0 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
					aria-controls="left-rail-content"
					aria-label="Collapse panel"
					title="Collapse panel"
					onclick={() => setCollapsed(true)}
				>
					<svg
						width="14"
						height="14"
						viewBox="0 0 24 24"
						fill="none"
						stroke="currentColor"
						stroke-width="1.75"
						stroke-linecap="round"
						stroke-linejoin="round"
						aria-hidden="true"
					>
						<path d="m15 6-6 6 6 6" />
					</svg>
				</button>
			</div>
			<div
				role="tabpanel"
				id="tabpanel-fleets"
				aria-labelledby="tab-fleets"
				hidden={activeTab !== 'fleets'}
				class="flex min-h-0 flex-1 flex-col gap-2 overflow-y-auto p-3"
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
					<div class="rounded border border-edge">
						<div class="flex items-center gap-1 px-1.5 py-1">
							<button
								type="button"
								class="flex h-4 w-4 shrink-0 items-center justify-center text-fg-muted hover:text-fg"
								aria-expanded={isGroupExpanded(UNASSIGNED_GROUP_ID)}
								aria-controls="unassigned-members"
								aria-label={isGroupExpanded(UNASSIGNED_GROUP_ID) ? 'Collapse' : 'Expand'}
								onclick={() =>
									(groupExpanded[UNASSIGNED_GROUP_ID] = !isGroupExpanded(UNASSIGNED_GROUP_ID))}
							>
								<svg
									class="transition-transform {isGroupExpanded(UNASSIGNED_GROUP_ID)
										? 'rotate-90'
										: ''}"
									width="10"
									height="10"
									viewBox="0 0 24 24"
									fill="currentColor"
									aria-hidden="true"
								>
									<path d="M8 5v14l11-7z" />
								</svg>
							</button>
							<span
								class="min-w-0 flex-1 truncate text-[10px] font-medium tracking-widest text-fg-muted"
							>
								UNASSIGNED
								<span class="font-mono text-fg-muted">({unassignedWardIds.length})</span>
							</span>
						</div>
						<div
							id="unassigned-members"
							hidden={!isGroupExpanded(UNASSIGNED_GROUP_ID)}
							class="flex flex-col gap-1.5 px-1.5 pb-1.5"
						>
							{#each unassignedWardIds as wardId (wardId)}
								<WardCard {wardId} ward={fleet.wards[wardId]} />
							{/each}
						</div>
					</div>
				{/if}
			</div>

			<div
				role="tabpanel"
				id="tabpanel-events"
				aria-labelledby="tab-events"
				hidden={activeTab !== 'events'}
				class="min-h-0 flex-1 overflow-y-auto p-3"
			>
				<EventsFeed />
			</div>
		</aside>
	{/if}
</div>
