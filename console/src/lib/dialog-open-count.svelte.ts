import { untrack } from 'svelte';

// How many Dialog instances (console-core's ui/dialog.svelte, which
// ConfirmDialog and every other dialog in this codebase is built on) are
// currently mounted. EdgeSheet's own Escape handling checks this before
// acting: without it, pressing Escape while a ConfirmDialog is open (e.g.
// a disarm/land/RTL confirmation opened from inside an expanded sheet)
// would fire BOTH Dialog's own Escape handler and EdgeSheet's - two
// separate <svelte:window onkeydown> listeners, each unaware of the
// other, both reacting to the same keypress. A count, not a boolean: two
// dialogs can legitimately be mounted at once in this codebase (e.g. a
// confirm dialog opened from inside another dialog's own content), and
// the second closing must not make this read "nothing is open" while the
// first is still up.
let count = $state(0);

export const dialogOpenCount = {
	get current() {
		return count;
	}
};

// Called by Dialog itself ($effect, mount/unmount) - not meant for any
// other consumer, so it's exported separately from the read-only getter
// above rather than exposing a public setter on it. Correctness here
// depends on every Dialog instance mounting fresh per open ({#if
// someCondition}<Dialog>{/if}, the pattern every existing call site
// already uses - same fact dialog.svelte's own focus-restore effect
// relies on) rather than toggling some always-mounted instance's own
// visibility: the returned cleanup only runs on unmount, so a Dialog that
// stayed mounted and merely hid itself would increment once and never
// decrement, permanently wedging EdgeSheet's Escape handling closed.
export function registerDialogOpen(): () => void {
	// untrack: `count += 1` both reads and writes count - called directly
	// from Dialog's own $effect, that read would register count as the
	// effect's own dependency, and the effect's write to it would then
	// re-trigger itself - confirmed live as a real infinite loop
	// (Svelte's "effect reads and writes the same piece of state" error).
	untrack(() => {
		count += 1;
	});
	return () => {
		untrack(() => {
			count -= 1;
		});
	};
}
