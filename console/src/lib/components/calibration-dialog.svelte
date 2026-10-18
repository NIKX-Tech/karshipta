<script lang="ts">
	import { fleet, type CalibrationRunState } from '$lib/fleet-store.svelte';
	import { CalibrationType } from '$lib/gen/karshipta/v1/calibration';
	import Dialog from '$lib/components/ui/dialog.svelte';

	interface Props {
		wardId: string;
		/** Picks which firmware's guide text/doc link shows once a sensor
		 * is selected - the same "know your client" rule param-spec.ts's
		 * own bindingFor() already follows for parameters. Only 'PX4'/
		 * 'ArduPilot' have real guides; anything else (not yet connected,
		 * unrecognized) shows no guide, same as configuration-tab.svelte's
		 * own unknown-autopilot handling. */
		autopilot: string;
		onclose: () => void;
	}

	const { wardId, autopilot, onclose }: Props = $props();

	// Live, current-only health per sensor (karshipta#248's picker
	// badges) - mirrors mavsdk::Telemetry::Health directly, via
	// WardState.flight. This is NOT a calibration history: neither
	// MAVSDK nor the underlying MAVLink protocol records a timestamp of
	// when a sensor was last calibrated, only whether it currently
	// passes - verified against mavsdk::Telemetry::Health's own fields
	// before building this, not assumed. undefined (not false) while
	// telemetry hasn't arrived yet, so "unknown" never renders as a false
	// "not calibrated". No entry for level horizon: MAVSDK's own Health
	// struct has no equivalent for it either.
	const flight = $derived(fleet.wards[wardId]?.state?.flight);
	const CALIBRATION_HEALTH: Partial<Record<CalibrationType, boolean | undefined>> = $derived({
		[CalibrationType.CALIBRATION_TYPE_GYRO]: flight?.gyrometerCalibrationOk,
		[CalibrationType.CALIBRATION_TYPE_ACCELEROMETER]: flight?.accelerometerCalibrationOk,
		[CalibrationType.CALIBRATION_TYPE_MAGNETOMETER]: flight?.magnetometerCalibrationOk
	});

	// gimbal_accelerometer is deliberately left out - no ward class with a
	// gimbal exists yet (gateway/docs/calibration-executor.md's own doc
	// comment).
	const CALIBRATION_TYPES = [
		CalibrationType.CALIBRATION_TYPE_GYRO,
		CalibrationType.CALIBRATION_TYPE_ACCELEROMETER,
		CalibrationType.CALIBRATION_TYPE_MAGNETOMETER,
		CalibrationType.CALIBRATION_TYPE_LEVEL_HORIZON
	];

	const CALIBRATION_TYPE_LABELS: Partial<Record<CalibrationType, string>> = {
		[CalibrationType.CALIBRATION_TYPE_GYRO]: 'Gyroscope',
		[CalibrationType.CALIBRATION_TYPE_ACCELEROMETER]: 'Accelerometer',
		[CalibrationType.CALIBRATION_TYPE_MAGNETOMETER]: 'Magnetometer (compass)',
		[CalibrationType.CALIBRATION_TYPE_LEVEL_HORIZON]: 'Level horizon'
	};

	interface CalibrationGuide {
		description: string;
		/** Real, individually verified doc URLs (fetched and confirmed to
		 * exist, not guessed) - same bar param-spec.ts's own docUrl fields
		 * are held to. PX4 has no standalone page per sensor (confirmed:
		 * only Level Horizon gets its own URL; gyro/accel/mag are each one
		 * section of the same Standard Configuration page), so those three
		 * share one link. ArduPilot's own "Calibrate Level" step is
		 * documented as part of its accelerometer page, not a separate
		 * one, so Level Horizon and Accelerometer share a link there too. */
		docUrl?: string;
	}

	const CALIBRATION_GUIDES: Record<string, Partial<Record<CalibrationType, CalibrationGuide>>> = {
		PX4: {
			[CalibrationType.CALIBRATION_TYPE_GYRO]: {
				description: 'Place the vehicle on a flat, level surface and keep it completely still.',
				docUrl: 'https://docs.px4.io/main/en/config/index'
			},
			[CalibrationType.CALIBRATION_TYPE_ACCELEROMETER]: {
				description:
					'Hold the vehicle in each of six orientations in turn (level, on each side, nose up, nose down) and hold still for each.',
				docUrl: 'https://docs.px4.io/main/en/config/index'
			},
			[CalibrationType.CALIBRATION_TYPE_MAGNETOMETER]: {
				description:
					'Rotate the vehicle slowly through all orientations, away from metal and magnetic interference, until every axis is covered.',
				docUrl: 'https://docs.px4.io/main/en/config/index'
			},
			[CalibrationType.CALIBRATION_TYPE_LEVEL_HORIZON]: {
				description:
					'Place the vehicle in its normal hover attitude on a level surface, then confirm. Corrects small frame/mount misalignment only - not a substitute for a full accelerometer calibration.',
				docUrl: 'https://docs.px4.io/main/en/config/level_horizon_calibration'
			}
		},
		ArduPilot: {
			[CalibrationType.CALIBRATION_TYPE_GYRO]: {
				description:
					'ArduPilot calibrates the gyroscope automatically on every boot, with the vehicle held still - this manual trigger repeats that same check on demand.'
				// No docUrl: this is automatic, not a documented manual
				// procedure - verified no such page exists before omitting
				// it, not assumed.
			},
			[CalibrationType.CALIBRATION_TYPE_ACCELEROMETER]: {
				description:
					'Hold the vehicle in each of six orientations in turn (level, on each side, nose up, nose down) and hold still for each. Mandatory before first flight.',
				docUrl: 'https://ardupilot.org/copter/docs/common-accelerometer-calibration.html'
			},
			[CalibrationType.CALIBRATION_TYPE_MAGNETOMETER]: {
				description:
					'Rotate the vehicle slowly through all orientations, away from metal and magnetic interference, until the calibration completes.',
				docUrl:
					'https://ardupilot.org/copter/docs/common-compass-calibration-in-mission-planner.html'
			},
			[CalibrationType.CALIBRATION_TYPE_LEVEL_HORIZON]: {
				description:
					'Place the vehicle in its normal hover attitude on a level surface, then confirm. ArduPilot\'s own "Calibrate Level" step - corrects up to 10 degrees of frame misalignment, not a substitute for the full accelerometer calibration.',
				docUrl: 'https://ardupilot.org/copter/docs/common-accelerometer-calibration.html'
			}
		}
	};

	function guideFor(type: CalibrationType): CalibrationGuide | undefined {
		return CALIBRATION_GUIDES[autopilot]?.[type];
	}

	const STATUS_LABELS: Record<CalibrationRunState['status'], string> = {
		running: 'Running',
		success: 'Success',
		failed: 'Failed',
		cancelled: 'Cancelled',
		rejected: 'Rejected'
	};

	const run = $derived(fleet.calibrationRuns[wardId]);

	// Three states, not two: picking a sensor only navigates to its own
	// info view (description + Docs) - it does NOT start anything on its
	// own. The operator reads what to do (and can open Docs) first, then
	// explicitly presses Start when ready; only that click ever sends
	// StartCalibration. selectedType is local, client-side-only
	// navigation state, independent of fleet.calibrationRuns (which only
	// exists once a request has actually been sent) - confirmed live as
	// the wrong UX when the two were conflated (landing on a sensor was
	// indistinguishable from running it).
	let selectedType = $state<CalibrationType | undefined>(undefined);
	// Once a real run exists, it's authoritative for which type is shown
	// (covers the gateway rejecting a stale/different type, though that
	// shouldn't happen in practice since this dialog only ever starts the
	// type the operator selected).
	const activeType = $derived(run?.calibrationType ?? selectedType);
	const guide = $derived(activeType !== undefined ? guideFor(activeType) : undefined);

	function selectType(type: CalibrationType): void {
		selectedType = type;
	}

	function backToList(): void {
		selectedType = undefined;
	}

	function startSelected(): void {
		if (selectedType !== undefined) fleet.requestStartCalibration(wardId, selectedType);
	}

	// The one way out of an active run, running or already settled: stop
	// it if it's still going (fire-and-forget, same as
	// requestCancelCalibration's own contract - harmless no-op if nothing
	// is running) and leave immediately, back to the plain sensor list.
	// Deliberately does not wait for a CANCELLED confirmation to come
	// back first - see clearCalibrationRun()'s own doc comment.
	function cancelAndGoBack(): void {
		fleet.requestCancelCalibration(wardId);
		fleet.clearCalibrationRun(wardId);
		selectedType = undefined;
	}

	function openDocs(): void {
		if (guide?.docUrl) window.open(guide.docUrl, '_blank', 'noopener,noreferrer');
	}
