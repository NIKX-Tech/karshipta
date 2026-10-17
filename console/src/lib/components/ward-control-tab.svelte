<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import { rightPanelUi } from '$lib/right-panel-ui.svelte';
	import CommandPanel from '$lib/components/command-panel.svelte';
	import HomePanel from '$lib/components/home-panel.svelte';

	interface Props {
		wardId: string;
		/** See WardTelemetryTab's sibling note: per-instance override of
		 * fleet.readonly, carried over from the old WardTab's own Props
		 * comment - a multi-tenant consumer can show a teammate's wards as
		 * view-only without touching the store-wide flag. */
		readonly?: boolean;
	}

	const { wardId, readonly }: Props = $props();
	const effectiveReadonly = $derived(readonly ?? fleet.readonly);
	const hasFlight = $derived(fleet.wards[wardId]?.state?.flight !== undefined);

	// Starts a single-ward FleetMission assignment (the real, persisted,
	// trackable mission model - docs/fleet-mission-model.md) and jumps
	// straight to its route-planning step, skipping the wizard's own "pick
	// wards" step since the ward is already known. This replaces the old
	// per-ward MissionPanel, which was a separate, less capable parallel
	// implementation of the same thing (no persisted record, no edit/remove
	// after start) - confirmed the fleet-wide wizard's ad-hoc selection
	// already supports a single ward as a first-class case, not a
	// workaround.
	function planMission() {
		fleetGroups.startMissionAssignment(undefined, [wardId]);
		rightPanelUi.openMissionTab();
	}
</script>

{#if !effectiveReadonly && hasFlight}
	<div class="flex flex-col gap-3">
		<CommandPanel {wardId} />
		<HomePanel {wardId} />
		<button type="button" class="plan-mission-button" onclick={planMission}>
			Plan mission for {wardId}
		</button>
	</div>
{:else if effectiveReadonly}
	<p class="text-xs text-fg-muted">Read-only: no commands available for this ward.</p>
{:else}
	<p class="text-xs text-fg-muted">This ward has no flight controls.</p>
{/if}

<style>
	.plan-mission-button {
		border: 1px solid var(--color-edge);
		border-radius: 0.25rem;
		padding: 0.5rem;
		font-size: 0.75rem;
		font-weight: 500;
	}
	.plan-mission-button:hover {
		border-color: var(--color-fg-muted);
	}
</style>
