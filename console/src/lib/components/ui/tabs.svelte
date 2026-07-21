<script module lang="ts">
	export interface TabItem {
		id: string;
		label: string;
	}
</script>

<script lang="ts">
	import type { Snippet } from 'svelte';

	interface Props {
		tabs: TabItem[];
		activeId: string;
		/** 'vertical' is an icon rail (ArrowUp/Down); 'horizontal' is a text tab strip (ArrowLeft/Right) */
		orientation?: 'horizontal' | 'vertical';
		onchange: (id: string) => void;
		/** per-tab content, e.g. an icon for a vertical rail or a label for a horizontal strip;
		 * `tab.label` is still what's read out via aria-label/title regardless of what this renders */
		children: Snippet<[TabItem]>;
	}

	const { tabs, activeId, orientation = 'horizontal', onchange, children }: Props = $props();

	let buttonEls = $state<(HTMLButtonElement | undefined)[]>([]);

	// WAI-ARIA APG automatic activation: moving focus to a tab also selects
	// it, no separate activation keypress. focus=true only for the keyboard
	// paths below - a click already both focuses and should activate its own
	// target, so it skips the extra imperative .focus() call.
	function activate(index: number, focus: boolean) {
		const tab = tabs[index];
		if (!tab) return;
		onchange(tab.id);
		if (focus) buttonEls[index]?.focus();
	}

	function onkeydown(event: KeyboardEvent, index: number) {
		const nextKey = orientation === 'vertical' ? 'ArrowDown' : 'ArrowRight';
		const prevKey = orientation === 'vertical' ? 'ArrowUp' : 'ArrowLeft';
		switch (event.key) {
			case nextKey:
				event.preventDefault();
				activate((index + 1) % tabs.length, true);
				break;
			case prevKey:
				event.preventDefault();
				activate((index - 1 + tabs.length) % tabs.length, true);
				break;
			case 'Home':
				event.preventDefault();
				activate(0, true);
				break;
			case 'End':
				event.preventDefault();
				activate(tabs.length - 1, true);
				break;
		}
	}
</script>

<div
	role="tablist"
	aria-orientation={orientation}
	class="flex {orientation === 'vertical' ? 'flex-col' : 'flex-row'} gap-1"
>
	{#each tabs as tab, index (tab.id)}
		<button
			bind:this={buttonEls[index]}
			type="button"
			role="tab"
			id="tab-{tab.id}"
			aria-selected={tab.id === activeId}
			aria-controls="tabpanel-{tab.id}"
			aria-label={tab.label}
			title={tab.label}
			tabindex={tab.id === activeId ? 0 : -1}
			onclick={() => activate(index, false)}
			onkeydown={(event) => onkeydown(event, index)}
			class="flex items-center justify-center rounded {orientation === 'vertical'
				? 'h-8 w-8'
				: 'px-3 py-2 text-xs font-medium'} {tab.id === activeId
				? 'bg-accent/15 text-accent'
				: 'text-fg-muted hover:bg-white/5 hover:text-fg'}"
		>
			{@render children(tab)}
		</button>
	{/each}
</div>
