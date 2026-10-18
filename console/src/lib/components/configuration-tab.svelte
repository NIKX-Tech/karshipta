<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { ParamListStatus } from '$lib/gen/karshipta/v1/parameter';
	import {
		CURATED_FIELDS,
		FIELD_GROUP_LABELS,
		FIELD_GROUP_WARNINGS,
		bindingFor,
		type FieldGroup
	} from '$lib/params/param-spec';
	import ParamFieldRow from '$lib/components/param-field-row.svelte';

	interface Props {
		wardId: string;
		/** See WardControlTab's own Props comment: per-instance override of fleet.readonly. */
		readonly?: boolean;
	}

	const { wardId, readonly }: Props = $props();
	const effectiveReadonly = $derived(readonly ?? fleet.readonly);
	const hasFlight = $derived(fleet.wards[wardId]?.state?.flight !== undefined);
	const autopilot = $derived(fleet.wards[wardId]?.info?.autopilot ?? '');

	const paramList = $derived(fleet.paramLists[wardId]);
	const loading = $derived(fleet.paramListLoading[wardId] ?? false);
	const paramsByName = $derived(
		Object.fromEntries((paramList?.params ?? []).map((param) => [param.name, param]))
	);

	function rawValueOf(name: string): number | undefined {
		const param = paramsByName[name];
		if (!param?.value) return undefined;
		return param.value.$case === 'intValue' ? param.value.intValue : param.value.floatValue;
	}

	const groups: FieldGroup[] = [
		'rtl',
		'battery',
		'rcLoss',
		'dataLinkLoss',
		'pidRoll',
		'pidPitch',
		'pidYaw'
	];

	function refresh() {
		fleet.requestGetParams(wardId);
	}

	// Auto-fetch once when this tab first sees a ward with a usable
	// connection and nothing has been fetched yet - matches the "load on
	// first need, not eagerly for every ward" convention the rest of this
	// panel follows (telemetry/control already stream regardless; this one
	// is a deliberate request instead, since a parameter dump is a real
	// MAVLink round trip, not free).
	let fetchedForWard: string | undefined;
	$effect(() => {
		if (hasFlight && autopilot && fetchedForWard !== wardId && !paramList && !loading) {
			fetchedForWard = wardId;
			refresh();
		}
	});

	// Which group's warning bubble is open, if any - a click/tap toggles
	// it (native title= alone covers hover but is unreliable-to-absent on
	// touch, so the icon needs its own visible bubble too, not just a
	// tooltip attribute). One at a time; closes on an outside click.
	let openWarningGroup = $state<FieldGroup | undefined>(undefined);
	function toggleWarning(group: FieldGroup) {
		openWarningGroup = openWarningGroup === group ? undefined : group;
	}
	$effect(() => {
		if (!openWarningGroup) return;
		const closeOnOutsideClick = (event: MouseEvent) => {
			if (!(event.target instanceof Element) || !event.target.closest('.warning-icon-wrap')) {
				openWarningGroup = undefined;
			}
		};
		window.addEventListener('click', closeOnOutsideClick, { capture: true });
		return () => window.removeEventListener('click', closeOnOutsideClick, { capture: true });
	});
</script>

