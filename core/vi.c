#include "vi.h"
#include "rdp.h"
#include "common.h"
#include "plugin.h"
#include "screen.h"
#include "rdram.h"
#include "trace_write.h"
#include "msg.h"
#include "irand.h"
#include "file.h"
#include "bitmap.h"
#include "parallel_c.hpp"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <memory.h>
#include <assert.h>

// anamorphic NTSC resolution
#define H_RES_NTSC 640
#define V_RES_NTSC 480

// anamorphic PAL resolution
#define H_RES_PAL 768
#define V_RES_PAL 576

// typical VI_V_SYNC values for NTSC and PAL
#define V_SYNC_NTSC 525
#define V_SYNC_PAL 625

// maximum possible size of the prescale area
#define PRESCALE_WIDTH H_RES_NTSC
#define PRESCALE_HEIGHT V_SYNC_PAL

// enable output of the normally not visible overscan area (adds black borders)
#define ENABLE_OVERSCAN 0

// enable TV fading emulation
#define ENABLE_TVFADEOUT 0

enum vi_type
{
    VI_TYPE_BLANK,      // no data, no sync
    VI_TYPE_RESERVED,   // unused, should never be set
    VI_TYPE_RGBA5551,   // 16 bit color (internally 18 bit RGBA5553)
    VI_TYPE_RGBA8888    // 32 bit color
};

enum vi_aa
{
    VI_AA_RESAMP_EXTRA_ALWAYS,  // resample and AA (always fetch extra lines)
    VI_AA_RESAMP_EXTRA,         // resample and AA (fetch extra lines if needed)
    VI_AA_RESAMP_ONLY,          // only resample (treat as all fully covered)
    VI_AA_REPLICATE             // replicate pixels, no interpolation
};

union vi_reg_ctrl
{
    struct {
        uint32_t type : 2;
        uint32_t gamma_dither_enable : 1;
        uint32_t gamma_enable : 1;
        uint32_t divot_enable : 1;
        uint32_t vbus_clock_enable : 1;
        uint32_t serrate : 1;
        uint32_t test_mode : 1;
        uint32_t aa_mode : 2;
        uint32_t reserved : 1;
        uint32_t kill_we : 1;
        uint32_t pixel_advance : 4;
        uint32_t dither_filter_enable : 1;
    };
    uint32_t raw;
};

struct ccvg
{
    uint8_t r, g, b, cvg;
};

#include "vi/gamma.c"
#include "vi/lerp.c"
#include "vi/divot.c"
#include "vi/video.c"
#include "vi/restore.c"
#include "vi/fetch.c"

// config
static struct core_config* config;

// states
static uint32_t prevvicurrent;
static int emucontrolsvicurrent;
static int prevserrate;
static int oldlowerfield;
static int32_t oldvstart;
static uint32_t prevwasblank;
#if ENABLE_TVFADEOUT
static uint32_t tvfadeoutstate[PRESCALE_HEIGHT];
#endif
static int vactivelines;
static int ispal;
static int minhpass;
static int maxhpass;
static uint32_t x_add;
static uint32_t x_start_init;
static uint32_t y_add;
static uint32_t y_start;
static int32_t v_sync;
static int vi_width_low;
static uint32_t frame_buffer;

static char screenshot_path[FILE_MAX_PATH];
static enum vi_mode vi_mode;

// prescale buffer
static int32_t prescale[PRESCALE_WIDTH * PRESCALE_HEIGHT];
static uint32_t prescale_ptr;
static int linecount;

// parsed VI registers
static union vi_reg_ctrl ctrl;
static int32_t hres, vres;
static int32_t hres_raw, vres_raw;
static int32_t v_start;
static int32_t h_start;

static struct
{
    int nolerp, vbusclock;
} onetimewarnings;

