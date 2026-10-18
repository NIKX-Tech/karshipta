import { ParamType } from '$lib/gen/karshipta/v1/parameter';

/**
 * Curated parameters, shipped in tiers (same panel, same infrastructure,
 * each tier just adds a curated group - karshipta#212's own framing).
 * Tier 1 (karshipta#212): RTL altitude/speed and the battery/RC-loss/
 * data-link-loss failsafe thresholds and actions. Tier 2 (karshipta#213):
 * rate and attitude PID gains for roll/pitch/yaw, one group per axis
 * ("grouped and labeled by axis, not a raw alphabetical list" - #213's
 * own scope). Everything here is a human-curated subset of the raw
 * MAVLink parameter set - a vehicle carries hundreds more, left to tier 3
 * (karshipta#214).
 *
 * PX4 and ArduPilot differ structurally here, not just in naming,
 * verified against each firmware's own docs rather than assumed (see each
 * field's own comment below for the specific divergence). None of that
 * ever reaches the UI as cross-firmware text, though: an operator looking
 * at a PX4 ward has no reason to be told what ArduPilot calls the same
 * concept, or that it doesn't exist there - confusing and irrelevant to
 * them (confirmed live, fixed after an initial version did exactly that).
 * A field with no binding for the connected ward's own firmware is simply
 * not rendered - see bindingFor() and configuration-tab.svelte's use of
 * it - and every description/docUrl below is already per-firmware, never
 * naming the other one.
 *
 * PID gain names (tier 2) are confirmed as real, actively-used parameter
 * names via multiple independent config files, forum posts, and (PX4 only)
 * the official multicopter tuning guide - not a single canonical
 * parameter-reference citation the way most of tier 1 has, but solid
 * practical confirmation, not a bare naming-pattern guess. Unlike tier 1's
 * failsafe thresholds, there's no documented min/max range for any of
 * these on either firmware - PID gains are tuned empirically, not bounded
 * by a published safe range - so none is given here; inventing one would
 * be worse than leaving it open. A bad gain here causes oscillation or a
 * crash directly and immediately, categorically higher risk than a
 * misconfigured failsafe threshold - see FIELD_GROUP_WARNINGS below.
 */

export type AutopilotFamily = 'PX4' | 'ArduPilot';

export type FieldGroup =
	'rtl' | 'battery' | 'rcLoss' | 'dataLinkLoss' | 'pidRoll' | 'pidPitch' | 'pidYaw';

/** Action/enum-valued parameters (failsafe actions) are shown as a raw
 * integer, never decoded to a label: the exact meaning of each code
 * varies by firmware and version, and guessing it wrong would be worse
 * than not guessing at all. isAction only changes the UI's own hint text;
 * docUrl (every binding has one, action or not) is what actually answers
 * "what does this mean" - linked to the specific page of that firmware's
 * own docs covering this exact parameter, not a generic parameter dump. */
export interface ParamBinding {
	name: string;
	paramType: ParamType;
	unit: string;
	isAction?: boolean;
	/** per-firmware wording - never references the other firmware by name */
	description: string;
	/** this firmware's own docs page covering this exact parameter */
	docUrl: string;
	/** raw wire value -> value shown/edited in the UI */
	toDisplay: (raw: number) => number;
	/** UI value -> raw wire value to send */
	toRaw: (display: number) => number;
	min?: number;
	max?: number;
	step?: number;
}

export interface CuratedField {
	key: string;
	label: string;
	group: FieldGroup;
	px4?: ParamBinding;
	ardupilot?: ParamBinding;
}

// Display-direction conversions are rounded to a couple decimal places:
// raw * 100 or raw / 100 on a plain JS number routinely lands on something
// like 7.000000000000001 (IEEE 754, not a bug in the math) - confirmed
// live, 0.07 norm rendered as that exact ugly string in the battery
// threshold field before this fix. Write-direction conversions stay
// unrounded (full precision sent over the wire); only what's shown back
// to the operator needs cleaning up.
function roundTo(value: number, decimals: number): number {
	const factor = 10 ** decimals;
	return Math.round(value * factor) / factor;
}

const identity = (value: number) => value;
const percentFromNorm = (raw: number) => roundTo(raw * 100, 2);
const normFromPercent = (display: number) => display / 100;
const metersFromCm = (raw: number) => roundTo(raw / 100, 2);
const cmFromMeters = (display: number) => Math.round(display * 100);

