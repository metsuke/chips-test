/*
    bombjack.c

    Bomb Jack arcade machine emulation.
*/
#include "common.h"
#define CHIPS_IMPL
#include "chips/z80.h"
#include "chips/ay38910.h"
#include "chips/crt.h"
#include "chips/clk.h"
#include "chips/mem.h"
#include "bombjack-roms.h"

// DBG ONLY!
#include <stdio.h>

static void app_init(void);
static void app_frame(void);
static void app_input(const sapp_event*);
static void app_cleanup(void);
static void bombjack_init(void);
static void bombjack_exec(uint32_t micro_seconds);
static void bombjack_ay_out(int port_id, uint8_t data, void* user_data);
static uint8_t bombjack_ay_in(int port_id, void* user_data);

static uint64_t bombjack_tick_main(int num, uint64_t pins, void* user_data);
static uint64_t bombjack_tick_sound(int num, uint64_t pins, void* user_data);
static void bombjack_ay_out(int port_id, uint8_t data, void* user_data);
static uint8_t bombjack_ay_in(int port_id, void* user_data);
static void bombjack_decode_video(void);

#define DISPLAY_WIDTH (32*8)
#define DISPLAY_HEIGHT (32*8)

/* the Bomb Jack arcade machine is actually 2 computers, the main board and sound board */
typedef struct {
    z80_t cpu;
    clk_t clk;
    uint8_t p1;         /* joystick 1 state */
    uint8_t nmi_mask;
    uint8_t p2;         /* joystick 2 state */
    uint8_t sys;        /* coins and start buttons */
    uint8_t dsw1;       /* dip-switches 1 */
    uint8_t dsw2;       /* dip-switches 2 */
    mem_t mem;
    uint8_t ram[0x2000];
} mainboard_t;

typedef struct {
    z80_t cpu;
    clk_t clk;
    ay38910_t ay[3];
    mem_t mem;
    uint8_t ram[0x0400];
} soundboard_t;

typedef struct {
    mainboard_t main;
    soundboard_t sound;
    mem_t mem_chars;
    mem_t mem_tiles;
    mem_t mem_sprites;
    mem_t mem_maps;
} bombjack_t;
bombjack_t bj;

sapp_desc sokol_main(int argc, char* argv[]) {
    args_init(argc, argv);
    return (sapp_desc) {
        .init_cb = app_init,
        .frame_cb = app_frame,
        .event_cb = app_input,
        .cleanup_cb = app_cleanup,
        .width = DISPLAY_WIDTH * 2,
        .height = DISPLAY_HEIGHT * 2,
        .window_title = "Bomb Jack"
    };
}

/* one time app init */
void app_init(void) {
    gfx_init(DISPLAY_WIDTH, DISPLAY_HEIGHT, 3, 4);
    clock_init();
    saudio_setup(&(saudio_desc){0});
    bombjack_init();
}

/* per-frame stuff */
void app_frame(void) {
    bombjack_exec(clock_frame_time());
    gfx_draw();
}

/* input handling */
void app_input(const sapp_event* event) {
    // FIXME
}

/* app shutdown */
void app_cleanup(void) {
    saudio_shutdown();
    gfx_shutdown();
}

