# PX4 / ArduPilot parameter catalog

A working reference of MAVLink parameters worth exposing through the console's
curated Configuration tiers (karshipta#212/#213/#214), organized by concept
rather than as a raw alphabetical dump - the same granularity the shipped
tier-1 panel already uses. Not an attempt at the full raw parameter set
(PX4 and ArduPilot each carry 700-1000+); that exhaustive, unsorted, raw-name
search is tier 3's own explicit scope (karshipta#214), not this document's.

**This is a living reference, not a guarantee.** Parameter names, units, and
defaults shift between firmware major versions, and a few rows below are
flagged where research during this pass found the name itself has changed
recently or couldn't be pinned to one exact spelling. Before wiring a new
field into the console, re-verify its exact name against the target firmware
version's own parameter reference - `GetParams` against a real connected
vehicle is the actual source of truth, this table is a map, not the territory.

Columns: **Configuration** (the concept) · **PX4 param ID** · **ArduPilot
param ID** · **Implemented** (karshipta#212's shipped tier-1 panel) ·
**Notes**.

## Return to launch

| Configuration | PX4 param ID | ArduPilot param ID | Implemented | Notes |
|---|---|---|---|---|
| RTL altitude | `RTL_RETURN_ALT` | `RTL_ALT` | Yes | PX4: meters. ArduPilot: centimeters. |
| RTL speed | - | `RTL_SPEED` | Yes | PX4 has no dedicated parameter; return mode reuses the general cruise-speed setting (`MPC_XY_CRUISE` on multicopter), not a single confirmed RTL-specific override. |
| RTL descend/loiter altitude | `RTL_DESCEND_ALT` | `RTL_ALT_FINAL` | No | Altitude for the final descent/loiter step before landing, distinct from the climb altitude above. |

## Battery failsafe

| Configuration | PX4 param ID | ArduPilot param ID | Implemented | Notes |
|---|---|---|---|---|
| Low/warn battery threshold | `BAT_LOW_THR` | `BATT_LOW_VOLT` | Yes | Different physical quantities: PX4 is a 0-1 remaining-capacity fraction, ArduPilot is a plain voltage. |
| Critical battery threshold | `BAT_CRIT_THR` | `BATT_CRT_VOLT` | Yes | |
| Emergency battery threshold | `BAT_EMERGEN_THR` | - | Yes | ArduPilot has two failsafe tiers (low, critical), not PX4's three. |
| Battery failsafe action | `COM_LOW_BAT_ACT` | `BATT_FS_LOW_ACT` | Yes | PX4 shares one action across all three tiers. |
| Critical battery failsafe action | - | `BATT_FS_CRT_ACT` | Yes | ArduPilot's separate action for its critical (second) tier. |

## RC loss failsafe

| Configuration | PX4 param ID | ArduPilot param ID | Implemented | Notes |
|---|---|---|---|---|
| RC loss timeout | `COM_RC_LOSS_T` | - | Yes | Seconds. ArduPilot triggers on a PWM threshold instead (below), not a timeout. |
| RC loss PWM threshold | - | `FS_THR_VALUE` | Yes | Microseconds. |
| RC loss failsafe action | `NAV_RCL_ACT` | `FS_THR_ENABLE` | Yes | On ArduPilot this same value also enables/disables the failsafe (0 = disabled). |

## Data link (GCS/telemetry) loss failsafe

| Configuration | PX4 param ID | ArduPilot param ID | Implemented | Notes |
|---|---|---|---|---|
| Data link loss timeout | `COM_DL_LOSS_T` | `FS_GCS_TIMEOUT` | Yes | Seconds on both. ArduPilot's default is 5s. An earlier draft of this catalog claimed ArduPilot had no configurable timeout here - wrong, corrected after sourcing the doc links for the shipped panel. |
| Data link loss failsafe action | `NAV_DLL_ACT` | `FS_GCS_ENABLE` | Yes | On ArduPilot this same value also enables/disables the failsafe (0 = disabled). |

## Geofence breach

| Configuration | PX4 param ID | ArduPilot param ID | Implemented | Notes |
|---|---|---|---|---|
| Fence breach action | `GF_ACTION` | `FENCE_ACTION` | No | What happens when the vehicle crosses the boundary karshipta#247 already enforces as a real MAVSDK geofence - the natural next addition, since the shape is already pushed but what happens on breach isn't surfaced yet. |
| Fence enable | implicit (set via `GF_ACTION` ≠ none) | `FENCE_ENABLE` | No | ArduPilot gates the whole feature behind one enable flag; PX4 has no separate enable, a 0 action effectively disables it. |
| Max horizontal distance/radius | `GF_MAX_HOR_DIST` | `FENCE_RADIUS` | No | Meters on both. 0 disables on PX4. This is the shape karshipta#247 already pushes as a polygon - these legacy cylinder params are PX4/ArduPilot's own built-in fallback fence, separate from (and possibly redundant with) our own geofence push. Worth confirming how the two interact before building a UI for this row. |
| Max altitude | `GF_MAX_VER_DIST` | `FENCE_ALT_MAX` | No | Meters. The altitude-enforcement gap tracked separately as karshipta#251. |
| Flight termination circuit breaker | `CBRK_FLIGHTTERM` | - | No | Must be explicitly reset for `GF_ACTION=Terminate` to actually kill the vehicle - a safety interlock on top of the action itself, not optional to skip if ever exposing Terminate as a selectable action. |

## EKF / position failsafe

| Configuration | PX4 param ID | ArduPilot param ID | Implemented | Notes |
|---|---|---|---|---|
| Position-loss failsafe action | `COM_POSCTL_NAVL` | `FS_EKF_ACTION` | No | PX4's exact current-version semantics are murky - a v1.16 forum report describes behavior diverging from the documented description. Re-verify directly against GetParams on a real vehicle before building this, not just this table. |
| EKF failsafe threshold | `COM_POS_FS_EPH` (position) | `FS_EKF_THRESH` | No | PX4 splits this into several distinct eph/epv/evh thresholds per failure mode; ArduPilot uses one 0-1 "variance" threshold (default 0.8) covering compass/position/velocity together. Not a clean 1:1 mapping - treat as a cluster of PX4 parameters against ArduPilot's one. |
| Disable position-accuracy checks | `CBRK_VELPOSERR` | - | No | A circuit breaker, not a tunable threshold - disables the check family entirely. High-risk to expose without a strong warning. |

## Arming checks

| Configuration | PX4 param ID | ArduPilot param ID | Implemented | Notes |
|---|---|---|---|---|
| Arming checks enabled | multiple `COM_ARM_*` flags (no single bitmask) | `ARMING_CHECK` | No | Structurally different: PX4 spreads this across many individual boolean-ish parameters (`COM_ARM_WO_GPS`, `COM_ARM_EKF_POS`, `COM_ARM_EKF_HGT`, `COM_ARM_EKF_VEL`, more not enumerated here), ArduPilot uses one bitmask parameter. Exposing ArduPilot's as a raw integer (same "don't decode" treatment as failsafe actions) is straightforward; PX4 needs its own per-flag UI, a materially bigger piece of work, not a drop-in parallel. |
| Arming required before flight | - | `ARMING_REQUIRE` | No | ArduPilot-specific; PX4 has no equivalent toggle (arming is always required). |

## PID - rate gains (inner loop)

| Configuration | PX4 param ID | ArduPilot param ID | Implemented | Notes |
|---|---|---|---|---|
| Roll rate P/I/D | `MC_ROLLRATE_P` / `_I` / `_D` | `ATC_RAT_RLL_P` / `_I` / `_D` | No | PX4 roll-axis naming inferred from the confirmed, documented yaw-axis pattern (`MC_YAWRATE_P/I/D/K`) and PX4's own consistent per-axis convention - not independently confirmed against the parameter reference the way yaw was. Verify before building. |
| Pitch rate P/I/D | `MC_PITCHRATE_P` / `_I` / `_D` | `ATC_RAT_PIT_P` / `_I` / `_D` | No | Same inferred-naming caveat as roll above. |
| Yaw rate P/I/D | `MC_YAWRATE_P` / `_I` / `_D` | `ATC_RAT_YAW_P` / `_I` / `_D` | No | PX4 side confirmed directly against the PID tuning guide. Also has a `_K` overall-gain term PX4-side (mixed parallel/standard controller form). |
| Max rotation rate (roll/pitch/yaw) | `MC_ROLLRATE_MAX` / `MC_PITCHRATE_MAX` / `MC_YAWRATE_MAX` | not independently researched | No | |

## PID - attitude/angle gains (outer loop)

| Configuration | PX4 param ID | ArduPilot param ID | Implemented | Notes |
|---|---|---|---|---|
| Roll angle P | `MC_ROLL_P` | `ATC_ANG_RLL_P` | No | PX4 side confirmed directly. |
| Pitch angle P | `MC_PITCH_P` | `ATC_ANG_PIT_P` | No | PX4 side confirmed directly; ArduPilot inferred from the confirmed roll-axis naming pattern. |
| Yaw angle P | `MC_YAW_P` | `ATC_ANG_YAW_P` | No | Same as pitch above. |

## Camera

| Configuration | PX4 param ID | ArduPilot param ID | Implemented | Notes |
|---|---|---|---|---|
| Trigger interval | `TRIG_INTERVAL` | not independently researched | No | Milliseconds between trigger events. |
| Trigger interface/mode | `TRIG_INTERFACE` (PX4 >= v1.12; older firmware used `TRIG_MODE`) | `CAM1_TYPE` (current docs; older material names it `CAM_TRIGG_TYPE`) | No | Both sides have visibly renamed this across firmware versions during this research pass - the clearest sign in this whole document that a name here needs re-verification against the actual connected vehicle before shipping any UI for it, not just before merging this table. |
| Shutter/relay duration | - | `CAM1_DURATION` | No | ArduPilot-specific relay-trigger timing; no PX4 equivalent found. |
| Capture feedback enable | `CAM_CAP_FBACK` | - | No | |

Per karshipta#256, camera control belongs in the Control tab (a live, in-flight
action), not Configuration - listed here anyway since this document's job is
inventorying the parameter surface, independent of which UI tab eventually
hosts it.

## Gimbal / mount

| Configuration | PX4 param ID | ArduPilot param ID | Implemented | Notes |
|---|---|---|---|---|
| Mount/gimbal type enable | implicit (set via `MNT_MODE_IN`/`MNT_MODE_OUT`) | `MNT1_TYPE` | No | ArduPilot gates its whole mount parameter set behind this selection (other `MNT1_*` params only appear once set); PX4 has no single equivalent gate. |
| Gimbal input mode | `MNT_MODE_IN` | `MNT1_DEFLT_MODE` | No | Values aren't 1:1 (PX4: RC/MAVLink-gimbal-v2/Auto; ArduPilot: a differently-numbered mode set, e.g. RC Targeting = 3 in one confirmed real-world config) - another raw-integer, don't-decode case like the failsafe actions above. |
| Gimbal output mode | `MNT_MODE_OUT` | not independently researched | No | |

Per karshipta#256, gimbal control belongs in the Control tab, same reasoning
as camera above.

## Calibration

Not parameter-based at all - triggered via MAVSDK's `calibration` plugin
(a guided, multi-step MAVLink command sequence with progress callbacks:
gyro, accelerometer, magnetometer/compass, level horizon), not `GetParams`/
`SetParam`. Listed here only so this catalog doesn't look like it forgot
about it; tracked under karshipta#248, not this table's own scope.

## Sources

Every PX4 URL is `docs.px4.io`; every ArduPilot URL is `ardupilot.org`.
Fetched/searched directly during karshipta#212's research, not assumed from
naming conventions alone, except where a row's own Notes column says
otherwise.

- https://docs.px4.io/main/en/config/safety.html - battery/RC-loss/data-link-loss
- https://docs.px4.io/main/en/flight_modes_mc/return.html - RTL altitude/descend
- https://ardupilot.org/copter/docs/rtl-mode.html - RTL altitude/speed
- https://ardupilot.org/copter/docs/failsafe-battery.html - battery tiers
- https://ardupilot.org/copter/docs/radio-failsafe.html - RC loss
- https://ardupilot.org/copter/docs/gcs-failsafe.html - data link loss
- https://docs.px4.io/v1.15/en/config/safety.html (and sibling versions) - geofence action/radius/altitude
- https://ardupilot.org/copter/docs/common-geofencing-landing-page.html, https://ardupilot.org/copter/docs/common-ac2_simple_geofence.html - fence params
- https://discuss.px4.io/t/com-posctl-navl-does-not-work-as-said-in-position-gnss-loss-failsafe-documentation-v1-16/48963 - PX4 position-failsafe caveat
- https://ardupilot.org/copter/docs/common-ekf-inav-failsafe.html - EKF failsafe
- https://docs.px4.io/main/en/flying/pre_flight_checks.html - PX4 arming checks
- https://discuss.ardupilot.org/t/arming-check-how-to-use-a-bitmask-parameter/81801, https://ardupilot.org/rover/docs/arming-your-rover.html - ArduPilot arming checks
- https://docs.px4.io/main/en/config_mc/pid_tuning_guide_multicopter.html - PX4 PID
- ArduPilot PID: no single authoritative page found (official multicopter tuning page didn't surface in search); sourced from `discuss.ardupilot.org` forum threads only - the weakest-sourced section of this document, flagged per-row above.
- https://docs.px4.io/main/en/peripherals/camera (and `/camera/fc_connected_camera`), https://docs.px4.io/main/en/advanced/gimbal_control - PX4 camera/gimbal
- https://ardupilot.org/copter/docs/common-camera-controls.html, https://ardupilot.org/sub/docs/common-mount-targeting.html - ArduPilot camera/gimbal
