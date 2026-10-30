<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { rightPanelUi, type RightPanelTab } from '$lib/right-panel-ui.svelte';
	import { isMobile } from '$lib/is-mobile.svelte';
	import { mobileSurfaceUi } from '$lib/mobile-surface-ui.svelte';
	import Tabs, { type TabItem } from '$lib/components/ui/tabs.svelte';
	import EdgeSheet from '$lib/components/ui/edge-sheet.svelte';
	import SheetHeader from '$lib/components/ui/sheet-header.svelte';
	import WardStatusStrip from '$lib/components/ward-status-strip.svelte';
	import WardTelemetryTab from '$lib/components/ward-telemetry-tab.svelte';
	import WardControlTab from '$lib/components/ward-control-tab.svelte';
	import MissionTab from '$lib/components/mission-tab.svelte';
	import ZonesTab from '$lib/components/zones-tab.svelte';
	import ConfigurationTab from '$lib/components/configuration-tab.svelte';

	interface Props {
		fleetLabel: string;
	}

	const { fleetLabel }: Props = $props();

	// The icon rail stays reachable regardless of whether a ward is
	// selected, which is the whole point of this restructure. Telemetry and
	// Control used to be one "Ward" tab - split once RTL/Set Home/the
	// Configuration content made that single scrollable tab too tall
	// (confirmed live). WardStatusStrip (below) stays visible above every
	// one of these tabs regardless of which is active, so at-a-glance
	// telemetry is never actually hidden by this split - only the deeper
	// detail is tab-gated. Fleet management itself lives in the left rail
	// (alongside the ward list it groups); Mission is for assigning a
	// mission to a Fleet or an ad-hoc set of wards; Zones is the
	// safety-area drawing tool; Configuration is tier-1 safety-critical
	// MAVLink parameter read/write (karshipta#212) - flight wards only.
	const TABS: TabItem[] = [
		{ id: 'telemetry', label: 'Telemetry' },
		{ id: 'control', label: 'Control' },
		{ id: 'mission', label: 'Missions' },
		{ id: 'zones', label: 'Zones' },
		{ id: 'configuration', label: 'Configuration' }
	];

	// Desktop keeps rightPanelUi.onTabChange's own collapsed/activeTab
	// toggle untouched; mobile routes the same tap through mobileSurfaceUi
	// instead, so opening this sheet closes the drawer - mirrors
	// karshipta-cloud's own right-panel.svelte identically.
	function onRailTabChange(id: string) {
		if (isMobile.current) {
			if (id === rightPanelUi.activeTab && mobileSurfaceUi.open === 'sheet') {
				mobileSurfaceUi.close();
				return;
			}
			rightPanelUi.activeTab = id as RightPanelTab;
			mobileSurfaceUi.openSheet();
			return;
		}
		rightPanelUi.onTabChange(id);
	}

	// Auto-switch to the Telemetry tab and auto-expand the moment selection
	// goes from "none" to "some": an operator who just selected a ward
	// wants to see it, not stay wherever the panel happened to be left.
	// Tracked via a plain (non-$state) variable read/written only inside
	// this effect, so it doesn't itself trigger reruns - only
	// fleet.selectedWardId does.
	let previouslySelected: string | undefined;
	$effect(() => {
		const selected = fleet.selectedWardId;
		if (selected !== undefined && previouslySelected === undefined) {
			rightPanelUi.activeTab = 'telemetry';
			// Desktop auto-expands the inline panel; mobile only shows the
			// top ward-detail card (see panelHeader below) - selecting a
			// ward shouldn't also shove the full tab content sheet over the
			// map unasked, the way tapping a rail icon explicitly does via
			// onRailTabChange above.
			if (isMobile.current) {
				mobileSurfaceUi.close();
			} else {
				rightPanelUi.setCollapsed(false);
			}
		}
		previouslySelected = selected;
	});

	// Control/Mission/Zones place a waypoint or zone vertex by tapping the
	// map itself - the sheet's outside-tap-to-collapse must never compete
	// with that tap for the same gesture.
	const sheetSuppressesOutsideDismiss = $derived(
		rightPanelUi.activeTab === 'control' ||
			rightPanelUi.activeTab === 'mission' ||
			rightPanelUi.activeTab === 'zones'
	);

	const activeLabel = $derived(TABS.find((tab) => tab.id === rightPanelUi.activeTab)?.label ?? '');

	// Shared by the desktop inline panel and the mobile EdgeSheet - Escape/
	// outside-tap calls this the same as the header's own collapse button.
	function closePanel() {
		if (isMobile.current) {
			mobileSurfaceUi.close();
		} else {
			rightPanelUi.setCollapsed(true);
		}
	}
