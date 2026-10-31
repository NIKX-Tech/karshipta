export type MobileSurface = 'drawer' | 'sheet' | 'theme' | 'units' | 'about' | null;

/**
 * Tracks which mobile-only overlay is open below the sm breakpoint, so
 * opening one closes the rest. Mirrors left-rail-ui.svelte.ts/
 * right-panel-ui.svelte.ts's singleton shape, but unlike those two, IS
 * exported from this package's barrel (see index.ts's own comment) - this
 * one instance is hard-wired inside ThemeToggle/UnitsToggle/AboutDialog
 * themselves (ready-made, exported components, not raw EdgeSheet
 * mechanics a consuming app composes itself), so a consuming app building
 * its own mobile shell around its own ward/drawer sheets has no way to
 * coordinate with theirs except by sharing this exact instance, not a
 * separate copy of the pattern. Confirmed live as a real bug in
 * karshipta-cloud before this was exported: its own left-rail.svelte/
 * right-panel.svelte sheets were coordinated through ITS OWN separate
 * mobile-surface-ui.svelte.ts, entirely unaware of this package's
 * internal one - tapping Theme or Units there opened console-core's own
 * sheet directly on top of whichever ward/drawer sheet the app's own
 * singleton still thought was open, since the two never touched each
 * other. Purely ephemeral (no localStorage): "what's open right now" has
 * no reason to survive a reload the way a collapsed preference does.
 *
 * 'theme'/'units'/'about' (ThemeToggle's/UnitsToggle's/AboutDialog's own
 * mobile bottom sheets): same treatment as 'drawer'/'sheet' - they close
 * each other and the drawer (same left-rail icon column, directly
 * adjacent triggers, a real collision confirmed live once drawer/sheet
 * became bottom sheets too), AND push the map viewport up clear of
 * themselves the same way, via their own themeHeight/unitsHeight/
 * aboutHeight below (+page.svelte's own mobileExternalSheetReserve reads
 * whichever one is actually open). Confirmed live as a real bug when
 * About was first added as a mobile sheet without this wiring: it opened
 * behind/alongside whichever other sheet was already up instead of
 * replacing it, and never pushed the map out of its own way the way
 * every other sheet already did.
 */
class MobileSurfaceUi {
	open = $state<MobileSurface>(null);

	// Real rendered height of ThemeToggle's/UnitsToggle's/AboutDialog's own
	// mobile EdgeSheet while open - set via its bind:panelHeight, same
	// shape as left-rail-ui.svelte.ts's/right-panel-ui.svelte.ts's own
	// bottomSheetHeight fields. Living here rather than on each component
	// directly: +page.svelte needs to read whichever one is relevant
	// without caring which component owns it, and this singleton already
	// is where "which surface, how tall" is tracked for the other two.
	themeHeight = $state(0);
	unitsHeight = $state(0);
	aboutHeight = $state(0);

	openDrawer(): void {
		this.open = 'drawer';
	}

	openSheet(): void {
		this.open = 'sheet';
	}

	openTheme(): void {
		this.open = 'theme';
	}

	openUnits(): void {
		this.open = 'units';
	}

	openAbout(): void {
		this.open = 'about';
	}

	close(): void {
		this.open = null;
	}
}

export const mobileSurfaceUi = new MobileSurfaceUi();
