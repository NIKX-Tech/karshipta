const COLLAPSED_STORAGE_KEY = 'karshipta.rightPanelCollapsed';

export type RightPanelTab = 'telemetry' | 'control' | 'mission' | 'zones';

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

	setCollapsed(value: boolean): void {
		this.collapsed = value;
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
