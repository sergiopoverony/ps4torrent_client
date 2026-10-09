#include "ui.h"
#include "app.h"
#include "api.h"
#include "scene.h"
#include "image.h"
#include "connstate.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include <orbis/Pad.h>
#include <orbis/UserService.h>

extern "C" uint64_t sceKernelGetProcessTime(void);

#define FRAME_WIDTH   1920
#define FRAME_HEIGHT  1080
#define FRAME_DEPTH      4

static const int FONT_PX   = 26;
static const int ROW_H     = 40;    // чуть выше, чтобы под строкой поместилась полоска прогресса
static const int BAR_H     = 4;
static const int BAR_GAP   = 4;     // отступ снизу строки до полоски (разделяет соседние строки)
static const int MARGIN_X  = 40;
static const int BANNER_LEFT = 12;   // логотип прижат ближе к левому краю, отдельно от MARGIN_X
static const int BANNER_TOP = 16;     // отступ сверху для баннера/заголовка
static const int BANNER_H   = 64;     // высота баннера на экране (title.png вписывается в неё)
static const int HEADER_Y  = BANNER_TOP + BANNER_H + 14;
static const int ROWS_Y    = HEADER_Y + ROW_H + 6;
static const int FOOTER_Y  = 1000;    // ниже - подсказка и титры, см. HINT_BASELINE/CREDIT_BASELINE
static const int VISIBLE   = (FOOTER_Y - 10 - ROWS_Y) / ROW_H;

// Колонки (в символах моноширинного шрифта)
static const int COL_NUM   = 0,  W_NUM   = 3;
static const int COL_NAME  = 5,  W_NAME  = 46;    // было 56: 10 символов отдано под новую колонку ETA
static const int COL_SIZE  = 53, W_SIZE  = 10;
static const int COL_DONE  = 65, W_DONE  = 17;
static const int COL_ETA   = 84, W_ETA   = 8;      // новая колонка: сколько примерно осталось
static const int COL_SPEED = 94, W_SPEED = 13;     // позиция не изменилась
static const int COL_PEERS = 109, W_PEERS = 6;     // позиция не изменилась

static const Color C_BG    = { 22,  24,  30 };
static const Color C_ROW2  = { 28,  31,  39 };
static const Color C_HEAD  = { 45,  50,  64 };
static const Color C_SEL   = { 72,  82,  96 };    // выделение: серый в тон прежнему синему (оттенок 215)
static const Color C_TEXT  = { 230, 230, 230 };
static const Color C_DIM   = { 150, 155, 165 };
static const Color C_DIMMER = { 95,  99,  108 };   // тусклее C_DIM, для титров внизу
static const Color C_TRACK  = { 55,  59,  68 };    // серая "дорожка" полоски прогресса
static const Color C_FILL   = { 81, 171, 236 };    // голубая заливка полоски (C_GREEN оставлен для статуса complete)
static const Color C_GREEN = { 110, 210, 130 };
static const Color C_AMBER = { 235, 180,  80 };
static const Color C_RED   = { 235, 100, 100 };
static const Color C_PANEL = { 40,  44,  56 };
static const Color C_EDGE  = { 110, 120, 150 };

static Scene2D* g_scene = NULL;
static FT_Face  g_font;
static int      g_cellW = 16;
static bool     g_ready = false;
static bool     g_padOk = false;

static int      g_sel = 0;
static int      g_top = 0;
static int      g_redraw = 2;
static int      g_frameID = 0;
static uint64_t g_lastTick = 0;
static Image    g_banner;
static bool     g_bannerOk = false;
static int      g_bannerW  = 0;   // ширина на экране при высоте BANNER_H (с сохранением пропорций)
static uint64_t g_lastRefresh = 0;

enum Modal { MODAL_NONE, MODAL_DELETE };
static Modal        g_modal = MODAL_NONE;
static std::string  g_modalHash;
static std::string  g_modalTitle;

// ---------------------------------------------------------------- UTF-8