/* initialize the Bombjack arcade hardware */
void bombjack_init(void) {
    memset(&bj, 0, sizeof(bj));

    /* setup the main board (4 MHz Z80) */
    clk_init(&bj.main.clk, 4000000);
    z80_init(&bj.main.cpu, &(z80_desc_t){
        .tick_cb = bombjack_tick_main
    });

    /* setup the sound board (3 MHz Z80, 3x 1.5 MHz AY-38910) */
    clk_init(&bj.sound.clk, 3000000);
    z80_init(&bj.sound.cpu, &(z80_desc_t){
        .tick_cb = bombjack_tick_sound
    });
    for (int i = 0; i < 3; i++) {
        ay38910_init(&bj.sound.ay[i], &(ay38910_desc_t) {
            .type = AY38910_TYPE_8910,
            .in_cb = bombjack_ay_in,
            .out_cb = bombjack_ay_out,
            .tick_hz = 1500000,
            .sound_hz = saudio_sample_rate(),
            .magnitude = 0.3f,
        });
    }

    /* dip switches */
    bj.main.dsw1 = (1<<6)|(1<<7); /* UPRIGHT|DEMO SOUND */
    bj.main.dsw2 = 0;
    
    /* main board memory map:
        0000..7FFF: ROM
        8000..8FFF: RAM
        9000..93FF: video ram
        9400..97FF: color ram
        9820..987F: sprite ram
        9C00..9CFF: palette
        9E00:       select background
        B000:       read: joystick 1, write: NMI mask
        B001:       read: joystick 2
        B002:       read: coins and start button
        B003:       ???
        B004:       read: dip-switches 1, write: flip screen
        B005:       read: dip-switches 2
        B800:       sound latch
        C000..DFFF: ROM
    */
    mem_init(&bj.main.mem);
    mem_map_rom(&bj.main.mem, 0, 0x0000, 0x2000, dump_09_j01b);
    mem_map_rom(&bj.main.mem, 0, 0x2000, 0x2000, dump_10_l01b);
    mem_map_rom(&bj.main.mem, 0, 0x4000, 0x2000, dump_11_m01b);
    mem_map_rom(&bj.main.mem, 0, 0x6000, 0x2000, dump_12_n01b);
    mem_map_ram(&bj.main.mem, 0, 0x8000, 0x2000, bj.main.ram);
    mem_map_rom(&bj.main.mem, 0, 0xC000, 0x2000, dump_13);

    /* sound board memory map */
    mem_init(&bj.sound.mem);
    mem_map_rom(&bj.sound.mem, 0, 0x0000, 0x2000, dump_01_h03t);
    mem_map_ram(&bj.sound.mem, 0, 0x4000, 0x0400, bj.sound.ram);

    /* various ROMs */
    mem_init(&bj.mem_chars); /* 512 char * 3 bitplanes * 8 bytes = 12 KB */
    mem_map_rom(&bj.mem_chars, 0, 0x0000, 0x1000, dump_03_e08t);
    mem_map_rom(&bj.mem_chars, 0, 0x1000, 0x1000, dump_04_h08t);
    mem_map_rom(&bj.mem_chars, 0, 0x2000, 0x1000, dump_05_k08t);

    mem_init(&bj.mem_tiles);
    mem_map_rom(&bj.mem_tiles, 0, 0x0000, 0x2000, dump_06_l08t);
    mem_map_rom(&bj.mem_tiles, 0, 0x2000, 0x2000, dump_07_n08t);
    mem_map_rom(&bj.mem_tiles, 0, 0x4000, 0x2000, dump_08_r08t);

    mem_init(&bj.mem_sprites);
    mem_map_rom(&bj.mem_sprites, 0, 0x0000, 0x2000, dump_16_m07b);
    mem_map_rom(&bj.mem_sprites, 0, 0x2000, 0x2000, dump_15_l07b);
    mem_map_rom(&bj.mem_sprites, 0, 0x4000, 0x2000, dump_14_j07b);

    mem_init(&bj.mem_maps);
    mem_map_rom(&bj.mem_maps, 0, 0x0000, 0x1000, dump_02_p04t);
}

/* run the emulation for one frame */
void bombjack_exec(uint32_t micro_seconds) {
    /* tick the main board */
    uint32_t ticks_to_run = clk_ticks_to_run(&bj.main.clk, micro_seconds);
    uint32_t ticks_executed = 0;
    while (ticks_executed < ticks_to_run) {
        ticks_executed += z80_exec(&bj.main.cpu, ticks_to_run);
    }
    clk_ticks_executed(&bj.main.clk, ticks_executed);
    bombjack_decode_video();
}