static void vi_screenshot_write(char* path, int32_t* buffer, int width, int height, int pitch, int output_height)
{
    msg_debug("screen: writing screenshot to '%s'", path);

    // prepare bitmap headers
    struct bitmap_info_header ihdr = {0};
    ihdr.size = sizeof(ihdr);
    ihdr.width = width;
    ihdr.height = output_height;
    ihdr.planes = 1;
    ihdr.bit_count = 32;
    ihdr.size_image = width * output_height * sizeof(int32_t);

    struct bitmap_file_header fhdr = {0};
    fhdr.type = 'B' | ('M' << 8);
    fhdr.off_bits = sizeof(fhdr) + sizeof(ihdr) + 10;
    fhdr.size = ihdr.size_image + fhdr.off_bits;

    FILE* fp = fopen(path, "wb");

    if (!fp) {
        msg_warning("Can't open screenshot file %s!", path);
        return;
    }

    // write bitmap headers
    fwrite(&fhdr, sizeof(fhdr), 1, fp);
    fwrite(&ihdr, sizeof(ihdr), 1, fp);

    // write bitmap contents
    fseek(fp, fhdr.off_bits, SEEK_SET);

    // check if interpolation is required and copy lines to bitmap
    if (height != output_height) {
        // nearest-neighbor mode
        for (int32_t y = output_height - 1; y >= 0; y--) {
            int iy = y * height / output_height;
            fwrite(buffer + pitch * iy, width * sizeof(int32_t), 1, fp);
        }
    } else {
        // direct mode
        for (int32_t y = height - 1; y >= 0; y--) {
            fwrite(buffer + pitch * y, width * sizeof(int32_t), 1, fp);
        }
    }

    fclose(fp);
}

void vi_init(struct core_config* _config)
{
    config = _config;

    vi_gamma_init();
    vi_restore_init();

    memset(prescale, 0, sizeof(prescale));
    vi_mode = VI_MODE_NORMAL;

    prevvicurrent = 0;
    emucontrolsvicurrent = -1;
    prevserrate = 0;
    oldlowerfield = 0;
    oldvstart = 1337;
    prevwasblank = 0;
}

