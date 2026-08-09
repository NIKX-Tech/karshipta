import type { AircraftCategory } from './types';

/** Shapes actually drawn on the map. 'light' and 'heavy' each get their own
 * real airliner silhouette now (see AIRCRAFT_ICON_MARKUP) rather than
 * sharing one - real top-down aircraft art makes the size difference between
 * a twin-engine jet and a wide-body obvious on its own, not just via marker
 * size. 'fighter' is not a category of its own (AircraftCategory has none) -
 * fleet-map.svelte's sync effect selects it directly, overriding
 * iconShapeForCategory's result, whenever a fixed-wing aircraft
 * (light/heavy) is also flagged military: a real military transport or
 * fighter should not read as "the same thing, different color" as civil
 * traffic, and there is no data to justify guessing which of 'light'/
 * 'heavy' + fighter vs. transport a given military aircraft actually is, so
 * one distinct silhouette covers all military fixed-wing regardless of
 * size category. Military rotorcraft keep the ordinary 'rotorcraft' shape
 * (a real helicopter silhouette already, not a civilian-only design) -
 * confirmed live against a real Royal Netherlands Air Force Apache. */
export type AircraftIconShape =
	| 'light-plane'
	| 'heavy-plane'
	| 'fighter'
	| 'rotorcraft'
	| 'glider'
	| 'uav'
	| 'ground'
	| 'unknown';

export function iconShapeForCategory(category: AircraftCategory): AircraftIconShape {
	if (category === 'light') return 'light-plane';
	if (category === 'heavy') return 'heavy-plane';
	if (
		category === 'rotorcraft' ||
		category === 'glider' ||
		category === 'uav' ||
		category === 'ground'
	) {
		return category;
	}
	return 'unknown';
}

// Real path data from a top-down helicopter silhouette by SVG Repo
// (svgrepo.com), a freely-licensed icon source - see the project's own
// credit at https://www.svgrepo.com/svg/424847/helicopter-bottom-view-silhouette.
// Rendered as real SVG now (see aircraftIconMarkup below), not rasterized
// to a canvas the way every shape here used to be - confirmed live that
// a rasterized, GPU-rotated icon loses thin diagonal detail at typical
// marker size, something a native SVG element, rendered by the browser's
// own vector rasterizer at full device resolution, doesn't suffer from.
const ROTORCRAFT_BODY_PATH =
	'M463.096,252.605l-133.38-52.861V78.503V47.101c0-4.338-3.519-7.851-7.851-7.851s-7.851,3.513-7.851,7.851v31.402h-11.569C293.433,32.987,266.884,0,235.512,0c-31.37,0-57.919,32.987-66.938,78.503h-19.416V47.101c0-4.338-3.519-7.851-7.851-7.851s-7.85,3.513-7.85,7.851v31.402v43.46l-109-43.2c-6.987-2.771-14.597-0.112-16.99,5.933c-2.395,6.045,1.327,13.187,8.312,15.961l117.678,46.639v80.363v23.551c0,4.341,3.518,7.851,7.85,7.851s7.851-3.51,7.851-7.851V227.66h48.1c7.64,25.239,14.703,58.196,14.703,94.207v78.502h7.851v39.528c0,8.079,7.027,14.644,15.701,14.644c8.674,0,15.699-6.564,15.699-14.644v-39.528h7.851v-78.502c0-35.618,6.984-68.655,14.606-94.207h40.347v23.551c0,4.341,3.519,7.851,7.851,7.851s7.851-3.51,7.851-7.851V227.66v-2.583l124.703,49.425c6.981,2.773,14.596,0.121,16.987-5.935C473.799,262.512,470.081,255.383,463.096,252.605z M314.015,94.204v99.322l-24.132-9.567c9.91-19.424,15.877-44.248,15.877-71.307c0-6.297-0.409-12.435-1.03-18.448H314.015z M149.158,94.204h17.132c-0.621,6.014-1.023,12.151-1.023,18.448c0,7.694,0.486,15.207,1.406,22.468l-17.515-6.939V94.204z M149.158,211.958v-58.436l23.536,9.327c1.775,5.688,3.829,11.093,6.155,16.186l-0.433-0.148c0,0,6.476,12.457,13.74,33.071H149.158z M278.714,211.958c0.749-2.18,1.479-4.208,2.22-6.215l15.682,6.215H278.714z';
