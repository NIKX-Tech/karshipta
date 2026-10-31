<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { mobileSurfaceUi } from '$lib/mobile-surface-ui.svelte';
	import AddWardForm from '$lib/components/add-ward-form.svelte';
	import EdgeSheet from '$lib/components/ui/edge-sheet.svelte';
	import SheetHeader from '$lib/components/ui/sheet-header.svelte';

	interface Props {
		onopenconnection: () => void;
		/** starts the click-to-place demo ward placement flow (see routes/+page.svelte) */
		onstartdemoplacement: () => void;
	}

	const { onopenconnection, onstartdemoplacement }: Props = $props();

	// Mobile's own dedicated "Add Ward" sheet (see mobile-surface-ui.svelte.ts's
	// own comment on 'add-ward'/addWardStep) - a separate component from
	// add-ward-menu.svelte, rendered once here in left-rail.svelte's own
	// top-level markup (alongside the drawer EdgeSheet, Theme, Units,
	// About), not nested inside the Fleets drawer's own closable content
	// the way that compact "+ Add Ward" trigger button is. Mounting it
	// there instead was a real, confirmed bug: the instant the trigger
	// opened this sheet (switching mobileSurfaceUi.open away from
	// 'drawer'), the Fleets EdgeSheet's own `{#if open}` unmounted
	// everything inside it, including this sheet itself, in the same
	// reactive update that was supposed to show it - it never had a
	// chance to render at all.
	let mobileFormPending = $state(false);

	function backToMenu() {
		mobileSurfaceUi.addWardStep = 'menu';
	}

	function close() {
		mobileSurfaceUi.close();
	}

	// onopenconnection (leftRailUi.openGatewayTab) only sets which tab is
	// active - it doesn't reopen the drawer itself, so pairing it with
	// close() alone left the operator looking at a bare map with nothing
	// open at all, the Gateway tab's own content never actually shown -
	// confirmed live as a real bug. openDrawer(), not close(), is what
	// actually surfaces it.
	function openSimulated() {
		if (!fleet.gatewayConnected) {
			onopenconnection();
			mobileSurfaceUi.openDrawer();
			return;
		}
		fleet.noteSimulatedWardRequested();
		mobileSurfaceUi.addWardStep = 'simulated';
	}

	function openReal() {
		if (!fleet.gatewayConnected) {
			onopenconnection();
			mobileSurfaceUi.openDrawer();
			return;
		}
		mobileSurfaceUi.addWardStep = 'real';
	}

	function addDemo() {
		close();
		onstartdemoplacement();
	}

	const title = $derived(
		mobileSurfaceUi.addWardStep === 'simulated'
			? 'Add simulated ward'
			: mobileSurfaceUi.addWardStep === 'real'
				? 'Connect real ward'
				: 'Add a ward'
	);
</script>

<!-- requireExplicitClose: same reasoning as every other left-rail sheet
     (Fleets/Gateway/Events, Theme, Units, About) - skimming this with a
     stray outside tap or Escape shouldn't dismiss it, only the header's
     own close button (or, mid-form, Cancel) should. -->
<EdgeSheet
	edge="bottom"
	open={mobileSurfaceUi.open === 'add-ward'}
	label={title}
	onclose={close}
	requireExplicitClose
	bind:panelHeight={mobileSurfaceUi.addWardHeight}
>
	{#snippet content()}
		<div class="flex h-full flex-col">
			<SheetHeader {title} onclose={close} />
			<div class="min-h-0 flex-1 overflow-y-auto p-3">
				{#if mobileSurfaceUi.addWardStep === 'menu'}
					<div class="flex flex-col gap-2">
						<button class="onboarding-button" onclick={addDemo}>
							Add demo ward
							<span class="block text-[10px] font-normal text-fg-muted">
								Instant, no gateway needed
							</span>
						</button>
						<button class="onboarding-button" onclick={openSimulated}>
							Add simulated ward
							<span class="block text-[10px] font-normal text-fg-muted">
								PX4 SITL, through a gateway
							</span>
						</button>
						<button class="onboarding-button" onclick={openReal}>
							Connect real ward
							<span class="block text-[10px] font-normal text-fg-muted">
								MAVLink over a gateway
							</span>
						</button>
						{#if !fleet.gatewayConnected}
							<p class="mt-1 text-[10px] text-fg-muted">
								Simulated and real wards need a running gateway; the Gateway tab in the left rail
								connects one.
							</p>
						{/if}
					</div>
				{:else if mobileSurfaceUi.addWardStep === 'simulated' || mobileSurfaceUi.addWardStep === 'real'}
					<AddWardForm
						mode={mobileSurfaceUi.addWardStep}
						onclose={backToMenu}
						onadded={close}
						bind:pending={mobileFormPending}
						hideHeading
					/>
				{/if}
			</div>
		</div>
	{/snippet}
</EdgeSheet>

<style>
	@reference '../../routes/layout.css';

	.onboarding-button {
		@apply rounded border border-edge px-3 py-2 text-left text-xs font-medium hover:border-accent;
	}
</style>
