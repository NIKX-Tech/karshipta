import { isNarrowDesktop } from './is-narrow-desktop.svelte';
import { rightPanelUi } from './right-panel-ui.svelte';

const COLLAPSED_STORAGE_KEY = 'karshipta.leftRailCollapsed';
const MOBILE_RAIL_VISIBLE_STORAGE_KEY = 'karshipta.leftRailMobileVisible';

export type LeftRailTab = 'gateway' | 'fleets' | 'events';

function readStoredCollapsed(): boolean {
	if (typeof localStorage === 'undefined') return false;
	return localStorage.getItem(COLLAPSED_STORAGE_KEY) === 'true';
}

function readStoredMobileRailVisible(): boolean {
	if (typeof localStorage === 'undefined') return false;
	return localStorage.getItem(MOBILE_RAIL_VISIBLE_STORAGE_KEY) === 'true';
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

	// Mobile only: whether the rail's own icon strip is currently shown at
	// all. Permanently showing both this rail and right-panel.svelte's own
	// (40px each) left the map and the bottom sheets feeling cramped on a
	// phone - confirmed as real, direct feedback, not a guess - so this
	// rail defaults hidden there and the hamburger trigger in +page.svelte's
	// header toggles it. Once shown, it behaves exactly like the right
	// rail (always-visible icons, tap one to open/close the bottom sheet)
	// - this flag only ever controls whether it exists at all, never how
	// it behaves once it does. Persisted (requested directly, for a
	// smoother feel across a refresh): an operator who deliberately opened
	// this rail shouldn't have it silently vanish again on reload, same
	// reasoning collapsed (below) already gets.
	mobileRailVisible = $state(readStoredMobileRailVisible());

	toggleMobileRail(): void {
		this.mobileRailVisible = !this.mobileRailVisible;
		if (typeof localStorage === 'undefined') return;
		localStorage.setItem(MOBILE_RAIL_VISIBLE_STORAGE_KEY, String(this.mobileRailVisible));
	}

	// Mobile only: the real rendered height of left-rail.svelte's own
	// bottom sheet (Gateway/Fleets/Events content), while open. Set via
	// that sheet's own bind:clientHeight; see right-panel-ui.svelte.ts's
	// identical bottomSheetHeight field for why (+page.svelte combines
	// both into one shared var fleet-map.svelte reads to push its own
	// viewport up, via mobileSurfaceUi.open to know which one's actually
	// relevant).
	bottomSheetHeight = $state(0);

	setCollapsed(value: boolean): void {
		this.collapsed = value;
		// Narrow desktop (768-1057.98px, see is-narrow-desktop.svelte.ts's
		// own comment): expanding this rail there collapses the right
		// panel, the same "only one open at a time" rule mobile already
		// enforces via mobile-surface-ui.svelte.ts, just triggered here
		// instead since side rails (not bottom sheets) are still the
		// layout at this width. Only acts on expand, never on collapse -
		// collapsing this rail has no business touching the other one.
		if (!value && isNarrowDesktop.current) rightPanelUi.setCollapsed(true);
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
