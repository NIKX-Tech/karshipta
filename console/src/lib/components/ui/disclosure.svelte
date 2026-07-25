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
		/** optional controls rendered as a sibling after the toggle button, e.g.
		 * a kebab menu - kept outside the button itself since nesting an
		 * interactive element inside another is invalid HTML */
		actions?: Snippet;
		children: Snippet;
	}

	const { id, label, expanded, onchange, trailing, actions, children }: Props = $props();
</script>

<div>
	<div class="flex items-center gap-1">
		<button
			type="button"
			aria-expanded={expanded}
			aria-controls="disclosure-{id}"
			onclick={() => onchange(!expanded)}
			class="flex min-w-0 flex-1 items-center gap-1.5 py-1 text-left text-[10px] font-medium tracking-widest text-fg-muted hover:text-fg"
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
		{#if actions}{@render actions()}{/if}
	</div>
	<div id="disclosure-{id}" hidden={!expanded} class="flex flex-col gap-2 py-1 pl-4">
		{@render children()}
	</div>
</div>
