<script lang="ts">
	import './layout.css';
	import favicon from '$lib/assets/logo-mark.svg';
	import { themeStore } from '$lib/theme.svelte';

	// Syncs themeStore.current (the reactive JS value FleetMap's own basemap
	// choice depends on) with the data-theme attribute app.html's own inline
	// script already applied pre-paint (see that script's own comment on why
	// it exists: avoiding a flash of the wrong theme). Without this,
	// themeStore.current stayed at its hardcoded 'dark' default until
	// ThemeToggle happened to mount and run its own (more complete, system-
	// preference-aware) sync logic - fine on desktop, where it's always
	// visible, but on mobile ThemeToggle lives inside left-rail.svelte's
	// collapsed rail, which doesn't render until the hamburger is tapped
	// (leftRailUi.mobileRailVisible defaults hidden). Confirmed live as a
	// real bug: a saved 'light' choice showed correctly on the chrome (CSS,
	// driven by the attribute) while the map stayed on its dark basemap
	// (JS-state-driven, never synced) until that rail was opened. Top-level,
	// not onMount: this needs to run before any child - including the map -
	// reads themeStore.current for the first time, same reasoning
	// +page.svelte's own fleet.bindDemoEngine() call gives for running
	// outside onMount. ThemeToggle's own onMount still runs afterward
	// wherever it does mount and refines this further (the three-way
	// light/dark/system preference this plain init() doesn't know about) -
	// harmless, since both just set the same themeStore.current value.
	if (typeof window !== 'undefined') {
		themeStore.init();
	}

	let { children } = $props();
</script>

<svelte:head><link rel="icon" href={favicon} /></svelte:head>
{@render children()}
