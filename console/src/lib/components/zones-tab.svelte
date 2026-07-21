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
	let newAltitudeMin = $state('');
	let newAltitudeMax = $state('');

	function startDrawing() {
		zoneStore.startDrawing();
		finishing = false;
	}

	function finish() {
		finishing = true;
		newName = '';
		newType = ZoneType.ZONE_TYPE_KEEP_OUT;
		newAltitudeMin = '';
		newAltitudeMax = '';
	}

	function cancelDrawing() {
		zoneStore.cancelDrawing();
		finishing = false;
	}

	function submitCreate(event: SubmitEvent) {
		event.preventDefault();
		const name = newName.trim();
		if (!name) return;
		const altitudeMinM = newAltitudeMin.trim() ? Number(newAltitudeMin) : undefined;
		const altitudeMaxM = newAltitudeMax.trim() ? Number(newAltitudeMax) : undefined;
		zoneStore.requestCreateZone(name, newType, altitudeMinM, altitudeMaxM);
		finishing = false;
	}

	let editingZoneId = $state<string | undefined>(undefined);
	let editName = $state('');
	let editType = $state<ZoneType>(ZoneType.ZONE_TYPE_KEEP_OUT);
	let editAltitudeMin = $state('');
	let editAltitudeMax = $state('');
	let deleteConfirmId = $state<string | undefined>(undefined);

	function startEdit(zone: Zone) {
		editingZoneId = zone.zoneId;
		editName = zone.name;
		editType = zone.type;
		editAltitudeMin = zone.altitudeMinM !== undefined ? String(zone.altitudeMinM) : '';
		editAltitudeMax = zone.altitudeMaxM !== undefined ? String(zone.altitudeMaxM) : '';
	}

	function submitEdit(event: SubmitEvent) {
		event.preventDefault();
		if (!editingZoneId) return;
		const trimmed = editName.trim();
		if (!trimmed) return;
		const altitudeMinM = editAltitudeMin.trim() ? Number(editAltitudeMin) : undefined;
		const altitudeMaxM = editAltitudeMax.trim() ? Number(editAltitudeMax) : undefined;
		zoneStore.requestUpdateZone(editingZoneId, trimmed, editType, altitudeMinM, altitudeMaxM);
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
					<div class="flex gap-1.5">
						<input
							type="number"
							bind:value={newAltitudeMin}
							placeholder="Alt min (m)"
							class="w-full rounded border border-edge bg-ink px-1.5 py-1 text-xs"
						/>
						<input
							type="number"
							bind:value={newAltitudeMax}
							placeholder="Alt max (m)"
							class="w-full rounded border border-edge bg-ink px-1.5 py-1 text-xs"
						/>
					</div>
					<div class="flex gap-1.5">
						<button type="submit" class="zones-tab-button-accent">Create</button>
						<button type="button" class="zones-tab-button" onclick={() => (finishing = false)}>
							Back
						</button>
						<button type="button" class="zones-tab-button" onclick={cancelDrawing}>Cancel</button>
					</div>
				</form>
			{/if}
		</div>
	{:else}
		<button
			type="button"
			class="self-start rounded border border-edge px-2 py-1 font-mono text-xs text-fg-muted hover:border-accent hover:text-fg"
			onclick={startDrawing}
		>
			+ Draw Zone
		</button>

		{#if zoneStore.zoneIds.length === 0}
			<p class="text-xs text-fg-muted">No zones yet.</p>
		{:else}
			<div class="flex flex-col gap-2">
				{#each zoneStore.zoneIds as zoneId (zoneId)}
					{@const zone = zoneStore.zones[zoneId]}
					{#if zone}
						<div class="rounded border border-edge p-2">
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
									<div class="flex gap-1.5">
										<input
											type="number"
											bind:value={editAltitudeMin}
											placeholder="Alt min (m)"
											class="w-full rounded border border-edge bg-ink px-1.5 py-0.5 text-[10px]"
										/>
										<input
											type="number"
											bind:value={editAltitudeMax}
											placeholder="Alt max (m)"
											class="w-full rounded border border-edge bg-ink px-1.5 py-0.5 text-[10px]"
										/>
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
								<div class="flex items-center gap-1.5">
									<span
										class="h-2 w-2 shrink-0 rounded-full"
										style="background-color: {zoneTypeColor(zone.type)}"
									></span>
									<button
										type="button"
										class="min-w-0 flex-1 truncate text-left text-xs font-medium hover:text-accent"
										onclick={() => startEdit(zone)}
										title="Edit"
									>
										{zone.name}
										<span class="font-mono text-[10px] text-fg-muted">
											({zoneTypeLabel(zone.type)} &middot; {zone.vertices.length} pts)
										</span>
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
		body="This removes the zone. It stops being checked against future missions immediately."
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
