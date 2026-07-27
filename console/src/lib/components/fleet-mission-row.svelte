<script lang="ts">
	import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import { FleetMissionStopAction, WardMissionStatus } from '$lib/gen/karshipta/v1/fleet';
	import type { FleetMission } from '$lib/gen/karshipta/v1/fleet';
	import ConfirmDialog from '$lib/components/confirm-dialog.svelte';

	interface Props {
		mission: FleetMission;
	}

	const { mission }: Props = $props();

	function wardStatusLabel(status: WardMissionStatus): string {
		switch (status) {
			case WardMissionStatus.WARD_MISSION_STATUS_UPLOADING:
				return 'uploading';
			case WardMissionStatus.WARD_MISSION_STATUS_ACTIVE:
				return 'active';
			case WardMissionStatus.WARD_MISSION_STATUS_REJECTED:
				return 'rejected';
			case WardMissionStatus.WARD_MISSION_STATUS_STOPPING:
				return 'stopping';
			case WardMissionStatus.WARD_MISSION_STATUS_STOPPED:
				return 'stopped';
			default:
				return 'pending';
		}
	}

	function wardStatusColorClass(status: WardMissionStatus): string {
		switch (status) {
			case WardMissionStatus.WARD_MISSION_STATUS_ACTIVE:
				return 'text-armed';
			case WardMissionStatus.WARD_MISSION_STATUS_REJECTED:
				return 'text-critical';
			case WardMissionStatus.WARD_MISSION_STATUS_STOPPING:
				return 'text-accent';
			default:
				return 'text-fg-muted';
		}
	}

	// STOPPED/REJECTED both count as "settled" for Remove's gate - matches
	// FleetManager::handle_remove_fleet_mission exactly (a REJECTED ward
	// never started, so there's nothing to stop either).
	const allSettled = $derived(
		mission.wardStates.every(
			(state) =>
				state.status === WardMissionStatus.WARD_MISSION_STATUS_STOPPED ||
				state.status === WardMissionStatus.WARD_MISSION_STATUS_REJECTED
		)
	);
	const targetLabel = $derived(
		mission.fleetId
			? (fleetGroups.fleets[mission.fleetId]?.name ?? mission.fleetId)
			: `${mission.wardPlans.length} ward${mission.wardPlans.length === 1 ? '' : 's'}`
	);

	let menuOpen = $state(false);
	let menuEl: HTMLDivElement | undefined = $state();
	let stopChoiceOpen = $state(false);
	let stopAction = $state(FleetMissionStopAction.FLEET_MISSION_STOP_ACTION_RTL);
	let removeConfirmOpen = $state(false);

	// close the menu on an outside click or Escape, same convention as
	// fleet-row.svelte's own kebab menu
	$effect(() => {
		if (!menuOpen) return;
		const handlePointerDown = (event: PointerEvent) => {
			if (menuEl && !menuEl.contains(event.target as Node)) menuOpen = false;
		};
		const handleKeydown = (event: KeyboardEvent) => {
			if (event.key === 'Escape') menuOpen = false;
		};
		window.addEventListener('pointerdown', handlePointerDown);
		window.addEventListener('keydown', handleKeydown);
		return () => {
			window.removeEventListener('pointerdown', handlePointerDown);
			window.removeEventListener('keydown', handleKeydown);
		};
	});
</script>

