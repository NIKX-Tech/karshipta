<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import { leftRailUi, type LeftRailTab } from '$lib/left-rail-ui.svelte';
	import { isMobile } from '$lib/is-mobile.svelte';
	import { mobileSurfaceUi } from '$lib/mobile-surface-ui.svelte';
	import AddWardMenu from '$lib/components/add-ward-menu.svelte';
	import WardCard from '$lib/components/ward-card.svelte';
	import FleetRow from '$lib/components/fleet-row.svelte';
	import EventsFeed from '$lib/components/events-feed.svelte';
	import ConnectionPanel from '$lib/components/connection-panel.svelte';
	import Tabs, { type TabItem } from '$lib/components/ui/tabs.svelte';
	import Disclosure from '$lib/components/ui/disclosure.svelte';
	import EdgeSheet from '$lib/components/ui/edge-sheet.svelte';
	import SheetHeader from '$lib/components/ui/sheet-header.svelte';
	import AboutDialog from '$lib/components/about-dialog.svelte';
	import ThemeToggle from '$lib/components/theme-toggle.svelte';
	import UnitsToggle from '$lib/components/units-toggle.svelte';

	interface Props {
		fleetLabel: string;
		onstartdemoplacement: () => void;
	}

	const { fleetLabel, onstartdemoplacement }: Props = $props();

	// Gateway first: it's the prerequisite for anything else here to show
	// real (non-demo) data, not just another content tab - the empty
	// state's "Connect real ward"/"Add simulated ward" CTAs already funnel
	// into it (leftRailUi.openGatewayTab()).
	const TABS: TabItem[] = [
		{ id: 'gateway', label: 'Gateway' },
		{ id: 'fleets', label: 'Fleets' },
		{ id: 'events', label: 'Events' }
	];
	const activeLabel = $derived(TABS.find((tab) => tab.id === leftRailUi.activeTab)?.label ?? '');

	const linkLabel = $derived(fleet.link.toUpperCase());
	// Same tone logic the header used to own; always shown on the Gateway
	// icon itself (not just while its tab is open), so connection status is
	// visible at a glance regardless of which tab is active or whether the
	// rail is collapsed - the thing that was missing before.
	const linkTone = $derived(
		fleet.link === 'down'
			? 'bg-critical'
			: fleet.link === 'connecting'
				? 'bg-fg-muted'
				: 'bg-accent animate-pulse'
	);

	// A small badge on the rail's Events icon for events that arrived while
	// that tab wasn't the visible one - cleared the moment it actually is.
	let lastSeenEventCount = $state(0);
	const hasUnseenEvents = $derived(fleet.events.length > lastSeenEventCount);
	$effect(() => {
		if (leftRailUi.activeTab === 'events' && !leftRailUi.collapsed) {
			lastSeenEventCount = fleet.events.length;
		}
	});

	const UNASSIGNED_GROUP_ID = 'unassigned';

	// Per-group expanded state is local UI state, not persisted - default
	// expanded, matching every other Disclosure in this console. A Ward
	// belonging to more than one Fleet appears under each of them; a Ward
	// in none appears under the Unassigned group, which is hidden entirely
	// once nothing is left in it (rather than showing a permanent empty row).
	let groupExpanded = $state<Record<string, boolean>>({});
	function isGroupExpanded(id: string): boolean {
		return groupExpanded[id] ?? true;
	}

	const unassignedWardIds = $derived(fleetGroups.unassignedWardIds(fleet.wardIds));

	let createFleetOpen = $state(false);
	let newFleetName = $state('');
	let newFleetDescription = $state('');

	function submitCreateFleet(event: SubmitEvent) {
		event.preventDefault();
		const name = newFleetName.trim();
		if (!name) return;
		fleetGroups.requestCreateFleet(name, newFleetDescription.trim());
		newFleetName = '';
		newFleetDescription = '';
		createFleetOpen = false;
	}

	// Desktop keeps leftRailUi.onTabChange's own collapsed/activeTab toggle
	// untouched; mobile routes the same tap through mobileSurfaceUi instead,
	// so opening this drawer closes the bottom sheet - mirrors
	// karshipta-cloud's own left-rail.svelte identically.
	function onRailTabChange(id: string) {
		if (isMobile.current) {
			if (id === leftRailUi.activeTab && mobileSurfaceUi.open === 'drawer') {
				mobileSurfaceUi.close();
				return;
			}
			leftRailUi.activeTab = id as LeftRailTab;
			mobileSurfaceUi.openDrawer();
			return;
		}
		leftRailUi.onTabChange(id);
	}

	function closeDrawer() {
		if (isMobile.current) {
			mobileSurfaceUi.close();
		} else {
			leftRailUi.setCollapsed(true);
		}
	}
