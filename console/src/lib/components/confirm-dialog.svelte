<script lang="ts">
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

	function onkeydown(event: KeyboardEvent) {
		if (event.key === 'Escape') oncancel();
	}
</script>

<svelte:window {onkeydown} />

<div class="fixed inset-0 z-50 flex items-center justify-center bg-black/60" role="presentation">
	<div
		role="alertdialog"
		aria-modal="true"
		aria-label={title}
		class="border-edge bg-panel w-80 rounded border p-4"
	>
		<h3 class="font-display text-sm font-semibold">{title}</h3>
		<p class="text-fg-muted mt-2 text-xs">{body}</p>
		{#if warning}
			<p class="text-accent mt-2 text-xs" role="alert">{warning}</p>
		{/if}
		<div class="mt-4 flex justify-end gap-2">
			<!-- autofocus on the safe action when a flight-critical dialog opens -->
			<!-- svelte-ignore a11y_autofocus -->
			<button
				autofocus
				onclick={oncancel}
				class="border-edge text-fg-muted hover:text-fg rounded border px-3 py-1.5 text-xs"
			>
				Cancel
			</button>
			<button
				onclick={onconfirm}
				class="bg-critical/15 border-critical/60 text-critical hover:bg-critical/25 rounded border px-3 py-1.5 text-xs font-medium"
			>
				{confirmLabel}
			</button>
		</div>
	</div>
</div>