<div class="rounded border border-edge p-2">
	<div class="flex items-start justify-between gap-1.5">
		<div class="min-w-0">
			<p class="truncate text-xs font-medium text-fg">
				{mission.missionName || 'Untitled mission'}
			</p>
			<p class="text-[10px] text-fg-muted">
				{targetLabel}
				{#if mission.repeatCount > 0}
					· {mission.repeatCount} extra pass{mission.repeatCount === 1 ? '' : 'es'}
				{/if}
			</p>
		</div>
		<div class="relative shrink-0" bind:this={menuEl}>
			<button
				type="button"
				class="flex h-6 w-6 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
				aria-expanded={menuOpen}
				aria-haspopup="true"
				aria-label="Mission actions"
				title="Mission actions"
				onclick={() => {
					menuOpen = !menuOpen;
					stopChoiceOpen = false;
				}}
			>
				<svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
					<circle cx="12" cy="5" r="1.75" />
					<circle cx="12" cy="12" r="1.75" />
					<circle cx="12" cy="19" r="1.75" />
				</svg>
			</button>
			{#if menuOpen}
				<div
					class="absolute top-full right-0 z-30 mt-1 flex w-52 flex-col gap-0.5 rounded border border-edge bg-panel p-1.5"
				>
					{#if !stopChoiceOpen}
						<button
							type="button"
							class="rounded px-2 py-1.5 text-left text-[10px] font-medium text-fg hover:bg-white/5"
							onclick={() => (stopChoiceOpen = true)}
						>
							Stop
						</button>
						<button
							type="button"
							class="rounded px-2 py-1.5 text-left text-[10px] font-medium text-fg hover:bg-white/5"
							onclick={() => {
								menuOpen = false;
								fleetGroups.startEditFleetMission(mission.fleetMissionId);
							}}
						>
							Edit
						</button>
						<button
							type="button"
							class="rounded px-2 py-1.5 text-left text-[10px] font-medium text-critical hover:bg-critical/15 disabled:cursor-not-allowed disabled:opacity-30"
							disabled={!allSettled}
							title={allSettled ? undefined : 'stop every ward first'}
							onclick={() => {
								menuOpen = false;
								removeConfirmOpen = true;
							}}
						>
							Remove
						</button>
					{:else}
						<p class="px-1.5 pb-1 text-[9px] font-medium tracking-widest text-fg-muted">
							STOP ACTION
						</p>
						<label class="flex items-center gap-1.5 px-1.5 py-0.5 text-[10px]">
							<input
								type="radio"
								name="stop-action-{mission.fleetMissionId}"
								checked={stopAction === FleetMissionStopAction.FLEET_MISSION_STOP_ACTION_RTL}
								onchange={() => (stopAction = FleetMissionStopAction.FLEET_MISSION_STOP_ACTION_RTL)}
							/>
							Return to launch
						</label>
						<label class="flex items-center gap-1.5 px-1.5 py-0.5 text-[10px]">
							<input
								type="radio"
								name="stop-action-{mission.fleetMissionId}"
								checked={stopAction === FleetMissionStopAction.FLEET_MISSION_STOP_ACTION_HOLD}
								onchange={() =>
									(stopAction = FleetMissionStopAction.FLEET_MISSION_STOP_ACTION_HOLD)}
							/>
							Hold in place
						</label>
						<label class="flex items-center gap-1.5 px-1.5 py-0.5 text-[10px]">
							<input
								type="radio"
								name="stop-action-{mission.fleetMissionId}"
								checked={stopAction === FleetMissionStopAction.FLEET_MISSION_STOP_ACTION_LAND}
								onchange={() =>
									(stopAction = FleetMissionStopAction.FLEET_MISSION_STOP_ACTION_LAND)}
							/>
							Land immediately
						</label>
						<div class="mt-1 flex gap-1.5 px-1.5">
							<button
								type="button"
								class="rounded border border-critical/60 bg-critical/15 px-2 py-1 text-[10px] font-medium text-critical hover:bg-critical/25"
								onclick={() => {
									menuOpen = false;
									stopChoiceOpen = false;
									fleetGroups.requestStopFleetMission(mission.fleetMissionId, stopAction);
								}}
							>
								Confirm
							</button>
							<button
								type="button"
								class="rounded border border-edge px-2 py-1 text-[10px] text-fg-muted hover:text-fg"
								onclick={() => (stopChoiceOpen = false)}
							>
								Back
							</button>
						</div>
					{/if}
				</div>
			{/if}
		</div>
	</div>

	<ul class="mt-1.5 space-y-0.5">
		{#each mission.wardStates as state (state.wardId)}
			<li class="flex items-center justify-between text-[10px]">
				<span class="font-mono text-fg-muted">{state.wardId}</span>
				<span class="font-mono {wardStatusColorClass(state.status)}">
					{wardStatusLabel(state.status)}
				</span>
			</li>
		{/each}
	</ul>
</div>

{#if removeConfirmOpen}
	<ConfirmDialog
		title="Remove mission"
		body="Removes {mission.missionName ||
			'this mission'} from the list. Every ward has already stopped or was never started."
		confirmLabel="Remove"
		onconfirm={() => {
			fleetGroups.requestRemoveFleetMission(mission.fleetMissionId);
			removeConfirmOpen = false;
		}}
		oncancel={() => (removeConfirmOpen = false)}
	/>
{/if}
