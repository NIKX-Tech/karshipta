<script lang="ts">
	import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import { FleetMissionAckStatus, fleetMissionAckStatusToJSON } from '$lib/gen/karshipta/v1/fleet';
	import FleetMissionRow from '$lib/components/fleet-mission-row.svelte';

	interface Props {
		onaddmission: () => void;
	}

	const { onaddmission }: Props = $props();

	// create/stop/remove/update all went through with zero visible outcome
	// before this - confirmed live as a real bug for update specifically:
	// editing a still-active mission's routes gets REJECTED by the gateway
	// ("cannot edit while any ward is still active; stop it first"), but
	// the wizard already closed optimistically on submit (assignMission()
	// returns before the ack arrives, see its own comment), so the
	// operator saw the wizard close, landed back here, and the mission
	// looked completely unchanged with no explanation anywhere - "nothing
	// happened" from their side, even though the store had the rejection
	// reason the whole time in fleetMissionRequests (trackMission's own
	// tracker, just never rendered by any component). Same list/styling
	// shape as command-panel.svelte's own tracker list; sorted newest
	// first since a request settling later than one filed after it (e.g.
	// a slow update acked after a quick stop) should still surface where
	// it's noticed, not buried under newer rows.
	const FLEET_MISSION_ACK_STATUS_PREFIX = 'FLEET_MISSION_ACK_STATUS_';
	function statusLabel(status: FleetMissionAckStatus): string {
		return fleetMissionAckStatusToJSON(status).replace(FLEET_MISSION_ACK_STATUS_PREFIX, '');
	}
	const requestTrackers = $derived(
		Object.values(fleetGroups.fleetMissionRequests).sort((a, b) => b.sentAtMs - a.sentAtMs)
	);
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

	{#if requestTrackers.length > 0}
		<ul class="space-y-1" aria-label="Mission request status">
			{#each requestTrackers as tracker (tracker.requestId)}
				<li class="flex items-baseline gap-2 text-[10px]">
					<span class="font-mono uppercase">{tracker.kind}</span>
					<span
						class={tracker.status === FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_ACCEPTED
							? 'text-armed'
							: tracker.status === FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_REJECTED
								? 'text-critical'
								: 'animate-pulse text-accent'}
					>
						{statusLabel(tracker.status)}
					</span>
					{#if tracker.message}
						<span class="truncate text-fg-muted">{tracker.message}</span>
					{/if}
				</li>
			{/each}
		</ul>
	{/if}
</div>
