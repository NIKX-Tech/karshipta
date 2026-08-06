<script lang="ts">
	import { onMount } from 'svelte';
	import { themeStore, type Theme } from '../theme.svelte';

	// themeStore itself only knows 'light'/'dark' and always defaults to
	// 'dark' with nothing stored (see theme.svelte.ts's own comment) -
	// 'system' is this component's own addition on top: a third preference
	// that resolves to whichever of those two the OS reports, and keeps
	// tracking it live while selected.
	type Preference = Theme | 'system';
	const STORAGE_KEY = 'karshipta:themePreference';
	const OPTIONS: { id: Preference; label: string }[] = [
		{ id: 'light', label: 'Light' },
		{ id: 'dark', label: 'Dark' },
		{ id: 'system', label: 'System' }
	];

	let preference = $state<Preference>('dark');
	let open = $state(false);
	let menuEl: HTMLDivElement | undefined = $state();

	function systemIsLight(): boolean {
		return window.matchMedia('(prefers-color-scheme: light)').matches;
	}

	function apply(pref: Preference) {
		themeStore.set(pref === 'system' ? (systemIsLight() ? 'light' : 'dark') : pref);
	}

	function choose(pref: Preference) {
		preference = pref;
		window.localStorage.setItem(STORAGE_KEY, pref);
		apply(pref);
		open = false;
	}

	// close the menu on an outside click or Escape, same convention as
	// add-ward-menu.svelte's own compact dropdown
	$effect(() => {
		if (!open) return;
		const handlePointerDown = (event: PointerEvent) => {
			if (menuEl && !menuEl.contains(event.target as Node)) open = false;
		};
		const handleKeydown = (event: KeyboardEvent) => {
			if (event.key === 'Escape') open = false;
		};
		window.addEventListener('pointerdown', handlePointerDown);
		window.addEventListener('keydown', handleKeydown);
		return () => {
			window.removeEventListener('pointerdown', handlePointerDown);
			window.removeEventListener('keydown', handleKeydown);
		};
	});

	onMount(() => {
		const stored = window.localStorage.getItem(STORAGE_KEY);
		if (stored === 'light' || stored === 'dark' || stored === 'system') {
			preference = stored;
		} else {
			// No explicit three-way choice saved yet - fall back to whatever the
			// old binary toggle (removed from this app's own topbar, see
			// index.ts's comment on ThemeToggle) already saved under
			// theme.svelte.ts's own key, so a returning user's prior light/dark
			// pick carries over instead of silently resetting to dark.
			const legacy = window.localStorage.getItem('karshipta:theme');
			preference = legacy === 'light' ? 'light' : 'dark';
		}
		apply(preference);

		const media = window.matchMedia('(prefers-color-scheme: light)');
		const onSystemChange = () => {
			if (preference === 'system') apply('system');
		};
		media.addEventListener('change', onSystemChange);
		return () => media.removeEventListener('change', onSystemChange);
	});
</script>

<div class="relative" bind:this={menuEl}>
	<button
		type="button"
		class="flex h-7 w-7 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
		aria-label="Theme: {preference}"
		aria-expanded={open}
		aria-haspopup="true"
		title="Theme: {preference}"
		onclick={() => (open = !open)}
	>
		{#if preference === 'light'}
			<svg
				width="16"
				height="16"
				viewBox="0 0 24 24"
				fill="none"
				stroke="currentColor"
				stroke-width="1.75"
				stroke-linecap="round"
				stroke-linejoin="round"
				aria-hidden="true"
			>
				<circle cx="12" cy="12" r="4.5" />
				<path
					d="M12 2.5v2.5M12 19v2.5M4.9 4.9l1.8 1.8M17.3 17.3l1.8 1.8M2.5 12H5M19 12h2.5M4.9 19.1l1.8-1.8M17.3 6.7l1.8-1.8"
				/>
			</svg>
		{:else if preference === 'dark'}
			<svg
				width="16"
				height="16"
				viewBox="0 0 24 24"
				fill="none"
				stroke="currentColor"
				stroke-width="1.75"
				stroke-linecap="round"
				stroke-linejoin="round"
				aria-hidden="true"
			>
				<path d="M20 14.5A8.5 8.5 0 0 1 9.5 4 8.5 8.5 0 1 0 20 14.5Z" />
			</svg>
		{:else}
			<svg
				width="16"
				height="16"
				viewBox="0 0 24 24"
				fill="none"
				stroke="currentColor"
				stroke-width="1.75"
				stroke-linecap="round"
				stroke-linejoin="round"
				aria-hidden="true"
			>
				<rect x="2.5" y="4.5" width="19" height="12" rx="1.5" />
				<path d="M8 20h8M12 16.5V20" />
			</svg>
		{/if}
	</button>
	{#if open}
		<div
			class="absolute right-0 bottom-full z-30 mb-1 w-32 rounded border border-edge bg-panel p-1"
			aria-label="Theme menu"
		>
			{#each OPTIONS as option (option.id)}
				<button
					type="button"
					class="flex w-full items-center justify-between rounded px-2 py-1.5 text-left text-xs hover:bg-white/5 {preference ===
					option.id
						? 'text-accent'
						: 'text-fg'}"
					onclick={() => choose(option.id)}
				>
					{option.label}
					{#if preference === option.id}
						<svg
							width="12"
							height="12"
							viewBox="0 0 24 24"
							fill="none"
							stroke="currentColor"
							stroke-width="2"
							stroke-linecap="round"
							stroke-linejoin="round"
							aria-hidden="true"
						>
							<path d="M20 6 9 17l-5-5" />
						</svg>
					{/if}
				</button>
			{/each}
		</div>
	{/if}
</div>
