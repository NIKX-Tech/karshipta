<script lang="ts">
	import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import FleetMissionList from '$lib/components/fleet-mission-list.svelte';
	import FleetMissionWizard from '$lib/components/fleet-mission-wizard.svelte';

	interface Props {
		fleetLabel: string;
	}

	const { fleetLabel }: Props = $props();

	// Default view is the mission list (fleet-mission-model.md): the wizard
	// only takes over once "Add mission" is pressed, or a card's "Edit"
	// action calls fleetGroups.startEditFleetMission() (which sets
	// missionAssignmentDraft directly, so the wizard shows even though
	// addingMission is still false).
	let addingMission = $state(false);

	function closeWizard() {
		addingMission = false;
	}
</script>

{#if fleetGroups.missionAssignmentDraft || addingMission}
	<FleetMissionWizard {fleetLabel} oncancel={closeWizard} onsubmitted={closeWizard} />
{:else}
	<FleetMissionList onaddmission={() => (addingMission = true)} />
{/if}