static int vi_process_start(void)
{
    uint32_t final = 0;

    uint32_t** vi_reg_ptr = plugin_get_vi_registers();

    v_start = (*vi_reg_ptr[VI_V_START] >> 16) & 0x3ff;
    h_start = (*vi_reg_ptr[VI_H_START] >> 16) & 0x3ff;

    int32_t v_end = *vi_reg_ptr[VI_V_START] & 0x3ff;
    int32_t h_end = *vi_reg_ptr[VI_H_START] & 0x3ff;

    hres =  h_end - h_start;
    vres = (v_end - v_start) >> 1; // vertical is measured in half-lines

    ctrl.raw = *vi_reg_ptr[VI_STATUS];

    // check for unexpected VI type bits set
    if (ctrl.type & ~3) {
        msg_error("Unknown framebuffer format %d", ctrl.type);
    }

    if (ctrl.vbus_clock_enable && !onetimewarnings.vbusclock)
    {
        msg_warning("rdp_update: vbus_clock_enable bit set in VI_CONTROL_REG register. Never run this code on your N64! It's rumored that turning this bit on\
                    will result in permanent damage to the hardware! Emulation will now continue.");
        onetimewarnings.vbusclock = 1;
    }

    vi_fetch_filter_ptr = vi_fetch_filter_func[ctrl.type & 1];

    v_sync = *vi_reg_ptr[VI_V_SYNC] & 0x3ff;
    x_add = *vi_reg_ptr[VI_X_SCALE] & 0xfff;

    if (ctrl.aa_mode == VI_AA_REPLICATE && ctrl.type == VI_TYPE_RGBA5551 && !onetimewarnings.nolerp && h_start < 0x80 && x_add <= 0x200)
    {
        msg_warning("Disabling VI interpolation in 16-bit color modes causes glitches on hardware if h_start is less than 128 pixels and x_scale is less or equal to 0x200.");
        onetimewarnings.nolerp = 1;
    }

    ispal = v_sync > (V_SYNC_NTSC + 25);
    h_start -= (ispal ? 128 : 108);

    x_start_init = (*vi_reg_ptr[VI_X_SCALE] >> 16) & 0xfff;

    int h_start_clamped = 0;

    if (h_start < 0)
    {
        x_start_init += (x_add * (-h_start));
        hres += h_start;

        h_start = 0;
        h_start_clamped = 1;
    }

    int validinterlace = (ctrl.type & 2) && ctrl.serrate;
    if (validinterlace && prevserrate && emucontrolsvicurrent < 0)
        emucontrolsvicurrent = (*vi_reg_ptr[VI_V_CURRENT_LINE] & 1) != prevvicurrent;

    int lowerfield = 0;
    if (validinterlace)
    {
        if (emucontrolsvicurrent == 1)
            lowerfield = (*vi_reg_ptr[VI_V_CURRENT_LINE] & 1) ^ 1;
        else if (!emucontrolsvicurrent)
        {
            if (v_start == oldvstart)
                lowerfield = oldlowerfield ^ 1;
            else
                lowerfield = v_start < oldvstart;
        }
    }

    oldlowerfield = lowerfield;

    if (validinterlace)
    {
        prevserrate = 1;
        prevvicurrent = *vi_reg_ptr[VI_V_CURRENT_LINE] & 1;
        oldvstart = v_start;
    }
    else
        prevserrate = 0;

    uint32_t lineshifter = !ctrl.serrate;

    int32_t vstartoffset = ispal ? 44 : 34;
    v_start = (v_start - vstartoffset) / 2;

    y_start = (*vi_reg_ptr[VI_Y_SCALE] >> 16) & 0xfff;
    y_add = *vi_reg_ptr[VI_Y_SCALE] & 0xfff;

    if (v_start < 0)
    {
        y_start += (y_add * (uint32_t)(-v_start));
        v_start = 0;
    }

    int hres_clamped = 0;

    if ((hres + h_start) > PRESCALE_WIDTH)
    {
        hres = PRESCALE_WIDTH - h_start;
        hres_clamped = 1;
    }

    if ((vres + v_start) > PRESCALE_HEIGHT)
    {
        vres = PRESCALE_HEIGHT - v_start;
        msg_warning("vres = %d v_start = %d v_video_start = %d", vres, v_start, (*vi_reg_ptr[VI_V_START] >> 16) & 0x3ff);
    }

    h_end = hres + h_start; // note: the result appears to be different to VI_H_START
    int32_t hrightblank = PRESCALE_WIDTH - h_end;

    vactivelines = v_sync - vstartoffset;
    if (vactivelines > PRESCALE_HEIGHT)
        msg_error("VI_V_SYNC_REG too big");
    if (vactivelines < 0)
        return 0;
    vactivelines >>= lineshifter;

    int validh = (hres > 0 && h_start < PRESCALE_WIDTH);

    uint32_t pix = 0;
    uint8_t cur_cvg = 0;

    int32_t *d = 0;

    minhpass = h_start_clamped ? 0 : 8;
    maxhpass =  hres_clamped ? hres : (hres - 7);

    if (!(ctrl.type & 2) && prevwasblank)
    {
        return 0;
    }

    linecount = ctrl.serrate ? (PRESCALE_WIDTH << 1) : PRESCALE_WIDTH;
    prescale_ptr = v_start * linecount + h_start + (lowerfield ? PRESCALE_WIDTH : 0);

    vi_width_low = *vi_reg_ptr[VI_WIDTH] & 0xfff;
    frame_buffer = *vi_reg_ptr[VI_ORIGIN] & 0xffffff;

    if (!frame_buffer) {
        return 0;
    }

#if ENABLE_TVFADEOUT
    int i;
    if (!(ctrl.type & 2))
    {
        memset(tvfadeoutstate, 0, PRESCALE_HEIGHT * sizeof(uint32_t));
        for (i = 0; i < PRESCALE_HEIGHT; i++)
            memset(&prescale[i * PRESCALE_WIDTH], 0, PRESCALE_WIDTH * sizeof(int32_t));
        prevwasblank = 1;
    }
    else
    {
        prevwasblank = 0;

        int j;
        if (h_start > 0 && h_start < PRESCALE_WIDTH)
        {
            for (i = 0; i < vactivelines; i++)
                memset(&prescale[i * PRESCALE_WIDTH], 0, h_start * sizeof(uint32_t));
        }
        if (h_end >= 0 && h_end < PRESCALE_WIDTH)
        {
            for (i = 0; i < vactivelines; i++)
                memset(&prescale[i * PRESCALE_WIDTH + h_end], 0, hrightblank * sizeof(uint32_t));
        }

        for (i = 0; i < ((v_start << ctrl.serrate) + lowerfield); i++)
        {
            if (tvfadeoutstate[i])
            {
                tvfadeoutstate[i]--;
                if (!tvfadeoutstate[i])
                {
                    if (validh)
                        memset(&prescale[i * PRESCALE_WIDTH + h_start], 0, hres * sizeof(uint32_t));
                    else
                        memset(&prescale[i * PRESCALE_WIDTH], 0, PRESCALE_WIDTH * sizeof(uint32_t));
                }
            }
        }
        if (!ctrl.serrate)
        {
            for(j = 0; j < vres; j++)
            {
                if (validh)
                    tvfadeoutstate[i] = 2;
                else if (tvfadeoutstate[i])
                {
                    tvfadeoutstate[i]--;
                    if (!tvfadeoutstate[i])
                    {
                        memset(&prescale[i * PRESCALE_WIDTH], 0, PRESCALE_WIDTH * sizeof(uint32_t));
                    }
                }

                i++;
            }
        }
        else
        {
            for(j = 0; j < vres; j++)
            {
                if (validh)
                    tvfadeoutstate[i] = 2;
                else if (tvfadeoutstate[i])
                {
                    tvfadeoutstate[i]--;
                    if (!tvfadeoutstate[i])
                        memset(&prescale[i * PRESCALE_WIDTH], 0, PRESCALE_WIDTH * sizeof(uint32_t));
                }

                if (tvfadeoutstate[i + 1])
                {
                    tvfadeoutstate[i + 1]--;
                    if (!tvfadeoutstate[i + 1])
                        if (validh)
                            memset(&prescale[(i + 1) * PRESCALE_WIDTH + h_start], 0, hres * sizeof(uint32_t));
                        else
                            memset(&prescale[(i + 1) * PRESCALE_WIDTH], 0, PRESCALE_WIDTH * sizeof(uint32_t));
                }

                i += 2;
            }
        }
        for (; i < vactivelines; i++)
        {
            if (tvfadeoutstate[i])
                tvfadeoutstate[i]--;
            if (!tvfadeoutstate[i])
                if (validh)
                    memset(&prescale[i * PRESCALE_WIDTH + h_start], 0, hres * sizeof(uint32_t));
                else
                    memset(&prescale[i * PRESCALE_WIDTH], 0, PRESCALE_WIDTH * sizeof(uint32_t));
        }
    }
#endif

    return validh;
}

