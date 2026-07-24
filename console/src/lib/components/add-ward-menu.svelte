<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import AddWardDialog from '$lib/components/add-ward-dialog.svelte';
	import Dialog from '$lib/components/ui/dialog.svelte';

	interface Props {
		onopenconnection: () => void;
		/** starts the click-to-place demo ward placement flow (see routes/+page.svelte) */
		onstartdemoplacement: () => void;
		/** 'full' is the big centered empty-state prompt; 'compact' is the always-visible fleet-rail control */
		variant?: 'full' | 'compact';
	}

	const { onopenconnection, onstartdemoplacement, variant = 'full' }: Props = $props();

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
		if (fleet.simulatedWardAddCount >= SIMULATED_WARNING_THRESHOLD) {
			simConfirmOpen = true;
			return;
		}
		startSimulated();
	}

	function startSimulated() {
		simConfirmOpen = false;
		fleet.noteSimulatedWardRequested();
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
		onstartdemoplacement();
	}
</script>

{#if variant === 'full'}
	<div class="pointer-events-auto w-96 rounded border border-edge bg-panel/95 p-5 text-center">
		<h2 class="font-display text-sm font-semibold">No wards yet</h2>
		<p class="mt-1 text-xs text-fg-muted">Add a ward to see it on the map.</p>

		<div class="mt-4 flex flex-col gap-2">
			<button class="onboarding-button" onclick={addDemo}>
				Add demo ward
				<span class="block text-[10px] font-normal text-fg-muted">Instant, no gateway needed</span>
			</button>
			<button class="onboarding-button" onclick={openSimulated}>
				Add simulated ward
				<span class="block text-[10px] font-normal text-fg-muted">PX4 SITL, through a gateway</span>
			</button>
			<button class="onboarding-button" onclick={openReal}>
				Connect real ward
				<span class="block text-[10px] font-normal text-fg-muted">MAVLink over a gateway</span>
			</button>
		</div>

		{#if !fleet.gatewayConnected}
			<p class="mt-3 text-[10px] text-fg-muted">
				Simulated and real wards need a running gateway; the Gateway tab in the left rail connects
				one.
			</p>
		{/if}
	</div>
{:else}
	<div class="relative">
		<button
			class="rounded border border-edge px-2 py-1 font-mono text-xs text-fg-muted hover:border-accent hover:text-fg"
			aria-label="Add Ward"
			aria-expanded={compactMenuOpen}
			onclick={() => (compactMenuOpen = !compactMenuOpen)}
		>
			+ Add Ward
		</button>
		{#if compactMenuOpen}
			<div
				class="absolute top-full left-0 z-30 mt-1 w-56 rounded border border-edge bg-panel p-1.5"
				aria-label="Add ward menu"
			>
				<button class="menu-item" onclick={addDemo}>Add demo ward</button>
				<button class="menu-item" onclick={openSimulated}>Add simulated ward</button>
				<button class="menu-item" onclick={openReal}>Connect real ward</button>
			</div>
		{/if}
	</div>
{/if}

{#if simConfirmOpen}
	<Dialog
		label="Add another simulated ward"
		variant="alertdialog"
		onclose={() => (simConfirmOpen = false)}
	>
		<h3 class="font-display text-sm font-semibold">Add another simulated ward?</h3>
		<p class="mt-2 text-xs text-fg-muted">
			Each simulated ward is a full autopilot build running on your machine. A few at once can make
			fans spin up and the machine run hot.
		</p>
		<div class="mt-4 flex justify-end gap-2">
			<button
				data-autofocus
				onclick={() => (simConfirmOpen = false)}
				class="rounded border border-edge px-3 py-1.5 text-xs text-fg-muted hover:text-fg"
			>
				Cancel
			</button>
			<button
				onclick={startSimulated}
				class="rounded border border-accent/60 bg-accent/15 px-3 py-1.5 text-xs font-medium text-accent hover:bg-accent/25"
			>
				Add anyway
			</button>
		</div>
	</Dialog>
{/if}

{#if dialogMode}
	<AddWardDialog mode={dialogMode} onclose={() => (dialogMode = undefined)} />
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
