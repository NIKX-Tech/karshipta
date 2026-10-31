<script lang="ts">
	import Dialog from '$lib/components/ui/dialog.svelte';
	import AddWardForm from '$lib/components/add-ward-form.svelte';

	interface Props {
		/** 'simulated' prefills a PX4 SITL preset; 'real' starts blank */
		mode: 'simulated' | 'real';
		onclose: () => void;
	}

	const { mode, onclose }: Props = $props();

	// AddWardForm's own pending state, surfaced here only so Dialog's
	// dismissible prop can read it - this component owns no form state of
	// its own any more (see AddWardForm, shared with this mobile sheet
	// equivalent in add-ward-menu.svelte).
	let pending = $state(false);
</script>

<Dialog
	label={mode === 'simulated' ? 'Add simulated ward' : 'Connect real ward'}
	dismissible={!pending}
	{onclose}
>
	<AddWardForm {mode} {onclose} bind:pending />
</Dialog>
