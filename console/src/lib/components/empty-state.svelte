<script lang="ts">
	import AddVehicleMenu from '$lib/components/add-vehicle-menu.svelte';

	interface Props {
		onopenconnection: () => void;
		onstartdemoplacement: () => void;
	}

	const { onopenconnection, onstartdemoplacement }: Props = $props();
</script>

<div
	class="pointer-events-none absolute inset-0 flex items-center justify-center"
	aria-label="No vehicles yet"
>
	<AddVehicleMenu {onopenconnection} {onstartdemoplacement} variant="full" />
</div>
