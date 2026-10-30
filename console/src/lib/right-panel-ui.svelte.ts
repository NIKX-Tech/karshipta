import { isNarrowDesktop } from './is-narrow-desktop.svelte';
import { leftRailUi } from './left-rail-ui.svelte';

const COLLAPSED_STORAGE_KEY = 'karshipta.rightPanelCollapsed';

export type RightPanelTab = 'telemetry' | 'control' | 'mission' | 'zones' | 'configuration';

function readStoredCollapsed(): boolean {
	if (typeof localStorage === 'undefined') return true;
	// Unlike left-rail's equivalent, absence means "default collapsed"
	// (true), not "default expanded" - so an unset key must be
	// distinguished from an explicit 'false', not just compared to 'true'.
	const stored = localStorage.getItem(COLLAPSED_STORAGE_KEY);
	return stored === null ? true : stored === 'true';
}

/**
 * Right panel's collapsed/active-tab state, lifted out of right-panel.svelte
 * into a singleton - same reasoning as left-rail-ui.svelte.ts's own doc
 * comment: components nested inside a tab's own content (e.g. the Control
 * tab's "Plan mission" shortcut) need to switch the panel to a different
 * tab without prop-drilling a callback up through WardControlTab ->
 * ward-tab-ish wrapper -> right-panel.svelte.
 */
class RightPanelUi {
	// Starts collapsed: nothing is selected yet, so there's nothing to show -
	// the selection effect in right-panel.svelte expands it the moment that
	// changes.
	collapsed = $state(readStoredCollapsed());
	activeTab = $state<RightPanelTab>('telemetry');

	// Mobile only: the real rendered height of right-panel.svelte's own
	// fixed ward-detail card (title/collapse button/WardStatusStrip,
	// pinned below the header - see that file's own comment on why it
	// moved there). Set via that card's own bind:clientHeight; read by
	// +page.svelte to push fleet-map.svelte's top overlays (the weather
	// widget, the loading toast) down clear of it - without this they'd
	// render right underneath the card, confirmed as the next thing this
	// move would otherwise break.
	topCardHeight = $state(0);

	// Mobile only: the real rendered height of right-panel.svelte's own
	// bottom sheet (the tab content panel - Telemetry/Control/etc), while
	// expanded. Set via that sheet's own bind:clientHeight; read by
	// +page.svelte, combined with left-rail-ui.svelte.ts's identical
	// field (mobileSurfaceUi.open already guarantees only one of the two
	// is ever relevant at a time) into one shared
	// --mobile-external-sheet-reserve var fleet-map.svelte shrinks its
	// own viewport by, so an open sheet pushes the map (and every
	// overlay positioned against it - the button column, weather,
	// legend) up clear of itself instead of just covering the bottom of
	// it - confirmed live as a real bug otherwise.
	bottomSheetHeight = $state(0);

	setCollapsed(value: boolean): void {
		this.collapsed = value;
		// Mirrors left-rail-ui.svelte.ts's own identical guard - see its
		// comment. Expanding this panel on a narrow desktop viewport
		// collapses the left rail instead of letting both squeeze the map.
		if (!value && isNarrowDesktop.current) leftRailUi.setCollapsed(true);
		if (typeof localStorage === 'undefined') return;
		localStorage.setItem(COLLAPSED_STORAGE_KEY, String(value));
	}

	/** Shared by every tab button: re-clicking the active, expanded tab collapses the panel; clicking anything else switches to it and expands. */
	onTabChange(id: string): void {
		if (id === this.activeTab && !this.collapsed) {
			this.setCollapsed(true);
			return;
		}
		this.activeTab = id as RightPanelTab;
		this.setCollapsed(false);
	}

	/** Called by WardControlTab's "Plan mission" shortcut, right after it starts a single-ward FleetMission assignment draft - jumps the operator straight to where that draft is now being edited. */
	openMissionTab(): void {
		this.activeTab = 'mission';
		this.setCollapsed(false);
	}
}

export const rightPanelUi = new RightPanelUi();