static void vi_process(void)
{
    struct ccvg viaa_array[0xa10 << 1];
    struct ccvg divot_array[0xa10 << 1];

    int cache_marker = 0, cache_next_marker = 0, divot_cache_marker = 0, divot_cache_next_marker = 0;
    int cache_marker_init = (x_start_init >> 10) - 1;

    struct ccvg *viaa_cache = &viaa_array[0];
    struct ccvg *viaa_cache_next = &viaa_array[0xa10];
    struct ccvg *divot_cache = &divot_array[0];
    struct ccvg *divot_cache_next = &divot_array[0xa10];

    struct ccvg color, nextcolor, scancolor, scannextcolor;

    uint32_t pixels = 0, nextpixels = 0, fetchbugstate = 0;

    int r = 0, g = 0, b = 0;
    int xfrac = 0, yfrac = 0;
    int line_x = 0, next_line_x = 0, prev_line_x = 0, far_line_x = 0;
    int prev_scan_x = 0, scan_x = 0, next_scan_x = 0, far_scan_x = 0;
    int prev_x = 0, cur_x = 0, next_x = 0, far_x = 0;

    bool cache_init = false;

    pixels = 0;

    int32_t j_start = 0;
    int32_t j_end = vres;
    int32_t j_add = 1;

    if (config->num_workers != 1) {
        j_start = parallel_worker_id();
        j_add = parallel_worker_num();
    }

    for (int32_t j = j_start; j < j_end; j += j_add) {
        uint32_t x_start = x_start_init;
        uint32_t curry = y_start + j * y_add;
        uint32_t nexty = y_start + (j + 1) * y_add;
        uint32_t prevy = curry >> 10;

        cache_marker = cache_next_marker = cache_marker_init;
        if (ctrl.divot_enable)
            divot_cache_marker = divot_cache_next_marker = cache_marker_init;

        int* d = prescale + prescale_ptr + linecount * j;

        yfrac = (curry >> 5) & 0x1f;
        pixels = vi_width_low * prevy;
        nextpixels = vi_width_low + pixels;

        if (prevy == (nexty >> 10))
            fetchbugstate = 2;
        else
            fetchbugstate >>= 1;

        for (int i = 0; i < hres; i++, x_start += x_add)
        {
            line_x = x_start >> 10;
            prev_line_x = line_x - 1;
            next_line_x = line_x + 1;
            far_line_x = line_x + 2;

            cur_x = pixels + line_x;
            prev_x = pixels + prev_line_x;
            next_x = pixels + next_line_x;
            far_x = pixels + far_line_x;

            scan_x = nextpixels + line_x;
            prev_scan_x = nextpixels + prev_line_x;
            next_scan_x = nextpixels + next_line_x;
            far_scan_x = nextpixels + far_line_x;

            line_x++;
            prev_line_x++;
            next_line_x++;
            far_line_x++;

            xfrac = (x_start >> 5) & 0x1f;

            int lerping = ctrl.aa_mode != VI_AA_REPLICATE && (xfrac || yfrac);

            if (prev_line_x > cache_marker)
            {
                vi_fetch_filter_ptr(&viaa_cache[prev_line_x], frame_buffer, prev_x, ctrl, vi_width_low, 0);
                vi_fetch_filter_ptr(&viaa_cache[line_x], frame_buffer, cur_x, ctrl, vi_width_low, 0);
                vi_fetch_filter_ptr(&viaa_cache[next_line_x], frame_buffer, next_x, ctrl, vi_width_low, 0);
                cache_marker = next_line_x;
            }
            else if (line_x > cache_marker)
            {
                vi_fetch_filter_ptr(&viaa_cache[line_x], frame_buffer, cur_x, ctrl, vi_width_low, 0);
                vi_fetch_filter_ptr(&viaa_cache[next_line_x], frame_buffer, next_x, ctrl, vi_width_low, 0);
                cache_marker = next_line_x;
            }
            else if (next_line_x > cache_marker)
            {
                vi_fetch_filter_ptr(&viaa_cache[next_line_x], frame_buffer, next_x, ctrl, vi_width_low, 0);
                cache_marker = next_line_x;
            }

            if (prev_line_x > cache_next_marker)
            {
                vi_fetch_filter_ptr(&viaa_cache_next[prev_line_x], frame_buffer, prev_scan_x, ctrl, vi_width_low, fetchbugstate);
                vi_fetch_filter_ptr(&viaa_cache_next[line_x], frame_buffer, scan_x, ctrl, vi_width_low, fetchbugstate);
                vi_fetch_filter_ptr(&viaa_cache_next[next_line_x], frame_buffer, next_scan_x, ctrl, vi_width_low, fetchbugstate);
                cache_next_marker = next_line_x;
            }
            else if (line_x > cache_next_marker)
            {
                vi_fetch_filter_ptr(&viaa_cache_next[line_x], frame_buffer, scan_x, ctrl, vi_width_low, fetchbugstate);
                vi_fetch_filter_ptr(&viaa_cache_next[next_line_x], frame_buffer, next_scan_x, ctrl, vi_width_low, fetchbugstate);
                cache_next_marker = next_line_x;
            }
            else if (next_line_x > cache_next_marker)
            {
                vi_fetch_filter_ptr(&viaa_cache_next[next_line_x], frame_buffer, next_scan_x, ctrl, vi_width_low, fetchbugstate);
                cache_next_marker = next_line_x;
            }

            if (ctrl.divot_enable)
            {
                if (far_line_x > cache_marker)
                {
                    vi_fetch_filter_ptr(&viaa_cache[far_line_x], frame_buffer, far_x, ctrl, vi_width_low, 0);
                    cache_marker = far_line_x;
                }

                if (far_line_x > cache_next_marker)
                {
                    vi_fetch_filter_ptr(&viaa_cache_next[far_line_x], frame_buffer, far_scan_x, ctrl, vi_width_low, fetchbugstate);
                    cache_next_marker = far_line_x;
                }

                if (line_x > divot_cache_marker)
                {
                    divot_filter(&divot_cache[line_x], viaa_cache[line_x], viaa_cache[prev_line_x], viaa_cache[next_line_x]);
                    divot_filter(&divot_cache[next_line_x], viaa_cache[next_line_x], viaa_cache[line_x], viaa_cache[far_line_x]);
                    divot_cache_marker = next_line_x;
                }
                else if (next_line_x > divot_cache_marker)
                {
                    divot_filter(&divot_cache[next_line_x], viaa_cache[next_line_x], viaa_cache[line_x], viaa_cache[far_line_x]);
                    divot_cache_marker = next_line_x;
                }

                if (line_x > divot_cache_next_marker)
                {
                    divot_filter(&divot_cache_next[line_x], viaa_cache_next[line_x], viaa_cache_next[prev_line_x], viaa_cache_next[next_line_x]);
                    divot_filter(&divot_cache_next[next_line_x], viaa_cache_next[next_line_x], viaa_cache_next[line_x], viaa_cache_next[far_line_x]);
                    divot_cache_next_marker = next_line_x;
                }
                else if (next_line_x > divot_cache_next_marker)
                {
                    divot_filter(&divot_cache_next[next_line_x], viaa_cache_next[next_line_x], viaa_cache_next[line_x], viaa_cache_next[far_line_x]);
                    divot_cache_next_marker = next_line_x;
                }

                color = divot_cache[line_x];
            }
            else
            {
                color = viaa_cache[line_x];
            }

            if (lerping)
            {
                if (ctrl.divot_enable)
                {
                    nextcolor = divot_cache[next_line_x];
                    scancolor = divot_cache_next[line_x];
                    scannextcolor = divot_cache_next[next_line_x];
                }
                else
                {
                    nextcolor = viaa_cache[next_line_x];
                    scancolor = viaa_cache_next[line_x];
                    scannextcolor = viaa_cache_next[next_line_x];
                }

                vi_vl_lerp(&color, scancolor, yfrac);
                vi_vl_lerp(&nextcolor, scannextcolor, yfrac);
                vi_vl_lerp(&color, nextcolor, xfrac);
            }

            r = color.r;
            g = color.g;
            b = color.b;

            gamma_filters(&r, &g, &b, ctrl);

            if (i >= minhpass && i < maxhpass)
                d[i] = (r << 16) | (g << 8) | b;
            else
                d[i] = 0;
        }

        if (!cache_init && y_add == 0x400) {
            cache_marker = cache_next_marker;
            cache_next_marker = cache_marker_init;

            struct ccvg* tempccvgptr = viaa_cache;
            viaa_cache = viaa_cache_next;
            viaa_cache_next = tempccvgptr;
            if (ctrl.divot_enable)
            {
                divot_cache_marker = divot_cache_next_marker;
                divot_cache_next_marker = cache_marker_init;
                tempccvgptr = divot_cache;
                divot_cache = divot_cache_next;
                divot_cache_next = tempccvgptr;
            }

            cache_init = true;
        }
    }
}

