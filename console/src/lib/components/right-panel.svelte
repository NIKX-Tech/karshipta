<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { rightPanelUi } from '$lib/right-panel-ui.svelte';
	import Tabs, { type TabItem } from '$lib/components/ui/tabs.svelte';
	import WardStatusStrip from '$lib/components/ward-status-strip.svelte';
	import WardTelemetryTab from '$lib/components/ward-telemetry-tab.svelte';
	import WardControlTab from '$lib/components/ward-control-tab.svelte';
	import MissionTab from '$lib/components/mission-tab.svelte';
	import ZonesTab from '$lib/components/zones-tab.svelte';
	import ThemeToggle from '$lib/components/theme-toggle.svelte';
	import UnitsToggle from '$lib/components/units-toggle.svelte';

	interface Props {
		fleetLabel: string;
	}

	const { fleetLabel }: Props = $props();

	// The icon rail stays reachable regardless of whether a ward is
	// selected, which is the whole point of this restructure. Telemetry and
	// Control used to be one "Ward" tab - split once RTL/Set Home/the
	// planned Configuration content made that single scrollable tab too
	// tall (confirmed live). WardStatusStrip (below) stays visible above
	// every one of these tabs regardless of which is active, so at-a-glance
	// telemetry is never actually hidden by this split - only the deeper
	// detail is tab-gated. Fleet management itself lives in the left rail
	// (alongside the ward list it groups); Mission is for assigning a
	// mission to a Fleet or an ad-hoc set of wards; Zones is the
	// safety-area drawing tool.
	const TABS: TabItem[] = [
		{ id: 'telemetry', label: 'Telemetry' },
		{ id: 'control', label: 'Control' },
		{ id: 'mission', label: 'Missions' },
		{ id: 'zones', label: 'Zones' }
	];

	// Auto-switch to the Telemetry tab and auto-expand the moment selection
	// goes from "none" to "some": an operator who just selected a ward
	// wants to see it, not stay wherever the panel happened to be left.
	// Tracked via a plain (non-$state) variable read/written only inside
	// this effect, so it doesn't itself trigger reruns - only
	// fleet.selectedWardId does.
	let previouslySelected: string | undefined;
	$effect(() => {
		const selected = fleet.selectedWardId;
		if (selected !== undefined && previouslySelected === undefined) {
			rightPanelUi.activeTab = 'telemetry';
			rightPanelUi.setCollapsed(false);
		}
		previouslySelected = selected;
	});

	const activeLabel = $derived(TABS.find((tab) => tab.id === rightPanelUi.activeTab)?.label ?? '');
</script>

<div class="flex h-full bg-panel">
	{#if !rightPanelUi.collapsed}
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
					onclick={() => rightPanelUi.setCollapsed(true)}
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
				id="tabpanel-telemetry"
				aria-labelledby="tab-telemetry"
				class="min-h-0 flex-1 overflow-y-auto p-3"
				hidden={rightPanelUi.activeTab !== 'telemetry'}
			>
				{#if fleet.selectedWardId}
					<WardTelemetryTab wardId={fleet.selectedWardId} />
				{:else}
					<p class="text-xs text-fg-muted">Select a ward to see its details.</p>
				{/if}
			</div>

			<div
				role="tabpanel"
				id="tabpanel-control"
				aria-labelledby="tab-control"
				class="min-h-0 flex-1 overflow-y-auto p-3"
				hidden={rightPanelUi.activeTab !== 'control'}
			>
				{#if fleet.selectedWardId}
					<WardControlTab wardId={fleet.selectedWardId} />
				{:else}
					<p class="text-xs text-fg-muted">Select a ward to command it.</p>
				{/if}
			</div>

			<div
				role="tabpanel"
				id="tabpanel-mission"
				aria-labelledby="tab-mission"
				class="min-h-0 flex-1 overflow-y-auto p-3"
				hidden={rightPanelUi.activeTab !== 'mission'}
			>
				<MissionTab {fleetLabel} />
			</div>

			<div
				role="tabpanel"
				id="tabpanel-zones"
				aria-labelledby="tab-zones"
				class="min-h-0 flex-1 overflow-y-auto p-3"
				hidden={rightPanelUi.activeTab !== 'zones'}
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
			activeId={rightPanelUi.activeTab}
			showSelection={!rightPanelUi.collapsed}
			onchange={(id) => rightPanelUi.onTabChange(id)}
			orientation="vertical"
		>
			{#snippet children(tab)}
				{#if tab.id === 'telemetry'}
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
						<circle cx="12" cy="12" r="3" />
						<line x1="12" y1="2" x2="12" y2="6" />
						<line x1="12" y1="18" x2="12" y2="22" />
						<line x1="2" y1="12" x2="6" y2="12" />
						<line x1="18" y1="12" x2="22" y2="12" />
					</svg>
				{:else if tab.id === 'control'}
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
						<line x1="3" y1="6" x2="21" y2="6" />
						<circle cx="8" cy="6" r="2" fill="currentColor" stroke="none" />
						<line x1="3" y1="12" x2="21" y2="12" />
						<circle cx="16" cy="12" r="2" fill="currentColor" stroke="none" />
						<line x1="3" y1="18" x2="21" y2="18" />
						<circle cx="11" cy="18" r="2" fill="currentColor" stroke="none" />
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

		<div class="flex-1"></div>
		<UnitsToggle />
		<ThemeToggle />
	</div>
</div>