</script>

{#snippet wardStatus()}
	{#if fleet.selectedWardId}
		<WardStatusStrip wardId={fleet.selectedWardId} />
	{/if}
{/snippet}

<!-- Desktop only now - mobile's own header moved into the bottom sheet
     below (see that EdgeSheet's own content snippet), not the floating
     top card: requested directly - the title/close row should look like
     every other bottom sheet's own SheetHeader and actually live in the
     sheet, while the top card keeps just the glanceable WardStatusStrip
     summary. A chevron that collapses this panel back to the icon rail,
     not a close/X - desktop has somewhere to collapse back to. -->
{#snippet desktopHeader()}
	<div class="flex items-center justify-between gap-2 border-b border-edge px-3 py-2">
		<p class="font-mono text-[10px] font-medium tracking-widest text-fg-muted">
			{activeLabel.toUpperCase()}
		</p>
		<button
			type="button"
			class="flex h-5 w-5 shrink-0 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
			aria-controls="right-panel-content"
			aria-label="Collapse panel"
			title="Collapse panel"
			onclick={closePanel}
		>
			<svg
				width="14"
				height="14"
				viewBox="0 0 24 24"
				fill="none"
				stroke="currentColor"
				stroke-width="1.75"
				stroke-linecap="round"
				stroke-linejoin="round"
				aria-hidden="true"
			>
				<path d="m9 6 6 6-6 6" />
			</svg>
		</button>
	</div>
{/snippet}

{#snippet tabContent()}
	<div
		role="tabpanel"
		id="tabpanel-telemetry"
		aria-labelledby="tab-telemetry"
		class="min-h-0 flex-1 overflow-y-auto p-3"
		hidden={rightPanelUi.activeTab !== 'telemetry'}
	>
		{#if fleet.selectedWardId}
			<WardTelemetryTab wardId={fleet.selectedWardId} />
		{:else}
			<p class="text-xs text-fg-muted">Select a ward to see its details.</p>
		{/if}
	</div>

	<div
		role="tabpanel"
		id="tabpanel-control"
		aria-labelledby="tab-control"
		class="min-h-0 flex-1 overflow-y-auto p-3"
		hidden={rightPanelUi.activeTab !== 'control'}
	>
		{#if fleet.selectedWardId}
			<WardControlTab wardId={fleet.selectedWardId} />
		{:else}
			<p class="text-xs text-fg-muted">Select a ward to command it.</p>
		{/if}
	</div>

	<div
		role="tabpanel"
		id="tabpanel-mission"
		aria-labelledby="tab-mission"
		class="min-h-0 flex-1 overflow-y-auto p-3"
		hidden={rightPanelUi.activeTab !== 'mission'}
	>
		<MissionTab {fleetLabel} />
	</div>

	<div
		role="tabpanel"
		id="tabpanel-zones"
		aria-labelledby="tab-zones"
		class="min-h-0 flex-1 overflow-y-auto p-3"
		hidden={rightPanelUi.activeTab !== 'zones'}
	>
		<ZonesTab />
	</div>

	<div
		role="tabpanel"
		id="tabpanel-configuration"
		aria-labelledby="tab-configuration"
		class="min-h-0 flex-1 overflow-y-auto p-3"
		hidden={rightPanelUi.activeTab !== 'configuration'}
	>
		{#if fleet.selectedWardId}
			<ConfigurationTab wardId={fleet.selectedWardId} />
		{:else}
			<p class="text-xs text-fg-muted">Select a ward to see its configuration.</p>
		{/if}
	</div>
{/snippet}

<!-- col-start-3/row-start-2: explicit, not implicit DOM-order grid
     placement - +page.svelte's grid container relies on each region's
     own box claiming its correct column regardless of what any sibling
     renders (see its own comment: LeftRail going absent on mobile once
     shifted this div and the map both one column left, a real,
     confirmed bug). This div itself is never conditionally absent, but
     it still needs an explicit column so it doesn't inherit whatever
     slot auto-placement happens to leave for it. -->
<div class="col-start-3 row-start-2 flex h-full bg-panel">
	{#if !rightPanelUi.collapsed && !isMobile.current}
		<div id="right-panel-content" class="flex w-72 flex-col border-l border-edge">
			{@render desktopHeader()}
			{@render wardStatus()}
			{@render tabContent()}
		</div>
	{/if}

	<!-- !isMobile.current || fleet.selectedWardId !== undefined: on mobile,
	     this rail's every tab needs a selected ward to mean anything
	     (Telemetry/Control/Mission/Zones/Configuration are all per-ward),
	     so it stays out of the way entirely until one exists - desktop is
	     unaffected (always shown there, same as before). Always last,
	     regardless of collapsed state, so this rail stays pinned to the
	     outer (right) edge of the screen when it IS shown; the expandable
	     content panel above grows/shrinks on its inboard side instead of
	     ever displacing the rail itself. max-md:relative z-[46]/bg-panel/
	     pt-/pb-: identical reasoning to karshipta-cloud's own
	     right-panel.svelte (same rail, same EdgeSheet primitive) - see
	     that file's own comment for the three confirmed live bugs this
	     guards against. This file's own EdgeSheet below clears both rails
	     via --mobile-left-rail-inset/--mobile-right-rail-inset, the same
	     vars the top card above reads - see +page.svelte's own comment on
	     why neither is a per-call prop any more. -->
	{#if !isMobile.current || fleet.selectedWardId !== undefined}
		<div
			class="flex w-10 shrink-0 flex-col items-center gap-1 border-l border-edge py-2 max-md:relative max-md:z-[46] max-md:bg-panel max-md:pt-[max(0.5rem,env(safe-area-inset-top,0px))] max-md:pb-[max(0.5rem,env(safe-area-inset-bottom,0px))]"
		>
			<!-- Mobile: collapsed is desktop-only state, persisted in
		     localStorage from whichever viewport the operator last used it
		     in - confirmed live as a real bug when this read
		     `!rightPanelUi.collapsed || mobileSurfaceUi.open === 'sheet'`
		     (an OR, not an actual branch): a stale collapsed=false saved
		     from a desktop session made the Telemetry tab highlight as
		     selected the instant a ward was picked on mobile, even though
		     the sheet itself correctly never opened (the selection effect
		     above deliberately calls mobileSurfaceUi.close(), not
		     setCollapsed(false), on mobile - see its own comment: picking a
		     ward shows the top card and this rail, not a tab's full sheet
		     unasked). A real ternary branch instead of an OR is what
		     actually fixes it - mobile's own highlight/mount state must
		     come only from mobileSurfaceUi.open, never from collapsed at
		     all. unlike left-rail.svelte's drawer, this sheet has a peek
		     state with no tabpanel content mounted at all (EdgeSheet only
		     renders its content snippet once expanded), so "mounted
		     whenever on mobile" would point aria-controls at an id that
		     isn't actually in the DOM during peek. max-md:order-2 on the
		     wrapper below, max-md:order-1 on the spacer that follows it:
		     requested directly, mirroring left-rail.svelte's identical
		     swap - these are the tabs an operator actually taps often, so
		     on mobile they move to the bottom of the rail, closer to
		     thumb reach, instead of the top. order only has an effect
		     when every reordered sibling gets an explicit value (left at
		     the default order:0, a sibling sorts by DOM position alone,
		     not where you'd expect) - here there's only the one other
		     sibling (the spacer) to match. Desktop is untouched: no
		     order-* class applies above the breakpoint, so DOM order
		     (tabs first) keeps rendering exactly as before. -->
			<div class="max-md:order-2">
				<Tabs
					tabs={TABS}
					activeId={rightPanelUi.activeTab}
					showSelection={isMobile.current
						? mobileSurfaceUi.open === 'sheet'
						: !rightPanelUi.collapsed}
					panelsMounted={isMobile.current
						? mobileSurfaceUi.open === 'sheet'
						: !rightPanelUi.collapsed}
					onchange={onRailTabChange}
					orientation="vertical"
				>
					{#snippet children(tab)}
						{#if tab.id === 'telemetry'}
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
								<circle cx="12" cy="12" r="3" />
								<line x1="12" y1="2" x2="12" y2="6" />
								<line x1="12" y1="18" x2="12" y2="22" />
								<line x1="2" y1="12" x2="6" y2="12" />
								<line x1="18" y1="12" x2="22" y2="12" />
							</svg>
						{:else if tab.id === 'control'}
							<!-- A retro arcade-style joystick: wide flat base, tapered boot,
					     prominent ball grip - reads as "fly this thing right now,"
					     distinct from Configuration's sliders (stored settings) below. -->
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
								<rect x="4" y="17" width="16" height="3.5" rx="1" />
								<path d="M9 17c0-2.8 1-4.3 3-4.3s3 1.5 3 4.3" />
								<line x1="12" y1="12.7" x2="12" y2="7" />
								<circle cx="12" cy="4.5" r="2.5" fill="currentColor" stroke="none" />
							</svg>
						{:else if tab.id === 'mission'}
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
								<path d="M4 19 9 8 14 14 20 4" stroke-dasharray="1.2 3" />
								<circle cx="4" cy="19" r="1.5" fill="currentColor" stroke="none" />
								<circle cx="9" cy="8" r="1.5" fill="currentColor" stroke="none" />
								<circle cx="14" cy="14" r="1.5" fill="currentColor" stroke="none" />
								<circle cx="20" cy="4" r="1.5" fill="currentColor" stroke="none" />
							</svg>
						{:else if tab.id === 'zones'}
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
								<path d="M12 2 21 7.5v9L12 22l-9-5.5v-9Z" />
							</svg>
						{:else if tab.id === 'configuration'}
							<!-- Sliders - a gear read as "whole-app settings" in testing,
					     which this isn't: it's per-ward stored parameter values. -->
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
								<line x1="3" y1="6" x2="21" y2="6" />
								<circle cx="8" cy="6" r="2" fill="currentColor" stroke="none" />
								<line x1="3" y1="12" x2="21" y2="12" />
								<circle cx="16" cy="12" r="2" fill="currentColor" stroke="none" />
								<line x1="3" y1="18" x2="21" y2="18" />
								<circle cx="11" cy="18" r="2" fill="currentColor" stroke="none" />
							</svg>
						{/if}
					{/snippet}
				</Tabs>
			</div>

			<div class="flex-1 max-md:order-1"></div>
		</div>
	{/if}
</div>

{#if isMobile.current && fleet.selectedWardId !== undefined}
	<!-- The ward-detail card: just WardStatusStrip, pinned just below the
	     app header - requested directly to stay up here even after the
	     title/close row (below) moved into the bottom sheet, matching
	     every other sheet's own SheetHeader instead of floating up here
	     on its own. Driven directly off selectedWardId,
	     independent of mobileSurfaceUi: it should show the moment a ward
	     is selected, same as before, while only an explicit rail-tap
	     (onRailTabChange) moves the tab-content sheet below to/from its
	     own open form - see this file's own selection effect and
	     onRailTabChange. top: var(--mobile-chrome-top): sits just under
	     the real header (+page.svelte measures its own rendered height
	     for this, since that height itself varies with
	     env(safe-area-inset-top) on a notched phone). left/right clear
	     each rail the same --mobile-left-rail-inset/--mobile-right-rail-inset
	     vars the bottom sheet below (and every other mobile sheet in this
	     app) reads - one source of truth for "how much rail is actually
	     showing right now" instead of this card computing its own copy,
	     see +page.svelte's own comment on why. z-[46], not z-[45]: this
	     card and the bottom sheet below are siblings in DOM order, the
	     sheet rendered after - at equal z-index that would paint the
	     sheet's own expanded-state outside-tap catcher (also z-[45], a
	     full-viewport fixed div) over this card, making its own collapse
	     button unclickable the moment the sheet opens. Same reasoning as
	     both icon rails already needing z-[46] over these overlays. -->
	<!-- border-t: this card sits flush against the header above it (same
	     bg-panel, card top = header bottom exactly), so the header's own
	     border-b alone wasn't a reliable seam between them - confirmed
	     live as a real bug, the two looked like one continuous panel with
	     no divider at all once a ward was selected. This card's own
	     border-t guarantees the line regardless of what's rendered above
	     it. -->
	<div
		bind:clientHeight={rightPanelUi.topCardHeight}
		class="fixed z-[46] flex flex-col border-t border-b border-edge bg-panel"
		style="top: var(--mobile-chrome-top, 0px); left: var(--mobile-left-rail-inset, 0px); right: var(--mobile-right-rail-inset, 0px);"
	>
		{@render wardStatus()}
	</div>
{/if}

{#if isMobile.current}
	<!-- The title/close row lives here now, not the floating top card
	     above (which keeps just WardStatusStrip) - the same SheetHeader
	     every other bottom sheet in this app uses, requested directly so
	     this sheet stops being the one that looked different. -->
	<EdgeSheet
		edge="bottom"
		open={mobileSurfaceUi.open === 'sheet'}
		label={activeLabel || 'Ward detail'}
		onclose={closePanel}
		suppressOutsideDismiss={sheetSuppressesOutsideDismiss}
		bind:panelHeight={rightPanelUi.bottomSheetHeight}
	>
		{#snippet content()}
			<div class="flex h-full flex-col">
				<SheetHeader title={activeLabel.toUpperCase()} onclose={closePanel} />
				{@render tabContent()}
			</div>
		{/snippet}
	</EdgeSheet>
{/if}
