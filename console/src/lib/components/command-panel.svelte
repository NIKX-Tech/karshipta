<script lang="ts">
	import { fleet, isTerminal } from '$lib/fleet-store.svelte';
	import { CommandStatus, commandStatusToJSON, type Command } from '$lib/gen/karshipta/v1/command';
	import ConfirmDialog from '$lib/components/confirm-dialog.svelte';

	interface Props {
		vehicleId: string;
	}

	const { vehicleId }: Props = $props();

	const COMMAND_STATUS_PREFIX = 'COMMAND_STATUS_';
	const DEFAULT_TAKEOFF_ALT_M = 20;

	type Confirmation = {
		title: string;
		body: string;
		confirmLabel: string;
		action: NonNullable<Command['action']>;
	};

	let takeoffAltM = $state(DEFAULT_TAKEOFF_ALT_M);
	let confirmation = $state<Confirmation | undefined>(undefined);

	const vehicleState = $derived(fleet.vehicles[vehicleId]?.state);
	const trackers = $derived(fleet.commandsFor(vehicleId).slice(0, 3));
	// debounce per command kind only; safety commands (land, rtl, disarm) must
	// stay available to preempt an executing maneuver
	const inflightKinds = $derived(
		new Set(
			fleet
				.commandsFor(vehicleId)
				.filter((tracker) => !isTerminal(tracker.status))
				.map((tracker) => tracker.kind)
		)
	);

	function send(action: NonNullable<Command['action']>) {
		fleet.sendCommand(vehicleId, action);
	}

	function confirmThenSend(confirmationRequest: Confirmation) {
		confirmation = confirmationRequest;
	}

	function statusLabel(status: CommandStatus): string {
		return commandStatusToJSON(status).replace(COMMAND_STATUS_PREFIX, '');
	}

	// goto: map click supplies the point, then we confirm here
	const pendingGoto = $derived(
		fleet.selectedVehicleId === vehicleId ? fleet.pendingGoto : undefined
	);
</script>

<section class="border-edge bg-panel/90 rounded border p-3" aria-label="Commands for {vehicleId}">
	<h3 class="text-fg-muted text-[10px] font-medium tracking-widest">COMMANDS</h3>

	<div class="mt-2 grid grid-cols-3 gap-1.5">
		<button
			class="command-button"
			disabled={!vehicleState || vehicleState.armed || inflightKinds.has('arm')}
			onclick={() => send({ $case: 'arm', arm: {} })}
		>
			Arm
		</button>
		<button
			class="command-button"
			disabled={!vehicleState ||
				!vehicleState.armed ||
				vehicleState.inAir ||
				inflightKinds.has('takeoff')}
			onclick={() => send({ $case: 'takeoff', takeoff: { altitudeRelM: takeoffAltM } })}
		>
			Takeoff
		</button>
		<button
			class="command-button {fleet.gotoArming ? 'border-selected text-selected' : ''}"
			disabled={!vehicleState || !vehicleState.inAir}
			onclick={() => (fleet.gotoArming ? fleet.cancelGoto() : fleet.armGoto())}
		>
			{fleet.gotoArming ? 'Pick point' : 'Goto'}
		</button>
		<button
			class="command-button"
			disabled={!vehicleState || !vehicleState.inAir}
			onclick={() =>
				confirmThenSend({
					title: `Land ${vehicleId}`,
					body: 'The vehicle will descend and land at its current position.',
					confirmLabel: 'Land',
					action: { $case: 'land', land: {} }
				})}
		>
			Land
		</button>
		<button
			class="command-button"
			disabled={!vehicleState || !vehicleState.inAir}
			onclick={() =>
				confirmThenSend({
					title: `Return ${vehicleId} to launch`,
					body: 'The vehicle will fly back to its home position and land.',
					confirmLabel: 'RTL',
					action: { $case: 'rtl', rtl: {} }
				})}
		>
			RTL
		</button>
		<button
			class="command-button"
			disabled={!vehicleState || !vehicleState.armed || inflightKinds.has('disarm')}
			onclick={() => {
				if (vehicleState?.inAir) {
					confirmThenSend({
						title: `Force disarm ${vehicleId} in air`,
						body: 'Motors stop immediately. The vehicle will fall. This is an emergency action.',
						confirmLabel: 'Force disarm',
						action: { $case: 'disarm', disarm: { force: true } }
					});
				} else {
					send({ $case: 'disarm', disarm: { force: false } });
				}
			}}
		>
			Disarm
		</button>
	</div>

	<label class="text-fg-muted mt-2 flex items-center gap-2 text-[10px]">
		Takeoff alt
		<input
			type="number"
			min="2"
			max="120"
			bind:value={takeoffAltM}
			class="border-edge bg-ink w-16 rounded border px-1.5 py-0.5 font-mono text-xs tabular-nums"
		/>
		m
	</label>

	{#if fleet.gotoArming}
		<p class="text-selected mt-2 text-[10px]">Click the map to set the goto target.</p>
	{/if}

	{#if trackers.length > 0}
		<ul class="mt-2 space-y-1" aria-label="Command status">
			{#each trackers as tracker (tracker.commandId)}
				<li class="flex items-baseline gap-2 text-[10px]">
					<span class="font-mono uppercase">{tracker.kind}</span>
					<span
						class={tracker.status === CommandStatus.COMMAND_STATUS_SUCCESS
							? 'text-armed'
							: tracker.status === CommandStatus.COMMAND_STATUS_REJECTED ||
								  tracker.status === CommandStatus.COMMAND_STATUS_TIMEOUT
								? 'text-critical'
								: 'text-accent animate-pulse'}
					>
						{statusLabel(tracker.status)}
					</span>
					{#if tracker.message}
						<span class="text-fg-muted truncate">{tracker.message}</span>
					{/if}
				</li>
			{/each}
		</ul>
	{/if}
</section>

{#if confirmation}
	{@const active = confirmation}
	<ConfirmDialog
		title={active.title}
		body={active.body}
		confirmLabel={active.confirmLabel}
		onconfirm={() => {
			send(active.action);
			confirmation = undefined;
		}}
		oncancel={() => (confirmation = undefined)}
	/>
{/if}

{#if pendingGoto}
	{@const point = pendingGoto}
	<ConfirmDialog
		title={`Goto for ${vehicleId}`}
		body={`Fly to ${point.latitudeDeg.toFixed(6)}, ${point.longitudeDeg.toFixed(6)} at current altitude.`}
		confirmLabel="Fly"
		onconfirm={() => {
			send({
				$case: 'goto',
				goto: {
					target: {
						latitudeDeg: point.latitudeDeg,
						longitudeDeg: point.longitudeDeg,
						altitudeMslM: 0,
						altitudeRelM: 0
					},
					speedMS: 0
				}
			});
			fleet.cancelGoto();
		}}
		oncancel={() => fleet.cancelGoto()}
	/>
{/if}

<style>
	.command-button {
		border: 1px solid var(--color-edge);
		border-radius: 0.25rem;
		padding: 0.375rem 0.5rem;
		font-size: 0.75rem;
		line-height: 1rem;
		font-weight: 500;
	}
	.command-button:hover:not(:disabled) {
		border-color: var(--color-fg-muted);
	}
	.command-button:disabled {
		cursor: not-allowed;
		opacity: 0.4;
	}
</style>
