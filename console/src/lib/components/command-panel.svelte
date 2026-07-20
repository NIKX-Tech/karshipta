<script lang="ts">
	import { fleet, isTerminal } from '$lib/fleet-store.svelte';
	import { geozoneStore } from '$lib/geozones/geozone-store.svelte';
	import { CommandStatus, commandStatusToJSON, type Command } from '$lib/gen/karshipta/v1/command';
	import ConfirmDialog from '$lib/components/confirm-dialog.svelte';

	interface Props {
		wardId: string;
	}

	const { wardId }: Props = $props();

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

	// This panel only renders while the ward has a flight field (see
	// ward-detail.svelte's gating), so flight is always set here, but the
	// wire type still marks it optional (unset for non-flight wards).
	const flightState = $derived(fleet.wards[wardId]?.state?.flight);
	const trackers = $derived(fleet.commandsFor(wardId).slice(0, 3));
	// debounce per command kind only; safety commands (land, rtl, disarm) must
	// stay available to preempt an executing maneuver
	const inflightKinds = $derived(
		new Set(
			fleet
				.commandsFor(wardId)
				.filter((tracker) => !isTerminal(tracker.status))
				.map((tracker) => tracker.kind)
		)
	);

	function send(action: NonNullable<Command['action']>) {
		fleet.sendCommand(wardId, action);
	}

	function confirmThenSend(confirmationRequest: Confirmation) {
		confirmation = confirmationRequest;
	}

	function statusLabel(status: CommandStatus): string {
		return commandStatusToJSON(status).replace(COMMAND_STATUS_PREFIX, '');
	}

	// goto: map click supplies the point, then we confirm here
	const pendingGoto = $derived(fleet.selectedWardId === wardId ? fleet.pendingGoto : undefined);
	const gotoWarning = $derived.by(() => {
		if (!pendingGoto) return undefined;
		const zones = geozoneStore.zonesContaining(pendingGoto.latitudeDeg, pendingGoto.longitudeDeg);
		if (zones.length === 0) return undefined;
		return `Target is inside ${zones.map((zone) => zone.name).join(', ')}.`;
	});
</script>

<section class="rounded border border-edge bg-panel/90 p-3" aria-label="Commands for {wardId}">
	<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">COMMANDS</h3>

	<div class="mt-2 grid grid-cols-3 gap-1.5">
		<button
			class="command-button"
			disabled={!flightState || flightState.armed || inflightKinds.has('arm')}
			title={!flightState ? 'no telemetry yet' : flightState.armed ? 'already armed' : undefined}
			onclick={() => send({ $case: 'arm', arm: {} })}
		>
			Arm
		</button>
		<button
			class="command-button"
			disabled={!flightState ||
				!flightState.armed ||
				flightState.inAir ||
				inflightKinds.has('takeoff')}
			title={!flightState
				? 'no telemetry yet'
				: flightState.inAir
					? 'already in air'
					: !flightState.armed
						? 'arm first'
						: undefined}
			onclick={() => send({ $case: 'takeoff', takeoff: { altitudeRelM: takeoffAltM } })}
		>
			Takeoff
		</button>
		<button
			class="command-button {fleet.gotoArming ? 'border-selected text-selected' : ''}"
			disabled={!flightState || !flightState.inAir}
			title={!flightState ? 'no telemetry yet' : !flightState.inAir ? 'not in air' : undefined}
			onclick={() => (fleet.gotoArming ? fleet.cancelGoto() : fleet.armGoto())}
		>
			{fleet.gotoArming ? 'Pick point' : 'Goto'}
		</button>
		<button
			class="command-button"
			disabled={!flightState || !flightState.inAir}
			title={!flightState ? 'no telemetry yet' : !flightState.inAir ? 'not in air' : undefined}
			onclick={() =>
				confirmThenSend({
					title: `Land ${wardId}`,
					body: 'The ward will descend and land at its current position.',
					confirmLabel: 'Land',
					action: { $case: 'land', land: {} }
				})}
		>
			Land
		</button>
		<button
			class="command-button"
			disabled={!flightState || !flightState.inAir}
			title={!flightState ? 'no telemetry yet' : !flightState.inAir ? 'not in air' : undefined}
			onclick={() =>
				confirmThenSend({
					title: `Return ${wardId} to launch`,
					body: 'The ward will fly back to its home position and land.',
					confirmLabel: 'RTL',
					action: { $case: 'rtl', rtl: {} }
				})}
		>
			RTL
		</button>
		<button
			class="command-button"
			disabled={!flightState || !flightState.armed || inflightKinds.has('disarm')}
			title={!flightState ? 'no telemetry yet' : !flightState.armed ? 'not armed' : undefined}
			onclick={() => {
				if (flightState?.inAir) {
					confirmThenSend({
						title: `Force disarm ${wardId} in air`,
						body: 'Motors stop immediately. The ward will fall. This is an emergency action.',
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

	<label class="mt-2 flex items-center gap-2 text-[10px] text-fg-muted">
		Takeoff alt
		<input
			type="number"
			min="2"
			max="120"
			bind:value={takeoffAltM}
			class="w-16 rounded border border-edge bg-ink px-1.5 py-0.5 font-mono text-xs tabular-nums"
		/>
		m
	</label>

	{#if fleet.gotoArming}
		<p class="mt-2 text-[10px] text-selected">Click the map to set the goto target.</p>
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
								: 'animate-pulse text-accent'}
					>
						{statusLabel(tracker.status)}
					</span>
					{#if tracker.message}
						<span class="truncate text-fg-muted">{tracker.message}</span>
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
		title={`Goto for ${wardId}`}
		body={`Fly to ${point.latitudeDeg.toFixed(6)}, ${point.longitudeDeg.toFixed(6)} at current altitude.`}
		warning={gotoWarning}
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
