<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { ParamListStatus } from '$lib/gen/karshipta/v1/parameter';
	import {
		CURATED_FIELDS,
		FIELD_GROUP_LABELS,
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

	const groups: FieldGroup[] = ['rtl', 'battery', 'rcLoss', 'dataLinkLoss'];

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
				<div>
					<h3 class="mb-1 text-[10px] font-medium tracking-widest text-fg-muted uppercase">
						{FIELD_GROUP_LABELS[group]}
					</h3>
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