{#if !hasFlight}
	<p class="text-xs text-fg-muted">This ward has no flight controller - no parameters to read.</p>
{:else if autopilot !== 'PX4' && autopilot !== 'ArduPilot'}
	<p class="text-xs text-fg-muted">
		This ward's autopilot family isn't known yet (reported "{autopilot || 'unknown'}"). Parameter
		names are curated per firmware (PX4/ArduPilot only) - wait for it to connect, or this firmware
		isn't supported here yet.
	</p>
{:else}
	<div class="flex flex-col gap-3">
		<div class="flex items-center justify-between gap-2">
			<span class="font-mono text-[10px] text-fg-muted">{autopilot}</span>
			<button type="button" class="refresh-button" disabled={loading} onclick={refresh}>
				{loading ? 'Loading…' : 'Refresh'}
			</button>
		</div>

		{#if paramList?.status === ParamListStatus.PARAM_LIST_STATUS_REJECTED}
			<p class="text-xs text-critical">{paramList.message}</p>
		{:else if !paramList && !loading}
			<p class="text-xs text-fg-muted">No parameters loaded yet.</p>
		{/if}

		{#each groups as group (group)}
			<!-- Only fields this ward's own firmware actually has - no mention
			     of the other firmware or of fields that don't apply here, see
			     param-spec.ts's own doc comment on why. -->
			{@const fieldsInGroup = CURATED_FIELDS.filter(
				(field) => field.group === group && bindingFor(field, autopilot)
			)}
			{#if fieldsInGroup.length > 0}
				<!-- Every field in a group shares one doc URL per firmware
				     (verified: PX4's battery/RC-loss/data-link-loss groups
				     all point at its one safety-config page; every ArduPilot
				     group has its own dedicated page) - one link here instead
				     of repeating "Docs" on every field row below, confirmed
				     live as genuinely repetitive and boring-looking. -->
				{@const groupDocUrl = bindingFor(fieldsInGroup[0], autopilot)?.docUrl}
				{@const warning = FIELD_GROUP_WARNINGS[group]}
				<div>
					<div class="mb-1 flex items-baseline justify-between gap-2">
						<span class="flex items-center gap-1">
							<h3 class="text-[10px] font-medium tracking-widest text-fg-muted uppercase">
								{FIELD_GROUP_LABELS[group]}
							</h3>
							{#if warning}
								<span class="warning-icon-wrap relative inline-flex">
									<button
										type="button"
										class="flex h-3.5 w-3.5 items-center justify-center text-critical"
										title={warning}
										aria-expanded={openWarningGroup === group}
										aria-label="Warning: {warning}"
										onclick={() => toggleWarning(group)}
									>
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
											<path d="M12 9v4" />
											<path d="M12 17h.01" />
											<path
												d="M10.29 3.86 1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0Z"
											/>
										</svg>
									</button>
									{#if openWarningGroup === group}
										<span
											role="tooltip"
											class="absolute top-full left-0 z-10 mt-1 w-56 rounded border border-edge bg-ink p-2 text-[10px] font-normal text-fg normal-case shadow-lg"
										>
											{warning}
										</span>
									{/if}
								</span>
							{/if}
						</span>
						{#if groupDocUrl}
							<!-- groupDocUrl is always a hardcoded, fully-qualified external https:// URL from param-spec.ts, never an internal route - resolve() is for this app's own route ids. -->
							<!-- eslint-disable svelte/no-navigation-without-resolve -->
							<a
								href={groupDocUrl}
								target="_blank"
								rel="noreferrer"
								class="text-[10px] text-fg-muted underline hover:text-fg"
							>
								Docs
							</a>
							<!-- eslint-enable svelte/no-navigation-without-resolve -->
						{/if}
					</div>
					<div class="rounded border border-edge px-2">
						{#each fieldsInGroup as field (field.key)}
							{@const binding = bindingFor(field, autopilot)}
							{#if binding}
								<ParamFieldRow
									{wardId}
									label={field.label}
									{binding}
									rawValue={rawValueOf(binding.name)}
									readonly={effectiveReadonly}
								/>
							{/if}
						{/each}
					</div>
				</div>
			{/if}
		{/each}
	</div>
{/if}

<style>
	.refresh-button {
		border: 1px solid var(--color-edge);
		border-radius: 0.25rem;
		padding: 0.25rem 0.5rem;
		font-size: 0.625rem;
		font-weight: 500;
	}
	.refresh-button:hover:not(:disabled) {
		border-color: var(--color-fg-muted);
	}
	.refresh-button:disabled {
		cursor: not-allowed;
		opacity: 0.4;
	}
</style>
