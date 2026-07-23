<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import Tabs, { type TabItem } from '$lib/components/ui/tabs.svelte';
	import WardStatusStrip from '$lib/components/ward-status-strip.svelte';
	import WardTab from '$lib/components/ward-tab.svelte';
	import MissionTab from '$lib/components/mission-tab.svelte';
	import ZonesTab from '$lib/components/zones-tab.svelte';

	interface Props {
		fleetLabel: string;
	}

	const { fleetLabel }: Props = $props();

	const COLLAPSED_STORAGE_KEY = 'karshipta.rightPanelCollapsed';

	// The icon rail stays reachable regardless of whether a ward is
	// selected, which is the whole point of this restructure. Fleet
	// management itself lives in the left rail (alongside the ward list it
	// groups); Mission is for assigning a mission to a Fleet or an ad-hoc
	// set of wards; Zones is the safety-area drawing tool.
	const TABS: TabItem[] = [
		{ id: 'ward', label: 'Ward' },
		{ id: 'mission', label: 'Missions' },
		{ id: 'zones', label: 'Zones' }
	];

	function readStoredCollapsed(): boolean {
		if (typeof localStorage === 'undefined') return true;
		// Unlike left-rail's equivalent, absence means "default collapsed"
		// (true), not "default expanded" - so an unset key must be
		// distinguished from an explicit 'false', not just compared to 'true'.
		const stored = localStorage.getItem(COLLAPSED_STORAGE_KEY);
		return stored === null ? true : stored === 'true';
	}

	// Starts collapsed (no ward selected yet has nothing to show); the
	// selection effect below expands it the moment that changes. This
	// mirrors the previous behavior where the panel didn't exist at all
	// until a ward was selected, while now the icon rail is always visible.
	let collapsed = $state(readStoredCollapsed());
	let activeTab = $state('ward');

	function setCollapsed(value: boolean) {
		collapsed = value;
		if (typeof localStorage === 'undefined') return;
		localStorage.setItem(COLLAPSED_STORAGE_KEY, String(value));
	}

	// Clicking whichever tab is already active (and open) closes the panel,
	// the same "click to toggle" affordance the rail button gives left-rail's
	// single directory button; clicking a different tab always switches to
	// it and ensures the panel is open, never closes on a real selection.
	function onTabChange(id: string) {
		if (id === activeTab && !collapsed) {
			setCollapsed(true);
			return;
		}
		activeTab = id;
		setCollapsed(false);
	}

	// Auto-switch to the Ward tab and auto-expand the moment selection goes
	// from "none" to "some": an operator who just selected a ward wants to
	// see it, not stay wherever the panel happened to be left. Tracked via
	// a plain (non-$state) variable read/written only inside this effect,
	// so it doesn't itself trigger reruns - only fleet.selectedWardId does.
	let previouslySelected: string | undefined;
	$effect(() => {
		const selected = fleet.selectedWardId;
		if (selected !== undefined && previouslySelected === undefined) {
			activeTab = 'ward';
			setCollapsed(false);
		}
		previouslySelected = selected;
	});

	const activeLabel = $derived(TABS.find((tab) => tab.id === activeTab)?.label ?? '');
</script>

<div class="flex h-full bg-panel">
	{#if !collapsed}
		<div id="right-panel-content" class="flex w-72 flex-col border-l border-edge">
			<div class="flex items-center justify-between gap-2 border-b border-edge px-3 py-2">
				<p class="font-mono text-[10px] font-medium tracking-widest text-fg-muted">
					{activeLabel.toUpperCase()}
				</p>
				<button
					type="button"
					class="flex h-5 w-5 shrink-0 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
					aria-controls="right-panel-content"
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
						<path d="m9 6 6 6-6 6" />
					</svg>
				</button>
			</div>

			{#if fleet.selectedWardId}
				<WardStatusStrip wardId={fleet.selectedWardId} />
			{/if}

			<div
				role="tabpanel"
				id="tabpanel-ward"
				aria-labelledby="tab-ward"
				class="min-h-0 flex-1 overflow-y-auto p-3"
				hidden={activeTab !== 'ward'}
			>
				{#if fleet.selectedWardId}
					<WardTab wardId={fleet.selectedWardId} />
				{:else}
					<p class="text-xs text-fg-muted">Select a ward to see its details.</p>
				{/if}
			</div>

			<div
				role="tabpanel"
				id="tabpanel-mission"
				aria-labelledby="tab-mission"
				class="min-h-0 flex-1 overflow-y-auto p-3"
				hidden={activeTab !== 'mission'}
			>
				<MissionTab {fleetLabel} />
			</div>

			<div
				role="tabpanel"
				id="tabpanel-zones"
				aria-labelledby="tab-zones"
				class="min-h-0 flex-1 overflow-y-auto p-3"
				hidden={activeTab !== 'zones'}
			>
				<ZonesTab />
			</div>
		</div>
	{/if}

	<!-- Always last, regardless of collapsed state, so this rail stays
	     pinned to the outer (right) edge of the screen; the expandable
	     content panel above grows/shrinks on its inboard side instead of
	     ever displacing the rail itself. -->
	<div class="flex w-10 shrink-0 flex-col items-center gap-1 border-l border-edge py-2">
		<Tabs
			tabs={TABS}
			activeId={activeTab}
			showSelection={!collapsed}
			onchange={onTabChange}
			orientation="vertical"
		>
			{#snippet children(tab)}
				{#if tab.id === 'ward'}
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
						<circle cx="12" cy="12" r="7" />
						<path d="M12 2v3M12 19v3M2 12h3M19 12h3" />
					</svg>
				{:else if tab.id === 'mission'}
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
						<path d="M4 19 9 8 14 14 20 4" stroke-dasharray="1.2 3" />
						<circle cx="4" cy="19" r="1.5" fill="currentColor" stroke="none" />
						<circle cx="9" cy="8" r="1.5" fill="currentColor" stroke="none" />
						<circle cx="14" cy="14" r="1.5" fill="currentColor" stroke="none" />
						<circle cx="20" cy="4" r="1.5" fill="currentColor" stroke="none" />
					</svg>
				{:else if tab.id === 'zones'}
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
						<path d="M12 2 21 7.5v9L12 22l-9-5.5v-9Z" />
					</svg>
				{/if}
			{/snippet}
		</Tabs>
	</div>
</div>
