<script lang="ts">
	import { zoneStore } from '$lib/zones/zone-store.svelte';
	import { ZoneType, type Zone } from '$lib/gen/karshipta/v1/fleet';
	import ConfirmDialog from '$lib/components/confirm-dialog.svelte';

	const ZONE_TYPES = [ZoneType.ZONE_TYPE_KEEP_IN, ZoneType.ZONE_TYPE_KEEP_OUT];

	function zoneTypeLabel(type: ZoneType): string {
		return type === ZoneType.ZONE_TYPE_KEEP_IN ? 'Keep in' : 'Keep out';
	}

	function zoneTypeColor(type: ZoneType): string {
		return type === ZoneType.ZONE_TYPE_KEEP_IN ? '#22c55e' : '#ef4444';
	}

	const draft = $derived(zoneStore.draft);
	/** sub-state of drawing: still clicking vertices, vs. naming the finished shape */
	let finishing = $state(false);
	let newName = $state('');
	let newType = $state<ZoneType>(ZoneType.ZONE_TYPE_KEEP_OUT);
	let newAltitudeMin = $state<number | undefined>(undefined);
	let newAltitudeMax = $state<number | undefined>(undefined);

	function startDrawing() {
		zoneStore.startDrawing();
		finishing = false;
	}

	function finish() {
		finishing = true;
		newName = '';
		newType = ZoneType.ZONE_TYPE_KEEP_OUT;
		newAltitudeMin = undefined;
		newAltitudeMax = undefined;
	}

	function cancelDrawing() {
		zoneStore.cancelDrawing();
		finishing = false;
	}

	function submitCreate(event: SubmitEvent) {
		event.preventDefault();
		const name = newName.trim();
		if (!name) return;
		zoneStore.requestCreateZone(name, newType, newAltitudeMin, newAltitudeMax);
		finishing = false;
	}

	let editingZoneId = $state<string | undefined>(undefined);
	let editName = $state('');
	let editType = $state<ZoneType>(ZoneType.ZONE_TYPE_KEEP_OUT);
	let editAltitudeMin = $state<number | undefined>(undefined);
	let editAltitudeMax = $state<number | undefined>(undefined);
	let deleteConfirmId = $state<string | undefined>(undefined);

	function startEdit(zone: Zone) {
		editingZoneId = zone.zoneId;
		editName = zone.name;
		editType = zone.type;
		editAltitudeMin = zone.altitudeMinM;
		editAltitudeMax = zone.altitudeMaxM;
	}

	function submitEdit(event: SubmitEvent) {
		event.preventDefault();
		if (!editingZoneId) return;
		const trimmed = editName.trim();
		if (!trimmed) return;
		zoneStore.requestUpdateZone(editingZoneId, trimmed, editType, editAltitudeMin, editAltitudeMax);
		editingZoneId = undefined;
	}
</script>