const ROTORCRAFT_TAIL_MARK_PATH =
	'M266.913,408.219c-4.328,0-7.851,3.518-7.851,7.85v54.954c0,4.332,3.522,7.851,7.851,7.851c4.332,0,7.85-3.519,7.85-7.851v-54.954C274.762,411.736,271.245,408.219,266.913,408.219z';

// Real top-down aircraft silhouettes (raster-traced via potrace, sourced as
// plain public-domain-style icon downloads, not hand-drawn) - each file's
// own <g transform="translate(...) scale(...)"><path d="..."/></g> is kept
// verbatim below (only fill/stroke swapped for currentColor/none so this
// app's normal/military/emergency recoloring still works), same shapes as
// their standalone .svg files, confirmed by rendering those files directly
// before embedding. HEAVY_PLANE_PATH: a classic four-engine wide-body.
// LIGHT_PLANE_PATH: a smaller twin-engine jet, visibly less massive on its
// own, not just smaller via marker size. FIGHTER_PATH: a delta-wing,
// twin-tail military jet silhouette - genuinely distinct from either
// civilian shape, not a recolor of one.
const HEAVY_PLANE_PATH =
	'M5549 12787 c-89 -25 -146 -87 -219 -238 -109 -226 -214 -677 -279 -1199 -41 -324 -44 -405 -50 -1331 l-6 -916 -25 -31 c-14 -17 -299 -270 -635 -562 -634 -551 -654 -567 -710 -542 -50 23 -59 54 -65 213 -5 140 -6 149 -30 174 -44 48 -70 55 -192 55 -130 0 -169 -14 -203 -71 -19 -32 -20 -55 -23 -471 l-3 -437 -585 -520 c-607 -540 -625 -554 -673 -524 -38 24 -46 63 -51 263 -6 216 -10 231 -73 271 -28 18 -47 19 -164 17 l-133 -3 -37 -38 -38 -37 -3 -453 -3 -454 -613 -529 c-337 -291 -627 -548 -644 -571 -81 -108 -90 -164 -91 -564 -1 -197 1 -217 17 -232 10 -10 30 -17 43 -17 15 0 146 74 314 176 980 596 1868 1105 2330 1336 423 212 951 405 1610 587 336 93 509 133 573 135 58 1 66 -1 88 -27 l24 -28 0 -1757 c0 -1073 4 -1812 10 -1897 6 -77 22 -212 37 -300 34 -206 35 -315 6 -371 -15 -27 -261 -274 -706 -707 -376 -366 -696 -682 -711 -701 -15 -20 -39 -62 -54 -94 -24 -51 -27 -70 -30 -192 -3 -148 5 -178 49 -194 27 -9 -12 -18 1198 280 299 74 558 134 575 134 21 0 40 -8 53 -23 12 -12 58 -104 104 -205 45 -100 86 -179 90 -175 4 4 53 93 110 196 56 103 113 196 127 207 45 36 -5 46 1197 -242 231 -55 449 -103 485 -105 58 -4 67 -3 85 17 18 20 20 36 20 174 0 148 -1 152 -30 211 -19 37 -62 93 -115 148 -47 48 -382 341 -745 650 -394 336 -667 576 -680 597 -33 56 -26 180 26 465 l44 240 5 1805 c4 1413 8 1810 17 1827 23 38 63 63 105 63 22 0 132 -23 246 -51 599 -148 1241 -358 1651 -540 445 -199 1410 -729 2581 -1418 178 -105 334 -191 346 -191 12 0 32 8 44 18 21 17 21 20 18 287 l-3 270 -27 59 c-16 32 -42 74 -59 93 -16 19 -290 264 -607 544 -317 280 -596 532 -619 560 l-43 50 0 442 c0 409 -1 445 -18 476 -31 58 -69 71 -205 71 -133 0 -165 -10 -197 -63 -18 -29 -20 -51 -20 -225 0 -222 -9 -262 -57 -262 -15 0 -40 8 -54 18 -14 9 -292 246 -617 526 l-592 509 0 426 c0 386 -2 429 -18 460 -33 66 -58 76 -200 79 -119 3 -129 2 -162 -20 -59 -39 -70 -75 -70 -223 0 -108 -3 -136 -19 -165 -22 -42 -57 -57 -105 -44 -25 6 -209 163 -648 548 -337 296 -624 555 -638 574 l-25 35 -6 926 c-6 944 -9 1013 -50 1296 -106 731 -251 1257 -391 1409 -49 54 -85 63 -159 43z';
