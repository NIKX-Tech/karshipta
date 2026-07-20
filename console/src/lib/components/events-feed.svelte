<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { Severity } from '$lib/gen/karshipta/v1/common';

	const MAX_VISIBLE = 8;

	const visible = $derived(fleet.events.slice(0, MAX_VISIBLE));

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

{#if visible.length > 0}
	<section
		class="border-edge bg-panel/90 absolute right-4 bottom-8 w-80 rounded border p-3"
		aria-label="Events"
		aria-live="polite"
	>
		<h3 class="text-fg-muted text-[10px] font-medium tracking-widest">EVENTS</h3>
		<ul class="mt-2 space-y-1">
			<!-- keyed by index: identical (ts, code, ward) tuples can occur in one tick -->
			{#each visible as event, index (index)}
				<li class="flex items-baseline gap-2 text-[11px]">
					<span
						class="inline-block h-1.5 w-1.5 shrink-0 self-center rounded-full {dotClass(
							event.severity
						)}"
					></span>
					<span class="text-fg-muted font-mono tabular-nums">{time(event.timestampMs)}</span>
					{#if event.wardId}
						<span class="font-mono">{event.wardId}</span>
					{/if}
					<span class="text-fg-muted truncate">{event.message}</span>
				</li>
			{/each}
		</ul>
	</section>
{/if}