static void vi_process_end(void)
{
    int32_t pitch = PRESCALE_WIDTH;

#if ENABLE_OVERSCAN
    // use entire prescale buffer
    int32_t width = PRESCALE_WIDTH;
    int32_t height = (ispal ? V_RES_PAL : V_RES_NTSC) >> !ctrl.serrate;
    int32_t output_height = V_RES_NTSC;
    int32_t* buffer = prescale;
#else
    // crop away overscan area from prescale
    int32_t width = maxhpass - minhpass;
    int32_t height = vres << ctrl.serrate;
    int32_t output_height = (vres << 1) * V_SYNC_NTSC / v_sync;
    int32_t x = h_start + minhpass;
    int32_t y = (v_start + oldlowerfield) << ctrl.serrate;
    int32_t* buffer = prescale + x + y * pitch;
#endif

    if (config->vi.widescreen) {
         output_height = output_height * 9 / 16;
    }

    screen_upload(buffer, width, height, pitch, output_height);

    if (screenshot_path[0]) {
        vi_screenshot_write(screenshot_path, buffer, width, height, pitch, output_height);
        screenshot_path[0] = 0;
    }
}

static int vi_process_start_fast(void)
{
    // note: this is probably a very, very crude method to get the frame size,
    // but should hopefully work most of the time
    uint32_t** vi_reg_ptr = plugin_get_vi_registers();

    int32_t v_start = (*vi_reg_ptr[VI_V_START] >> 16) & 0x3ff;
    int32_t h_start = (*vi_reg_ptr[VI_H_START] >> 16) & 0x3ff;

    int32_t v_end = *vi_reg_ptr[VI_V_START] & 0x3ff;
    int32_t h_end = *vi_reg_ptr[VI_H_START] & 0x3ff;

    hres =  h_end - h_start;
    vres = (v_end - v_start) >> 1; // vertical is measured in half-lines

    if (hres <= 0 || vres <= 0) {
        return 0;
    }

    x_add = *vi_reg_ptr[VI_X_SCALE] & 0xfff;
    y_add = *vi_reg_ptr[VI_Y_SCALE] & 0xfff;

    hres_raw = x_add * hres / 1024;
    vres_raw = y_add * vres / 1024;

    if (hres_raw <= 0 || vres_raw <= 0) {
        return 0;
    }

    // drop every other interlaced frame to avoid "wobbly" output due to the
    // vertical offset
    // TODO: completely skip rendering these frames in unfiltered to improve
    // performance?
    if (*vi_reg_ptr[VI_V_CURRENT_LINE] & 1) {
        return 0;
    }

    vi_width_low = *vi_reg_ptr[VI_WIDTH] & 0xfff;
    frame_buffer = *vi_reg_ptr[VI_ORIGIN] & 0xffffff;

    if (!frame_buffer) {
        return 0;
    }

    ctrl.raw = *vi_reg_ptr[VI_STATUS];

    v_sync = *vi_reg_ptr[VI_V_SYNC] & 0x3ff;

    // skip blank/invalid modes
    if (!(ctrl.type & 2)) {
        return 0;
    }

    // check for unexpected VI type bits set
    if (ctrl.type & ~3) {
        msg_error("Unknown framebuffer format %d", ctrl.type);
    }

    return 1;
}

