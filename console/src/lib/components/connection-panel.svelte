<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';

	interface Props {
		onclose: () => void;
	}

	const { onclose }: Props = $props();

	const DEFAULT_GATEWAY_URL = 'ws://localhost:8765';

	let urlInput = $state(fleet.gatewayUrl || DEFAULT_GATEWAY_URL);

	function onkeydown(event: KeyboardEvent) {
		if (event.key === 'Escape') onclose();
	}

	function connect() {
		const url = urlInput.trim();
		if (!url) return;
		fleet.connectGateway(url);
	}
</script>

<svelte:window {onkeydown} />

<section
	class="border-edge bg-panel absolute top-11 right-3 z-40 w-72 rounded border p-3"
	aria-label="Gateway connection"
>
	<div class="flex items-center justify-between">
		<h3 class="text-fg-muted text-[10px] font-medium tracking-widest">GATEWAY</h3>
		<button
			class="text-fg-muted hover:text-fg rounded px-1 text-sm leading-none"
			aria-label="Close connection panel"
			onclick={onclose}
		>
			&#x2715;
		</button>
	</div>

	<label class="mt-2 block text-[10px]">
		<span class="text-fg-muted">WebSocket URL</span>
		<input
			type="text"
			bind:value={urlInput}
			disabled={fleet.gatewayConnected}
			placeholder={DEFAULT_GATEWAY_URL}
			class="border-edge bg-ink mt-1 w-full rounded border px-2 py-1 font-mono text-xs disabled:opacity-50"
		/>
	</label>

	<div class="mt-2 flex gap-1.5">
		{#if fleet.gatewayConnected || fleet.link === 'connecting'}
			<button class="connection-button" onclick={() => fleet.disconnectGateway()}>
				{fleet.link === 'connecting' ? 'Cancel' : 'Disconnect'}
			</button>
		{:else}
			<button class="connection-button" onclick={connect}>Connect</button>
		{/if}
	</div>

	<p class="text-fg-muted mt-2 text-[10px]">
		Real hardware and simulated (PX4 SITL) wards both connect through a running gateway. See
		<a
			href="https://github.com/NIKX-Tech/karshipta/blob/main/docs/quickstart.md"
			target="_blank"
			rel="noreferrer"
			class="text-selected underline">docs/quickstart.md</a
		> to run one.
	</p>
</section>

<style>
	@reference '../../routes/layout.css';

	.connection-button {
		@apply flex-1 rounded border border-edge px-2 py-1.5 text-xs font-medium hover:border-fg-muted;
	}
</style>
