import { ParamType } from '$lib/gen/karshipta/v1/parameter';

/**
 * Tier 1 curated safety-critical parameters (karshipta#212): RTL
 * altitude/speed and the battery/RC-loss/data-link-loss failsafe
 * thresholds and actions. Everything here is a human-curated subset of
 * the raw MAVLink parameter set - a vehicle carries hundreds more, left
 * to tier 3 (karshipta#214).
 *
 * PX4 and ArduPilot name - and in several cases structurally differ on -
 * the same safety concept, verified against each firmware's own docs
 * rather than assumed:
 * - RTL speed has no dedicated PX4 parameter at all (RTL reuses general
 *   cruise-speed settings, not a single confirmed override); ArduPilot
 *   has RTL_SPEED.
 * - Low/warn battery threshold is a 0-1 capacity fraction on PX4
 *   (BAT_LOW_THR) but a plain voltage on ArduPilot (BATT_LOW_VOLT) -
 *   genuinely different physical quantities, not just different units of
 *   the same one.
 * - PX4 has three battery tiers (low/critical/emergency) sharing one
 *   action parameter (COM_LOW_BAT_ACT); ArduPilot has two tiers
 *   (low/critical), each with its own action parameter.
 * - ArduPilot's RC-loss failsafe is a PWM threshold (FS_THR_VALUE), not a
 *   timeout like PX4's COM_RC_LOSS_T; ArduPilot has no separate
 *   configurable data-link-loss timeout PX4's COM_DL_LOSS_T has an
 *   equivalent for.
 *
 * Rather than force every field into a fake 1:1 shape across firmwares,
 * each field's px4/ardupilot binding is independently optional - a field
 * with only one firmware's binding present still renders, just without
 * that other firmware's row (see missingNote).
 */

export type AutopilotFamily = 'PX4' | 'ArduPilot';

export type FieldGroup = 'rtl' | 'battery' | 'rcLoss' | 'dataLinkLoss';

/** Action/enum-valued parameters (failsafe actions) are shown as a raw
 * integer, never decoded to a label: the exact meaning of each code
 * varies by firmware and version, and guessing it wrong would be worse
 * than not guessing at all. isAction drives that raw-integer treatment
 * in the UI and adds a "consult your autopilot's docs" hint. */
