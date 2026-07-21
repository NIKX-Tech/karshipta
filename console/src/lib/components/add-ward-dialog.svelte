<script lang="ts">
	import { fleet, isConfigTerminal } from '$lib/fleet-store.svelte';
	import { WardClass } from '$lib/gen/karshipta/v1/common';
	import { WardConfigStatus } from '$lib/gen/karshipta/v1/fleet';
	import Dialog from '$lib/components/ui/dialog.svelte';

	interface Props {
		/** 'simulated' prefills a PX4 SITL preset; 'real' starts blank */
		mode: 'simulated' | 'real';
		onclose: () => void;
	}

	const { mode, onclose }: Props = $props();

	const SITL_DEFAULT_URL = 'udpin://0.0.0.0:14540';
	const WARD_CLASS_PREFIX = 'WARD_CLASS_';

	// Every WardClass but UNSPECIFIED: the connect mechanism here is MAVLink
	// (connection_url + mavlink_system_id), which isn't flight-exclusive - a
	// MAVLink-speaking tracker or tag connects through this exact same
	// dialog, not just multirotors/fixed-wing/etc.
	const WARD_CLASSES = [
		WardClass.WARD_CLASS_MULTIROTOR,
		WardClass.WARD_CLASS_FIXED_WING,
		WardClass.WARD_CLASS_VTOL,
		WardClass.WARD_CLASS_HELICOPTER,
		WardClass.WARD_CLASS_GROUND,
		WardClass.WARD_CLASS_UNDERWATER,
		WardClass.WARD_CLASS_SURFACE_VESSEL,
		WardClass.WARD_CLASS_LIVESTOCK_TAG,
		WardClass.WARD_CLASS_GENERIC_TRACKER
	];

	function classLabel(wardClass: WardClass): string {
		return WardClass[wardClass].replace(WARD_CLASS_PREFIX, '');
	}

	// each dialog instance is freshly mounted for a fixed mode (EmptyState
	// never flips mode on a live instance), so seeding from the prop once is
	// correct; snapshot it outside $state to avoid the reactivity lint.
	const initialMode = mode;

	let wardId = $state('');
	let name = $state('');
	let wardClass = $state(WardClass.WARD_CLASS_MULTIROTOR);
	let connectionUrl = $state(initialMode === 'simulated' ? SITL_DEFAULT_URL : '');
	let mavlinkSystemId = $state(0);
	let activeRequestId = $state<string | undefined>(undefined);

	const tracker = $derived(activeRequestId ? fleet.wardConfigRequests[activeRequestId] : undefined);
	const pending = $derived(tracker !== undefined && !isConfigTerminal(tracker.status));
	const rejected = $derived(tracker?.status === WardConfigStatus.WARD_CONFIG_STATUS_REJECTED);

	$effect(() => {
		if (tracker?.status === WardConfigStatus.WARD_CONFIG_STATUS_ACCEPTED) {
			onclose();
		}
	});

	function submit(event: SubmitEvent) {
		event.preventDefault();
		if (!wardId.trim() || !connectionUrl.trim()) return;
		activeRequestId = fleet.requestAddWard({
			wardId: wardId.trim(),
			name: name.trim(),
			wardClass,
			connectionUrl: connectionUrl.trim(),
			mavlinkSystemId
		});
	}
</script>

<Dialog
	label={mode === 'simulated' ? 'Add simulated ward' : 'Connect real ward'}
	dismissible={!pending}
	{onclose}
>
	<form onsubmit={submit}>
		<h3 class="font-display text-sm font-semibold">
			{mode === 'simulated' ? 'Add simulated ward' : 'Connect real ward'}
		</h3>
		<p class="mt-1 text-xs text-fg-muted">
			{#if fleet.gatewayConnected}
				Sent to the connected gateway; it starts connecting immediately.
			{:else}
				Requires a connected gateway.
				<span class="text-critical">Not connected.</span> Open the gateway panel first.
			{/if}
		</p>

		<div class="mt-3 space-y-2">
			<label class="block text-[10px]">
				<span class="text-fg-muted">Ward ID</span>
				<input
					type="text"
					bind:value={wardId}
					required
					placeholder="alpha-1"
					disabled={pending}
					data-autofocus
					class="mt-1 w-full rounded border border-edge bg-ink px-2 py-1 font-mono text-xs disabled:opacity-50"
				/>
			</label>
			<label class="block text-[10px]">
				<span class="text-fg-muted">Name (optional)</span>
				<input
					type="text"
					bind:value={name}
					disabled={pending}
					class="mt-1 w-full rounded border border-edge bg-ink px-2 py-1 text-xs disabled:opacity-50"
				/>
			</label>
			<label class="block text-[10px]">
				<span class="text-fg-muted">Class</span>
				<select
					bind:value={wardClass}
					disabled={pending}
					class="mt-1 w-full rounded border border-edge bg-ink px-2 py-1 text-xs disabled:opacity-50"
				>
					{#each WARD_CLASSES as candidateClass (candidateClass)}
						<option value={candidateClass}>{classLabel(candidateClass)}</option>
					{/each}
				</select>
			</label>
			<label class="block text-[10px]">
				<span class="text-fg-muted">Connection URL</span>
				<input
					type="text"
					bind:value={connectionUrl}
					required
					placeholder="udpin://0.0.0.0:14540"
					disabled={pending}
					class="mt-1 w-full rounded border border-edge bg-ink px-2 py-1 font-mono text-xs disabled:opacity-50"
				/>
			</label>
			<label class="block text-[10px]">
				<span class="text-fg-muted">MAVLink system id (0 = first autopilot)</span>
				<input
					type="number"
					min="0"
					max="255"
					bind:value={mavlinkSystemId}
					disabled={pending}
					class="mt-1 w-full rounded border border-edge bg-ink px-2 py-1 font-mono text-xs tabular-nums disabled:opacity-50"
				/>
			</label>
		</div>

		{#if rejected && tracker}
			<p class="mt-2 text-xs text-critical" role="alert">Rejected: {tracker.message}</p>
		{/if}

		<div class="mt-4 flex justify-end gap-2">
			<button
				type="button"
				onclick={onclose}
				disabled={pending}
				class="rounded border border-edge px-3 py-1.5 text-xs text-fg-muted hover:text-fg disabled:opacity-50"
			>
				Cancel
			</button>
			<button
				type="submit"
				disabled={pending || !fleet.gatewayConnected}
				class="rounded border border-accent/60 bg-accent/15 px-3 py-1.5 text-xs font-medium text-accent hover:bg-accent/25 disabled:cursor-not-allowed disabled:opacity-40"
			>
				{pending ? 'Adding...' : 'Add ward'}
			</button>
		</div>
	</form>
</Dialog>
