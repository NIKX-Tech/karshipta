<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { geozoneStore } from '$lib/geozones/geozone-store.svelte';
	import ConfirmDialog from '$lib/components/confirm-dialog.svelte';

	interface Props {
		wardId: string;
	}

	const { wardId }: Props = $props();

	let confirmStart = $state(false);

	const draft = $derived(fleet.missionDraft?.wardId === wardId ? fleet.missionDraft : undefined);
	const uploaded = $derived(fleet.uploadedMissions[wardId]);
	const progress = $derived(fleet.missionProgress[wardId]);
	// This panel only renders while the ward has a flight field (see
	// ward-detail.svelte's gating), so flight is always set here.
	const flightState = $derived(fleet.wards[wardId]?.state?.flight);

	const startWarning = $derived.by(() => {
		if (!uploaded) return undefined;
		const names = uploaded.items.flatMap((item) => {
			const position = item.position;
			if (!position) return [];
			return geozoneStore
				.zonesContaining(position.latitudeDeg, position.longitudeDeg)
				.map((zone) => zone.name);
		});
		const uniqueNames = [...new Set(names)];
		if (uniqueNames.length === 0) return undefined;
		return `Route crosses ${uniqueNames.join(', ')}.`;
	});
</script>

<section class="border-edge bg-panel/90 rounded border p-3" aria-label="Mission for {wardId}">
	<h3 class="text-fg-muted text-[10px] font-medium tracking-widest">MISSION</h3>

	{#if !draft}
		<button class="mission-button mt-2" onclick={() => fleet.startPlanning(wardId)}>
			{uploaded ? 'Plan new mission' : 'Plan mission'}
		</button>
	{:else}
		<p class="text-selected mt-2 text-[10px]">Click the map to add waypoints.</p>
		{#if draft.waypoints.length > 0}
			<ol class="mt-2 space-y-1" aria-label="Waypoints">
				{#each draft.waypoints as waypoint, index (index)}
					<li class="flex items-center gap-2 text-[10px]">
						<span class="text-selected w-5 font-mono">{index + 1}</span>
						<span class="text-fg-muted font-mono tabular-nums">
							{waypoint.latitudeDeg.toFixed(4)}, {waypoint.longitudeDeg.toFixed(4)}
						</span>
						<input
							type="number"
							min="2"
							max="120"
							aria-label="Waypoint {index + 1} altitude"
							bind:value={waypoint.altitudeRelM}
							class="border-edge bg-ink ml-auto w-12 rounded border px-1 py-0.5 font-mono text-[10px] tabular-nums"
						/>
						<span class="text-fg-muted">m</span>
						<button
							class="text-fg-muted hover:text-critical px-0.5"
							aria-label="Remove waypoint {index + 1}"
							onclick={() => fleet.removeWaypoint(index)}
						>
							&#x2715;
						</button>
					</li>
				{/each}
			</ol>
		{/if}
		<label class="text-fg-muted mt-2 flex items-center gap-2 text-[10px]">
			Repeat
			<input
				type="number"
				min="0"
				max="100"
				bind:value={draft.repeatCount}
				class="border-edge bg-ink w-12 rounded border px-1 py-0.5 font-mono text-xs tabular-nums"
			/>
			extra passes
		</label>
		<div class="mt-2 flex gap-1.5">
			<button
				class="mission-button"
				disabled={draft.waypoints.length === 0}
				onclick={() => {
					if (fleet.uploadMission()) fleet.cancelPlanning();
				}}
			>
				Upload
			</button>
			<button class="mission-button" onclick={() => fleet.cancelPlanning()}>Cancel</button>
		</div>
	{/if}

	{#if uploaded && !draft}
		<p class="text-fg-muted mt-2 font-mono text-[10px] tabular-nums">
			{uploaded.name} &middot; {uploaded.items.length} wp &middot; {uploaded.repeatCount} repeats
		</p>
		<div class="mt-2 flex gap-1.5">
			<button
				class="mission-button"
				disabled={!flightState?.armed}
				title={flightState?.armed ? undefined : 'arm first'}
				onclick={() => (confirmStart = true)}
			>
				Start
			</button>
			<button
				class="mission-button"
				onclick={() => fleet.sendCommand(wardId, { $case: 'pauseMission', pauseMission: {} })}
			>
				Pause
			</button>
		</div>
	{/if}

	{#if progress}
		<p class="mt-2 font-mono text-[10px] tabular-nums" aria-label="Mission progress">
			{#if progress.finished}
				<span class="text-armed">finished</span>
			{:else}
				<span class="text-accent">wp {progress.currentSeq + 1}/{progress.totalItems}</span>
			{/if}
		</p>
	{/if}
</section>

{#if confirmStart}
	<ConfirmDialog
		title={`Start mission on ${wardId}`}
		body={`The ward will fly ${uploaded?.items.length ?? 0} waypoints${(uploaded?.repeatCount ?? 0) > 0 ? ` and repeat ${uploaded?.repeatCount} more times` : ''}.`}
		warning={startWarning}
		confirmLabel="Start"
		onconfirm={() => {
			fleet.sendCommand(wardId, { $case: 'startMission', startMission: {} });
			confirmStart = false;
		}}
		oncancel={() => (confirmStart = false)}
	/>
{/if}

<style>
	.mission-button {
		border: 1px solid var(--color-edge);
		border-radius: 0.25rem;
		padding: 0.375rem 0.5rem;
		font-size: 0.75rem;
		line-height: 1rem;
		font-weight: 500;
	}
	.mission-button:hover:not(:disabled) {
		border-color: var(--color-fg-muted);
	}
	.mission-button:disabled {
		cursor: not-allowed;
		opacity: 0.4;
	}
</style>
