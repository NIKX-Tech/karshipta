<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { ParamAckStatus, ParamType } from '$lib/gen/karshipta/v1/parameter';
	import type { ParamBinding } from '$lib/params/param-spec';

	interface Props {
		wardId: string;
		label: string;
		/** already resolved to this ward's own firmware - see configuration-tab.svelte's bindingFor() call */
		binding: ParamBinding;
		/** the live wire value for binding.name, if the last GetParams response included it */
		rawValue: number | undefined;
		readonly: boolean;
	}

	const { wardId, label, binding, rawValue, readonly }: Props = $props();

	const currentDisplayValue = $derived(
		rawValue !== undefined ? binding.toDisplay(rawValue) : undefined
	);

	// Local edit buffer: starts at the current value every time a fresh
	// GetParams response changes it, but typing doesn't fight that sync -
	// only a genuinely new wire value resets the buffer (see the effect
	// below), the same "don't clobber what the operator is mid-typing"
	// concern RTL altitude's own input in home-panel.svelte already solved.
	// Both start undefined and are set inside the effect, not from
	// currentDisplayValue directly, so reading a $derived value only
	// captures its value at declaration time (Svelte 5's
	// state_referenced_locally warning) never applies here.
	let draftValue = $state<number | undefined>(undefined);
	let lastSyncedFrom: number | undefined;
	$effect(() => {
		if (currentDisplayValue !== lastSyncedFrom) {
			draftValue = currentDisplayValue;
			lastSyncedFrom = currentDisplayValue;
		}
	});

	const latestWrite = $derived(
		fleet.paramWritesFor(wardId).find((tracker) => tracker.paramName === binding.name)
	);
	const saving = $derived(latestWrite?.status === ParamAckStatus.PARAM_ACK_STATUS_UNSPECIFIED);
	const dirty = $derived(draftValue !== undefined && draftValue !== currentDisplayValue);

	function save() {
		if (draftValue === undefined) return;
		const raw = binding.toRaw(draftValue);
		fleet.requestSetParam(wardId, {
			name: binding.name,
			type: binding.paramType,
			value:
				binding.paramType === ParamType.PARAM_TYPE_FLOAT
					? { $case: 'floatValue', floatValue: raw }
					: { $case: 'intValue', intValue: raw }
		});
	}
</script>

<div class="flex flex-col gap-1 border-b border-edge py-2 last:border-b-0">
	<div class="flex items-baseline justify-between gap-2">
		<span class="text-xs font-medium">{label}</span>
		<span class="font-mono text-[10px] text-fg-muted">{binding.name}</span>
	</div>
	<p class="text-[10px] text-fg-muted">{binding.description}</p>
	{#if binding.isAction}
		<p class="text-[9px] text-fg-muted">
			Numeric failsafe action code - see this group's Docs link above for what each value means.
		</p>
	{/if}
	<div class="flex items-center gap-1.5">
		<input
			type="number"
			bind:value={draftValue}
			min={binding.min}
			max={binding.max}
			step={binding.step ?? (binding.isAction ? 1 : 'any')}
			disabled={readonly}
			class="w-24 rounded border border-edge bg-ink px-1.5 py-0.5 text-xs disabled:opacity-50"
		/>
		{#if binding.unit}
			<span class="text-[10px] text-fg-muted">{binding.unit}</span>
		{/if}
		<button
			type="button"
			class="param-save-button"
			disabled={readonly || !dirty || saving}
			onclick={save}
		>
			{saving ? 'Saving…' : 'Save'}
		</button>
	</div>
	{#if latestWrite && latestWrite.status !== ParamAckStatus.PARAM_ACK_STATUS_UNSPECIFIED}
		<p
			class="text-[10px]"
			class:text-critical={latestWrite.status === ParamAckStatus.PARAM_ACK_STATUS_REJECTED}
			class:text-selected={latestWrite.status === ParamAckStatus.PARAM_ACK_STATUS_SUCCESS}
		>
			{latestWrite.status === ParamAckStatus.PARAM_ACK_STATUS_SUCCESS
				? 'Confirmed by the vehicle.'
				: latestWrite.message}
		</p>
	{/if}
</div>

<style>
	.param-save-button {
		border: 1px solid var(--color-edge);
		border-radius: 0.25rem;
		padding: 0.125rem 0.5rem;
		font-size: 0.625rem;
		font-weight: 500;
	}
	.param-save-button:hover:not(:disabled) {
		border-color: var(--color-fg-muted);
	}
	.param-save-button:disabled {
		cursor: not-allowed;
		opacity: 0.4;
	}
</style>