/* main board tick callback */
uint64_t bombjack_tick_main(int num, uint64_t pins, void* user_data) {

    const uint16_t addr = Z80_GET_ADDR(pins);
    if (pins & Z80_MREQ) {
        /* memory request */
        switch (addr) {
            case 0xB000:
                /* read: joystick port 1:
                    0:  right
                    1:  left
                    2:  up
                    3:  down
                    5:  btn
                  write: IRQ mask
                */
                if (pins & Z80_RD) {
                    Z80_SET_DATA(pins, bj.main.p1);
                }
                else if (pins & Z80_WR) {
                    bj.main.nmi_mask = Z80_GET_DATA(pins);
                }
                break;
            case 0xB001:
                /* joystick port 2 */
                if (pins & Z80_RD) {
                    Z80_SET_DATA(pins, bj.main.p2);
                }
                else if (pins & Z80_WR) {
                    printf("Trying to write joy2\n");
                }
                break;
            case 0xB002:
                /* system:
                    0:  coin1
                    1:  coin2
                    2:  start1
                    3:  start2
                */
                if (pins & Z80_RD) {
                    Z80_SET_DATA(pins, bj.main.sys);
                }
                else if (pins & Z80_WR) {
                    printf("Trying to write sys\n");
                }
                break;
            case 0xB003:
                /* ??? */
                /*
                if (pins & Z80_RD) {
                    printf("read from 0xB003\n");
                }
                else if (pins & Z80_WR) {
                    printf("write to 0xB003\n");
                }
                */
                break;
            case 0xB004:
                /* read: dip-switches 1
                   write: flip screen
                */
                if (pins & Z80_RD) {
                    Z80_SET_DATA(pins, bj.main.dsw1);
                }
                else if (pins & Z80_WR) {
                    printf("flip screen\n");
                }
                break;
            case 0xB005:
                /* read: dip-switches 2 */
                if (pins & Z80_RD) {
                    Z80_SET_DATA(pins, bj.main.dsw2);
                }
                else if (pins & Z80_WR) {
                    printf("write to 0xB005\n");
                }
                break;
            case 0xB800:
                /* sound latch */
                if (pins & Z80_RD) {
                    printf("read sound latch\n");
                }
                else {
                    printf("write sound latch\n");
                }
                break;
            default:
                if (pins & Z80_RD) {
                    Z80_SET_DATA(pins, mem_rd(&bj.main.mem, addr));
                }
                else if (pins & Z80_WR) {
                    mem_wr(&bj.main.mem, addr, Z80_GET_DATA(pins));
                }
        }
    }
    else if (pins & Z80_IORQ) {
        /* IO request */
        printf("IO: 0x%04x\n", addr);
    }
    return pins & Z80_PIN_MASK;
}

/* sound board tick callback */
uint64_t bombjack_tick_sound(int num, uint64_t pins, void* user_data) {

    return pins;
}

/* AY port output callback */
void bombjack_ay_out(int port_id, uint8_t data, void* user_data) {

}

/* AY port input callback */
uint8_t bombjack_ay_in(int port_id, void* user_data) {
    return 0xFF;
}

void bombjack_decode_background(void) {
    /* 16x16 background tiles, each tile 16x16 */
    const uint8_t bg_image = mem_rd(&bj.main.mem, 0x9E00);
    uint32_t* dst = gfx_framebuffer();
    for (uint16_t y = 0; y < 16; y++) {
        for (uint16_t x = 0; x < 16; x++) {
            uint16_t addr = ((bg_image & 0x07)*0x200) + (y * 16 + x);
            uint8_t code = (bg_image & 0x10) ? mem_rd(&bj.mem_maps, addr) : 0;
            uint8_t attr = mem_rd(&bj.mem_maps, addr + 0x0100);
            uint8_t color = attr & 0x0F;
            uint8_t flip_y = attr & 0x80;
            for (int yy = 0; yy < 16; yy++) {
                for (int xx = 0; xx < 16; xx++) {
                    *dst++ = 0xFF000000 | (color * 16);
                }
                dst += 240;
            }
            dst -= (16 * 256);
            dst += 16;
        }
        dst += (15 * 256);
    }
    assert(dst == (gfx_framebuffer() + 256*256));
}

void bombjack_decode_foreground(void) {
    /* 32x32 tiles, each 8x8 */
    uint32_t* dst = gfx_framebuffer();
    for (uint32_t y = 0; y < 32; y++) {
        for (uint32_t x = 0; x < 32; x++) {
            uint16_t offset = y * 32 + x;
            uint8_t chr = mem_rd(&bj.main.mem, 0x9000 + offset);
            uint8_t col = mem_rd(&bj.main.mem, 0x9400 + offset);
            /* 512 foreground tiles */
            uint16_t tile = chr | ((col & 0x10)<<4);
            uint8_t color = col & 0x0F;
            uint16_t tile_addr = tile * 8;
            for (int yy = 0; yy < 8; yy++) {
                /* 3 bit planes, 8 bytes per char */
                uint8_t bm0 = mem_rd(&bj.mem_chars, tile_addr);
                uint8_t bm1 = mem_rd(&bj.mem_chars, tile_addr + 512*8);
                uint8_t bm2 = mem_rd(&bj.mem_chars, tile_addr + 2*512*8);
                uint8_t bm = bm0 | bm1 | bm2;
                for (int xx = 7; xx >= 0; xx--) {
                    /* FIXME: lookup color */
                    *dst++ = (bm & (1<<xx)) ? 0xFFFFFFFF : 0xFF000000;
                }
                tile_addr++;
                dst += 248;
            }
            dst -= (8 * 256);
            dst += 8;
        }
        dst += (7 * 256);
    }
    assert(dst == (gfx_framebuffer() + 256*256));
}

void bombjack_decode_video() {
    bombjack_decode_background();
    bombjack_decode_foreground();
}