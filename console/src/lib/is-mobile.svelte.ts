// EdgeSheet itself is breakpoint-agnostic (a pure presentational primitive),
// so each app shell decides when to use it, same as
// left-rail-ui.svelte.ts/right-panel-ui.svelte.ts are per-repo rather than
// shared. 767.98px mirrors Tailwind v4's own `max-md:` variant (`@media
// (width < 48rem)`, i.e. strictly under 768px) rather than inventing a
// second breakpoint value to keep in sync with it - raised from the
// original 639.98/max-sm: a tablet-width viewport (640-768px) still read
// better with the mobile layout (single rail, bottom sheets) than the
// desktop one, confirmed directly rather than guessed. Every max-sm: class
// built against the old breakpoint needs to move to max-md: together with
// this, or the two drift out of sync.
const QUERY = '(max-width: 767.98px)';

function createIsMobile() {
	let matches = $state(typeof window === 'undefined' ? false : window.matchMedia(QUERY).matches);

	if (typeof window !== 'undefined') {
		const mql = window.matchMedia(QUERY);
		mql.addEventListener('change', (event) => {
			matches = event.matches;
		});
	}

	return {
		get current() {
			return matches;
		}
	};
}

// Module-level singleton: every consumer shares one matchMedia listener
// instead of each component wiring and tearing down its own.
export const isMobile = createIsMobile();
