<script lang="ts">
	import { fleet } from '$lib/fleet-store.svelte';
	import { Severity } from '$lib/gen/karshipta/v1/common';

	// Severity now reads as a left accent on the whole card, not a leading
	// dot - a bare dot sat awkwardly once each event became a two-line
	// card instead of a single packed row (confirmed live: "the small
	// circle in the beginning is a bit bad in this card form").
	function accentClass(severity: Severity): string {
		switch (severity) {
			case Severity.SEVERITY_CRITICAL:
			case Severity.SEVERITY_EMERGENCY:
				return 'border-l-critical';
			case Severity.SEVERITY_WARNING:
				return 'border-l-accent';
			default:
				return 'border-l-edge';
		}
	}

	// Relative, not absolute HH:MM:SS: a live feed is read at a glance, and
	// "2m ago" says something immediately where a wall-clock time made
	// every reader do their own subtraction - confirmed live as the
	// requested change. `now` ticks every 10s purely to keep these labels
	// from going stale while the panel sits open; past events themselves
	// never change, so nothing else would trigger a re-render of this list.
	let now = $state(Date.now());
	$effect(() => {
		const interval = setInterval(() => (now = Date.now()), 10_000);
		return () => clearInterval(interval);
	});

	function timeAgo(timestampMs: number, nowMs: number): string {
		const seconds = Math.max(0, Math.floor((nowMs - timestampMs) / 1000));
		if (seconds < 5) return 'just now';
		if (seconds < 60) return `${seconds}s ago`;
		const minutes = Math.floor(seconds / 60);
		if (minutes < 60) return `${minutes}m ago`;
		const hours = Math.floor(minutes / 60);
		if (hours < 24) return `${hours}h ago`;
		return `${Math.floor(hours / 24)}d ago`;
	}
</script>

{#if fleet.events.length === 0}
	<p class="text-xs text-fg-muted">No events yet.</p>
{:else}
	<ul class="flex flex-col gap-1.5" aria-live="polite">
		<!-- keyed by index: identical (ts, code, ward) tuples can occur in one tick -->
		{#each fleet.events as event, index (index)}
			<li
				class="flex flex-col gap-1 border-l-2 py-0.5 pl-2 text-[11px] {accentClass(event.severity)}"
			>
				<div class="flex items-baseline justify-between gap-2">
					<span class="min-w-0 truncate text-fg">{event.message}</span>
					<span class="shrink-0 font-mono text-[10px] text-fg-muted tabular-nums">
						{timeAgo(event.timestampMs, now)}
					</span>
				</div>
				{#if event.wardId}
					<!-- Plain descriptive text under the event name, not a
					     chip/badge - a bordered pill read as a separate,
					     clickable-looking label competing with the event
					     itself for attention, confirmed live as the wrong
					     weight for "which ward this happened to". -->
					<span class="font-mono text-[10px] text-fg-muted">{event.wardId}</span>
				{/if}
			</li>
		{/each}
	</ul>
{/if}
