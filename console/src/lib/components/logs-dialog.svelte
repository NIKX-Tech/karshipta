<script lang="ts">
	import { fleet, isLogDownloadTerminal } from '$lib/fleet-store.svelte';
	import { LogEntryListStatus } from '$lib/gen/karshipta/v1/log_file';
	import Dialog from '$lib/components/ui/dialog.svelte';

	interface Props {
		wardId: string;
		onclose: () => void;
	}

	const { wardId, onclose }: Props = $props();

	const entryList = $derived(fleet.logEntries[wardId]);
	const loading = $derived(fleet.logEntriesLoading[wardId] ?? false);
	const downloads = $derived(fleet.logDownloadsFor(wardId));

	function refresh(): void {
		fleet.requestGetLogEntries(wardId);
	}

	// This component is freshly mounted each time the dialog opens (its
	// caller wraps it in {#if open}, same convention every other dialog in
	// this codebase uses), so an effect with no reactive reads runs exactly
	// once per open - fetch the current list right away instead of making
	// the operator click Refresh every time, since a log list fetched the
	// last time this dialog was open can easily be stale (a new flight may
	// have landed another log since).
	$effect(() => {
		refresh();
	});

	function download(logId: number): void {
		fleet.requestDownloadLogFile(wardId, logId);
	}

	// Most recent download attempt for this entry, if any - downloads is
	// already newest-first (logDownloadsFor's own sort), so the first match
	// is the latest. A log entry can be downloaded more than once in a
	// session (e.g. retrying a rejection), each a distinct tracker.
	function trackerFor(logId: number) {
		return downloads.find((tracker) => tracker.logId === logId);
	}

	function formatSize(bytes: number): string {
		if (bytes < 1024) return `${bytes} B`;
		if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
		return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
	}

	// A 3s "done" checkmark flash, deliberately NOT tied to the tracker's
	// own lifetime: fleet-store keeps a settled tracker around for
	// TRACKER_LINGER_MS (6s, shared with every other tracker kind app-wide -
	// commands, param writes, ward config - so that's not a knob this one
	// button should bend) purely so a rejection message stays readable;
	// the success icon is a much shorter, purely presentational flash,
	// independent of that. Keyed by logId (not requestId) since that's
	// what the row itself is keyed by; handledRequestIds (plain, not
	// reactive - just bookkeeping) stops the same completed download from
	// re-triggering its own flash on every unrelated reactive recompute.
	let flashingLogIds = $state<Record<number, boolean>>({});
	// Plain object, not a Set - this is non-reactive bookkeeping only (never
	// read by the template), and a mutable built-in Set inside a component
	// is what svelte/prefer-svelte-reactivity's lint rule exists to flag.
	const handledRequestIds: Record<string, boolean> = {};
	$effect(() => {
		for (const tracker of downloads) {
			if (tracker.status === 'success' && !handledRequestIds[tracker.requestId]) {
				handledRequestIds[tracker.requestId] = true;
				flashingLogIds[tracker.logId] = true;
				setTimeout(() => {
					delete flashingLogIds[tracker.logId];
				}, 3000);
			}
		}
	});

	type DownloadButtonState = 'idle' | 'busy' | 'done' | 'rejected';
	function buttonState(logId: number): DownloadButtonState {
		if (flashingLogIds[logId]) return 'done';
		const tracker = trackerFor(logId);
		if (!tracker || isLogDownloadTerminal(tracker.status)) {
			return tracker?.status === 'rejected' ? 'rejected' : 'idle';
		}
		return 'busy';
	}

	function downloadTitle(logId: number): string {
		const tracker = trackerFor(logId);
		switch (buttonState(logId)) {
			case 'done':
				return 'Downloaded';
			case 'rejected':
				return 'Retry download';
			case 'busy':
				return tracker?.status === 'downloading' && tracker.totalSizeBytes > 0
					? `Downloading: ${Math.round((tracker.receivedBytes / tracker.totalSizeBytes) * 100)}%`
					: 'Requesting download…';
			default:
				return 'Download';
		}
	}
</script>

