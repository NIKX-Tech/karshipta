<script lang="ts">
	import Dialog from '$lib/components/ui/dialog.svelte';

	interface Props {
		title: string;
		body: string;
		/** non-blocking notice, e.g. a target inside an airspace zone; operator still decides */
		warning?: string;
		confirmLabel: string;
		onconfirm: () => void;
		oncancel: () => void;
	}

	const { title, body, warning, confirmLabel, onconfirm, oncancel }: Props = $props();
</script>

<Dialog label={title} variant="alertdialog" onclose={oncancel}>
	<h3 class="font-display text-sm font-semibold">{title}</h3>
	<p class="mt-2 text-xs text-fg-muted">{body}</p>
	{#if warning}
		<p class="mt-2 text-xs text-accent" role="alert">{warning}</p>
	{/if}
	<div class="mt-4 flex justify-end gap-2">
		<!-- autofocus on the safe action when a flight-critical dialog opens -->
		<button
			data-autofocus
			onclick={oncancel}
			class="rounded border border-edge px-3 py-1.5 text-xs text-fg-muted hover:text-fg"
		>
			Cancel
		</button>
		<button
			onclick={onconfirm}
			class="rounded border border-critical/60 bg-critical/15 px-3 py-1.5 text-xs font-medium text-critical hover:bg-critical/25"
		>
			{confirmLabel}
		</button>
	</div>
</Dialog>
