<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import Tabs, { type TabItem } from '$lib/components/ui/tabs.svelte';
	import WardStatusStrip from '$lib/components/ward-status-strip.svelte';
	import WardTab from '$lib/components/ward-tab.svelte';
	import MissionTab from '$lib/components/mission-tab.svelte';

	interface Props {
		fleetLabel: string;
	}

	const { fleetLabel }: Props = $props();

	const COLLAPSED_STORAGE_KEY = 'karshipta.rightPanelCollapsed';

	// Zones lands in Step 7 - the icon rail stays reachable regardless of
	// whether a ward is selected, which is the whole point of this
	// restructure. Fleet management itself lives in the left rail
	// (alongside the ward list it groups); this tab is specifically for
	// assigning a mission to a Fleet or an ad-hoc set of wards.
	const TABS: TabItem[] = [
		{ id: 'ward', label: 'Ward' },
		{ id: 'mission', label: 'Mission' }
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

	function onTabChange(id: string) {
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
			<p
				class="border-b border-edge px-3 py-2 font-mono text-[10px] font-medium tracking-widest text-fg-muted"
			>
				{activeLabel.toUpperCase()}
			</p>

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
		</div>
	{/if}

	<!-- Always last, regardless of collapsed state, so this rail stays
	     pinned to the outer (right) edge of the screen; the expandable
	     content panel above grows/shrinks on its inboard side instead of
	     ever displacing the rail itself. -->
	<div class="flex w-10 shrink-0 flex-col items-center gap-1 border-l border-edge py-2">
		<button
			type="button"
			class="mb-1 flex h-8 w-8 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
			aria-expanded={!collapsed}
			aria-controls="right-panel-content"
			aria-label={collapsed ? 'Expand panel' : 'Collapse panel'}
			title={collapsed ? 'Expand panel' : 'Collapse panel'}
			onclick={() => setCollapsed(!collapsed)}
		>
			{collapsed ? '‹' : '›'}
		</button>

		<Tabs tabs={TABS} activeId={activeTab} onchange={onTabChange} orientation="vertical">
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
						<path d="M22 2 11 13" />
						<path d="M22 2 15 22l-4-9-9-4Z" />
					</svg>
				{/if}
			{/snippet}
		</Tabs>
	</div>
</div>
