import { shouldFilterMessage } from '../../shared/input_filter.mjs';

const CC_WT_POSITION = 71; /* Knob 1 */
const CC_VOLUME      = 78; /* Knob 8 */

let wtFilename   = 'default.wav';
let wtPosition   = 0.0;
let volume       = 0.8;

/* Display state */
let paramLabel   = null;
let paramValue   = null;
let labelTimeout = 0;
const LABEL_TICKS = 88; /* ~2 seconds at 44 ticks/sec */

function refreshDisplay() {
    clear_screen();
    if (paramLabel && labelTimeout > 0) {
        draw_text(0, 0, 'CREST', 1);
        draw_text(0, 16, paramLabel, 1);
        draw_text(0, 32, paramValue, 1);
    } else {
        draw_text(0, 0, 'CREST', 1);
        draw_text(0, 16, wtFilename, 1);
    }
    host_flush_display();
}

function showParam(label, value) {
    paramLabel   = label;
    paramValue   = value;
    labelTimeout = LABEL_TICKS;
    refreshDisplay();
}

globalThis.init = function () {
    const fn = host_module_get_param('wt_filename');
    if (fn && fn.length > 0) wtFilename = fn;
    refreshDisplay();
};

globalThis.tick = function () {
    if (labelTimeout > 0) {
        labelTimeout--;
        if (labelTimeout === 0) {
            paramLabel = null;
            paramValue = null;
            refreshDisplay();
        }
    }
};

globalThis.onMidiMessageInternal = function (data) {
    if (shouldFilterMessage(data)) return;

    const [status, cc, value] = data;
    const isCC = (status & 0xF0) === 0xB0;
    if (!isCC) return;

    if (cc === CC_WT_POSITION) {
        wtPosition = value / 127.0;
        const str  = wtPosition.toFixed(3);
        host_module_set_param('wt_position', str);
        showParam('WT POS', str);
    } else if (cc === CC_VOLUME) {
        volume    = value / 127.0;
        const str = volume.toFixed(3);
        host_module_set_param('volume', str);
        showParam('VOLUME', str);
    }
};

globalThis.onMidiMessageExternal = function (data) {
    /* Not used — Crest is not an overtake module */
};
