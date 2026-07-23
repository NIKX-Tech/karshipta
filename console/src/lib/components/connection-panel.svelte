<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';

	interface Props {
		onclose: () => void;
	}

	const { onclose }: Props = $props();

	const DEFAULT_GATEWAY_URL = 'ws://localhost:8765';
	const DEFAULT_RELAY_URL = 'wss://relay.example.com/ws';

	let mode = $state<'websocket' | 'relay'>('websocket');
	let urlInput = $state(fleet.gatewayUrl || DEFAULT_GATEWAY_URL);
	let relayUrlInput = $state(fleet.relayUrl || DEFAULT_RELAY_URL);
	let deviceIdInput = $state(fleet.relayDeviceId);
	let deviceTokenInput = $state(fleet.relayDeviceToken);
	let pairCodeInput = $state('');
	let pairError = $state('');
	let pairing = $state(false);

	function onkeydown(event: KeyboardEvent) {
		if (event.key === 'Escape') onclose();
	}

	function connect() {
		if (mode === 'websocket') {
			const url = urlInput.trim();
			if (!url) return;
			fleet.connectGateway(url);
			return;
		}
		const relayUrl = relayUrlInput.trim();
		const deviceId = deviceIdInput.trim();
		const deviceToken = deviceTokenInput.trim();
		if (!relayUrl || !deviceId || !deviceToken) return;
		fleet.connectRelay(relayUrl, deviceId, deviceToken);
	}

	async function pair() {
		const code = pairCodeInput.trim();
		if (!code) return;
		pairing = true;
		pairError = '';
		try {
			await fleet.pairRelay(code);
			pairCodeInput = '';
		} catch (error) {
			pairError = error instanceof Error ? error.message : String(error);
		} finally {
			pairing = false;
		}
	}
</script>

<svelte:window {onkeydown} />

<section
	class="absolute top-11 right-3 z-40 w-72 rounded border border-edge bg-panel p-3"
	aria-label="Gateway connection"
>
	<div class="flex items-center justify-between">
		<h3 class="text-[10px] font-medium tracking-widest text-fg-muted">GATEWAY</h3>
		<button
			class="rounded px-1 text-sm leading-none text-fg-muted hover:text-fg"
			aria-label="Close connection panel"
			onclick={onclose}
		>
			&#x2715;
		</button>
	</div>

	<div class="mt-2 flex gap-1.5 text-[10px]" role="group" aria-label="Transport">
		<button
			type="button"
			aria-pressed={mode === 'websocket'}
			disabled={fleet.gatewayConnected || fleet.link === 'connecting'}
			onclick={() => (mode = 'websocket')}
			class="mode-button"
			class:mode-button-active={mode === 'websocket'}
		>
			WebSocket
		</button>
		<button
			type="button"
			aria-pressed={mode === 'relay'}
			disabled={fleet.gatewayConnected || fleet.link === 'connecting'}
			onclick={() => (mode = 'relay')}
			class="mode-button"
			class:mode-button-active={mode === 'relay'}
		>
			Relay
		</button>
	</div>

	{#if mode === 'websocket'}
		<label class="mt-2 block text-[10px]">
			<span class="text-fg-muted">WebSocket URL</span>
			<input
				type="text"
				bind:value={urlInput}
				disabled={fleet.gatewayConnected}
				placeholder={DEFAULT_GATEWAY_URL}
				class="mt-1 w-full rounded border border-edge bg-ink px-2 py-1 font-mono text-xs disabled:opacity-50"
			/>
		</label>
	{:else}
		<label class="mt-2 block text-[10px]">
			<span class="text-fg-muted">Relay URL</span>
			<input
				type="text"
				bind:value={relayUrlInput}
				disabled={fleet.gatewayConnected || fleet.link === 'connecting'}
				placeholder={DEFAULT_RELAY_URL}
				class="mt-1 w-full rounded border border-edge bg-ink px-2 py-1 font-mono text-xs disabled:opacity-50"
			/>
		</label>
		<label class="mt-2 block text-[10px]">
			<span class="text-fg-muted">Device ID</span>
			<input
				type="text"
				bind:value={deviceIdInput}
				disabled={fleet.gatewayConnected || fleet.link === 'connecting'}
				class="mt-1 w-full rounded border border-edge bg-ink px-2 py-1 font-mono text-xs disabled:opacity-50"
			/>
		</label>
		<label class="mt-2 block text-[10px]">
			<span class="text-fg-muted">Device token</span>
			<input
				type="password"
				bind:value={deviceTokenInput}
				disabled={fleet.gatewayConnected || fleet.link === 'connecting'}
				class="mt-1 w-full rounded border border-edge bg-ink px-2 py-1 font-mono text-xs disabled:opacity-50"
			/>
		</label>
	{/if}

	<div class="mt-2 flex gap-1.5">
		{#if fleet.gatewayConnected || fleet.link === 'connecting'}
			<button class="connection-button" onclick={() => fleet.disconnectGateway()}>
				{fleet.link === 'connecting' ? 'Cancel' : 'Disconnect'}
			</button>
		{:else}
			<button class="connection-button" onclick={connect}>Connect</button>
		{/if}
	</div>

	{#if mode === 'relay' && fleet.link !== 'down' && fleet.relayAwaitingPair}
		<div class="mt-3 border-t border-edge pt-2">
			<label class="block text-[10px]">
				<span class="text-fg-muted">Pairing code</span>
				<input
					type="text"
					inputmode="numeric"
					bind:value={pairCodeInput}
					placeholder="483921"
					class="mt-1 w-full rounded border border-edge bg-ink px-2 py-1 font-mono text-xs"
				/>
			</label>
			<button class="connection-button mt-1.5 w-full" disabled={pairing} onclick={pair}>
				{pairing ? 'Pairing...' : 'Pair'}
			</button>
			{#if pairError}
				<p class="mt-1 text-[10px] text-critical">{pairError}</p>
			{/if}
			<p class="mt-1 text-[10px] text-fg-muted">
				Enter the code shown by the gateway's pairing tool.
			</p>
		</div>
	{/if}

	<p class="mt-2 text-[10px] text-fg-muted">
		{#if mode === 'websocket'}
			Real hardware and simulated (PX4 SITL) wards both connect through a running gateway. See
			<a
				href="https://github.com/NIKX-Tech/karshipta/blob/main/docs/quickstart.md"
				target="_blank"
				rel="noreferrer"
				class="text-selected underline">docs/quickstart.md</a
			> to run one.
		{:else}
			Relay reaches a gateway with no open inbound port, end-to-end encrypted. Device ID/token come
			from relayly's device registration (POST /api/v1/devices or its CLI); pairing itself is
			one-time per device ID. See
			<a
				href="https://github.com/NIKX-Tech/karshipta/blob/main/gateway/docs/relay-transport.md"
				target="_blank"
				rel="noreferrer"
				class="text-selected underline">gateway/docs/relay-transport.md</a
			>.
		{/if}
	</p>
</section>

<style>
	@reference '../../routes/layout.css';

	.connection-button {
		@apply flex-1 rounded border border-edge px-2 py-1.5 text-xs font-medium hover:border-fg-muted disabled:opacity-50;
	}

	.mode-button {
		@apply flex-1 rounded border border-edge px-2 py-1 font-medium text-fg-muted hover:border-fg-muted disabled:opacity-50;
	}

	.mode-button-active {
		@apply border-selected text-fg;
	}
</style>
