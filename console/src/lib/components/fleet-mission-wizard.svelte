<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import { checkMissionAgainstZones } from '$lib/zones/mission-zone-check';
	import ConfirmDialog from '$lib/components/confirm-dialog.svelte';
	import WaypointList from '$lib/components/waypoint-list.svelte';

	interface Props {
		fleetLabel: string;
		oncancel: () => void;
		onsubmitted: () => void;
	}

	const { fleetLabel, oncancel, onsubmitted }: Props = $props();

	// Edit (fleet-mission-row.svelte's "Edit" action) already calls
	// startEditFleetMission() before this component mounts, which sets the
	// draft with wards fixed - reopen straight at step 2 in that case. Add
	// mounts with no draft yet (step 1 builds the selection that eventually
	// calls startMissionAssignment()); read once, not reactively, so
	// stepping forward within one mount doesn't get overridden.
	let step = $state<1 | 2>(fleetGroups.missionAssignmentDraft ? 2 : 1);

	let targetFleetId = $state<string | undefined>(undefined);
	let targetWardIds = $state<string[]>([]);
	let missionName = $state(`mission-${new Date().toISOString().slice(11, 19)}`);
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
	const activeRoute = $derived(draft ? (draft.routes[draft.activeWardId] ?? []) : []);
	// Live, per-ward warning shown right where the operator is planning -
	// not just at the final confirm dialog (which still shows the
	// aggregate across every ward, see assignWarning below).
	const activeWarning = $derived(checkMissionAgainstZones(activeRoute));
	const everyWardHasRoute = $derived(
		draft ? draft.wardIds.every((wardId) => (draft.routes[wardId]?.length ?? 0) > 0) : false
	);
	const assignWarning = $derived(
		draft
			? checkMissionAgainstZones(draft.wardIds.flatMap((wardId) => draft.routes[wardId] ?? []))
			: undefined
	);

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

	function planRoutes() {
		const wardIds = targetFleetId
			? (fleetGroups.fleets[targetFleetId]?.wardIds ?? [])
			: targetWardIds;
		if (wardIds.length === 0) return;
		fleet.cancelPlanning();
		fleetGroups.startMissionAssignment(targetFleetId, wardIds);
		step = 2;
	}

	function cancel() {
		fleetGroups.cancelMissionAssignment();
		oncancel();
	}
</script>

{#if step === 1}
	<div class="flex flex-col gap-3">
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

		<div class="flex gap-1.5">
			<button
				type="button"
				class="mission-tab-button"
				disabled={targetWardCount === 0}
				onclick={planRoutes}
			>
				Plan routes ({targetWardCount})
			</button>
			<button type="button" class="mission-tab-button" onclick={cancel}>Cancel</button>
		</div>
	</div>
{:else if draft}
	<div class="flex flex-col gap-2">
		<p class="text-xs text-fg-muted">
			Planning for <span class="font-medium">{draftTargetLabel}</span> - a separate route per ward.
		</p>

		{#if draft.wardIds.length > 1}
			<div class="flex flex-wrap gap-1" role="tablist" aria-label="Ward being planned">
				{#each draft.wardIds as wardId (wardId)}
					{@const waypointCount = draft.routes[wardId]?.length ?? 0}
					<button
						type="button"
						role="tab"
						aria-selected={draft.activeWardId === wardId}
						class="rounded border px-1.5 py-0.5 font-mono text-[10px] {draft.activeWardId === wardId
							? 'border-selected text-selected'
							: 'border-edge text-fg-muted hover:text-fg'}"
						onclick={() => fleetGroups.setActiveWard(wardId)}
					>
						{wardId}
						<span class="tabular-nums">({waypointCount})</span>
					</button>
				{/each}
			</div>
		{/if}

		<p class="text-[10px] text-selected">
			Click the map to add waypoints to <span class="font-mono">{draft.activeWardId}</span>'s route.
		</p>
		{#if activeWarning}
			<p class="text-[10px] text-accent" role="alert">{activeWarning}</p>
		{/if}
		<WaypointList
			waypoints={activeRoute}
			onRemove={(index) => fleetGroups.removeAssignmentWaypoint(index)}
		/>

		<label class="flex items-center gap-1.5 text-[10px] text-fg-muted">
			Name
			<input
				type="text"
				bind:value={missionName}
				class="flex-1 rounded border border-edge bg-ink px-1.5 py-0.5 text-[10px]"
			/>
		</label>
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
				disabled={!everyWardHasRoute}
				title={everyWardHasRoute ? undefined : 'every ward needs at least one waypoint'}
				onclick={() => (assignConfirmOpen = true)}
			>
				{draft.editingFleetMissionId ? 'Save' : 'Assign'}
			</button>
			<button type="button" class="mission-tab-button" onclick={cancel}>Cancel</button>
		</div>
	</div>
{/if}

{#if assignConfirmOpen && draft}
	<ConfirmDialog
		title={draft.editingFleetMissionId ? 'Update mission' : 'Assign mission'}
		body="{draft.wardIds.length} ward(s) will each fly their own independent route."
		warning={assignWarning}
		confirmLabel={draft.editingFleetMissionId ? 'Save' : 'Assign'}
		onconfirm={() => {
			if (fleetGroups.assignMission(missionName)) onsubmitted();
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
