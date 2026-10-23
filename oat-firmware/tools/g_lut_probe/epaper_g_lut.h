#ifndef EPAPER_G_LUT_H
#define EPAPER_G_LUT_H
/* Register-LUT builder for the Jadard JD7966x 4-color e-paper controllers
   (JD79660A on the Waveshare 1.54" G).

   What is documented: the panel-setting register has a LUT_EN bit ("LUT from
   register" instead of the factory table in MTP), and the data-transmission
   timing diagram shows the VCOM LUT being written with command 0x20 before
   the image data. What is NOT documented for this chip is the byte layout of
   the LUT registers. Jadard uses two layouts on its other chips:

     format 1 (JD79651CA / JD79653A): 8 groups x 7 bytes = 56 bytes
        [group repeat][lvl<<6|frames]x4 [state1 repeat][state2 repeat]
     format 2 (JD79686AB):            10 groups x 6 bytes = 60 bytes
        [lvl1 lvl2 lvl3 lvl4 packed][frames]x4 [repeat]

   Level codes, from the border-level table of the JD79665 (same family):
     00 = VCOM (no field)   01 = VSP (+15 V)   10 = VSN (-15 V)   11 = VSPL (weak +)
   The pixel sees source minus VCOM, so a weak NEGATIVE field is made by
   putting VCOM at VSPL (or VSP) and the source at 0 V (or VSPL).

   tools/g_lut_probe finds the layout, the register-to-code map and the
   polarity on real hardware; user_config.h then carries the answers.         */
#include <stdint.h>
#include <string.h>

enum { G_LVL_VCOM = 0, G_LVL_VSP = 1, G_LVL_VSN = 2, G_LVL_VSPL = 3 };

typedef struct { uint8_t level; uint16_t frames; } g_phase_t;

#define G_LUT_BYTES_FMT1 56
#define G_LUT_BYTES_FMT2 60
#define G_LUT_MAX_BYTES  60

static inline int g_lut_bytes(int format) { return format == 2 ? G_LUT_BYTES_FMT2 : G_LUT_BYTES_FMT1; }

/* Encode a phase list into out[G_LUT_MAX_BYTES]. A phase longer than one slot
   (63 frames in format 1, 255 in format 2) is split over consecutive slots at
   the same level. Returns bytes to send, 0 if it does not fit. */
static inline int g_lut_encode(int format, int group_repeat, const g_phase_t *ph, int n, uint8_t *out)
{
    const int slot_max  = (format == 2) ? 255 : 63;
    const int groups    = (format == 2) ? 10 : 8;
    const int gbytes    = (format == 2) ? 6 : 7;
    const int per_group = 4;
    uint8_t lvl[40], frm[40];
    int ns = 0;
    for (int i = 0; i < n; i++) {
        int left = ph[i].frames;
        while (left > 0) {
            if (ns >= groups * per_group) return 0;
            int f = left > slot_max ? slot_max : left;
            lvl[ns] = ph[i].level & 3;
            frm[ns] = (uint8_t)f;
            ns++;
            left -= f;
        }
    }
    memset(out, 0, G_LUT_MAX_BYTES);
    for (int g = 0; g < groups; g++) {
        uint8_t *b   = out + g * gbytes;
        int      base = g * per_group;
        bool     used = base < ns;
        if (format == 2) {
            for (int k = 0; k < per_group; k++) {
                int s = base + k;
                if (s < ns) { b[0] |= lvl[s] << (6 - 2 * k); b[1 + k] = frm[s]; }
            }
            b[5] = used ? 1 : 0;                 /* repeat count: 1 = run once */
        } else {
            b[0] = used ? (uint8_t)group_repeat : 0;   /* 0 = run once, no repeat */
            for (int k = 0; k < per_group; k++) {
                int s = base + k;
                if (s < ns) b[1 + k] = (lvl[s] << 6) | frm[s];
            }
        }
    }
    return groups * gbytes;
}

/* The black/white transition waveform. Pixel codes in a fast frame:
     0 = stays black   1 = stays white   2 = white -> black   3 = black -> white
   Two phases. Black phase: a strong pulse on code 2 only. White phase: a weak
   field of the opposite sign on code 3 only; every other pixel and VCOM move
   together so they see 0 V. DC balance over a black/white round trip wants
   15 V x black_frames = 3 V x white_frames, i.e. white_frames = 5 x black.  */
typedef struct {
    int format;        /* 1 or 2 */
    int group_repeat;  /* format 1 only: byte 0 of a used group */
    int polarity;      /* 1: black = VSP, white = weak negative (E Ink patent)
                          2: black = VSN, white = weak positive              */
    int white_scheme;  /* polarity 1 only. 1: VCOM = VSPL (3 V), source 0 V
                                           2: VCOM = VSP, source = VSPL (12 V) */
    int black_frames;
    int white_frames;
} g_fast_cfg_t;

/* VSPL rail voltage code for the scheme (3.0 V = 0x00, +0.1 V per step). */
static inline uint8_t g_fast_vspl_code(const g_fast_cfg_t *c)
{
    return (c->polarity == 1 && c->white_scheme == 2) ? 0x5A /* 12.0 V */ : 0x00 /* 3.0 V */;
}

/* Fill the VCOM phases and the four code phases (2 phases each). */
static inline void g_fast_phases(const g_fast_cfg_t *c, g_phase_t vcom[2], g_phase_t code[4][2])
{
    uint8_t nb = (uint8_t)c->black_frames, nw = (uint8_t)c->white_frames;
    uint8_t blk_lvl, w_vcom, w_idle, w_act;
    if (c->polarity == 1) {
        blk_lvl = G_LVL_VSP;
        if (c->white_scheme == 2) { w_vcom = G_LVL_VSP;  w_idle = G_LVL_VSP;  w_act = G_LVL_VSPL; }
        else                      { w_vcom = G_LVL_VSPL; w_idle = G_LVL_VSPL; w_act = G_LVL_VCOM; }
    } else {
        blk_lvl = G_LVL_VSN;
        w_vcom = G_LVL_VCOM; w_idle = G_LVL_VCOM; w_act = G_LVL_VSPL;
    }
    vcom[0]    = (g_phase_t){G_LVL_VCOM, nb};  vcom[1]    = (g_phase_t){w_vcom, nw};
    code[0][0] = (g_phase_t){G_LVL_VCOM, nb};  code[0][1] = (g_phase_t){w_idle, nw};   /* stays black */
    code[1][0] = (g_phase_t){G_LVL_VCOM, nb};  code[1][1] = (g_phase_t){w_idle, nw};   /* stays white */
    code[2][0] = (g_phase_t){blk_lvl,    nb};  code[2][1] = (g_phase_t){w_idle, nw};   /* -> black    */
    code[3][0] = (g_phase_t){G_LVL_VCOM, nb};  code[3][1] = (g_phase_t){w_act,  nw};   /* -> white    */
}
#endif
