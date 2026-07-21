<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { fleetGroups, isFleetConfigTerminal } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import ConfirmDialog from '$lib/components/confirm-dialog.svelte';
	import WardCard from '$lib/components/ward-card.svelte';

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
	/** toggles the member list between "browse" (WardCards) and "edit
	 * membership" (checkboxes over every ward, not just current members) */
	let editingMembers = $state(false);

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
	<div class="rounded border border-edge">
		<div class="flex items-center gap-1 px-1.5 py-1">
			<button
				type="button"
				class="flex h-4 w-4 shrink-0 items-center justify-center text-fg-muted hover:text-fg"
				aria-expanded={expanded}
				aria-controls="fleet-members-{fleetId}"
				aria-label={expanded ? 'Collapse' : 'Expand'}
				onclick={() => (expanded = !expanded)}
			>
				<svg
					class="transition-transform {expanded ? 'rotate-90' : ''}"
					width="10"
					height="10"
					viewBox="0 0 24 24"
					fill="currentColor"
					aria-hidden="true"
				>
					<path d="M8 5v14l11-7z" />
				</svg>
			</button>

			<button
				type="button"
				class="min-w-0 flex-1 truncate text-left text-[10px] font-medium tracking-widest text-fg-muted hover:text-fg"
				onclick={startEdit}
				title="Edit"
			>
				{fleetLabel.toUpperCase()} &middot; {group.name}
				<span class="font-mono text-fg-muted">({group.wardIds.length})</span>
			</button>

			<div class="relative">
				<button
					type="button"
					class="flex h-6 w-6 shrink-0 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
					aria-expanded={editingMembers}
					aria-label="Manage members"
					title="Manage members"
					onclick={() => (editingMembers = !editingMembers)}
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
						<path d="M8 6h11M8 12h11M8 18h11" />
						<path d="M3 6.5 4 7.5 6 5.5M3 12.5 4 13.5 6 11.5M3 18.5 4 19.5 6 17.5" />
					</svg>
				</button>
				{#if editingMembers}
					<div
						class="absolute top-full right-0 z-30 mt-1 flex w-56 flex-col gap-1.5 rounded border border-edge bg-panel p-2"
						aria-label="Manage members"
					>
						{#if fleet.wardIds.length === 0}
							<p class="text-[10px] text-fg-muted">No wards to add yet.</p>
						{:else}
							{#each fleet.wardIds as wardId (wardId)}
								<label class="flex items-center gap-1.5 text-[10px]">
									<input
										type="checkbox"
										checked={group.wardIds.includes(wardId)}
										onchange={(event) => toggleMember(wardId, event.currentTarget.checked)}
									/>
									<span class="font-mono">{wardId}</span>
								</label>
							{/each}
						{/if}
					</div>
				{/if}
			</div>
			<button
				type="button"
				class="flex h-6 w-6 shrink-0 items-center justify-center rounded text-fg-muted hover:bg-critical/15 hover:text-critical disabled:cursor-not-allowed disabled:opacity-30"
				aria-label="Delete {group.name}"
				title="Delete {fleetLabel}"
				disabled={pending}
				onclick={() => (deleteConfirmOpen = true)}
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

		{#if editing}
			<form class="flex flex-col gap-1.5 px-1.5 pb-1.5" onsubmit={submitEdit}>
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

		<div
			id="fleet-members-{fleetId}"
			hidden={!expanded || editing}
			class="flex flex-col gap-1.5 px-1.5 pb-1.5"
		>
			{#if group.wardIds.length === 0}
				<p class="text-[10px] text-fg-muted">No wards yet.</p>
			{:else}
				{#each group.wardIds.filter((wardId) => wardId in fleet.wards) as wardId (wardId)}
					<WardCard {wardId} ward={fleet.wards[wardId]} />
				{/each}
			{/if}
		</div>
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
