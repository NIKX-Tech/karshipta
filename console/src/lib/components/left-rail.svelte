<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import { leftRailUi } from '$lib/left-rail-ui.svelte';
	import AddWardMenu from '$lib/components/add-ward-menu.svelte';
	import WardCard from '$lib/components/ward-card.svelte';
	import FleetRow from '$lib/components/fleet-row.svelte';
	import EventsFeed from '$lib/components/events-feed.svelte';
	import ConnectionPanel from '$lib/components/connection-panel.svelte';
	import Tabs, { type TabItem } from '$lib/components/ui/tabs.svelte';

	interface Props {
		fleetLabel: string;
		onstartdemoplacement: () => void;
	}

	const { fleetLabel, onstartdemoplacement }: Props = $props();

	// Gateway first: it's the prerequisite for anything else here to show
	// real (non-demo) data, not just another content tab - the empty
	// state's "Connect real ward"/"Add simulated ward" CTAs already funnel
	// into it (leftRailUi.openGatewayTab()).
	const TABS: TabItem[] = [
		{ id: 'gateway', label: 'Gateway' },
		{ id: 'fleets', label: 'Fleets' },
		{ id: 'events', label: 'Events' }
	];
	const activeLabel = $derived(TABS.find((tab) => tab.id === leftRailUi.activeTab)?.label ?? '');

	const linkLabel = $derived(fleet.link.toUpperCase());
	// Same tone logic the header used to own; always shown on the Gateway
	// icon itself (not just while its tab is open), so connection status is
	// visible at a glance regardless of which tab is active or whether the
	// rail is collapsed - the thing that was missing before.
	const linkTone = $derived(
		fleet.link === 'down'
			? 'bg-critical'
			: fleet.link === 'connecting'
				? 'bg-fg-muted'
				: 'bg-accent animate-pulse'
	);

	// A small badge on the rail's Events icon for events that arrived while
	// that tab wasn't the visible one - cleared the moment it actually is.
	let lastSeenEventCount = $state(0);
	const hasUnseenEvents = $derived(fleet.events.length > lastSeenEventCount);
	$effect(() => {
		if (leftRailUi.activeTab === 'events' && !leftRailUi.collapsed) {
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
			activeId={leftRailUi.activeTab}
			showSelection={!leftRailUi.collapsed}
			onchange={(id) => leftRailUi.onTabChange(id)}
			orientation="vertical"
		>
			{#snippet children(tab)}
				{#if tab.id === 'gateway'}
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
							<circle cx="12" cy="12" r="2.5" />
							<path d="M7.5 16.5a7 7 0 0 1 0-9" />
							<path d="M16.5 7.5a7 7 0 0 1 0 9" />
							<path d="M4.5 19.5a11 11 0 0 1 0-15" />
							<path d="M19.5 4.5a11 11 0 0 1 0 15" />
						</svg>
						<span
							class="absolute -top-0.5 -right-0.5 h-1.5 w-1.5 rounded-full ring-2 ring-panel {linkTone}"
							aria-hidden="true"
						></span>
					</span>
				{:else if tab.id === 'fleets'}
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

	{#if !leftRailUi.collapsed}
		<aside id="left-rail-content" class="flex w-80 flex-col" aria-label={activeLabel}>
			<div class="flex items-center justify-between gap-2 border-b border-edge px-3 py-2">
				<div class="flex items-center gap-2">
					<p class="font-mono text-[10px] font-medium tracking-widest text-fg-muted">
						{activeLabel.toUpperCase()}
					</p>
					{#if leftRailUi.activeTab === 'gateway'}
						<span class="flex items-center gap-1">
							<span class="inline-block h-2 w-2 rounded-full {linkTone}"></span>
							<span class="font-mono text-[10px] text-fg-muted">{linkLabel}</span>
						</span>
					{:else if leftRailUi.activeTab === 'fleets'}
						<span class="font-mono text-[10px] text-fg-muted tabular-nums">
							{fleet.wardIds.length} ward{fleet.wardIds.length === 1 ? '' : 's'}
						</span>
					{/if}
				</div>
				<button
					type="button"
					class="flex h-5 w-5 shrink-0 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
					aria-controls="left-rail-content"
					aria-label="Collapse panel"
					title="Collapse panel"
					onclick={() => leftRailUi.setCollapsed(true)}
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
				id="tabpanel-gateway"
				aria-labelledby="tab-gateway"
				hidden={leftRailUi.activeTab !== 'gateway'}
				class="min-h-0 flex-1 overflow-y-auto p-3"
			>
				<ConnectionPanel />
			</div>

			<div
				role="tabpanel"
				id="tabpanel-fleets"
				aria-labelledby="tab-fleets"
				hidden={leftRailUi.activeTab !== 'fleets'}
				class="flex min-h-0 flex-1 flex-col gap-2 overflow-y-auto p-3"
			>
				<div class="flex gap-1.5">
					<AddWardMenu
						variant="compact"
						onopenconnection={() => leftRailUi.openGatewayTab()}
						{onstartdemoplacement}
					/>

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
				hidden={leftRailUi.activeTab !== 'events'}
				class="min-h-0 flex-1 overflow-y-auto p-3"
			>
				<EventsFeed />
			</div>
		</aside>
	{/if}
</div>
