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

	let preference = $state<Preference>('system');
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
			// pick carries over instead of being silently overridden. Only a
			// genuinely first-ever visit (no key at all, either storage key)
			// defaults to 'system' rather than a hardcoded 'dark'.
			const legacy = window.localStorage.getItem('karshipta:theme');
			preference = legacy === 'light' ? 'light' : legacy === 'dark' ? 'dark' : 'system';
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
			<!-- Sun-with-a-moon-notch, not a monitor glyph (reads as "your OS",
			     not "light and dark at once") or the earlier abstract
			     half-circle (unclear at 16px). Geometry adapted from
			     Streamline's "Light Dark Mode" icon (rays + circle + crescent
			     overlap), recolored to this icon set's own single-currentColor
			     stroke convention instead of its original two-tone fill. -->
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
				<circle cx="12" cy="12" r="6.3" />
				<path
					d="M17.97 14.03c-.64.22-1.32.33-2.03.33-3.48 0-6.31-2.82-6.31-6.31 0-.71.12-1.39.34-2.03-2.49.85-4.28 3.2-4.28 5.97 0 3.48 2.82 6.31 6.31 6.31 2.77 0 5.13-1.79 5.97-4.27Z"
				/>
				<path
					d="M18.31 12h3.94M19.25 4.75l-2.79 2.79M12 1.75v3.94M7.54 7.54 4.75 4.75M5.69 12H1.75M7.54 16.46l-2.79 2.79M12 18.31v3.94M16.46 16.46l2.79 2.79"
				/>
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
