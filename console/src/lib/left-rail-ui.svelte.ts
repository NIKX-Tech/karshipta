const COLLAPSED_STORAGE_KEY = 'karshipta.leftRailCollapsed';

export type LeftRailTab = 'gateway' | 'fleets' | 'events';

function readStoredCollapsed(): boolean {
	if (typeof localStorage === 'undefined') return false;
	return localStorage.getItem(COLLAPSED_STORAGE_KEY) === 'true';
}

/**
 * Left rail's collapsed/active-tab state, lifted out of left-rail.svelte
 * into a singleton (matching fleet/fleetGroups/zoneStore's existing
 * pattern) so components outside the rail - EmptyState, AddWardMenu's
 * "Connect real ward" CTA - can open the Gateway tab directly, the same
 * way clicking the rail's own Gateway icon would, without prop-drilling a
 * callback across sibling components.
 */
class LeftRailUi {
	// Starts expanded, unlike the right panel: the fleet list is core,
	// always-relevant content, not something that only becomes relevant
	// after a later action the way ward detail does.
	collapsed = $state(readStoredCollapsed());
	activeTab = $state<LeftRailTab>('fleets');

	setCollapsed(value: boolean): void {
		this.collapsed = value;
		if (typeof localStorage === 'undefined') return;
		localStorage.setItem(COLLAPSED_STORAGE_KEY, String(value));
	}

	/** Shared by every rail tab button: re-clicking the active, expanded tab collapses the rail; clicking anything else switches to it and expands. */
	onTabChange(id: string): void {
		if (id === this.activeTab && !this.collapsed) {
			this.setCollapsed(true);
			return;
		}
		this.activeTab = id as LeftRailTab;
		this.setCollapsed(false);
	}

	/** Called by EmptyState/AddWardMenu's connect CTAs in place of the old floating ConnectionPanel toggle. */
	openGatewayTab(): void {
		this.activeTab = 'gateway';
		this.setCollapsed(false);
	}
}

export const leftRailUi = new LeftRailUi();