// Doc pages, each verified to actually exist and cover the parameters
// cited against it (fetched directly, not guessed from a URL pattern):
// PX4's safety-config page covers every PX4 failsafe parameter below in
// one place; PX4's return-mode page is the separate home for RTL_RETURN_ALT.
// ArduPilot splits by concern into its own dedicated pages per failsafe
// kind, so each ArduPilot binding points at a different one.
const PX4_RETURN_DOCS = 'https://docs.px4.io/main/en/flight_modes_mc/return.html';
const PX4_SAFETY_DOCS = 'https://docs.px4.io/main/en/config/safety.html';
const AP_RTL_DOCS = 'https://ardupilot.org/copter/docs/rtl-mode.html';
const AP_BATTERY_DOCS = 'https://ardupilot.org/copter/docs/failsafe-battery.html';
const AP_RADIO_DOCS = 'https://ardupilot.org/copter/docs/radio-failsafe.html';
const AP_GCS_DOCS = 'https://ardupilot.org/copter/docs/gcs-failsafe.html';
// PX4's multicopter PID tuning guide covers rate and attitude gains for
// every axis in one page. ArduPilot has no single page naming every
// ATC_RAT_*/ATC_ANG_* parameter directly (checked); this is the official
// entry point to Copter's actual tuning process, confirmed to exist and
// cover tuning (fetched directly), even though it doesn't spell out every
// parameter name inline - the honest best link, not a guess.
const PX4_PID_DOCS = 'https://docs.px4.io/main/en/config_mc/pid_tuning_guide_multicopter.html';
const AP_PID_DOCS = 'https://ardupilot.org/copter/docs/tuning-process-instructions.html';

/**
 * Builds the 4 curated fields for one PID axis (rate P/I/D + angle P) -
 * a factory instead of 12 near-identical literal objects, both firmwares
 * following the exact same naming scheme per axis (karshipta#213):
 * PX4 rate terms are `MC_{axis}RATE_{P,I,D}`, PX4 angle term is
 * `MC_{axis}_P`; ArduPilot rate terms are `ATC_RAT_{axis}_{P,I,D}`,
 * angle term is `ATC_ANG_{axis}_P`.
 */
function pidAxisFields(
	axisLabel: 'Roll' | 'Pitch' | 'Yaw',
	group: Extract<FieldGroup, 'pidRoll' | 'pidPitch' | 'pidYaw'>,
	keyPrefix: 'roll' | 'pitch' | 'yaw',
	px4RatePrefix: string,
	px4AnglePrefix: string,
	apAxisCode: 'RLL' | 'PIT' | 'YAW'
): CuratedField[] {
	const axisLower = axisLabel.toLowerCase();
	const rateTerm = (term: 'P' | 'I' | 'D'): CuratedField => ({
		key: `${keyPrefix}_rate_${term.toLowerCase()}`,
		label: `${axisLabel} rate ${term}`,
		group,
		px4: {
			name: `${px4RatePrefix}_${term}`,
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: '',
			description: `${term} gain for the ${axisLower} rate (inner-loop) controller.`,
			docUrl: PX4_PID_DOCS,
			toDisplay: identity,
			toRaw: identity
		},
		ardupilot: {
			name: `ATC_RAT_${apAxisCode}_${term}`,
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: '',
			description: `${term} gain for the ${axisLower} rate (inner-loop) controller.`,
			docUrl: AP_PID_DOCS,
			toDisplay: identity,
			toRaw: identity
		}
	});
	const angleTerm: CuratedField = {
		key: `${keyPrefix}_angle_p`,
		label: `${axisLabel} angle P`,
		group,
		px4: {
			name: `${px4AnglePrefix}_P`,
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: '',
			description: `Proportional gain for the ${axisLower} attitude (outer-loop) controller.`,
			docUrl: PX4_PID_DOCS,
			toDisplay: identity,
			toRaw: identity
		},
		ardupilot: {
			name: `ATC_ANG_${apAxisCode}_P`,
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: '',
			description: `Proportional gain for the ${axisLower} attitude (outer-loop) controller.`,
			docUrl: AP_PID_DOCS,
			toDisplay: identity,
			toRaw: identity
		}
	};
	return [rateTerm('P'), rateTerm('I'), rateTerm('D'), angleTerm];
}