export interface ParamBinding {
	name: string;
	paramType: ParamType;
	unit: string;
	isAction?: boolean;
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
	description: string;
	px4?: ParamBinding;
	ardupilot?: ParamBinding;
	/** set when one firmware has no equivalent at all, so the UI can say why a row is missing instead of looking broken */
	missingNote?: Partial<Record<AutopilotFamily, string>>;
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

export const CURATED_FIELDS: CuratedField[] = [
	{
		key: 'rtl_altitude',
		label: 'RTL altitude',
		group: 'rtl',
		description: 'Altitude the ward climbs to (if not already higher) before flying home on RTL.',
		px4: {
			name: 'RTL_RETURN_ALT',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: 'm',
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
			toDisplay: metersFromCm,
			toRaw: cmFromMeters,
			min: 2,
			max: 80,
			step: 1
		}
	},
	{
		key: 'rtl_speed',
		label: 'RTL speed',
		group: 'rtl',
		description: 'Horizontal speed during the return flight.',
		ardupilot: {
			name: 'RTL_SPEED',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: 'm/s',
			toDisplay: metersFromCm,
			toRaw: cmFromMeters,
			min: 0,
			max: 20,
			step: 0.5
		},
		missingNote: {
			PX4: 'PX4 has no dedicated RTL speed parameter - return mode uses the vehicle’s general cruise speed setting instead.'
		}
	},
	{
		key: 'battery_low_threshold',
		label: 'Low/warn battery threshold',
		group: 'battery',
		description:
			'Level that triggers a low-battery warning (PX4) or the low-tier failsafe (ArduPilot).',
		px4: {
			name: 'BAT_LOW_THR',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: '%',
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
		description:
			'Level that triggers the Return action on PX4, or ArduPilot’s second (critical) failsafe tier.',
		px4: {
			name: 'BAT_CRIT_THR',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: '%',
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
			toDisplay: identity,
			toRaw: identity,
			min: 0,
			max: 60,
			step: 0.1
		}
	},
	{
		key: 'battery_emergency_threshold',
		label: 'Emergency battery threshold',
		group: 'battery',
		description: 'Level that triggers an immediate land.',
		px4: {
			name: 'BAT_EMERGEN_THR',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: '%',
			toDisplay: percentFromNorm,
			toRaw: normFromPercent,
			min: 3,
			max: 10,
			step: 1
		},
		missingNote: {
			ArduPilot:
				'ArduPilot has two battery failsafe tiers (low, critical), not PX4’s three - there is no third "emergency" level.'
		}
	},
	{
		key: 'battery_low_action',
		label: 'Battery failsafe action',
		group: 'battery',
		description:
			'What the ward does when a battery threshold trips. PX4 shares one action across all three tiers; ArduPilot’s low-tier action is separate from its critical-tier action below.',
		px4: {
			name: 'COM_LOW_BAT_ACT',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			toDisplay: identity,
			toRaw: identity
		},
		ardupilot: {
			name: 'BATT_FS_LOW_ACT',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			toDisplay: identity,
			toRaw: identity
		}
	},
	{
		key: 'battery_critical_action',
		label: 'Critical battery failsafe action',
		group: 'battery',
		description: 'ArduPilot’s separate action for its critical (second) battery tier.',
		ardupilot: {
			name: 'BATT_FS_CRT_ACT',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			toDisplay: identity,
			toRaw: identity
		},
		missingNote: {
			PX4: 'PX4 uses the single battery failsafe action above for every tier.'
		}
	},
	{
		key: 'rc_loss_timeout',
		label: 'RC loss timeout',
		group: 'rcLoss',
		description: 'How long the radio link can be missing before the RC-loss failsafe triggers.',
		px4: {
			name: 'COM_RC_LOSS_T',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: 's',
			toDisplay: identity,
			toRaw: identity,
			min: 0,
			max: 35,
			step: 0.1
		},
		missingNote: {
			ArduPilot: 'ArduPilot triggers RC-loss on a PWM threshold (below), not a timeout.'
		}
	},
	{
		key: 'rc_loss_pwm_threshold',
		label: 'RC loss PWM threshold',
		group: 'rcLoss',
		description: 'Throttle PWM value below which ArduPilot treats the radio link as lost.',
		ardupilot: {
			name: 'FS_THR_VALUE',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: 'µs',
			toDisplay: identity,
			toRaw: identity,
			min: 925,
			max: 1100,
			step: 1
		},
		missingNote: {
			PX4: 'PX4 uses a timeout (above), not a PWM threshold.'
		}
	},
	{
		key: 'rc_loss_action',
		label: 'RC loss failsafe action',
		group: 'rcLoss',
		description:
			'What the ward does on RC loss. On ArduPilot this same parameter also enables/disables the failsafe (0 = disabled).',
		px4: {
			name: 'NAV_RCL_ACT',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			toDisplay: identity,
			toRaw: identity
		},
		ardupilot: {
			name: 'FS_THR_ENABLE',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			toDisplay: identity,
			toRaw: identity
		}
	},
	{
		key: 'data_link_loss_timeout',
		label: 'Data link loss timeout',
		group: 'dataLinkLoss',
		description:
			'How long the ground station/telemetry link can be missing before the failsafe triggers.',
		px4: {
			name: 'COM_DL_LOSS_T',
			paramType: ParamType.PARAM_TYPE_FLOAT,
			unit: 's',
			toDisplay: identity,
			toRaw: identity,
			min: 0,
			max: 60,
			step: 0.5
		},
		missingNote: {
			ArduPilot:
				'No separately configurable timeout was confirmed for ArduPilot’s GCS failsafe; only the enable/action parameter below exists.'
		}
	},
	{
		key: 'data_link_loss_action',
		label: 'Data link loss failsafe action',
		group: 'dataLinkLoss',
		description:
			'What the ward does on data-link loss. On ArduPilot this same parameter also enables/disables the failsafe (0 = disabled).',
		px4: {
			name: 'NAV_DLL_ACT',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
			toDisplay: identity,
			toRaw: identity
		},
		ardupilot: {
			name: 'FS_GCS_ENABLE',
			paramType: ParamType.PARAM_TYPE_INT,
			unit: '',
			isAction: true,
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