static void vi_process_fast(void)
{
    int32_t y_start = 0;
    int32_t y_end = vres_raw;
    int32_t y_add = 1;

    if (config->num_workers != 1) {
        y_start = parallel_worker_id();
        y_add = parallel_worker_num();
    }

    for (int32_t y = y_start; y < y_end; y += y_add) {
        int32_t line = y * vi_width_low;
        uint32_t* dst = prescale + y * hres_raw;

        for (int32_t x = 0; x < hres_raw; x++) {
            uint32_t r, g, b;

            switch (config->vi.mode) {
                case VI_MODE_COLOR:
                    switch (ctrl.type) {
                        case VI_TYPE_RGBA5551: {
                            uint16_t pix = rdram_read_idx16((frame_buffer >> 1) + line + x);
                            r = ((pix >> 11) & 0x1f) << 3;
                            g = ((pix >>  6) & 0x1f) << 3;
                            b = ((pix >>  1) & 0x1f) << 3;
                            break;
                        }

                        case VI_TYPE_RGBA8888: {
                            uint32_t pix = rdram_read_idx32((frame_buffer >> 2) + line + x);
                            r = (pix >> 24) & 0xff;
                            g = (pix >> 16) & 0xff;
                            b = (pix >>  8) & 0xff;
                            break;
                        }

                        default:
                            assert(false);
                    }
                    break;

                case VI_MODE_DEPTH: {
                    r = g = b = rdram_read_idx16((rdp_get_zb_address() >> 1) + line + x) >> 8;
                    break;
                }

                case VI_MODE_COVERAGE: {
                    // TODO: incorrect for RGBA8888?
                    uint8_t hval;
                    uint16_t pix;
                    rdram_read_pair16(&pix, &hval, (frame_buffer >> 1) + line + x);
                    r = g = b = (((pix & 1) << 2) | hval) << 5;
                    break;
                }

                default:
                    assert(false);
            }

            gamma_filters(&r, &g, &b, ctrl);

            dst[x] = (r << 16) | (g << 8) | b;
        }
    }
}

