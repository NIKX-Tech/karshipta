<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import AddVehicleDialog from '$lib/components/add-vehicle-dialog.svelte';

	interface Props {
		onopenconnection: () => void;
		/** 'full' is the big centered empty-state prompt; 'compact' is the always-visible fleet-rail control */
		variant?: 'full' | 'compact';
	}

	const { onopenconnection, variant = 'full' }: Props = $props();

	const SIMULATED_WARNING_THRESHOLD = 1;

	let dialogMode = $state<'simulated' | 'real' | undefined>(undefined);
	let simConfirmOpen = $state(false);
	let compactMenuOpen = $state(false);

	function openSimulated() {
		compactMenuOpen = false;
		if (!fleet.gatewayConnected) {
			onopenconnection();
			return;
		}
		if (fleet.simulatedVehicleAddCount >= SIMULATED_WARNING_THRESHOLD) {
			simConfirmOpen = true;
			return;
		}
		startSimulated();
	}

	function startSimulated() {
		simConfirmOpen = false;
		fleet.noteSimulatedVehicleRequested();
		dialogMode = 'simulated';
	}

	function openReal() {
		compactMenuOpen = false;
		if (!fleet.gatewayConnected) {
			onopenconnection();
			return;
		}
		dialogMode = 'real';
	}

	function addDemo() {
		compactMenuOpen = false;
		fleet.addDemoVehicle();
	}
</script>

{#if variant === 'full'}
	<div class="border-edge bg-panel/95 pointer-events-auto w-96 rounded border p-5 text-center">
		<h2 class="font-display text-sm font-semibold">No vehicles yet</h2>
		<p class="text-fg-muted mt-1 text-xs">Add a vehicle to see it on the map.</p>

		<div class="mt-4 flex flex-col gap-2">
			<button class="onboarding-button" onclick={addDemo}>
				Add demo vehicle
				<span class="text-fg-muted block text-[10px] font-normal">Instant, no gateway needed</span>
			</button>
			<button class="onboarding-button" onclick={openSimulated}>
				Add simulated vehicle
				<span class="text-fg-muted block text-[10px] font-normal">PX4 SITL, through a gateway</span>
			</button>
			<button class="onboarding-button" onclick={openReal}>
				Connect real vehicle
				<span class="text-fg-muted block text-[10px] font-normal">MAVLink over a gateway</span>
			</button>
		</div>

		{#if !fleet.gatewayConnected}
			<p class="text-fg-muted mt-3 text-[10px]">
				Simulated and real vehicles need a running gateway; the amber GATEWAY button top-right
				connects one.
			</p>
		{/if}
	</div>
{:else}
	<div class="relative">
		<button
			class="border-edge hover:border-accent text-fg-muted hover:text-fg rounded border px-2 py-1 font-mono text-xs"
			aria-label="Add vehicle"
			aria-expanded={compactMenuOpen}
			onclick={() => (compactMenuOpen = !compactMenuOpen)}
		>
			+ Add vehicle
		</button>
		{#if compactMenuOpen}
			<div
				class="border-edge bg-panel absolute top-full left-0 z-30 mt-1 w-56 rounded border p-1.5"
				aria-label="Add vehicle menu"
			>
				<button class="menu-item" onclick={addDemo}>Add demo vehicle</button>
				<button class="menu-item" onclick={openSimulated}>Add simulated vehicle</button>
				<button class="menu-item" onclick={openReal}>Connect real vehicle</button>
			</div>
		{/if}
	</div>
{/if}

{#if simConfirmOpen}
	<div class="fixed inset-0 z-50 flex items-center justify-center bg-black/60" role="presentation">
		<div
			role="alertdialog"
			aria-modal="true"
			aria-label="Add another simulated vehicle"
			class="border-edge bg-panel w-80 rounded border p-4"
		>
			<h3 class="font-display text-sm font-semibold">Add another simulated vehicle?</h3>
			<p class="text-fg-muted mt-2 text-xs">
				Each simulated vehicle is a full autopilot build running on your machine. A few at once can
				make fans spin up and the machine run hot.
			</p>
			<div class="mt-4 flex justify-end gap-2">
				<button
					onclick={() => (simConfirmOpen = false)}
					class="border-edge text-fg-muted hover:text-fg rounded border px-3 py-1.5 text-xs"
				>
					Cancel
				</button>
				<button
					onclick={startSimulated}
					class="bg-accent/15 border-accent/60 text-accent hover:bg-accent/25 rounded border px-3 py-1.5 text-xs font-medium"
				>
					Add anyway
				</button>
			</div>
		</div>
	</div>
{/if}

{#if dialogMode}
	<AddVehicleDialog mode={dialogMode} onclose={() => (dialogMode = undefined)} />
{/if}

<style>
	@reference '../../routes/layout.css';

	.onboarding-button {
		@apply rounded border border-edge px-3 py-2 text-left text-xs font-medium hover:border-accent;
	}
	.menu-item {
		@apply w-full rounded px-2 py-1.5 text-left text-xs hover:bg-ink;
	}
</style>
