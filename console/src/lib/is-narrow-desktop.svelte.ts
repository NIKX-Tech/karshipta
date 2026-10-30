// Desktop (isMobile.current false, i.e. >= 768px) still runs out of room
// for both left-rail.svelte and right-panel.svelte expanded at once well
// before the two can comfortably share the viewport with the map - 1058px
// is where that was actually confirmed to start pinching the map into an
// unusably thin strip. Below that, same as isMobile.svelte.ts's own
// 767.98 upper bound on the mobile query, exactly one of the two rails
// should ever be expanded at a time (see left-rail-ui.svelte.ts's and
// right-panel-ui.svelte.ts's own setCollapsed - this flag gates the
// mutual-exclusion check there, not here). 767.98 lower bound: anything
// narrower than that is already in the mobile layout (bottom sheets, not
// side rails), which has its own, separate mutual-exclusion mechanism
// (mobile-surface-ui.svelte.ts) - this query must never overlap it.
const QUERY = '(min-width: 768px) and (max-width: 1057.98px)';

function createIsNarrowDesktop() {
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

// Module-level singleton, same reasoning as isMobile.svelte.ts's own.
export const isNarrowDesktop = createIsNarrowDesktop();
