<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import type { Command } from '$lib/gen/karshipta/v1/command';
	import ConfirmDialog from '$lib/components/confirm-dialog.svelte';

	interface Props {
		wardId: string;
	}

	const { wardId }: Props = $props();

	type Confirmation = {
		title: string;
		body: string;
		confirmLabel: string;
		action: NonNullable<Command['action']>;
	};

	// Same shape as command-panel.svelte's own takeoffAltM: a concrete
	// default, always sent. An earlier version of this field used 0 as a
	// sentinel for "leave the flight controller's current setting alone" -
	// dropped because the console has no way to read back and show what
	// that current setting actually is (get_takeoff_altitude() exists
	// gateway-side but nothing calls it or exposes it on the wire yet), so
	// the sentinel was asking the operator to trust a number they could
	// never see. A plain default they can see and change is more honest.
	const DEFAULT_RTL_ALT_M = 20;
	let rtlAltM = $state(DEFAULT_RTL_ALT_M);
	let confirmation = $state<Confirmation | undefined>(undefined);

	// This panel only renders while the ward has a flight field (see
	// ward-tab.svelte's gating), so flight is always set here, but the
	// wire type still marks it optional (unset for non-flight wards).
	const flightState = $derived(fleet.wards[wardId]?.state?.flight);

	function send(action: NonNullable<Command['action']>) {
		fleet.sendCommand(wardId, action);
	}

	// map click supplies the point, then we confirm here - identical
	// handoff shape to goto in command-panel.svelte
	const pendingSetHome = $derived(
		fleet.selectedWardId === wardId ? fleet.pendingSetHome : undefined
	);
</script>

<section
	class="rounded border border-edge bg-panel/90 p-3"
	aria-label="Return to launch for {wardId}"
>
	<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">RETURN TO LAUNCH</h3>

	<div class="mt-2 flex gap-1.5">
		<button
			class="home-button flex-1"
			disabled={!flightState || !flightState.inAir}
			title={!flightState ? 'no telemetry yet' : !flightState.inAir ? 'not in air' : undefined}
			onclick={() =>
				(confirmation = {
					title: `Return ${wardId} to launch`,
					body: 'The ward will fly back to its home position and land.',
					confirmLabel: 'RTL',
					action: { $case: 'rtl', rtl: { altitudeRelM: rtlAltM } }
				})}
		>
			RTL
		</button>
		<button
			class="home-button flex-1 {fleet.setHomeArming ? 'border-selected text-selected' : ''}"
			disabled={!flightState}
			title={!flightState ? 'no telemetry yet' : undefined}
			onclick={() => (fleet.setHomeArming ? fleet.cancelSetHome() : fleet.armSetHome())}
		>
			{fleet.setHomeArming ? 'Pick point' : 'Set home'}
		</button>
	</div>

	<label class="mt-2 flex items-center gap-2 text-[10px] text-fg-muted">
		RTL alt
		<input
			type="number"
			min="2"
			max="120"
			bind:value={rtlAltM}
			class="w-16 rounded border border-edge bg-ink px-1.5 py-0.5 font-mono text-xs tabular-nums"
		/>
		m
	</label>

	{#if fleet.setHomeArming}
		<p class="mt-2 text-[10px] text-selected">Click the map to set the new home position.</p>
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

{#if pendingSetHome}
	{@const point = pendingSetHome}
	<ConfirmDialog
		title={`Set home for ${wardId}`}
		body={`RTL will return to ${point.latitudeDeg.toFixed(6)}, ${point.longitudeDeg.toFixed(6)} instead of the launch point.`}
		confirmLabel="Set home"
		onconfirm={() => {
			send({
				$case: 'setHome',
				setHome: {
					target: {
						latitudeDeg: point.latitudeDeg,
						longitudeDeg: point.longitudeDeg,
						altitudeMslM: 0,
						altitudeRelM: 0
					}
				}
			});
			fleet.cancelSetHome();
		}}
		oncancel={() => fleet.cancelSetHome()}
	/>
{/if}

<style>
	.home-button {
		border: 1px solid var(--color-edge);
		border-radius: 0.25rem;
		padding: 0.375rem 0.5rem;
		font-size: 0.75rem;
		line-height: 1rem;
		font-weight: 500;
	}
	.home-button:hover:not(:disabled) {
		border-color: var(--color-fg-muted);
	}
	.home-button:disabled {
		cursor: not-allowed;
		opacity: 0.4;
	}
</style>
