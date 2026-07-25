<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { fleetGroups, isFleetConfigTerminal } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import ConfirmDialog from '$lib/components/confirm-dialog.svelte';
	import WardCard from '$lib/components/ward-card.svelte';
	import Disclosure from '$lib/components/ui/disclosure.svelte';

	interface Props {
		fleetId: string;
		fleetLabel: string;
	}

	const { fleetId, fleetLabel }: Props = $props();

	const group = $derived(fleetGroups.fleets[fleetId]);
	const pending = $derived(
		fleetGroups.configRequestsFor(fleetId).some((tracker) => !isFleetConfigTerminal(tracker.status))
	);

	let expanded = $state(true);
	/** edits name and description together (RenameFleet updates both at once) */
	let editing = $state(false);
	let editName = $state('');
	let editDescription = $state('');
	let deleteConfirmOpen = $state(false);
	/** the "Manage members / Delete" floating menu behind the row's kebab button */
	let menuOpen = $state(false);
	let menuEl: HTMLDivElement | undefined = $state();
	/** within that menu: the action list, or (once "Manage members" is picked)
	 * checkboxes over every ward, not just current members */
	let showMembers = $state(false);

	// close the menu on an outside click or Escape, same convention as
	// fleet-map.svelte's layers menu
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

	function startEdit() {
		if (!group) return;
		editName = group.name;
		editDescription = group.description;
		editing = true;
	}

	function submitEdit(event: SubmitEvent) {
		event.preventDefault();
		if (!group) return;
		const trimmedName = editName.trim();
		const trimmedDescription = editDescription.trim();
		if (trimmedName && (trimmedName !== group.name || trimmedDescription !== group.description)) {
			fleetGroups.requestRenameFleet(fleetId, trimmedName, trimmedDescription);
		}
		editing = false;
	}

	function toggleMember(wardId: string, checked: boolean) {
		if (checked) {
			fleetGroups.requestAddWardToFleet(fleetId, wardId);
		} else {
			fleetGroups.requestRemoveWardFromFleet(fleetId, wardId);
		}
	}
</script>

{#if group}
	<div class="rounded border border-edge px-1.5 py-1">
		<Disclosure
			id="fleet-{fleetId}"
			label="{fleetLabel.toUpperCase()} · {group.name}"
			{expanded}
			onchange={(value) => (expanded = value)}
		>
			{#snippet trailing()}
				<span class="font-mono text-fg-muted">({group.wardIds.length})</span>
			{/snippet}
			{#snippet actions()}
				<div class="relative" bind:this={menuEl}>
					<button
						type="button"
						class="flex h-6 w-6 shrink-0 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
						aria-expanded={menuOpen}
						aria-haspopup="true"
						aria-label="Fleet actions"
						title="Fleet actions"
						onclick={() => {
							menuOpen = !menuOpen;
							if (menuOpen) showMembers = false;
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
							class="absolute top-full right-0 z-30 mt-1 flex w-56 flex-col gap-0.5 rounded border border-edge bg-panel p-1.5"
						>
							{#if !showMembers}
								<button
									type="button"
									class="rounded px-2 py-1.5 text-left text-[10px] font-medium text-fg hover:bg-white/5"
									onclick={() => {
										menuOpen = false;
										startEdit();
									}}
								>
									Edit name & description
								</button>
								<button
									type="button"
									class="rounded px-2 py-1.5 text-left text-[10px] font-medium text-fg hover:bg-white/5"
									onclick={() => (showMembers = true)}
								>
									Manage members
								</button>
								<button
									type="button"
									class="rounded px-2 py-1.5 text-left text-[10px] font-medium text-critical hover:bg-critical/15 disabled:cursor-not-allowed disabled:opacity-30"
									disabled={pending}
									onclick={() => {
										menuOpen = false;
										deleteConfirmOpen = true;
									}}
								>
									Delete {fleetLabel}
								</button>
							{:else}
								<p class="px-1.5 pb-1 text-[9px] font-medium tracking-widest text-fg-muted">
									MEMBERS
								</p>
								{#if fleet.wardIds.length === 0}
									<p class="px-1.5 pb-1 text-[10px] text-fg-muted">No wards to add yet.</p>
								{:else}
									{#each fleet.wardIds as wardId (wardId)}
										<label class="flex items-center gap-1.5 px-1.5 py-0.5 text-[10px]">
											<input
												type="checkbox"
												checked={group.wardIds.includes(wardId)}
												onchange={(event) => toggleMember(wardId, event.currentTarget.checked)}
											/>
											<span class="font-mono">{wardId}</span>
										</label>
									{/each}
								{/if}
							{/if}
						</div>
					{/if}
				</div>
			{/snippet}

			{#if !editing}
				{#if group.wardIds.length === 0}
					<p class="text-[10px] text-fg-muted">No wards yet.</p>
				{:else}
					{#each group.wardIds.filter((wardId) => wardId in fleet.wards) as wardId (wardId)}
						<WardCard {wardId} ward={fleet.wards[wardId]} />
					{/each}
				{/if}
			{/if}
		</Disclosure>

		{#if editing}
			<form class="flex flex-col gap-1.5 pt-1" onsubmit={submitEdit}>
				<!-- svelte-ignore a11y_autofocus -->
				<input
					type="text"
					bind:value={editName}
					autofocus
					placeholder="Name"
					onkeydown={(event) => {
						if (event.key === 'Escape') editing = false;
					}}
					class="w-full rounded border border-edge bg-ink px-1.5 py-0.5 text-[10px]"
				/>
				<input
					type="text"
					bind:value={editDescription}
					placeholder="Description (optional)"
					onkeydown={(event) => {
						if (event.key === 'Escape') editing = false;
					}}
					class="w-full rounded border border-edge bg-ink px-1.5 py-0.5 text-[10px]"
				/>
				<div class="flex gap-1.5">
					<button
						type="submit"
						class="rounded border border-accent/60 bg-accent/15 px-2 py-0.5 text-[10px] font-medium text-accent hover:bg-accent/25"
					>
						Save
					</button>
					<button
						type="button"
						onclick={() => (editing = false)}
						class="rounded border border-edge px-2 py-0.5 text-[10px] text-fg-muted hover:text-fg"
					>
						Cancel
					</button>
				</div>
			</form>
		{/if}
	</div>
{/if}

{#if deleteConfirmOpen}
	<ConfirmDialog
		title="Delete {group?.name ?? fleetId}"
		body="This removes the {fleetLabel.toLowerCase()} and its ward groupings. Wards themselves are not affected."
		confirmLabel="Delete"
		onconfirm={() => {
			fleetGroups.requestDeleteFleet(fleetId);
			deleteConfirmOpen = false;
		}}
		oncancel={() => (deleteConfirmOpen = false)}
	/>
{/if}
