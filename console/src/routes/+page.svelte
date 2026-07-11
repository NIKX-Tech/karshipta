<script lang="ts">
	import { onMount } from 'svelte';
	import { env } from '$env/dynamic/public';
	import { fleet } from '$lib/fleet-store.svelte';
	import { WebSocketTransport, type FleetTransport } from '$lib/transport';
	import { FAKE_FLEET_CENTER, FakeGateway } from '$lib/fake/fleet-sim';
	import FleetMap from '$lib/components/fleet-map.svelte';
	import VehicleCard from '$lib/components/vehicle-card.svelte';
	import VehicleDetail from '$lib/components/vehicle-detail.svelte';
	import EventsFeed from '$lib/components/events-feed.svelte';

	// Real gateway when PUBLIC_GATEWAY_WS_URL is set, FakeGateway otherwise.
	// Both implement FleetTransport and feed the store through the same
	// applyEnvelope path. onMount, not $effect: feeding the store must not
	// make this block depend on it.
	onMount(() => {
		fleet.readonly = env.PUBLIC_READONLY === 'true';
		const gatewayUrl = env.PUBLIC_GATEWAY_WS_URL;
		let transport: FleetTransport;
		if (gatewayUrl) {
			transport = new WebSocketTransport(gatewayUrl, {
				onEnvelope: (envelope) => fleet.applyEnvelope(envelope),
				onStatus: (status) => {
					fleet.link = status === 'open' ? 'live' : status === 'connecting' ? 'connecting' : 'down';
				}
			});
		} else {
			transport = new FakeGateway((envelope) => fleet.applyEnvelope(envelope));
			fleet.link = 'sim';
		}
		fleet.bindSender((envelope) => transport.send(envelope));
		transport.start();
		return () => {
			transport.stop();
			fleet.clear();
		};
	});

	const linkLabel = $derived(fleet.link.toUpperCase());
	const linkTone = $derived(
		fleet.link === 'down'
			? 'bg-critical'
			: fleet.link === 'connecting'
				? 'bg-fg-muted'
				: 'bg-accent animate-pulse'
	);
</script>

<svelte:head>
	<title>Karshipta Console</title>
</svelte:head>

<div class="bg-ink grid h-dvh grid-cols-[16rem_minmax(0,1fr)_auto] grid-rows-[auto_minmax(0,1fr)]">
	<header
		class="border-edge bg-panel col-span-3 flex items-center gap-2 border-b px-4 py-2"
		aria-label="Karshipta"
	>
		<span class="bg-accent h-2 w-2" aria-hidden="true"></span>
		<span class="font-display text-xs font-medium tracking-[0.25em]">KARSHIPTA</span>
		<span class="text-fg-muted ml-auto font-mono text-[10px] tabular-nums">
			FLEET {fleet.vehicleIds.length}
		</span>
		{#if fleet.readonly}
			<span
				class="border-edge text-fg-muted rounded border px-1.5 py-0.5 font-mono text-[10px]"
				role="status"
				aria-label="Read-only session"
			>
				VIEWER
			</span>
		{/if}
		<span class="flex items-center gap-1.5" role="status" aria-label="Gateway link {linkLabel}">
			<span class="inline-block h-2 w-2 rounded-full {linkTone}"></span>
			<span class="text-fg-muted font-mono text-[10px]">{linkLabel}</span>
		</span>
	</header>

	<aside class="flex flex-col gap-2 overflow-y-auto p-3" aria-label="Fleet">
		{#each fleet.vehicleIds as vehicleId (vehicleId)}
			<VehicleCard {vehicleId} vehicle={fleet.vehicles[vehicleId]} />
		{/each}
	</aside>

	<div class="relative">
		<FleetMap centerLat={FAKE_FLEET_CENTER.lat} centerLon={FAKE_FLEET_CENTER.lon} />
		<EventsFeed />
	</div>

	{#if fleet.selectedVehicleId}
		<VehicleDetail vehicleId={fleet.selectedVehicleId} />
	{/if}
</div>
