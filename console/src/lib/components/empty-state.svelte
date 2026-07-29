<script lang="ts">
	import AddWardMenu from '$lib/components/add-ward-menu.svelte';

	interface Props {
		onopenconnection: () => void;
		onstartdemoplacement: () => void;
	}

	const { onopenconnection, onstartdemoplacement }: Props = $props();
</script>

<div
	class="pointer-events-none absolute inset-0 flex items-center justify-center"
	aria-label="No wards yet"
>
	<AddWardMenu {onopenconnection} {onstartdemoplacement} variant="full" />
</div>