const LIGHT_PLANE_PATH =
	'M5280 12769 c-73 -40 -92 -59 -152 -154 -114 -177 -215 -484 -307 -940 -103 -508 -117 -821 -105 -2380 12 -1612 12 -1584 -8 -1621 -10 -18 -20 -58 -22 -90 -4 -49 -10 -62 -40 -93 -28 -28 -38 -33 -48 -23 -9 9 -7 27 11 80 18 56 20 76 12 111 -5 23 -11 44 -13 47 -3 2 -19 -18 -36 -45 -34 -52 -162 -168 -334 -303 -59 -45 -154 -125 -212 -176 -57 -51 -107 -89 -109 -85 -2 5 0 37 5 73 5 36 9 178 9 316 1 271 -5 309 -53 336 -17 10 -79 13 -248 13 -198 0 -229 -2 -258 -18 -18 -10 -37 -30 -43 -45 -22 -58 -33 -367 -20 -556 6 -100 16 -201 20 -226 8 -40 32 -72 61 -82 10 -3 23 -65 34 -160 l6 -48 -273 -218 c-381 -305 -870 -692 -1362 -1077 -798 -625 -1042 -819 -1407 -1123 -162 -134 -198 -168 -198 -189 0 -14 -11 -38 -25 -55 -14 -16 -25 -39 -25 -50 0 -11 -32 -72 -70 -135 -39 -64 -70 -124 -70 -133 0 -28 11 -50 24 -50 9 0 12 -33 12 -122 0 -68 4 -140 8 -161 l7 -37 25 30 c32 37 42 38 46 3 2 -22 8 -28 28 -28 23 0 25 5 30 50 l5 50 747 339 747 339 4 36 c2 20 4 6 5 -33 2 -50 5 -67 15 -63 15 6 1031 460 1412 630 l280 126 3 40 c2 23 4 41 6 41 1 0 90 13 197 30 238 37 229 36 229 5 0 -30 -8 -30 165 -5 250 35 618 79 685 82 l65 3 6 -790 c6 -807 10 -891 55 -1310 19 -183 59 -442 90 -582 28 -130 30 -190 9 -221 -19 -26 -212 -173 -865 -659 -267 -199 -500 -377 -518 -396 -19 -19 -38 -52 -43 -73 -12 -52 -12 -454 0 -454 5 0 121 27 258 61 136 33 320 78 408 99 88 21 273 66 410 100 268 66 494 120 499 120 2 0 6 -32 10 -72 8 -88 25 -118 66 -118 22 0 30 -5 30 -17 1 -50 40 -314 49 -331 6 -12 19 -23 29 -26 13 -4 25 -38 49 -138 33 -136 60 -218 73 -218 13 0 40 82 73 218 24 100 36 134 49 138 10 3 23 14 29 26 9 17 48 281 49 330 0 12 10 18 32 20 41 4 56 33 65 123 3 36 7 65 9 65 5 0 231 -54 499 -120 138 -34 322 -79 410 -100 88 -21 272 -66 408 -99 137 -34 253 -61 258 -61 11 0 11 388 0 448 -6 28 -21 56 -42 79 -19 19 -243 190 -499 381 -641 477 -866 648 -884 674 -22 31 -20 90 8 221 31 140 71 399 90 582 45 419 49 503 55 1310 l6 790 65 -3 c67 -3 435 -47 685 -82 173 -25 165 -25 165 5 0 31 -9 32 229 -5 107 -17 196 -30 197 -30 2 0 4 -18 6 -41 l3 -40 280 -126 c381 -170 1397 -624 1413 -630 9 -4 12 13 14 63 1 39 3 53 5 33 l4 -36 747 -339 747 -339 5 -50 c5 -45 7 -50 30 -50 20 0 26 6 28 28 4 35 14 34 46 -3 l25 -30 7 37 c4 21 8 93 8 160 0 90 3 123 12 123 13 0 24 22 24 50 0 9 -31 69 -70 133 -38 63 -70 124 -70 135 0 11 -11 34 -25 50 -14 17 -25 41 -25 55 0 21 -36 55 -197 189 -366 304 -610 498 -1408 1123 -492 385 -981 772 -1362 1077 l-273 218 6 48 c11 95 24 157 34 160 29 10 53 42 61 82 4 25 14 126 20 226 13 189 2 498 -20 556 -6 15 -25 35 -43 45 -29 16 -60 18 -258 18 -169 0 -231 -3 -248 -13 -47 -27 -54 -65 -53 -326 0 -133 4 -275 9 -316 5 -41 7 -78 5 -83 -2 -4 -52 34 -109 85 -58 51 -153 131 -212 176 -172 135 -300 251 -334 303 -17 27 -33 47 -36 45 -2 -3 -8 -24 -13 -47 -8 -35 -6 -55 12 -111 18 -53 20 -71 11 -80 -10 -10 -20 -5 -48 23 -30 31 -36 44 -40 93 -2 32 -12 72 -22 90 -20 37 -20 9 -8 1621 13 1721 -5 2004 -170 2670 -78 316 -152 513 -246 656 -57 88 -91 121 -165 158 -56 28 -78 26 -143 -10z';