export const CURATED_FIELDS: CuratedField[] = [
	{
		key: 'rtl_altitude',
		label: 'RTL altitude',
		group: 'rtl',
		px4: {
			name: 'RTL_RETURN_ALT',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: 'm',
			description: 'Altitude the ward climbs to (if not already higher) before flying home.',
			docUrl: PX4_RETURN_DOCS,
			toDisplay: identity,
			toRaw: identity,
			min: 1,
			max: 150,
			step: 1
		},
		ardupilot: {
			name: 'RTL_ALT',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: 'm',
			description: 'Minimum altitude the ward climbs to before flying home.',
			docUrl: AP_RTL_DOCS,
			toDisplay: metersFromCm,
			toRaw: cmFromMeters,
			min: 2,
			max: 80,
			step: 1
		}
	},
	{
		// PX4 has no dedicated RTL speed parameter at all - return mode
		// reuses the vehicle's general cruise-speed setting, no single
		// confirmed override - so this field has no px4 binding and simply
		// doesn't render for a PX4 ward.
		key: 'rtl_speed',
		label: 'RTL speed',
		group: 'rtl',
		ardupilot: {
			name: 'RTL_SPEED',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: 'm/s',
			description: 'Horizontal speed during the return flight.',
			docUrl: AP_RTL_DOCS,
			toDisplay: metersFromCm,
			toRaw: cmFromMeters,
			min: 0,
			max: 20,
			step: 0.5
		}
	},
	{
		// Different physical quantities, not just different units of the
		// same one: PX4's is a 0-1 remaining-capacity fraction, ArduPilot's
		// is a plain voltage.
		key: 'battery_low_threshold',
		label: 'Low/warn battery threshold',
		group: 'battery',
		px4: {
			name: 'BAT_LOW_THR',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: '%',
			description: 'Remaining-capacity level that triggers a low-battery warning.',
			docUrl: PX4_SAFETY_DOCS,
			toDisplay: percentFromNorm,
			toRaw: normFromPercent,
			min: 12,
			max: 50,
			step: 1
		},
		ardupilot: {
			name: 'BATT_LOW_VOLT',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: 'V',
			description: 'Voltage level that triggers the low-tier battery failsafe.',
			docUrl: AP_BATTERY_DOCS,
			toDisplay: identity,
			toRaw: identity,
			min: 0,
			max: 60,
			step: 0.1
		}
	},
	{
		key: 'battery_critical_threshold',
		label: 'Critical battery threshold',
		group: 'battery',
		px4: {
			name: 'BAT_CRIT_THR',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: '%',
			description: 'Remaining-capacity level that triggers the Return action.',
			docUrl: PX4_SAFETY_DOCS,
			toDisplay: percentFromNorm,
			toRaw: normFromPercent,
			min: 5,
			max: 25,
			step: 1
		},
		ardupilot: {
			name: 'BATT_CRT_VOLT',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: 'V',
			description: 'Voltage level that triggers the critical (second-tier) battery failsafe.',
			docUrl: AP_BATTERY_DOCS,
			toDisplay: identity,
			toRaw: identity,
			min: 0,
			max: 60,
			step: 0.1
		}
	},
	{
		// ArduPilot has two battery failsafe tiers (low, critical), not
		// PX4's three - no "emergency" level exists there, so this field
		// has no ardupilot binding.
		key: 'battery_emergency_threshold',
		label: 'Emergency battery threshold',
		group: 'battery',
		px4: {
			name: 'BAT_EMERGEN_THR',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: '%',
			description: 'Remaining-capacity level that triggers an immediate land.',
			docUrl: PX4_SAFETY_DOCS,
			toDisplay: percentFromNorm,
			toRaw: normFromPercent,
			min: 3,
			max: 10,
			step: 1
		}
	},
	{
		// PX4 shares one action across all three battery tiers; ArduPilot
		// gives the low tier its own action, separate from the critical
		// tier's (battery_critical_action below) - a real structural
		// difference, not just a naming one.
		key: 'battery_low_action',
		label: 'Battery failsafe action',
		group: 'battery',
		px4: {
			name: 'COM_LOW_BAT_ACT',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			description: 'What the ward does when any battery threshold above trips.',
			docUrl: PX4_SAFETY_DOCS,
			toDisplay: identity,
			toRaw: identity
		},
		ardupilot: {
			name: 'BATT_FS_LOW_ACT',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			description: 'What the ward does when the low-tier battery threshold trips.',
			docUrl: AP_BATTERY_DOCS,
			toDisplay: identity,
			toRaw: identity
		}
	},
	{
		key: 'battery_critical_action',
		label: 'Critical battery failsafe action',
		group: 'battery',
		ardupilot: {
			name: 'BATT_FS_CRT_ACT',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			description: 'What the ward does when the critical (second-tier) battery threshold trips.',
			docUrl: AP_BATTERY_DOCS,
			toDisplay: identity,
			toRaw: identity
		}
	},
	{
		// ArduPilot's RC-loss failsafe triggers on a PWM threshold
		// (rc_loss_pwm_threshold below), not a timeout - no ardupilot
		// binding here.
		key: 'rc_loss_timeout',
		label: 'RC loss timeout',
		group: 'rcLoss',
		px4: {
			name: 'COM_RC_LOSS_T',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: 's',
			description: 'How long the radio link can be missing before the RC-loss failsafe triggers.',
			docUrl: PX4_SAFETY_DOCS,
			toDisplay: identity,
			toRaw: identity,
			min: 0,
			max: 35,
			step: 0.1
		}
	},
	{
		// The ArduPilot counterpart to PX4's timeout above - a threshold,
		// not a duration, so no px4 binding here.
		key: 'rc_loss_pwm_threshold',
		label: 'RC loss PWM threshold',
		group: 'rcLoss',
		ardupilot: {
			name: 'FS_THR_VALUE',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: 'µs',
			description: 'Throttle PWM value below which the radio link is treated as lost.',
			docUrl: AP_RADIO_DOCS,
			toDisplay: identity,
			toRaw: identity,
			min: 925,
			max: 1100,
			step: 1
		}
	},
	{
		key: 'rc_loss_action',
		label: 'RC loss failsafe action',
		group: 'rcLoss',
		px4: {
			name: 'NAV_RCL_ACT',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			description: 'What the ward does on RC loss.',
			docUrl: PX4_SAFETY_DOCS,
			toDisplay: identity,
			toRaw: identity
		},
		ardupilot: {
			name: 'FS_THR_ENABLE',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			description:
				'What the ward does on RC loss. This value also enables/disables the failsafe (0 = disabled).',
			docUrl: AP_RADIO_DOCS,
			toDisplay: identity,
			toRaw: identity
		}
	},
	{
		key: 'data_link_loss_timeout',
		label: 'Data link loss timeout',
		group: 'dataLinkLoss',
		px4: {
			name: 'COM_DL_LOSS_T',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: 's',
			description:
				'How long the ground station/telemetry link can be missing before the failsafe triggers.',
			docUrl: PX4_SAFETY_DOCS,
			toDisplay: identity,
			toRaw: identity,
			min: 0,
			max: 60,
			step: 0.5
		},
		ardupilot: {
			name: 'FS_GCS_TIMEOUT',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: 's',
			description: 'How long the ground station link can be missing before the failsafe triggers.',
			docUrl: AP_GCS_DOCS,
			toDisplay: identity,
			toRaw: identity,
			min: 1,
			max: 120,
			step: 0.5
		}
	},
	{
		key: 'data_link_loss_action',
		label: 'Data link loss failsafe action',
		group: 'dataLinkLoss',
		px4: {
			name: 'NAV_DLL_ACT',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			description: 'What the ward does on data-link loss.',
			docUrl: PX4_SAFETY_DOCS,
			toDisplay: identity,
			toRaw: identity
		},
		ardupilot: {
			name: 'FS_GCS_ENABLE',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			description:
				'What the ward does on data-link loss. This value also enables/disables the failsafe (0 = disabled).',
			docUrl: AP_GCS_DOCS,
			toDisplay: identity,
			toRaw: identity
		}
	},
	// Tier 2 (karshipta#213): rate and attitude PID gains, one group per
	// axis - see pidAxisFields()'s own doc comment for the naming scheme.
	...pidAxisFields('Roll', 'pidRoll', 'roll', 'MC_ROLLRATE', 'MC_ROLL', 'RLL'),
	...pidAxisFields('Pitch', 'pidPitch', 'pitch', 'MC_PITCHRATE', 'MC_PITCH', 'PIT'),
	...pidAxisFields('Yaw', 'pidYaw', 'yaw', 'MC_YAWRATE', 'MC_YAW', 'YAW')
];

