<script lang="ts">
	import { onMount } from 'svelte';
	import { env } from '$env/dynamic/public';
	import { fleet } from '$lib/fleet-store.svelte';
	import { WebSocketTransport, type FleetTransport } from '$lib/transport';
	import { FAKE_FLEET_CENTER, FakeGateway } from '$lib/fake/fleet-sim';
	import FleetMap from '$lib/components/fleet-map.svelte';
	import VehicleCard from '$lib/components/vehicle-card.svelte';
	import CommandPanel from '$lib/components/command-panel.svelte';
	import EventsFeed from '$lib/components/events-feed.svelte';

	// Real gateway when PUBLIC_GATEWAY_WS_URL is set, FakeGateway otherwise.
	// Both implement FleetTransport and feed the store through the same
	// applyEnvelope path. onMount, not $effect: feeding the store must not
	// make this block depend on it.
	onMount(() => {
		const gatewayUrl = env.PUBLIC_GATEWAY_WS_URL;
		const transport: FleetTransport = gatewayUrl
			? new WebSocketTransport(gatewayUrl, {
					onEnvelope: (envelope) => fleet.applyEnvelope(envelope)
				})
			: new FakeGateway((envelope) => fleet.applyEnvelope(envelope));
		fleet.bindSender((envelope) => transport.send(envelope));
		transport.start();
		return () => {
			transport.stop();
			fleet.clear();
		};
	});
</script>

<svelte:head>
	<title>Karshipta Console</title>
</svelte:head>

<main class="bg-ink relative h-dvh w-full">
	<FleetMap centerLat={FAKE_FLEET_CENTER.lat} centerLon={FAKE_FLEET_CENTER.lon} />
	<aside class="absolute top-4 left-4 flex w-64 flex-col gap-2" aria-label="Fleet status">
		<header
			class="border-edge bg-panel/90 flex items-center gap-2 rounded border px-3 py-2"
			aria-label="Karshipta"
		>
			<span class="bg-accent h-2 w-2" aria-hidden="true"></span>
			<span class="font-display text-xs font-medium tracking-[0.25em]">KARSHIPTA</span>
		</header>
		{#each fleet.vehicleIds as vehicleId (vehicleId)}
			<VehicleCard {vehicleId} vehicle={fleet.vehicles[vehicleId]} />
		{/each}
		{#if fleet.selectedVehicleId}
			<CommandPanel vehicleId={fleet.selectedVehicleId} />
		{/if}
	</aside>
	<EventsFeed />
</main>