static int decode_utf8(const unsigned char*& p)
{
    unsigned c = *p++;
    if (c < 0x80) return (int)c;

    int extra;
    unsigned cp;
    if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else return '?';

    while (extra-- > 0) {
        if ((*p & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (*p & 0x3F);
        p++;
    }
    return (int)cp;
}

static int count_cp(const std::string& s)
{
    const unsigned char* p = (const unsigned char*)s.c_str();
    int n = 0;
    while (*p) { decode_utf8(p); n++; }
    return n;
}

static std::string fit(const std::string& s, int maxChars)
{
    if (count_cp(s) <= maxChars) return s;
    const unsigned char* start = (const unsigned char*)s.c_str();
    const unsigned char* p = start;
    for (int i = 0; i < maxChars - 1; i++) decode_utf8(p);
    return std::string((const char*)start, (size_t)(p - start)) + "\xE2\x80\xA6";
}

// ---------------------------------------------------------------- текст

static void draw_text(const std::string& s, int x, int baseline, Color fg, Color bg)
{
    FT_GlyphSlot slot = g_font->glyph;
    const unsigned char* p = (const unsigned char*)s.c_str();

    while (*p) {
        int cp = decode_utf8(p);
        FT_UInt gi = FT_Get_Char_Index(g_font, (FT_ULong)cp);

        if (FT_Load_Glyph(g_font, gi, FT_LOAD_DEFAULT)) continue;
        if (FT_Render_Glyph(slot, ft_render_mode_normal)) continue;

        for (unsigned yy = 0; yy < slot->bitmap.rows; yy++) {
            for (unsigned xx = 0; xx < slot->bitmap.width; xx++) {
                unsigned a = slot->bitmap.buffer[yy * slot->bitmap.pitch + xx];
                if (a == 0) continue;

                int px = x + (int)xx + slot->bitmap_left;
                int py = baseline + (int)yy - slot->bitmap_top;
                if (px < 0 || py < 0 || px >= FRAME_WIDTH || py >= FRAME_HEIGHT) continue;

                Color c;
                c.r = (uint8_t)((bg.r * (255 - a) + fg.r * a) / 255);
                c.g = (uint8_t)((bg.g * (255 - a) + fg.g * a) / 255);
                c.b = (uint8_t)((bg.b * (255 - a) + fg.b * a) / 255);
                g_scene->PutPixel(px, py, c);
            }
        }
        x += (int)(slot->advance.x >> 6);
    }
}

static int col_x(int col) { return MARGIN_X + col * g_cellW; }

static void put_left(int col, const std::string& s, int baseline, Color fg, Color bg)
{
    draw_text(s, col_x(col), baseline, fg, bg);
}

static void put_right(int col, int width, const std::string& s, int baseline, Color fg, Color bg)
{
    std::string t = fit(s, width);
    int n = count_cp(t);
    draw_text(t, col_x(col + width - n), baseline, fg, bg);
}

static void put_center(int cx, const std::string& s, int baseline, Color fg, Color bg)
{
    int w = count_cp(s) * g_cellW;
    draw_text(s, cx - w / 2, baseline, fg, bg);
}

// Рисует картинку (ближайший сосед, с масштабированием) с альфа-смешением по фону bg.
// Фон передаём явно (как и в draw_text), потому что читать обратно из видеопамяти накладно.
static void draw_image(const Image& img, int dstX, int dstY, int dstW, int dstH, Color bg)
{
    if (!img.pixels || img.width <= 0 || img.height <= 0 || dstW <= 0 || dstH <= 0) return;

    for (int y = 0; y < dstH; y++) {
        int sy = y * img.height / dstH;
        int py = dstY + y;
        if (py < 0 || py >= FRAME_HEIGHT) continue;

        for (int x = 0; x < dstW; x++) {
            int sx = x * img.width / dstW;
            int px = dstX + x;
            if (px < 0 || px >= FRAME_WIDTH) continue;

            const unsigned char* s = img.pixels + ((size_t)sy * img.width + sx) * 4;
            unsigned a = s[3];
            if (a == 0) continue;

            Color c;
            c.r = (uint8_t)((bg.r * (255 - a) + s[0] * a) / 255);
            c.g = (uint8_t)((bg.g * (255 - a) + s[1] * a) / 255);
            c.b = (uint8_t)((bg.b * (255 - a) + s[2] * a) / 255);
            g_scene->PutPixel(px, py, c);
        }
    }
}

// ---------------------------------------------------------------- форматирование

static std::string fmt_size(uint64_t b)
{
    char buf[40];
    if (b >= 1073741824ULL)      snprintf(buf, sizeof(buf), "%.1fGB", b / 1073741824.0);
    else if (b >= 1048576ULL)    snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
    else if (b >= 1024ULL)       snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
    else                         snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
    return buf;
}

static std::string fmt_speed(int kbps)
{
    char buf[40];
    if (kbps >= 1024) snprintf(buf, sizeof(buf), "%.1f MB/s", kbps / 1024.0);
    else              snprintf(buf, sizeof(buf), "%d KB/s", kbps);
    return buf;
}

static std::string fmt_eta(long long s)
{
    if (s <= 0) return "-";   // неизвестно, или уже готово (статус и так это покажет)
    char buf[32];
    if (s < 3600) snprintf(buf, sizeof(buf), "%lldm", s / 60 + (s % 60 ? 1 : 0));
    else if (s < 86400) snprintf(buf, sizeof(buf), "%lldh%lldm", s / 3600, (s % 3600) / 60);
    else snprintf(buf, sizeof(buf), "%lldd", s / 86400);
    return buf;
}

static std::string status_label(const std::string& s)
{
    if (s == "checking")    return "checking";
    if (s == "downloading") return "downloading";
    if (s == "paused")      return "paused";
    if (s == "complete")    return "complete";
    if (s == "offline")     return "drive gone";
    if (s == "waiting")     return "waiting";
    if (s == "queued")      return "queued";
    return s;
}

static Color status_color(const std::string& s)
{
    if (s == "complete")    return C_GREEN;
    if (s == "downloading") return C_TEXT;
    if (s == "offline")     return C_RED;
    if (s == "waiting")     return C_AMBER;
    if (s == "paused")      return C_AMBER;
    return C_DIM;
}

// ---------------------------------------------------------------- отрисовка

static void draw_row(int vis, int idx)
{
    const Item& it = g_status.items[idx];
    bool selected = (idx == g_sel);
    int top = ROWS_Y + vis * ROW_H;
    Color bg = selected ? C_SEL : ((idx % 2) ? C_ROW2 : C_BG);
    g_scene->FillRect(0, top, FRAME_WIDTH, ROW_H, bg);
    int base = top + 25;

    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%d", idx + 1);
    put_right(COL_NUM, W_NUM, tmp, base, C_DIM, bg);

    put_left(COL_NAME, fit(it.title, W_NAME), base, C_TEXT, bg);
    put_right(COL_SIZE, W_SIZE, fmt_size(it.size), base, it.lowSpace ? C_AMBER : C_TEXT, bg);

    char dbuf[64];
    snprintf(dbuf, sizeof(dbuf), "%s (%d%%)", fmt_size(it.done).c_str(), it.pct);
    put_right(COL_DONE, W_DONE, dbuf, base, it.status == "complete" ? C_GREEN : C_TEXT, bg);

    bool showEta = (it.status == "downloading");
    put_right(COL_ETA, W_ETA, showEta ? fmt_eta(it.etaSec) : "-", base, showEta ? C_TEXT : C_DIM, bg);

    std::string stText;
    if (it.status == "downloading") stText = fmt_speed(it.speedKB);
    else                            stText = status_label(it.status);
    put_right(COL_SPEED, W_SPEED, stText, base, status_color(it.status), bg);

    if (it.status == "downloading") snprintf(tmp, sizeof(tmp), "%d", it.peers);
    else                             snprintf(tmp, sizeof(tmp), "-");
    put_right(COL_PEERS, W_PEERS, tmp, base, C_TEXT, bg);

    // Тонкая полоска прогресса под строкой: от начала названия до правого края колонки Status
    // (Peers в неё не входит). Серая "дорожка", голубая заливка по проценту.
    int barX0 = col_x(COL_NAME);
    int barX1 = col_x(COL_SPEED + W_SPEED);
    int barW = barX1 - barX0;
    int pct = it.pct;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int barY = top + ROW_H - BAR_GAP - BAR_H;
    g_scene->FillRect(barX0, barY, barW, BAR_H, C_TRACK);
    int fillW = barW * pct / 100;
    if (fillW > 0) g_scene->FillRect(barX0, barY, fillW, BAR_H, C_FILL);
}

static void draw_panel(int w, int h)
{
    int x = (FRAME_WIDTH - w) / 2;
    int y = (FRAME_HEIGHT - h) / 2;
    g_scene->FillRect(x - 3, y - 3, w + 6, h + 6, C_EDGE);
    g_scene->FillRect(x, y, w, h, C_PANEL);
}

static void draw_modal()
{
    if (g_modal != MODAL_DELETE) return;

    const int W = 1200, H = 300;
    draw_panel(W, H);
    int cx = FRAME_WIDTH / 2;
    int y0 = (FRAME_HEIGHT - H) / 2;

    put_center(cx, "Remove this torrent?", y0 + 50, C_TEXT, C_PANEL);
    put_center(cx, fit(g_modalTitle, 64), y0 + 100, C_DIM, C_PANEL);
    put_center(cx, "Downloaded files stay on the drive unless you choose to delete them too", y0 + 160, C_AMBER, C_PANEL);
    put_center(cx, "[Cross] remove   [Square] remove + files   [Circle] cancel", y0 + 230, C_TEXT, C_PANEL);
}

static void draw_all()
{
    g_scene->FillRect(0, 0, FRAME_WIDTH, FRAME_HEIGHT, C_BG);

    int statusX;
    // Баннер занимает BANNER_TOP..BANNER_TOP+BANNER_H; строку статуса центрируем в нём по высоте,
    // а не прижимаем к низу, иначе рядом с крупным логотипом она выглядит мелкой и прижатой.
    int statusBaseline = BANNER_TOP + BANNER_H / 2 + 8;
    if (g_bannerOk) {
        draw_image(g_banner, BANNER_LEFT, BANNER_TOP, g_bannerW, BANNER_H, C_BG);
        statusX = BANNER_LEFT + g_bannerW + 28;
    } else {
        statusX = col_x(0);
        statusBaseline = BANNER_TOP + 25;
    }

    {
        // Первый элемент строки показывает состояние связи, три варианта:
        //  - есть и 127.0.0.1, и настоящая сеть  -> IP:port (как раньше)
        //  - 127.0.0.1 отвечает, а сети нет       -> NO INTERNET (жёлтым)
        //  - 127.0.0.1 не отвечает дольше 10 с    -> NO PAYLOAD (красным)
        // Короткие промахи (пока ps4torrentd пересоздаёт веб-сокет после смены сети) НЕ считаются:
        // в это время держим последнее известное состояние, см. connstate.h.
        // Диски/скорость/пиры показываем всегда по последним известным данным.
        ConnState cs = conn_state(g_status.ok, g_lastOkUs, sceKernelGetProcessTime(), g_status.netIp);

        std::string line;
        Color lineColor;
        if (cs == CONN_NO_PAYLOAD) {
            line = "NO PAYLOAD";
            lineColor = C_RED;
        } else if (cs == CONN_NO_INTERNET) {
            line = "NO INTERNET";
            lineColor = C_AMBER;
        } else {
            line = g_status.netIp + ":" + std::to_string(g_port);
            lineColor = C_TEXT;
            bool anyLow = false;
            for (size_t i = 0; i < g_status.drives.size(); i++) if (g_status.drives[i].low) anyLow = true;
            if (anyLow) lineColor = C_AMBER;
        }

        for (size_t i = 0; i < g_status.drives.size(); i++) {
            const Drive& d = g_status.drives[i];
            if (!line.empty()) line += " | ";
            line += (d.isInternal ? "HDD: " : "USB: ");
            line += fmt_size(d.freeBytes) + " free";
        }

        char stats[120];
        snprintf(stats, sizeof(stats), " | %s | %u peers | %d parallel",
                 fmt_speed(g_status.totalSpeedKB).c_str(), g_status.incoming, g_status.maxParallel);
        line += stats;

        draw_text(fit(line, 95), statusX, statusBaseline, lineColor, C_BG);
    }

    g_scene->FillRect(0, HEADER_Y, FRAME_WIDTH, ROW_H, C_HEAD);
    int hb = HEADER_Y + 25;
    put_right(COL_NUM, W_NUM, "#", hb, C_DIM, C_HEAD);
    put_left(COL_NAME, "Title", hb, C_DIM, C_HEAD);
    put_right(COL_SIZE, W_SIZE, "Size", hb, C_DIM, C_HEAD);
    put_right(COL_DONE, W_DONE, "Done", hb, C_DIM, C_HEAD);
    put_right(COL_ETA, W_ETA, "ETA", hb, C_DIM, C_HEAD);
    put_right(COL_SPEED, W_SPEED, "Status", hb, C_DIM, C_HEAD);
    put_right(COL_PEERS, W_PEERS, "Peers", hb, C_DIM, C_HEAD);

    if (g_status.items.empty()) {
        const char* msg = g_status.ok ? "No torrents yet. Drop a .torrent on the USB drive or in /data/pkg/torrents"
                                       : "Waiting for the first reply from ps4torrentd...";
        put_center(FRAME_WIDTH / 2, msg, 460, C_DIM, C_BG);
    }

    for (int v = 0; v < VISIBLE; v++) {
        int idx = g_top + v;
        if (idx >= (int)g_status.items.size()) break;
        draw_row(v, idx);
    }

    static const int HINT_BASELINE   = FOOTER_Y + 25;         // 1025
    static const int CREDIT_BASELINE = HINT_BASELINE + 32;    // 1057: с запасом до низа экрана (1080)

    put_center(FRAME_WIDTH / 2, "Up/Down - select   Cross - pause/resume   Triangle - remove",
               HINT_BASELINE, C_DIM, C_BG);

    put_center(FRAME_WIDTH / 2, "ps4torrent client v1.5.2.4 Created by SergioPoverony and Mr.Claude",
               CREDIT_BASELINE, C_DIMMER, C_BG);

    draw_modal();
}

// ---------------------------------------------------------------- геймпад

static int g_pad = -1;

static bool pad_init()
{
    if (scePadInit() != 0) return false;

    OrbisUserServiceInitializeParams param;
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);

    int userID = 0;
    sceUserServiceGetInitialUser(&userID);

    g_pad = scePadOpen(userID, 0, 0, NULL);
    return g_pad >= 0;
}

static unsigned pad_read()
{
    if (g_pad < 0) return 0;
    OrbisPadData data;
    memset(&data, 0, sizeof(data));
    scePadReadState(g_pad, &data);
    return (unsigned)data.buttons;
}

static bool step_repeat(bool down, uint64_t& since, uint64_t& last, uint64_t now)
{
    if (!down) { since = 0; return false; }
    if (since == 0) { since = now; last = now; return true; }
    if (now - since > 400000 && now - last >= 70000) { last = now; return true; }
    return false;
}

// Обновляет данные с сервера и запоминает момент последнего УСПЕШНОГО ответа (нужен для
// "запаса времени" перед надписью NO PAYLOAD, см. connstate.h).
static bool refresh_status()
{
    bool ok = api_refresh_status();
    if (ok) g_lastOkUs = sceKernelGetProcessTime();
    return ok;
}

// ---------------------------------------------------------------- управление раздачей

static void toggle_selected()
{
    if (g_sel < 0 || g_sel >= (int)g_status.items.size()) return;
    const Item& it = g_status.items[g_sel];
    if (it.status == "complete" || it.status == "offline") return;

    if (it.paused) api_resume(it.hash);
    else           api_pause(it.hash);

    refresh_status();   // сразу подтягиваем новое состояние, не дожидаясь таймера
    g_redraw = 2;
}

// ---------------------------------------------------------------- публичные функции

bool ui_init()
{
    g_scene = new Scene2D(FRAME_WIDTH, FRAME_HEIGHT, FRAME_DEPTH);
    if (!g_scene->Init(0xC000000, 2)) {
        logf_("ui: scene init FAILED, working without screen");
        return false;
    }

    const char* fontPath = "/app0/assets/fonts/DejaVuSansMono.ttf";
    if (!g_scene->InitFont(&g_font, fontPath, FONT_PX)) {
        logf_("ui: font init FAILED (%s), working without screen", fontPath);
        return false;
    }

    FT_UInt gi = FT_Get_Char_Index(g_font, 'M');
    if (!FT_Load_Glyph(g_font, gi, FT_LOAD_DEFAULT)) {
        int w = (int)(g_font->glyph->advance.x >> 6);
        if (w > 0) g_cellW = w;
    }

    g_bannerOk = g_banner.load("/app0/assets/images/title.png");
    if (g_bannerOk) {
        g_bannerW = BANNER_H * g_banner.width / g_banner.height;
        logf_("ui: banner loaded, %dx%d -> %dx%d on screen", g_banner.width, g_banner.height, g_bannerW, BANNER_H);
    } else {
        logf_("ui: no banner image, using text title instead");
    }

    g_padOk = pad_init();
    logf_("ui: ready, cell width %d, visible rows %d, pad %s", g_cellW, VISIBLE, g_padOk ? "ok" : "FAILED");

    g_lastTick = sceKernelGetProcessTime();
    g_lastRefresh = 0;
    g_ready = true;
    return true;
}

void ui_mark_dirty() { g_redraw = 2; }

void ui_pump()
{
    if (!g_ready) return;

    uint64_t now = sceKernelGetProcessTime();

    // Раз в секунду обновляем список с сервера.
    if (now - g_lastRefresh >= 1000000) {
        g_lastRefresh = now;
        refresh_status();
        g_redraw = 2;
    }

    unsigned b = pad_read();
    static unsigned prev = 0;
    unsigned pressed = b & ~prev;
    prev = b;

    bool up       = (b & ORBIS_PAD_BUTTON_UP) != 0;
    bool down     = (b & ORBIS_PAD_BUTTON_DOWN) != 0;
    bool cross    = (pressed & ORBIS_PAD_BUTTON_CROSS) != 0;
    bool circle   = (pressed & ORBIS_PAD_BUTTON_CIRCLE) != 0;
    bool triangle = (pressed & ORBIS_PAD_BUTTON_TRIANGLE) != 0;
    bool square   = (pressed & ORBIS_PAD_BUTTON_SQUARE) != 0;

    static uint64_t upSince = 0, upLast = 0, downSince = 0, downLast = 0;

    int n = (int)g_status.items.size();

    if (g_modal == MODAL_DELETE) {
        upSince = downSince = 0;
        if (cross)       { api_delete(g_modalHash, false); refresh_status(); g_modal = MODAL_NONE; g_redraw = 2; }
        else if (square)  { api_delete(g_modalHash, true);  refresh_status(); g_modal = MODAL_NONE; g_redraw = 2; }
        else if (circle)  { g_modal = MODAL_NONE; g_redraw = 2; }
    } else if (n > 0) {
        if (step_repeat(up, upSince, upLast, now) && g_sel > 0)           { g_sel--; g_redraw = 2; }
        if (step_repeat(down, downSince, downLast, now) && g_sel + 1 < n) { g_sel++; g_redraw = 2; }

        if (cross) toggle_selected();
        if (triangle) {
            g_modalHash = g_status.items[g_sel].hash;
            g_modalTitle = g_status.items[g_sel].title;
            g_modal = MODAL_DELETE;
            g_redraw = 2;
        }
    }

    if (g_sel >= n) g_sel = (n > 0) ? n - 1 : 0;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + VISIBLE) g_top = g_sel - VISIBLE + 1;
    if (g_top < 0) g_top = 0;

    if (now - g_lastTick >= 1000000) { g_lastTick = now; g_redraw = 2; }

    if (g_redraw > 0) {
        draw_all();
        g_scene->SubmitFlip(g_frameID);
        g_scene->FrameWait(g_frameID);
        g_scene->FrameBufferSwap();
        g_frameID++;
        g_redraw--;
    }
}