</script>

{#if !isMobile.current || leftRailUi.mobileRailVisible}
	<!-- col-start-1/row-start-2: explicit, not implicit DOM-order grid
	     placement - this div is conditionally absent (desktop: never;
	     mobile: until the hamburger trigger in +page.svelte's header
	     shows it, see leftRailUi.mobileRailVisible's own comment), and
	     +page.svelte's grid container relies on each region's own box
	     claiming its correct column regardless of whether this one
	     renders at all - see that file's own comment on the real bug
	     this fixes (the map and RightPanel both shifted one column left
	     without it). -->
	<div class="col-start-1 row-start-2 flex h-full border-r border-edge bg-panel">
		<!-- max-md:relative z-[46]/bg-panel/pt-/pb-: this rail is mobile
		     JS-gated (see the {#if} above), not CSS-breakpoint-gated, but
		     once visible there it's still in-flow content sharing the
		     screen with EdgeSheet's own fixed, z-[45] overlays (its own
		     bottom sheet below, and right-panel.svelte's) - a `position:
		     fixed` element with any z-index paints above plain in-flow
		     content regardless of DOM order, so without z-[46] this rail
		     would go unclickable the moment either sheet expands, same
		     confirmed bug right-panel.svelte's identical rail already
		     needed this fix for. bg-panel for the same reason that file's
		     own comment gives: this div has no background of its own
		     (only its border and icons are opaque), and a transparent
		     strip promoted to its own stacking context lets whatever's
		     behind it - the map, mid-pan - show through. pt-/pb- clear
		     the notch/home-indicator the same way, growing past py-2's
		     own 0.5rem only when the device actually has an inset to
		     clear. -->
		<div
			class="flex w-10 shrink-0 flex-col items-center gap-1 border-r border-edge py-2 max-md:relative max-md:z-[46] max-md:bg-panel max-md:pt-[max(0.5rem,env(safe-area-inset-top,0px))] max-md:pb-[max(0.5rem,env(safe-area-inset-bottom,0px))]"
		>
			<!-- max-md:z-[46]: ThemeToggle's/UnitsToggle's own mobile
			     EdgeSheet (below) renders its z-[45] outside-tap catcher as
			     a DOM sibling of everything else in this icon column, not
			     a true top-level sibling the way left-rail.svelte's own
			     drawer EdgeSheet is (see ThemeToggle/UnitsToggle's own
			     comment on their trigger for the full reasoning) - without
			     this, these tabs went unclickable the same way the About
			     button below did, confirmed live, whenever Theme or Units
			     was open. max-md:order-3: requested directly - on mobile
			     these primary navigation tabs (the ones an operator
			     actually taps often) move to the bottom of the rail,
			     closer to thumb reach, while Units/Theme/About (secondary,
			     rarely-tapped) take the top instead. order has no effect
			     without a value on every reordered sibling (anything left
			     at the default order:0 sorts before/after by DOM position
			     alone, not where you'd expect), hence the matching
			     max-md:order-1/-2 on the spacer and the three toggles
			     below. Desktop is untouched - no order-* class applies
			     above the breakpoint, so DOM order (this group first)
			     keeps rendering exactly as before. -->
			<div class="max-md:relative max-md:z-[46] max-md:order-3">
				<Tabs
					tabs={TABS}
					activeId={leftRailUi.activeTab}
					showSelection={!leftRailUi.collapsed || mobileSurfaceUi.open === 'drawer'}
					onchange={onRailTabChange}
					orientation="vertical"
				>
					{#snippet children(tab)}
						{#if tab.id === 'gateway'}
							<span class="relative flex items-center justify-center">
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
									<circle cx="12" cy="12" r="2.5" />
									<path d="M7.5 16.5a7 7 0 0 1 0-9" />
									<path d="M16.5 7.5a7 7 0 0 1 0 9" />
									<path d="M4.5 19.5a11 11 0 0 1 0-15" />
									<path d="M19.5 4.5a11 11 0 0 1 0 15" />
								</svg>
								<span
									class="absolute -top-0.5 -right-0.5 h-1.5 w-1.5 rounded-full ring-2 ring-panel {linkTone}"
									aria-hidden="true"
								></span>
							</span>
						{:else if tab.id === 'fleets'}
							<svg
								width="16"
								height="16"
								viewBox="0 0 24 24"
								fill="currentColor"
								aria-hidden="true"
							>
								<path d="M12 2 15 9 12 7.3 9 9Z" />
								<path d="M5 12 8 19 5 17.3 2 19Z" />
								<path d="M19 12 22 19 19 17.3 16 19Z" />
							</svg>
						{:else if tab.id === 'events'}
							<span class="relative flex items-center justify-center">
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
									<circle cx="12" cy="12" r="8" />
									<path d="M12 7v5l3.5 2" />
								</svg>
								{#if hasUnseenEvents}
									<span
										class="absolute -top-0.5 -right-0.5 h-1.5 w-1.5 rounded-full bg-accent ring-2 ring-panel"
										aria-hidden="true"
									></span>
								{/if}
							</span>
						{/if}
					{/snippet}
				</Tabs>
			</div>

			<div class="flex-1 max-md:order-2"></div>
			<!-- Individually wrapped, not just reordered as a group: none of
			     these three components accept a class prop (each renders
			     more than one root - a trigger plus a conditionally-shown
			     EdgeSheet/Dialog - so there's no single element to forward
			     one to), and order only has an effect on an actual flex
			     item, not on a component boundary. -->
			<div class="max-md:order-1"><UnitsToggle /></div>
			<div class="max-md:order-1"><ThemeToggle /></div>
			<div class="max-md:order-1"><AboutDialog /></div>
		</div>

		{#if !leftRailUi.collapsed && !isMobile.current}
			<aside id="left-rail-content" class="flex w-80 flex-col" aria-label={activeLabel}>
				{@render drawerContent()}
			</aside>
		{/if}
	</div>
{/if}

{#if isMobile.current}
	<!-- edge="bottom", matching right-panel.svelte's sheet exactly (same
	     non-dimmed, rounded-top, max-h-[80vh] treatment) - not edge="left":
	     there's no map-overlay-covering-the-content rationale here that a
	     full-screen drawer would need to justify, and tapping this rail's
	     own icon (above) jumps straight to the sheet's expanded form, the
	     same way tapping a right-rail icon does, with no peek state built
	     in front of it. Rail clearance (both edges) comes from
	     --mobile-left-rail-inset/--mobile-right-rail-inset, set once in
	     +page.svelte and read directly by EdgeSheet - see that file's own
	     comment on why this isn't a per-call prop any more.
	     requireExplicitClose: requested directly - this (and every
	     other left-rail sheet: Theme, Units, About) should only ever
	     close via its own explicit close button, not an accidental
	     outside tap or Escape while skimming it. -->
	<EdgeSheet
		edge="bottom"
		open={mobileSurfaceUi.open === 'drawer'}
		label={activeLabel || 'Fleets'}
		onclose={closeDrawer}
		requireExplicitClose
		bind:panelHeight={leftRailUi.bottomSheetHeight}
	>
		{#snippet content()}
			{@render drawerContent()}
		{/snippet}
	</EdgeSheet>
{/if}

{#snippet drawerHeaderExtra()}
	{#if leftRailUi.activeTab === 'gateway'}
		<span class="flex items-center gap-1">
			<span class="inline-block h-2 w-2 rounded-full {linkTone}"></span>
			<span class="font-mono text-[10px] text-fg-muted">{linkLabel}</span>
		</span>
	{:else if leftRailUi.activeTab === 'fleets'}
		<span class="font-mono text-[10px] text-fg-muted tabular-nums">
			{fleet.wardIds.length} ward{fleet.wardIds.length === 1 ? '' : 's'}
		</span>
	{/if}
{/snippet}

{#snippet drawerContent()}
	<div class="flex h-full flex-col">
		{#if isMobile.current}
			<!-- Mobile: the same SheetHeader every other bottom sheet in this
			     app uses (fleet-map.svelte's layers/attribution sheets,
			     right-panel.svelte's own ward sheet, ThemeToggle's/
			     UnitsToggle's own) - confirmed live as a real bug otherwise,
			     this one used to hand-roll a smaller h-5 close button and
			     tighter padding than every other sheet, a visible
			     inconsistency between sheets that are otherwise meant to
			     feel like one system. -->
			<SheetHeader
				title={activeLabel.toUpperCase()}
				onclose={closeDrawer}
				extra={drawerHeaderExtra}
			/>
		{:else}
			<!-- Desktop: unchanged - a chevron that collapses this panel
			     back to the icon rail, not a close/X (there's still
			     something to collapse back to, unlike the mobile sheet
			     above, which has nothing left once closed). -->
			<div class="flex items-center justify-between gap-2 border-b border-edge px-3 py-2">
				<div class="flex items-center gap-2">
					<p class="font-mono text-[10px] font-medium tracking-widest text-fg-muted">
						{activeLabel.toUpperCase()}
					</p>
					{@render drawerHeaderExtra()}
				</div>
				<button
					type="button"
					class="flex h-5 w-5 shrink-0 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
					aria-controls="left-rail-content"
					aria-label="Collapse panel"
					title="Collapse panel"
					onclick={closeDrawer}
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
						<path d="m15 6-6 6 6 6" />
					</svg>
				</button>
			</div>
		{/if}

		<div
			role="tabpanel"
			id="tabpanel-gateway"
			aria-labelledby="tab-gateway"
			hidden={leftRailUi.activeTab !== 'gateway'}
			class="min-h-0 flex-1 overflow-y-auto p-3"
		>
			<ConnectionPanel />
		</div>

		<div
			role="tabpanel"
			id="tabpanel-fleets"
			aria-labelledby="tab-fleets"
			hidden={leftRailUi.activeTab !== 'fleets'}
			class="flex min-h-0 flex-1 flex-col gap-2 overflow-y-auto p-3"
		>
			<div class="flex flex-col gap-1.5">
				<div class="flex gap-1.5">
					<AddWardMenu
						variant="compact"
						onopenconnection={() => leftRailUi.openGatewayTab()}
						{onstartdemoplacement}
					/>

					<button
						type="button"
						class="rounded border border-edge px-2 py-1 font-mono text-xs text-fg-muted hover:border-accent hover:text-fg disabled:cursor-not-allowed disabled:opacity-40 disabled:hover:border-edge disabled:hover:text-fg-muted"
						aria-label="Create {fleetLabel}"
						aria-expanded={createFleetOpen}
						disabled={fleet.wardIds.length === 0}
						title={fleet.wardIds.length === 0 ? 'Add a ward first' : undefined}
						onclick={() => (createFleetOpen = !createFleetOpen)}
					>
						+ New {fleetLabel}
					</button>
				</div>
				<!-- Inline, not an absolute-positioned popover: this panel
				     scrolls (overflow-y-auto above), and an absolutely
				     positioned child gets clipped by that ancestor the moment
				     the list is long enough to actually scroll. Same
				     in-flow expand FleetRow's own "Edit name" form already
				     uses just below. -->
				{#if createFleetOpen}
					<form
						class="flex flex-col gap-1.5 rounded border border-edge bg-panel p-2"
						aria-label="New {fleetLabel}"
						onsubmit={submitCreateFleet}
					>
						<!-- svelte-ignore a11y_autofocus -->
						<input
							type="text"
							bind:value={newFleetName}
							required
							autofocus
							placeholder="{fleetLabel} name"
							onkeydown={(event) => {
								if (event.key === 'Escape') createFleetOpen = false;
							}}
							class="w-full rounded border border-edge bg-ink px-1.5 py-1 text-xs"
						/>
						<input
							type="text"
							bind:value={newFleetDescription}
							placeholder="Description (optional)"
							onkeydown={(event) => {
								if (event.key === 'Escape') createFleetOpen = false;
							}}
							class="w-full rounded border border-edge bg-ink px-1.5 py-1 text-xs"
						/>
						<div class="flex gap-1.5">
							<button
								type="submit"
								class="rounded border border-accent/60 bg-accent/15 px-2 py-1 text-xs font-medium text-accent hover:bg-accent/25"
							>
								Create
							</button>
							<button
								type="button"
								onclick={() => (createFleetOpen = false)}
								class="rounded border border-edge px-2 py-1 text-xs text-fg-muted hover:text-fg"
							>
								Cancel
							</button>
						</div>
					</form>
				{/if}
			</div>

			{#each fleetGroups.fleetIds as fleetId (fleetId)}
				<FleetRow {fleetId} {fleetLabel} />
			{/each}

			{#if unassignedWardIds.length > 0}
				<div class="rounded border border-edge px-1.5 py-1">
					<Disclosure
						id={UNASSIGNED_GROUP_ID}
						label="UNASSIGNED"
						expanded={isGroupExpanded(UNASSIGNED_GROUP_ID)}
						onchange={(value) => (groupExpanded[UNASSIGNED_GROUP_ID] = value)}
					>
						{#snippet trailing()}
							<span class="font-mono text-fg-muted">({unassignedWardIds.length})</span>
						{/snippet}
						{#each unassignedWardIds as wardId (wardId)}
							<WardCard {wardId} ward={fleet.wards[wardId]} />
						{/each}
					</Disclosure>
				</div>
			{/if}
		</div>

		<div
			role="tabpanel"
			id="tabpanel-events"
			aria-labelledby="tab-events"
			hidden={leftRailUi.activeTab !== 'events'}
			class="min-h-0 flex-1 overflow-y-auto p-3"
		>
			<EventsFeed />
		</div>
	</div>
{/snippet}