static void vi_process_end_fast(void)
{
    int32_t filtered_height = (vres << 1) * V_SYNC_NTSC / v_sync;
    int32_t output_height = hres_raw * filtered_height / hres;

    if (config->vi.widescreen) {
        output_height = output_height * 9 / 16;
    }

    screen_upload(prescale, hres_raw, vres_raw, hres_raw, output_height);

    if (screenshot_path[0]) {
        vi_screenshot_write(screenshot_path, prescale, hres_raw, vres_raw, hres_raw, output_height);
        screenshot_path[0] = 0;
    }
}

void vi_update(void)
{
    // clear buffer after switching VI modes to make sure that black borders are
    // actually black and don't contain garbage
    if (config->vi.mode != vi_mode) {
        memset(prescale, 0, sizeof(prescale));
        vi_mode = config->vi.mode;
    }

    if (trace_write_is_open()) {
        trace_write_vi(plugin_get_vi_registers());
    }

    // select filter functions based on config
    int (*vi_process_start_ptr)(void);
    void (*vi_process_ptr)(void);
    void (*vi_process_end_ptr)(void);

    // check for configuration errors
    if (config->vi.mode >= VI_MODE_NUM) {
        msg_error("Invalid VI mode: %d", config->vi.mode);
    }

    if (config->vi.mode == VI_MODE_NORMAL) {
        vi_process_start_ptr = vi_process_start;
        vi_process_ptr = vi_process;
        vi_process_end_ptr = vi_process_end;
    } else {
        vi_process_start_ptr = vi_process_start_fast;
        vi_process_ptr = vi_process_fast;
        vi_process_end_ptr = vi_process_end_fast;
    }

    // try to init VI frame, abort if there's nothing to display
    if (!vi_process_start_ptr()) {
        return;
    }

    // run filter update in parallel if enabled
    if (config->num_workers != 1) {
        parallel_run(vi_process_ptr);
    } else {
        vi_process_ptr();
    }

    // finish and send buffer to screen
    vi_process_end_ptr();

    // render frame to screen
    screen_swap();
}

void vi_screenshot(char* path)
{
    strcpy(screenshot_path, path);
}

void vi_close(void)
{
}