<Dialog label="Flight logs - {wardId}" {onclose}>
	<h3 class="font-display text-sm font-semibold">Flight logs</h3>
	<p class="mt-1 font-mono text-[10px] text-fg-muted">{wardId}</p>

	<div class="mt-3 max-h-80 overflow-y-auto">
		{#if entryList?.status === LogEntryListStatus.LOG_ENTRY_LIST_STATUS_REJECTED}
			<p class="text-xs text-critical">{entryList.message}</p>
		{:else if !entryList && !loading}
			<p class="text-xs text-fg-muted">No logs loaded yet.</p>
		{:else if entryList && entryList.entries.length === 0}
			<p class="text-xs text-fg-muted">This ward has no flight logs.</p>
		{:else if entryList}
			<ul class="flex flex-col gap-1">
				{#each entryList.entries as entry (entry.logId)}
					{@const tracker = trackerFor(entry.logId)}
					<li class="flex items-center justify-between gap-2 border-l-2 border-l-edge py-0.5 pl-2">
						<div class="flex min-w-0 flex-col">
							<span class="truncate text-xs text-fg">{entry.date}</span>
							<span class="font-mono text-[10px] text-fg-muted">{formatSize(entry.sizeBytes)}</span>
							{#if tracker?.status === 'rejected'}
								<span class="text-[10px] text-critical">{tracker.message}</span>
							{/if}
						</div>
						<button
							type="button"
							class="icon-button shrink-0"
							disabled={buttonState(entry.logId) === 'busy'}
							aria-label={downloadTitle(entry.logId)}
							title={downloadTitle(entry.logId)}
							onclick={() => download(entry.logId)}
						>
							{#if buttonState(entry.logId) === 'done'}
								<svg
									width="14"
									height="14"
									viewBox="0 0 24 24"
									fill="none"
									stroke="currentColor"
									stroke-width="2"
									stroke-linecap="round"
									stroke-linejoin="round"
									class="text-accent"
									aria-hidden="true"
								>
									<path d="M20 6 9 17l-5-5" />
								</svg>
							{:else if buttonState(entry.logId) === 'busy'}
								<svg
									width="14"
									height="14"
									viewBox="0 0 24 24"
									fill="none"
									stroke="currentColor"
									stroke-width="2"
									stroke-linecap="round"
									class="animate-spin"
									aria-hidden="true"
								>
									<path d="M21 12a9 9 0 1 1-6.219-8.56" />
								</svg>
							{:else if buttonState(entry.logId) === 'rejected'}
								<svg
									width="14"
									height="14"
									viewBox="0 0 24 24"
									fill="none"
									stroke="currentColor"
									stroke-width="2"
									stroke-linecap="round"
									stroke-linejoin="round"
									class="text-critical"
									aria-hidden="true"
								>
									<path d="M21 12a9 9 0 1 1-2.636-6.364" />
									<path d="M21 3v6h-6" />
								</svg>
							{:else}
								<svg
									width="14"
									height="14"
									viewBox="0 0 24 24"
									fill="none"
									stroke="currentColor"
									stroke-width="2"
									stroke-linecap="round"
									stroke-linejoin="round"
									aria-hidden="true"
								>
									<path d="M12 15V3" />
									<path d="M3 15v4a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-4" />
									<path d="m7 10 5 5 5-5" />
								</svg>
							{/if}
						</button>
					</li>
				{/each}
			</ul>
		{/if}
	</div>

	<div class="mt-4 flex justify-end gap-2">
		<button
			type="button"
			disabled={loading}
			onclick={refresh}
			class="rounded border border-edge px-3 py-1.5 text-xs text-fg-muted hover:text-fg disabled:cursor-not-allowed disabled:opacity-50"
		>
			{loading ? 'Loading…' : 'Refresh'}
		</button>
		<button
			data-autofocus
			type="button"
			onclick={onclose}
			class="rounded border border-edge px-3 py-1.5 text-xs text-fg-muted hover:text-fg"
		>
			Close
		</button>
	</div>
</Dialog>

<style>
	.icon-button {
		display: flex;
		height: 1.5rem;
		width: 1.5rem;
		align-items: center;
		justify-content: center;
		border-radius: 0.25rem;
		color: var(--color-fg-muted);
	}
	.icon-button:hover:not(:disabled) {
		background: var(--color-edge);
		color: var(--color-fg);
	}
	.icon-button:disabled {
		cursor: not-allowed;
	}
</style>
