<script lang="ts">
	import { fleet, isConfigTerminal } from '$lib/fleet-store.svelte';
	import { VehicleType } from '$lib/gen/karshipta/v1/common';
	import { VehicleConfigStatus } from '$lib/gen/karshipta/v1/fleet';

	interface Props {
		/** 'simulated' prefills a PX4 SITL preset; 'real' starts blank */
		mode: 'simulated' | 'real';
		onclose: () => void;
	}

	const { mode, onclose }: Props = $props();

	const SITL_DEFAULT_URL = 'udpin://0.0.0.0:14540';
	const VEHICLE_TYPE_PREFIX = 'VEHICLE_TYPE_';

	const VEHICLE_TYPES = [
		VehicleType.VEHICLE_TYPE_MULTIROTOR,
		VehicleType.VEHICLE_TYPE_FIXED_WING,
		VehicleType.VEHICLE_TYPE_VTOL,
		VehicleType.VEHICLE_TYPE_HELICOPTER,
		VehicleType.VEHICLE_TYPE_GROUND,
		VehicleType.VEHICLE_TYPE_UNDERWATER,
		VehicleType.VEHICLE_TYPE_SURFACE_VESSEL
	];

	function typeLabel(type: VehicleType): string {
		return VehicleType[type].replace(VEHICLE_TYPE_PREFIX, '');
	}

	// each dialog instance is freshly mounted for a fixed mode (EmptyState
	// never flips mode on a live instance), so seeding from the prop once is
	// correct; snapshot it outside $state to avoid the reactivity lint.
	const initialMode = mode;

	let vehicleId = $state('');
	let name = $state('');
	let type = $state(VehicleType.VEHICLE_TYPE_MULTIROTOR);
	let connectionUrl = $state(initialMode === 'simulated' ? SITL_DEFAULT_URL : '');
	let mavlinkSystemId = $state(0);
	let activeRequestId = $state<string | undefined>(undefined);

	const tracker = $derived(
		activeRequestId ? fleet.vehicleConfigRequests[activeRequestId] : undefined
	);
	const pending = $derived(tracker !== undefined && !isConfigTerminal(tracker.status));
	const rejected = $derived(tracker?.status === VehicleConfigStatus.VEHICLE_CONFIG_STATUS_REJECTED);

	$effect(() => {
		if (tracker?.status === VehicleConfigStatus.VEHICLE_CONFIG_STATUS_ACCEPTED) {
			onclose();
		}
	});

	function onkeydown(event: KeyboardEvent) {
		if (event.key === 'Escape' && !pending) onclose();
	}

	function submit(event: SubmitEvent) {
		event.preventDefault();
		if (!vehicleId.trim() || !connectionUrl.trim()) return;
		activeRequestId = fleet.requestAddVehicle({
			vehicleId: vehicleId.trim(),
			name: name.trim(),
			type,
			connectionUrl: connectionUrl.trim(),
			mavlinkSystemId
		});
	}
</script>

<svelte:window {onkeydown} />

<div class="fixed inset-0 z-50 flex items-center justify-center bg-black/60" role="presentation">
	<div
		role="dialog"
		aria-modal="true"
		aria-label={mode === 'simulated' ? 'Add simulated vehicle' : 'Connect real vehicle'}
		class="border-edge bg-panel w-80 rounded border p-4"
	>
		<form onsubmit={submit}>
			<h3 class="font-display text-sm font-semibold">
				{mode === 'simulated' ? 'Add simulated vehicle' : 'Connect real vehicle'}
			</h3>
			<p class="text-fg-muted mt-1 text-xs">
				{#if fleet.gatewayConnected}
					Sent to the connected gateway; it starts connecting immediately.
				{:else}
					Requires a connected gateway.
					<span class="text-critical">Not connected.</span> Open the gateway panel first.
				{/if}
			</p>

			<div class="mt-3 space-y-2">
				<label class="block text-[10px]">
					<span class="text-fg-muted">Vehicle ID</span>
					<input
						type="text"
						bind:value={vehicleId}
						required
						placeholder="alpha-1"
						disabled={pending}
						class="border-edge bg-ink mt-1 w-full rounded border px-2 py-1 font-mono text-xs disabled:opacity-50"
					/>
				</label>
				<label class="block text-[10px]">
					<span class="text-fg-muted">Name (optional)</span>
					<input
						type="text"
						bind:value={name}
						disabled={pending}
						class="border-edge bg-ink mt-1 w-full rounded border px-2 py-1 text-xs disabled:opacity-50"
					/>
				</label>
				<label class="block text-[10px]">
					<span class="text-fg-muted">Type</span>
					<select
						bind:value={type}
						disabled={pending}
						class="border-edge bg-ink mt-1 w-full rounded border px-2 py-1 text-xs disabled:opacity-50"
					>
						{#each VEHICLE_TYPES as vehicleType (vehicleType)}
							<option value={vehicleType}>{typeLabel(vehicleType)}</option>
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
						class="border-edge bg-ink mt-1 w-full rounded border px-2 py-1 font-mono text-xs disabled:opacity-50"
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
						class="border-edge bg-ink mt-1 w-full rounded border px-2 py-1 font-mono text-xs tabular-nums disabled:opacity-50"
					/>
				</label>
			</div>

			{#if rejected && tracker}
				<p class="text-critical mt-2 text-xs" role="alert">Rejected: {tracker.message}</p>
			{/if}

			<div class="mt-4 flex justify-end gap-2">
				<button
					type="button"
					onclick={onclose}
					disabled={pending}
					class="border-edge text-fg-muted hover:text-fg rounded border px-3 py-1.5 text-xs disabled:opacity-50"
				>
					Cancel
				</button>
				<button
					type="submit"
					disabled={pending || !fleet.gatewayConnected}
					class="bg-accent/15 border-accent/60 text-accent hover:bg-accent/25 rounded border px-3 py-1.5 text-xs font-medium disabled:cursor-not-allowed disabled:opacity-40"
				>
					{pending ? 'Adding...' : 'Add vehicle'}
				</button>
			</div>
		</form>
	</div>
</div>
