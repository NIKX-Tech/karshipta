<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { Severity } from '$lib/gen/karshipta/v1/common';

	function dotClass(severity: Severity): string {
		switch (severity) {
			case Severity.SEVERITY_CRITICAL:
			case Severity.SEVERITY_EMERGENCY:
				return 'bg-critical';
			case Severity.SEVERITY_WARNING:
				return 'bg-accent';
			default:
				return 'bg-fg-muted';
		}
	}

	function time(timestampMs: number): string {
		return new Date(timestampMs).toLocaleTimeString('en-GB', { hour12: false });
	}
</script>

{#if fleet.events.length === 0}
	<p class="text-xs text-fg-muted">No events yet.</p>
{:else}
	<ul class="flex flex-col gap-1.5" aria-live="polite">
		<!-- keyed by index: identical (ts, code, ward) tuples can occur in one tick -->
		{#each fleet.events as event, index (index)}
			<li class="flex items-baseline gap-2 text-[11px]">
				<span
					class="inline-block h-1.5 w-1.5 shrink-0 self-center rounded-full {dotClass(
						event.severity
					)}"
				></span>
				<span class="font-mono text-fg-muted tabular-nums">{time(event.timestampMs)}</span>
				{#if event.wardId}
					<span class="font-mono">{event.wardId}</span>
				{/if}
				<span class="text-fg-muted">{event.message}</span>
			</li>
		{/each}
	</ul>
{/if}
