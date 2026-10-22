/**
 * Relative time label ("3s ago", "2m ago") shared by anything that shows a
 * live-ticking age next to a timestamp (events-feed.svelte, ward freshness
 * indicators). A reader scanning a live feed or a ward card wants "how long
 * ago", not a wall-clock time they have to subtract themselves.
 */
export function formatTimeAgo(timestampMs: number, nowMs: number): string {
	const seconds = Math.max(0, Math.floor((nowMs - timestampMs) / 1000));
	if (seconds < 5) return 'just now';
	if (seconds < 60) return `${seconds}s ago`;
	const minutes = Math.floor(seconds / 60);
	if (minutes < 60) return `${minutes}m ago`;
	const hours = Math.floor(minutes / 60);
	if (hours < 24) return `${hours}h ago`;
	return `${Math.floor(hours / 24)}d ago`;
}