const FIGHTER_PATH =
	'M3381 12677 c-82 -252 -228 -795 -290 -1077 -41 -190 -68 -423 -111 -955 -44 -561 -67 -958 -79 -1380 -16 -516 -19 -586 -38 -747 l-17 -148 -238 0 -238 0 0 -32 c0 -18 -11 -208 -25 -423 -14 -214 -25 -406 -25 -425 0 -27 -74 -177 -306 -620 -584 -1117 -877 -1633 -1307 -2305 -403 -629 -588 -934 -674 -1110 -34 -69 -35 -72 -29 -165 14 -240 161 -845 225 -929 19 -24 19 -25 47 -6 39 24 1200 1186 1794 1795 266 272 486 496 490 498 4 2 8 -152 9 -342 l2 -344 -720 -1154 -721 -1155 1 -189 c1 -221 11 -360 28 -377 9 -9 39 -5 134 21 314 84 1000 254 1161 287 139 29 183 35 201 27 36 -17 77 -96 111 -217 38 -131 14 -120 277 -129 100 -4 198 -9 218 -12 36 -5 36 -6 43 -62 3 -31 12 -112 20 -178 8 -67 24 -229 36 -360 22 -245 42 -403 59 -445 l9 -24 10 25 c15 39 30 148 52 386 24 265 35 369 56 536 l17 128 224 0 c226 0 278 7 298 40 4 7 20 64 36 127 34 138 55 185 89 194 14 3 66 -2 115 -13 118 -24 954 -234 1167 -292 91 -25 175 -46 187 -46 34 0 41 56 41 333 l0 257 -715 1144 -715 1144 2 347 3 347 420 -428 c1385 -1415 1862 -1890 1889 -1881 24 8 68 127 141 381 79 277 119 469 119 576 1 92 -1 98 -41 182 -67 140 -171 308 -620 998 -445 685 -740 1204 -1371 2411 l-290 555 -29 460 -28 459 -238 3 c-179 2 -237 5 -237 15 0 6 -7 68 -15 137 -19 159 -35 470 -35 671 0 454 -107 1907 -166 2269 -42 253 -331 1315 -364 1335 -5 3 -27 -50 -49 -118z';

