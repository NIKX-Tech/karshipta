<script lang="ts">
	import { onMount } from 'svelte';
	import { env } from '$env/dynamic/public';
	import { fleet } from '$lib/fleet-store.svelte';
	import { WebSocketTransport } from '$lib/transport';
	import { FAKE_FLEET_CENTER, startFakeFleet } from '$lib/fake/fleet-sim';
	import FleetMap from '$lib/components/fleet-map.svelte';
	import VehicleCard from '$lib/components/vehicle-card.svelte';

	// Real gateway when PUBLIC_GATEWAY_WS_URL is set, fake fleet otherwise.
	// Both feed the store through the same applyEnvelope path. onMount, not
	// $effect: feeding the store must not make this block depend on it.
	onMount(() => {
		const gatewayUrl = env.PUBLIC_GATEWAY_WS_URL;
		if (gatewayUrl) {
			const transport = new WebSocketTransport(gatewayUrl, {
				onEnvelope: (envelope) => fleet.applyEnvelope(envelope)
			});
			transport.start();
			return () => {
				transport.stop();
				fleet.clear();
			};
		}
		const stopFakeFleet = startFakeFleet((envelope) => fleet.applyEnvelope(envelope));
		return () => {
			stopFakeFleet();
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
	</aside>
</main>
