import { ParamType } from '$lib/gen/karshipta/v1/parameter';

/**
 * Tier 1 curated safety-critical parameters (karshipta#212): RTL
 * altitude/speed and the battery/RC-loss/data-link-loss failsafe
 * thresholds and actions. Everything here is a human-curated subset of
 * the raw MAVLink parameter set - a vehicle carries hundreds more, left
 * to tier 3 (karshipta#214).
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
 */

export type AutopilotFamily = 'PX4' | 'ArduPilot';

export type FieldGroup = 'rtl' | 'battery' | 'rcLoss' | 'dataLinkLoss';

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
	}
];

export const FIELD_GROUP_LABELS: Record<FieldGroup, string> = {
	rtl: 'Return to launch',
	battery: 'Battery failsafe',
	rcLoss: 'RC loss failsafe',
	dataLinkLoss: 'Data link loss failsafe'
};

export function bindingFor(field: CuratedField, autopilot: string): ParamBinding | undefined {
	if (autopilot === 'PX4') return field.px4;
	if (autopilot === 'ArduPilot') return field.ardupilot;
	return undefined;
}
