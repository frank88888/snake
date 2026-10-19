#include <gb/gb.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <gbdk/console.h>
#include <gbdk/font.h>

#define COLS      20     /* 螢幕寬：20 格 (每格 8x8 像素) */
#define ROWS      18     /* 螢幕高：18 格 */
#define MAX_LEN   120    /* 蛇的最大長度 */
#define MAX_LEVEL 12     /* 速度等級 0~12 */
#define TILE_BASE 128    /* 遊戲圖塊從 128 開始，避免蓋掉前面的文字字型 */

/* 每個等級幾個畫面更新一次：等級越高數值越小，蛇越快 */
static const uint8_t level_delay[MAX_LEVEL + 1] = {
    24, 21, 18, 16, 14, 12, 10, 9, 8, 7, 6, 5, 4
};

/* 2bpp 圖塊資料：每個 8x8 圖塊 16 bytes，每列 2 bytes */
static const uint8_t tile_data[] = {
    /* 0: 空白 */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 1: 蛇身 (實心黑) */
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    /* 2: 食物 (棋盤格) */
    0xAA,0xAA,0x55,0x55,0xAA,0xAA,0x55,0x55,
    0xAA,0xAA,0x55,0x55,0xAA,0xAA,0x55,0x55,
    /* 3: 牆壁 (淺灰) */
    0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,
    0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,
    /* 4: 蛇頭 (有眼睛，封面用) */
    0xFF,0xFF,0xFF,0xFF,0xE7,0xE7,0xE7,0xE7,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
};

#define T_BLANK ((uint8_t)(TILE_BASE + 0))
#define T_BODY  ((uint8_t)(TILE_BASE + 1))
#define T_FOOD  ((uint8_t)(TILE_BASE + 2))
#define T_WALL  ((uint8_t)(TILE_BASE + 3))
#define T_HEAD  ((uint8_t)(TILE_BASE + 4))

static uint8_t sx[MAX_LEN], sy[MAX_LEN];  /* 蛇身座標，[0] 為蛇頭 */
static uint8_t len;
static int8_t  dx, dy;
static uint8_t fx, fy;                    /* 食物座標 */
static bool    dead;
static uint8_t level;                     /* 目前速度等級 0~12 */

/* 簡單的 8-bit 亂數 (xorshift)，不依賴額外函式庫 */
static uint8_t rnd_state = 0x5A;
static uint8_t rnd8(void) {
    uint8_t x = rnd_state;
    x ^= (uint8_t)(x << 3);
    x ^= (uint8_t)(x >> 5);
    x ^= (uint8_t)(x << 1);
    rnd_state = x ? x : 1;
    return x;
}

/* ---- 音效 (聲道 1 方波) ---- */
static void sound_on(void) {
    NR52_REG = 0x80;
    NR50_REG = 0x77;
    NR51_REG = 0xFF;
}

/* 吃到食物：Nokia 風格的一聲短 Beep (約 60ms) */
static void sound_eat(void) {
    NR10_REG = 0x00;          /* 關閉音高掃頻（Pace=0），維持單純純音 */
    NR11_REG = 0x80;          /* Duty cycle 設為 50%（10b），更接近 Nokia 電磁式蜂鳴器的方波質感；音長計數器初始為 0。 */
    NR12_REG = 0xF1;          /* 初始音量開到最大（1111b），Envelope 遞減速度設為每階 1/64 秒（001b），製造乾脆且不留殘音的急速衰減。*/
    NR13_REG = 0xC1;          /* 頻率數值低 8 位元（搭配 NR14 組合出 $x \approx 1857$）。 */
    NR14_REG = 0x87;          /* Bit 7 設為 1 觸發發聲（Trigger）；高 3 位頻率為 111b（0x07）。 */
}

static void play_note(uint16_t freq, uint8_t ms) {
    NR10_REG = 0x00;
    NR11_REG = 0x80;
    NR12_REG = 0xF0;
    NR13_REG = (uint8_t)(freq & 0xFF);
    NR14_REG = 0x80 | (uint8_t)(freq >> 8);
    delay(ms);
}

/* 死亡：下行的幾聲低音 (仿原作) */
static void sound_die(void) {
    play_note(0x726, 110);
    play_note(0x6DD, 110);
    play_note(0x64B, 110);
    play_note(0x571, 250);
    NR12_REG = 0x00;          /* 關閉聲道 */
}

static void clear_screen(void) {
    uint8_t x, y;
    for (y = 0; y < ROWS; y++)
        for (x = 0; x < COLS; x++)
            set_bkg_tile_xy(x, y, T_BLANK);
}

static void wait_release(void) {
    while (joypad()) wait_vbl_done();
}

