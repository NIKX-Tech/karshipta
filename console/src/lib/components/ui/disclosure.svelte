<script lang="ts">
	import type { Snippet } from 'svelte';

	interface Props {
		/** unique per instance - used to link the button to its region via aria-controls */
		id: string;
		label: string;
		expanded: boolean;
		onchange: (expanded: boolean) => void;
		/** optional trailing content next to the label, e.g. a member count */
		trailing?: Snippet;
		children: Snippet;
	}

	const { id, label, expanded, onchange, trailing, children }: Props = $props();
</script>

<div>
	<button
		type="button"
		aria-expanded={expanded}
		aria-controls="disclosure-{id}"
		onclick={() => onchange(!expanded)}
		class="flex w-full items-center gap-1.5 py-1 text-left text-[10px] font-medium tracking-widest text-fg-muted hover:text-fg"
	>
		<svg
			class="shrink-0 transition-transform {expanded ? 'rotate-90' : ''}"
			width="10"
			height="10"
			viewBox="0 0 24 24"
			fill="currentColor"
			aria-hidden="true"
		>
			<path d="M8 5v14l11-7z" />
		</svg>
		<span class="truncate">{label}</span>
		{#if trailing}{@render trailing()}{/if}
	</button>
	<div id="disclosure-{id}" hidden={!expanded} class="flex flex-col gap-2 py-1 pl-4">
		{@render children()}
	</div>
</div>
