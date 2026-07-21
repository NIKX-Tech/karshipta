<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import { checkMissionAgainstZones } from '$lib/zones/mission-zone-check';
	import ConfirmDialog from '$lib/components/confirm-dialog.svelte';
	import WaypointList from '$lib/components/waypoint-list.svelte';

	interface Props {
		fleetLabel: string;
	}

	const { fleetLabel }: Props = $props();

	let targetFleetId = $state<string | undefined>(undefined);
	let targetWardIds = $state<string[]>([]);
	let assignConfirmOpen = $state(false);

	const draft = $derived(fleetGroups.missionAssignmentDraft);
	const targetWardCount = $derived(
		targetFleetId ? (fleetGroups.fleets[targetFleetId]?.wardIds.length ?? 0) : targetWardIds.length
	);
	const draftTargetLabel = $derived.by(() => {
		if (!draft) return '';
		if (draft.fleetId) return fleetGroups.fleets[draft.fleetId]?.name ?? draft.fleetId;
		return `${draft.wardIds.length} ward${draft.wardIds.length === 1 ? '' : 's'}`;
	});
	const assignWarning = $derived(draft ? checkMissionAgainstZones(draft.waypoints) : undefined);

	function selectFleet(fleetId: string) {
		targetFleetId = targetFleetId === fleetId ? undefined : fleetId;
		if (targetFleetId) targetWardIds = [];
	}

	function toggleWard(wardId: string, checked: boolean) {
		targetWardIds = checked
			? [...targetWardIds, wardId]
			: targetWardIds.filter((id) => id !== wardId);
		if (targetWardIds.length > 0) targetFleetId = undefined;
	}

	function planRoute() {
		const wardIds = targetFleetId
			? (fleetGroups.fleets[targetFleetId]?.wardIds ?? [])
			: targetWardIds;
		if (wardIds.length === 0) return;
		fleet.cancelPlanning();
		fleetGroups.startMissionAssignment(targetFleetId, wardIds);
	}

	function cancel() {
		fleetGroups.cancelMissionAssignment();
		targetFleetId = undefined;
		targetWardIds = [];
	}
</script>

<div class="flex flex-col gap-3">
	{#if draft}
		<p class="text-xs text-fg-muted">
			Assigning to <span class="font-medium">{draftTargetLabel}</span>.
		</p>
		<p class="text-[10px] text-selected">Click the map to add waypoints.</p>
		<WaypointList
			waypoints={draft.waypoints}
			onRemove={(index) => fleetGroups.removeAssignmentWaypoint(index)}
		/>
		<label class="flex items-center gap-2 text-[10px] text-fg-muted">
			Repeat
			<input
				type="number"
				min="0"
				max="100"
				bind:value={draft.repeatCount}
				class="w-12 rounded border border-edge bg-ink px-1 py-0.5 font-mono text-xs tabular-nums"
			/>
			extra passes
		</label>
		<div class="flex gap-1.5">
			<button
				type="button"
				class="mission-tab-button"
				disabled={draft.waypoints.length === 0}
				onclick={() => (assignConfirmOpen = true)}
			>
				Assign
			</button>
			<button type="button" class="mission-tab-button" onclick={cancel}>Cancel</button>
		</div>
	{:else}
		<div>
			<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">
				{fleetLabel.toUpperCase()}S
			</h3>
			{#if fleetGroups.fleetIds.length === 0}
				<p class="mt-1 text-xs text-fg-muted">No {fleetLabel.toLowerCase()}s yet.</p>
			{:else}
				<div class="mt-1 space-y-1">
					{#each fleetGroups.fleetIds as fleetId (fleetId)}
						{@const group = fleetGroups.fleets[fleetId]}
						{#if group}
							<label class="flex items-center gap-1.5 text-xs">
								<input
									type="radio"
									name="mission-target-fleet"
									checked={targetFleetId === fleetId}
									onchange={() => selectFleet(fleetId)}
								/>
								{group.name}
								<span class="ml-auto font-mono text-[10px] text-fg-muted tabular-nums"
									>{group.wardIds.length}</span
								>
							</label>
						{/if}
					{/each}
				</div>
			{/if}
		</div>

		<div>
			<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">OR PICK WARDS</h3>
			<div class="mt-1 space-y-1">
				{#each fleet.wardIds as wardId (wardId)}
					<label class="flex items-center gap-1.5 text-xs">
						<input
							type="checkbox"
							checked={targetWardIds.includes(wardId)}
							onchange={(event) => toggleWard(wardId, event.currentTarget.checked)}
						/>
						<span class="font-mono">{wardId}</span>
					</label>
				{/each}
			</div>
		</div>

		<button
			type="button"
			class="mission-tab-button self-start"
			disabled={targetWardCount === 0}
			onclick={planRoute}
		>
			Plan route ({targetWardCount})
		</button>
	{/if}
</div>

{#if assignConfirmOpen && draft}
	<ConfirmDialog
		title="Assign mission"
		body="{draft.wardIds.length} ward(s) will each fly an independent copy of this {draft.waypoints
			.length}-waypoint route."
		warning={assignWarning}
		confirmLabel="Assign"
		onconfirm={() => {
			fleetGroups.assignMission(`mission-${new Date().toISOString().slice(11, 19)}`);
			assignConfirmOpen = false;
		}}
		oncancel={() => (assignConfirmOpen = false)}
	/>
{/if}

<style>
	.mission-tab-button {
		border: 1px solid var(--color-edge);
		border-radius: 0.25rem;
		padding: 0.375rem 0.5rem;
		font-size: 0.75rem;
		font-weight: 500;
	}
	.mission-tab-button:hover:not(:disabled) {
		border-color: var(--color-fg-muted);
	}
	.mission-tab-button:disabled {
		cursor: not-allowed;
		opacity: 0.4;
	}
</style>