</script>

<!-- dismissible={!run}: calibration state is sensitive (karshipta#248) -
     once a run actually exists (StartCalibration was sent), this dialog
     cannot be dismissed by Escape, backdrop click, or any "back"
     affordance other than the active view's own Cancel button below,
     running or already settled. Selecting a sensor to read about it
     first does NOT set this - nothing has been sent to the vehicle yet,
     so the sensor info view stays freely dismissible/back-able, same as
     the plain list. -->
<Dialog label="Calibrate sensors - {wardId}" dismissible={!run} {onclose}>
	<h3 class="font-display text-sm font-semibold">Calibrate sensors</h3>

	{#if !run && selectedType === undefined}
		<p class="mt-1 text-xs text-fg-muted">
			Ward must be disarmed and stationary. Pick a sensor to calibrate:
		</p>
		<div class="mt-3 flex flex-col gap-1.5">
			{#each CALIBRATION_TYPES as type (type)}
				{@const ok = CALIBRATION_HEALTH[type]}
				<button type="button" class="type-button" onclick={() => selectType(type)}>
					<span class="flex items-center justify-between gap-2">
						<span>{CALIBRATION_TYPE_LABELS[type]}</span>
						{#if ok !== undefined}
							<span
								class="text-[10px] font-medium tracking-wide uppercase {ok
									? 'text-accent'
									: 'text-critical'}"
							>
								{ok ? 'OK' : 'Not calibrated'}
							</span>
						{/if}
					</span>
				</button>
			{/each}
		</div>

		<div class="mt-4 flex items-center justify-between gap-2">
			<span class="font-mono text-[10px] text-fg-muted">{wardId}</span>
			<button
				type="button"
				onclick={onclose}
				class="rounded border border-edge px-3 py-1.5 text-xs text-fg-muted hover:text-fg"
			>
				Close
			</button>
		</div>
	{:else if !run && selectedType !== undefined}
		<!-- Selected but not started yet: read the description/Docs, then
		     press Start when ready - or Back without anything having been
		     sent to the vehicle at all. -->
		<div class="mt-3 flex flex-col gap-2">
			<span class="text-xs font-medium text-fg">{CALIBRATION_TYPE_LABELS[selectedType]}</span>
			{#if guide}
				<p class="text-xs text-fg-muted">{guide.description}</p>
			{/if}
		</div>

		<div class="mt-4 flex items-center justify-between gap-2">
			<span class="font-mono text-[10px] text-fg-muted">{wardId}</span>
			<div class="flex items-center gap-2">
				{#if guide?.docUrl}
					<button
						type="button"
						onclick={openDocs}
						class="rounded border border-edge px-3 py-1.5 text-xs text-fg-muted hover:text-fg"
					>
						Docs
					</button>
				{/if}
				<button
					type="button"
					onclick={backToList}
					class="rounded border border-edge px-3 py-1.5 text-xs text-fg-muted hover:text-fg"
				>
					Back
				</button>
				<button type="button" class="start-button" onclick={startSelected}> Start </button>
			</div>
		</div>
	{:else}
		<div class="mt-3 flex flex-col gap-2">
			<div class="flex items-center justify-between gap-2">
				<span class="text-xs font-medium text-fg">
					{CALIBRATION_TYPE_LABELS[run.calibrationType]}
				</span>
				<span
					class="text-[10px] font-medium tracking-wide uppercase {run.status === 'success'
						? 'text-accent'
						: run.status === 'failed' || run.status === 'rejected'
							? 'text-critical'
							: 'text-fg-muted'}"
				>
					{STATUS_LABELS[run.status]}
				</span>
			</div>

			<!-- How to actually perform this specific calibration - a fixed
			     description for the whole procedure, shown for as long as
			     this view is up (not just the live instruction below, which
			     only appears once the gateway starts sending step-by-step
			     updates and may lag behind the view opening). -->
			{#if guide}
				<p class="text-xs text-fg-muted">{guide.description}</p>
			{/if}

			{#if run.hasInstruction}
				<p class="text-xs text-fg">{run.instruction}</p>
			{/if}

			{#if run.hasProgress}
				<div class="h-1.5 w-full overflow-hidden rounded-full bg-edge">
					<div
						class="h-full rounded-full bg-accent transition-[width]"
						style="width: {Math.round(run.progress * 100)}%"
					></div>
				</div>
			{/if}

			{#if run.status !== 'running' && run.message}
				<p class="text-xs text-critical">{run.message}</p>
			{/if}
		</div>

		<div class="mt-4 flex items-center justify-between gap-2">
			<span class="font-mono text-[10px] text-fg-muted">{wardId}</span>
			<div class="flex items-center gap-2">
				{#if guide?.docUrl}
					<button
						type="button"
						onclick={openDocs}
						class="rounded border border-edge px-3 py-1.5 text-xs text-fg-muted hover:text-fg"
					>
						Docs
					</button>
				{/if}
				<button type="button" class="cancel-button" onclick={cancelAndGoBack}>Cancel</button>
			</div>
		</div>
	{/if}
</Dialog>

<style>
	.type-button {
		border: 1px solid var(--color-edge);
		border-radius: 0.25rem;
		padding: 0.5rem 0.75rem;
		text-align: left;
		font-size: 0.75rem;
	}
	.type-button:hover {
		border-color: var(--color-fg-muted);
	}
	.start-button {
		border: 1px solid var(--color-accent);
		border-radius: 0.25rem;
		background: color-mix(in srgb, var(--color-accent) 15%, transparent);
		padding: 0.375rem 0.75rem;
		font-size: 0.75rem;
		font-weight: 500;
		color: var(--color-accent);
	}
	.start-button:hover {
		background: color-mix(in srgb, var(--color-accent) 25%, transparent);
	}
	.cancel-button {
		border: 1px solid var(--color-critical);
		border-radius: 0.25rem;
		padding: 0.375rem 0.75rem;
		font-size: 0.75rem;
		color: var(--color-critical);
	}
	.cancel-button:hover {
		background: color-mix(in srgb, var(--color-critical) 10%, transparent);
	}
</style>
