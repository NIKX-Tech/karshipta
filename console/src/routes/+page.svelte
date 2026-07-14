<script lang="ts">
	import { onMount } from 'svelte';
	import { env } from '$env/dynamic/public';
	import { fleet } from '$lib/fleet-store.svelte';
	import { geozoneStore } from '$lib/geozones/geozone-store.svelte';
	import { FAKE_FLEET_CENTER, FakeGateway } from '$lib/fake/fleet-sim';
	import FleetMap from '$lib/components/fleet-map.svelte';
	import VehicleCard from '$lib/components/vehicle-card.svelte';
	import VehicleDetail from '$lib/components/vehicle-detail.svelte';
	import EventsFeed from '$lib/components/events-feed.svelte';
	import ConnectionPanel from '$lib/components/connection-panel.svelte';
	import EmptyState from '$lib/components/empty-state.svelte';
	import AddVehicleMenu from '$lib/components/add-vehicle-menu.svelte';
	import logoMark from '$lib/assets/logo-mark.svg';

	let connectionPanelOpen = $state(false);

	// The demo engine is always available (instant, local, no network); the
	// gateway is opt-in and explicit. PUBLIC_GATEWAY_WS_URL/PUBLIC_READONLY
	// stay as automation overrides (docker, CI) but no longer decide what the
	// console shows by default: it opens empty and every vehicle is a UI
	// action. onMount, not $effect: feeding the store must not make this
	// block depend on it.
	onMount(() => {
		fleet.readonly = env.PUBLIC_READONLY === 'true';
		geozoneStore.configure(env.PUBLIC_OPENAIP_KEY);
		fleet.bindDemoEngine(new FakeGateway((envelope) => fleet.applyEnvelope(envelope, 'demo')));
		const gatewayUrl = env.PUBLIC_GATEWAY_WS_URL;
		if (gatewayUrl) fleet.connectGateway(gatewayUrl);
		return () => fleet.teardown();
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
		<img src={logoMark} alt="" class="h-3 w-auto" aria-hidden="true" />
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
		<button
			class="flex items-center gap-1.5 rounded px-1.5 py-0.5 hover:bg-white/5"
			onclick={() => (connectionPanelOpen = !connectionPanelOpen)}
			aria-expanded={connectionPanelOpen}
			aria-label="Gateway connection, currently {linkLabel}"
		>
			<span class="inline-block h-2 w-2 rounded-full {linkTone}"></span>
			<span class="text-fg-muted font-mono text-[10px]">{linkLabel}</span>
		</button>
	</header>

	<aside class="flex flex-col gap-2 overflow-y-auto p-3" aria-label="Fleet">
		<AddVehicleMenu variant="compact" onopenconnection={() => (connectionPanelOpen = true)} />
		{#each fleet.vehicleIds as vehicleId (vehicleId)}
			<VehicleCard {vehicleId} vehicle={fleet.vehicles[vehicleId]} />
		{/each}
	</aside>

	<div class="relative">
		<FleetMap centerLat={FAKE_FLEET_CENTER.lat} centerLon={FAKE_FLEET_CENTER.lon} />
		<EventsFeed />
		{#if fleet.vehicleIds.length === 0}
			<EmptyState onopenconnection={() => (connectionPanelOpen = true)} />
		{/if}
	</div>

	{#if fleet.selectedVehicleId}
		<VehicleDetail vehicleId={fleet.selectedVehicleId} />
	{/if}

	{#if connectionPanelOpen}
		<ConnectionPanel onclose={() => (connectionPanelOpen = false)} />
	{/if}
</div>