<div class="flex flex-col gap-3">
	{#if draft}
		<div>
			<p class="text-[10px] text-selected">Click the map to add vertices.</p>
			<p class="mt-1 font-mono text-[10px] text-fg-muted tabular-nums">
				{draft.vertices.length} vertices
			</p>
			{#if !finishing}
				<div class="mt-2 flex flex-wrap gap-1.5">
					<button
						type="button"
						class="zones-tab-button"
						disabled={draft.vertices.length === 0}
						onclick={() => zoneStore.removeLastVertex()}
					>
						Remove last point
					</button>
					<button
						type="button"
						class="zones-tab-button"
						disabled={draft.vertices.length < 3}
						onclick={finish}
					>
						Finish
					</button>
					<button type="button" class="zones-tab-button" onclick={cancelDrawing}>Cancel</button>
				</div>
			{:else}
				<form class="mt-2 flex flex-col gap-1.5" onsubmit={submitCreate}>
					<!-- svelte-ignore a11y_autofocus -->
					<input
						type="text"
						bind:value={newName}
						required
						autofocus
						placeholder="Zone name"
						class="w-full rounded border border-edge bg-ink px-1.5 py-1 text-xs"
					/>
					<div class="flex gap-3">
						{#each ZONE_TYPES as type (type)}
							<label class="flex items-center gap-1 text-[10px]">
								<input
									type="radio"
									name="new-zone-type"
									checked={newType === type}
									onchange={() => (newType = type)}
								/>
								{zoneTypeLabel(type)}
							</label>
						{/each}
					</div>
					<div class="mt-1.5 flex flex-col gap-1 border-t border-edge pt-2.5">
						<span class="text-[10px] font-medium text-fg-muted">Altitude (optional)</span>
						<div class="flex gap-1.5">
							<label class="flex flex-1 flex-col-reverse gap-0.5 text-[10px] text-fg-muted">
								<input
									type="number"
									bind:value={newAltitudeMin}
									placeholder="none"
									class="w-full rounded border border-edge bg-ink px-1.5 py-1 text-xs text-fg"
								/>
								Min (m)
							</label>
							<label class="flex flex-1 flex-col-reverse gap-0.5 text-[10px] text-fg-muted">
								<input
									type="number"
									bind:value={newAltitudeMax}
									placeholder="none"
									class="w-full rounded border border-edge bg-ink px-1.5 py-1 text-xs text-fg"
								/>
								Max (m)
							</label>
						</div>
						<span class="text-[10px] text-fg-muted">
							Advisory only - leave blank for a boundary-only zone, or for a ward class with no
							altitude (e.g. a generic GPS tracker).
						</span>
					</div>
					<div class="flex gap-1.5">
						<button type="submit" class="zones-tab-button-accent">Create</button>
						<button
							type="button"
							class="zones-tab-button"
							title="Keep this shape and go back to adding/removing vertices"
							onclick={() => (finishing = false)}
						>
							Edit shape
						</button>
						<button
							type="button"
							class="zones-tab-button"
							title="Discard this zone entirely, including the shape drawn so far"
							onclick={cancelDrawing}
						>
							Cancel
						</button>
					</div>
				</form>
			{/if}
		</div>
	{:else}
		<button
			type="button"
			class="self-start rounded border border-accent/60 bg-accent/15 px-2 py-1 font-mono text-xs font-medium text-accent hover:bg-accent/25"
			onclick={startDrawing}
		>
			+ Draw Zone
		</button>
		<p class="text-[10px] text-fg-muted">
			Keep-in/keep-out boundaries are enforced on every connected ward's autopilot. Altitude limits
			are advisory only; they appear as a warning before upload, not a hard block.
		</p>

		{#if zoneStore.zoneIds.length === 0}
			<p class="text-xs text-fg-muted">No zones yet.</p>
		{:else}
			<div class="flex flex-col gap-2">
				{#each zoneStore.zoneIds as zoneId (zoneId)}
					{@const zone = zoneStore.zones[zoneId]}
					{#if zone}
						<div class="rounded border border-edge p-2.5">
							{#if editingZoneId === zoneId}
								<form class="flex flex-col gap-1.5" onsubmit={submitEdit}>
									<!-- svelte-ignore a11y_autofocus -->
									<input
										type="text"
										bind:value={editName}
										required
										autofocus
										class="w-full rounded border border-edge bg-ink px-1.5 py-0.5 text-[10px]"
									/>
									<div class="flex gap-3">
										{#each ZONE_TYPES as type (type)}
											<label class="flex items-center gap-1 text-[10px]">
												<input
													type="radio"
													name="edit-zone-type-{zoneId}"
													checked={editType === type}
													onchange={() => (editType = type)}
												/>
												{zoneTypeLabel(type)}
											</label>
										{/each}
									</div>
									<div class="mt-1 flex flex-col gap-1 border-t border-edge pt-2">
										<span class="text-[9px] font-medium text-fg-muted">Altitude (optional)</span>
										<div class="flex gap-1.5">
											<label class="flex flex-1 flex-col-reverse gap-0.5 text-[9px] text-fg-muted">
												<input
													type="number"
													bind:value={editAltitudeMin}
													placeholder="none"
													class="w-full rounded border border-edge bg-ink px-1.5 py-0.5 text-[10px] text-fg"
												/>
												Min (m)
											</label>
											<label class="flex flex-1 flex-col-reverse gap-0.5 text-[9px] text-fg-muted">
												<input
													type="number"
													bind:value={editAltitudeMax}
													placeholder="none"
													class="w-full rounded border border-edge bg-ink px-1.5 py-0.5 text-[10px] text-fg"
												/>
												Max (m)
											</label>
										</div>
										<span class="text-[9px] text-fg-muted">
											Advisory only - leave blank for a boundary-only zone.
										</span>
									</div>
									<div class="flex gap-1.5">
										<button type="submit" class="zones-tab-button-accent">Save</button>
										<button
											type="button"
											class="zones-tab-button"
											onclick={() => (editingZoneId = undefined)}
										>
											Cancel
										</button>
									</div>
								</form>
							{:else}
								<div class="flex flex-col gap-1.5">
									<div class="flex items-start justify-between gap-2">
										<button
											type="button"
											class="flex min-w-0 items-center gap-1.5 text-left hover:text-accent"
											onclick={() => startEdit(zone)}
											title="Edit"
										>
											<span
												class="h-2 w-2 shrink-0 rounded-full"
												style="background-color: {zoneTypeColor(zone.type)}"
											></span>
											<span class="truncate text-xs font-medium">{zone.name}</span>
										</button>
										<button
											type="button"
											class="flex h-6 w-6 shrink-0 items-center justify-center rounded text-fg-muted hover:bg-critical/15 hover:text-critical"
											aria-label="Delete {zone.name}"
											title="Delete zone"
											onclick={() => (deleteConfirmId = zoneId)}
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
												<path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13" />
												<path d="M10 11v6M14 11v6" />
											</svg>
										</button>
									</div>
									<dl
										class="grid grid-cols-[4.5rem_1fr] gap-x-2 gap-y-1 pl-3.5 font-mono text-[10px]"
									>
										<dt class="text-fg-muted">Type</dt>
										<dd>{zoneTypeLabel(zone.type)}</dd>
										<dt class="text-fg-muted">Shape</dt>
										<dd>{zone.vertices.length} points</dd>
										<dt class="text-fg-muted">Altitude</dt>
										<dd>
											{#if zone.altitudeMinM !== undefined || zone.altitudeMaxM !== undefined}
												{zone.altitudeMinM ?? 'no min'}&ndash;{zone.altitudeMaxM ?? 'no max'} m
												<span
													class="text-fg-muted"
													title="Altitude limits are advisory only; the boundary shape above is enforced, altitude is not."
												>
													(advisory only)
												</span>
											{:else}
												<span class="text-fg-muted">not set</span>
											{/if}
										</dd>
									</dl>
								</div>
							{/if}
						</div>
					{/if}
				{/each}
			</div>
		{/if}
	{/if}
</div>

{#if deleteConfirmId}
	{@const targetZoneId = deleteConfirmId}
	{@const zone = zoneStore.zones[targetZoneId]}
	<ConfirmDialog
		title="Delete {zone?.name ?? targetZoneId}"
		body="This removes the zone, including its enforced boundary on every connected ward. Its autopilot geofence is re-pushed immediately without this zone."
		confirmLabel="Delete"
		onconfirm={() => {
			zoneStore.requestDeleteZone(targetZoneId);
			deleteConfirmId = undefined;
		}}
		oncancel={() => (deleteConfirmId = undefined)}
	/>
{/if}

<style>
	.zones-tab-button {
		border: 1px solid var(--color-edge);
		border-radius: 0.25rem;
		padding: 0.25rem 0.5rem;
		font-size: 0.625rem;
		font-weight: 500;
	}
	.zones-tab-button:hover:not(:disabled) {
		border-color: var(--color-fg-muted);
	}
	.zones-tab-button:disabled {
		cursor: not-allowed;
		opacity: 0.4;
	}
	.zones-tab-button-accent {
		border: 1px solid color-mix(in srgb, var(--color-accent) 60%, transparent);
		background: color-mix(in srgb, var(--color-accent) 15%, transparent);
		color: var(--color-accent);
		border-radius: 0.25rem;
		padding: 0.25rem 0.5rem;
		font-size: 0.625rem;
		font-weight: 500;
	}
</style>
