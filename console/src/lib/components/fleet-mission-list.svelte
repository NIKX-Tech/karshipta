<script lang="ts">
	import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import FleetMissionRow from '$lib/components/fleet-mission-row.svelte';

	interface Props {
		onaddmission: () => void;
	}

	const { onaddmission }: Props = $props();
</script>

<div class="flex flex-col gap-2">
	<button
		type="button"
		class="self-start rounded border border-accent/60 bg-accent/15 px-2 py-1 text-xs font-medium text-accent hover:bg-accent/25"
		onclick={onaddmission}
	>
		+ Add mission
	</button>

	{#if fleetGroups.fleetMissionIds.length === 0}
		<p class="text-xs text-fg-muted">No fleet missions yet.</p>
	{:else}
		{#each fleetGroups.fleetMissionIds as fleetMissionId (fleetMissionId)}
			{@const mission = fleetGroups.fleetMissions[fleetMissionId]}
			{#if mission}
				<FleetMissionRow {mission} />
			{/if}
		{/each}
	{/if}
</div>