export const FIELD_GROUP_LABELS: Record<FieldGroup, string> = {
	rtl: 'Return to launch',
	battery: 'Battery failsafe',
	rcLoss: 'RC loss failsafe',
	dataLinkLoss: 'Data link loss failsafe',
	pidRoll: 'Roll PID',
	pidPitch: 'Pitch PID',
	pidYaw: 'Yaw PID'
};

/** Shown once per matching group, not per field: a change here affects
 * flight stability directly and immediately (oscillation, loss of
 * control), categorically higher risk than a misconfigured failsafe
 * threshold above - unlike those, there's no documented safe range to
 * fall back on, so the caution has to live in the UI text itself. */
export const FIELD_GROUP_WARNINGS: Partial<Record<FieldGroup, string>> = {
	pidRoll:
		'Directly affects flight stability. Change gains in small increments (the PX4 tuning guide suggests 20-30% per step, less for final tuning) and test cautiously.',
	pidPitch:
		'Directly affects flight stability. Change gains in small increments and test cautiously.',
	pidYaw: 'Directly affects flight stability. Change gains in small increments and test cautiously.'
};

export function bindingFor(field: CuratedField, autopilot: string): ParamBinding | undefined {
	if (autopilot === 'PX4') return field.px4;
	if (autopilot === 'ArduPilot') return field.ardupilot;
	return undefined;
}
