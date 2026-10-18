<script lang="ts">
	import LogsDialog from '$lib/components/logs-dialog.svelte';

	interface Props {
		wardId: string;
	}

	const { wardId }: Props = $props();

	let open = $state(false);
</script>

<!-- The sole trigger for the dialog that holds the actual list/download UI
     (logs-dialog.svelte) - deliberately just a button, no label/section/
     divider of its own: it sits inline on ward-telemetry-tab.svelte's own
     identity line (autopilot/firmware/class), not a growing block that
     would make the tab noisier the longer a ward has been flying. -->
<button type="button" class="trigger-button shrink-0" onclick={() => (open = true)}>
	Flight logs
</button>

{#if open}
	<LogsDialog {wardId} onclose={() => (open = false)} />
{/if}

<style>
	.trigger-button {
		border: 1px solid var(--color-edge);
		border-radius: 0.25rem;
		padding: 0.25rem 0.5rem;
		font-size: 0.625rem;
		font-weight: 500;
	}
	.trigger-button:hover {
		border-color: var(--color-fg-muted);
	}
</style>