/* 封面：貪食蛇圖案 + 閃爍的 START (每秒閃一次)，按 START 進入 */
static void title_screen(void) {
    uint8_t i, t = 0;
    bool shown = true;

    clear_screen();
    for (i = 0; i < COLS; i++) {
        set_bkg_tile_xy(i, 0, T_WALL);
        set_bkg_tile_xy(i, ROWS - 1, T_WALL);
    }
    gotoxy(7, 3);
    printf("SNAKE");

    /* 盤繞的蛇：頭在右，尾在左，旁邊有食物 */
    for (i = 0; i < 9; i++) set_bkg_tile_xy(4 + i, 7, T_BODY);
    set_bkg_tile_xy(13, 7, T_HEAD);
    for (i = 0; i < 4; i++) set_bkg_tile_xy(4, 8 + i, T_BODY);
    for (i = 0; i < 9; i++) set_bkg_tile_xy(4 + i, 11, T_BODY);
    set_bkg_tile_xy(15, 7, T_FOOD);

    wait_release();
    while (1) {
        if (t == 0) {
            gotoxy(7, 14);
            printf(shown ? "START" : "     ");
            shown = !shown;
        }
        if (++t >= 30) t = 0;     /* 亮 0.5 秒、滅 0.5 秒 = 每秒閃一次 */
        rnd_state = (uint8_t)DIV_REG | 1;
        wait_vbl_done();
        if (joypad() & J_START) break;
    }
    wait_release();
}

static void show_level(void) {
    gotoxy(6, 9);
    printf("< %u%u >", (unsigned)(level / 10), (unsigned)(level % 10));
}

/* 選擇速度等級 LEVEL 0~12 */
static void level_screen(void) {
    uint8_t k, prev = 0xFF;

    clear_screen();
    gotoxy(5, 5);
    printf("LEVEL 0-12");
    gotoxy(3, 12);
    printf("LEFT/RIGHT:SET");
    gotoxy(3, 14);
    printf("START:GO");
    show_level();

    wait_release();
    while (1) {
        wait_vbl_done();
        k = joypad();
        if (k != prev) {
            if ((k & J_RIGHT) && level < MAX_LEVEL) { level++; show_level(); }
            if ((k & J_LEFT)  && level > 0)         { level--; show_level(); }
            if (k & (J_START | J_A)) break;
        }
        prev = k;
    }
    wait_release();
}

static void draw_field(void) {
    uint8_t x, y;
    for (y = 0; y < ROWS; y++) {
        for (x = 0; x < COLS; x++) {
            bool wall = (x == 0 || y == 0 || x == COLS - 1 || y == ROWS - 1);
            set_bkg_tile_xy(x, y, wall ? T_WALL : T_BLANK);
        }
    }
}

static void spawn_food(void) {
    uint8_t i;
    bool ok;
    do {
        fx = 1 + (rnd8() % (COLS - 2));
        fy = 1 + (rnd8() % (ROWS - 2));
        ok = true;
        for (i = 0; i < len; i++) {
            if (sx[i] == fx && sy[i] == fy) { ok = false; break; }
        }
    } while (!ok);
    set_bkg_tile_xy(fx, fy, T_FOOD);
}

static void reset_game(void) {
    uint8_t i;
    draw_field();
    len = 4;
    for (i = 0; i < len; i++) {
        sx[i] = 8 - i;
        sy[i] = 8;
        set_bkg_tile_xy(sx[i], sy[i], T_BODY);
    }
    dx = 1;
    dy = 0;
    dead = false;
    spawn_food();
}

static void step(void) {
    int hx = sx[0] + dx;
    int hy = sy[0] + dy;
    uint8_t i, tx, ty;
    bool eat, grew;

    /* 撞牆 */
    if (hx < 1 || hx > COLS - 2 || hy < 1 || hy > ROWS - 2) {
        dead = true;
        return;
    }

    eat = (hx == fx && hy == fy);

    /* 撞到自己：沒吃到食物時尾巴會移開，所以不檢查最後一節 */
    for (i = 0; i < (eat ? len : len - 1); i++) {
        if (sx[i] == hx && sy[i] == hy) {
            dead = true;
            return;
        }
    }

    tx = sx[len - 1];
    ty = sy[len - 1];

    grew = eat && (len < MAX_LEN);
    if (grew) len++;

    /* 身體往後移一格 */
    for (i = len - 1; i > 0; i--) {
        sx[i] = sx[i - 1];
        sy[i] = sy[i - 1];
    }
    sx[0] = (uint8_t)hx;
    sy[0] = (uint8_t)hy;

    set_bkg_tile_xy(sx[0], sy[0], T_BODY);

    if (eat) {
        sound_eat();
        spawn_food();
    }
    if (!grew) {
        set_bkg_tile_xy(tx, ty, T_BLANK);   /* 清除舊尾巴 */
    }
}

static void read_input(void) {
    uint8_t k = joypad();
    if      ((k & J_UP)    && dy != 1)  { dx = 0;  dy = -1; }
    else if ((k & J_DOWN)  && dy != -1) { dx = 0;  dy = 1;  }
    else if ((k & J_LEFT)  && dx != 1)  { dx = -1; dy = 0;  }
    else if ((k & J_RIGHT) && dx != -1) { dx = 1;  dy = 0;  }
}

void main(void) {
    uint8_t frame;

    DISPLAY_OFF;
    font_init();
    font_set(font_load(font_ibm));
    set_bkg_data(TILE_BASE, 5, tile_data);
    BGP_REG = 0xE4;                 /* 標準灰階調色盤 */
    sound_on();
    SHOW_BKG;
    DISPLAY_ON;

    level = 3;

    while (1) {
        title_screen();
        level_screen();
        reset_game();
        frame = 0;

        while (!dead) {
            wait_vbl_done();
            read_input();
            if (++frame >= level_delay[level]) {
                frame = 0;
                step();
            }
        }

        sound_die();
        gotoxy(5, 8);
        printf("GAME OVER");
        delay(500);
        while (!(joypad() & J_START)) wait_vbl_done();
        wait_release();
    }
}