interface AircraftIconMarkup {
	/** Passed straight to the wrapping <svg viewBox="...">. */
	viewBox: string;
	/** Inner path/shape elements. Simple hand-drawn shapes rely on
	 * fill="currentColor" from the wrapping <svg> (see fleet-map.svelte);
	 * the real potrace-traced silhouettes set fill/stroke explicitly on
	 * their own <g> (still currentColor/none, just spelled out) since an
	 * SVG presentation attribute on an element overrides one merely
	 * inherited from an ancestor - the outer <svg>'s fill would otherwise
	 * be shadowed by these files' own original fill="#000000". */
	innerHtml: string;
}

// All shapes point "up" (north) so a plain CSS rotate() by heading (see
// fleet-map.svelte) lines up correctly. The three potrace-traced silhouettes
// (HEAVY_PLANE_PATH/LIGHT_PLANE_PATH/FIGHTER_PATH) already point up in their
// source files (confirmed by rendering each standalone before embedding);
// the remaining hand-drawn shapes (glider/uav/ground/unknown) keep the
// original simple straight-line convention - those categories are rare
// enough on a real map that a detailed silhouette isn't worth sourcing.
const AIRCRAFT_ICON_MARKUP: Record<AircraftIconShape, AircraftIconMarkup> = {
	'heavy-plane': {
		viewBox: '0 0 1116 1280',
		innerHtml: `<g fill="currentColor" stroke="none" transform="translate(0,1280) scale(0.1,-0.1)"><path d="${HEAVY_PLANE_PATH}" /></g>`
	},
	'light-plane': {
		viewBox: '0 0 1072 1280',
		innerHtml: `<g fill="currentColor" stroke="none" transform="translate(0,1280) scale(0.1,-0.1)"><path d="${LIGHT_PLANE_PATH}" /></g>`
	},
	fighter: {
		viewBox: '0 0 687 1280',
		innerHtml: `<g fill="currentColor" stroke="none" transform="translate(0,1280) scale(0.1,-0.1)"><path d="${FIGHTER_PATH}" /></g>`
	},
	rotorcraft: {
		viewBox: '0 0 478.874 478.873',
		innerHtml: `<path d="${ROTORCRAFT_BODY_PATH}" /><path d="${ROTORCRAFT_TAIL_MARK_PATH}" />`
	},
	glider: {
		viewBox: '0 0 32 32',
		innerHtml:
			'<rect x="14.5" y="5" width="3" height="22" /><rect x="3" y="14.5" width="26" height="2.2" />'
	},
	uav: {
		viewBox: '0 0 32 32',
		innerHtml: `
			<line x1="9" y1="9" x2="23" y2="23" stroke="currentColor" stroke-width="2" />
			<line x1="23" y1="9" x2="9" y2="23" stroke="currentColor" stroke-width="2" />
			<rect x="13" y="13" width="6" height="6" />
			<circle cx="8" cy="8" r="3.4" />
			<circle cx="24" cy="8" r="3.4" />
			<circle cx="8" cy="24" r="3.4" />
			<circle cx="24" cy="24" r="3.4" />
		`
	},
	ground: {
		viewBox: '0 0 32 32',
		innerHtml: '<rect x="9" y="11" width="14" height="10" rx="3" />'
	},
	unknown: {
		viewBox: '0 0 32 32',
		innerHtml: '<path d="M16 8 L23 16 L16 24 L9 16 Z" />'
	}
};

export function aircraftIconMarkup(shape: AircraftIconShape): AircraftIconMarkup {
	return AIRCRAFT_ICON_MARKUP[shape];
}
