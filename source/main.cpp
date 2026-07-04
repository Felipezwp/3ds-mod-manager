/*
 * Universal 3DS Mod Manager  (LayeredFS + SaltySD hot-swapper)  v4.0
 * ---------------------------------------------------------------------------
 * Swaps the active mod for a game by MOVING folders between a central
 * per-title mod repository and the game's "active" location:
 *
 *   Stored (all games) : sdmc:/3ds/3dsmods/<TitleID>/<mod name>
 *   Active (most games): sdmc:/luma/titles/<TitleID>        (Luma LayeredFS)
 *   Active (Smash 3DS) : sdmc:/saltysd/smash                (SaltySD)
 *
 * Smash 3DS packs its data inside dt/ls archives in romfs, so plain LayeredFS
 * cannot replace individual files. SaltySD is a code.ips patch (applied by
 * Luma game patching from luma/titles/<SmashTID>/code.ips) that redirects the
 * game's file loads to sdmc:/saltysd/smash/. This app therefore:
 *   - swaps Smash mod folders in/out of saltysd/smash, unwrapping the mod's
 *     romfs/ subfolder on activation (SaltySD reads animcmd/, model/, ...
 *     directly from saltysd/smash) and re-wrapping it on return to the repo,
 *   - keeps the SaltySD loader (code.ips) alive in luma/titles/<SmashTID>/,
 *     self-healing from a pristine copy at the repo root (preferred) or any
 *     mod folder that carries one.
 *
 * Activating a stored mod (name-preserving, never deletes anything):
 *   1. If a mod is active, move it back into the repo under its own name.
 *   2. Move the selected stored mod into the active location.
 * Disabling moves the active mod back to the repo -> the game boots vanilla.
 *
 * "Tidy" (Y) migrates every legacy location into the repo:
 *   - loose luma/titles/<TitleID>_<mod> and Disabled<TitleID> folders
 *   - ModMoon slot folders: 3ds/ModMoon/<TitleID>/<Slot_N>
 *   - stray saltysd/<Slot_N> folders (Smash only)
 *
 * Game names resolve from the installed title's SMDH metadata; gamename.txt
 * overrides; a small offline table covers uninstalled games.
 *
 * UI: citro2d, animated background, selectable color themes (SELECT button).
 *
 * Build: libctru + citro2d / devkitARM
 */

#include <citro2d.h>
#include <3ds.h>
#include <curl/curl.h>    // self-updater transport (TLS via mbedTLS)
#include <malloc.h>       // memalign (soc:U buffer)
#include <mbedtls/pk.h>      // update signature verification
#include <mbedtls/sha256.h>
#include <dirent.h>       // POSIX directory iteration (opendir/readdir)
#include <sys/stat.h>     // mkdir
#include <unistd.h>       // rmdir
#include <cctype>         // tolower
#include <cmath>          // sinf
#include <cstdarg>        // va_list (smdh lookup trace)
#include <cstdio>         // fopen/fgets/rename
#include <cstdlib>        // strtoull / rand
#include <cstring>        // strcmp/strchr/strlen
#include <algorithm>      // std::sort / std::find_if / std::min
#include <string>
#include <utility>        // std::pair (game icon cache)
#include <vector>

// Single source of truth for the app version (shown in the header, stamped
// into the lookup log, and compared against GitHub release tags).
#define APP_VER "4.0.0"

// ---------------------------------------------------------------------------
// Locations
// ---------------------------------------------------------------------------
static const char *LUMA_TITLES  = "sdmc:/luma/titles";
static const char *MOD_REPO     = "sdmc:/3ds/3dsmods";
static const char *MODMOON_REPO = "sdmc:/3ds/ModMoon";   // imported by Tidy
static const char *SETTINGS_TXT = "sdmc:/3ds/3dsmods/settings.txt";

// SaltySD-managed titles: their active mod lives in a fixed SD folder that the
// SaltySD code patch reads, NOT in luma/titles. The loader (code.ips) must
// stay in luma/titles/<TitleID>/ for Luma game patching to apply it.
// USA and EUR Smash are built in; more title IDs (other regions / other
// SaltySD games) can be added one-per-line in sdmc:/3ds/3dsmods/saltysd.txt.
static const char *SALTY_ACTIVE   = "sdmc:/saltysd/smash";
static const char *SALTY_PARENT   = "sdmc:/saltysd";
static const char *SALTY_LIST_TXT = "sdmc:/3ds/3dsmods/saltysd.txt";
static std::vector<std::string> g_saltyTids = {
    "00040000000EDF00",   // Super Smash Bros. (USA)
    "00040000000EE000",   // Super Smash Bros. (EUR)
};

// Per-folder marker files holding a mod's human-readable name (modname.txt
// preferred; desc.txt, used by ModMoon, is read as a fallback).
static const char *MARKER_FILE   = "modname.txt";
static const char *ALT_MARKER    = "desc.txt";

// Optional per-title file holding the game's display name (overrides SMDH).
static const char *GAMENAME_FILE = "gamename.txt";

// Name written for an active mod that carries no marker of its own.
static const std::string FALLBACK_NAME = "Previous";

// ---------------------------------------------------------------------------
// Offline fallback names, used only when a title's SMDH can't be read (game
// not installed on this console). gamename.txt and SMDH both take priority.
// ---------------------------------------------------------------------------
struct NamedTitle { const char *id; const char *name; };
static const NamedTitle TITLE_NAMES[] = {
    { "00040000000EDF00", "Super Smash Bros. (3DS)"    },
    { "000400000007AF00", "YouTube"                    },
    { "0004000000287000", "CTRXplorer"                 },
    { "0004000000030800", "Mario Kart 7"               },
    { "0004000000053F00", "Super Mario 3D Land"        },
    { "00040000001B8700", "Minecraft: New 3DS Edition" },
    { "00040000001B5000", "Pokemon Ultra Sun"          },
    { "00040000001D1A00", "Luigi's Mansion"            },
};

// ---------------------------------------------------------------------------
// Themes
// ---------------------------------------------------------------------------
#define RGBA8C(r,g,b,a) C2D_Color32(r, g, b, a)
#define RGB8(r,g,b)     C2D_Color32(r, g, b, 0xFF)

struct Theme {
    const char *name;
    u32 bgTop, bgBot;      // background gradient
    u32 panel, panel2;     // chrome / inset
    u32 accent, secondary, info;   // hue trio (headers, pills, particles)
    u32 selL, selR;        // selection bar gradient
    u32 text, muted;       // typography
};

static const Theme THEMES[] = {
    { "Midnight",
      RGB8(0x12,0x13,0x1F), RGB8(0x26,0x1E,0x3C),
      RGBA8C(0x24,0x28,0x3B,0xEE), RGB8(0x2F,0x35,0x49),
      RGB8(0x7A,0xA2,0xF7), RGB8(0xBB,0x9A,0xF7), RGB8(0x2A,0xC3,0xDE),
      RGB8(0x3D,0x59,0xA1), RGB8(0x55,0x3D,0x8F),
      RGB8(0xC0,0xCA,0xF5), RGB8(0x6E,0x77,0xA8) },
    { "Crimson",
      RGB8(0x1C,0x0E,0x13), RGB8(0x3A,0x12,0x20),
      RGBA8C(0x33,0x1A,0x24,0xEE), RGB8(0x47,0x21,0x2E),
      RGB8(0xF7,0x76,0x8E), RGB8(0xFF,0x9E,0x64), RGB8(0xE0,0xAF,0x68),
      RGB8(0x8A,0x2A,0x3C), RGB8(0xA3,0x45,0x67),
      RGB8(0xF2,0xD5,0xDC), RGB8(0x9A,0x6B,0x78) },
    { "Ocean",
      RGB8(0x0B,0x16,0x22), RGB8(0x0E,0x3A,0x44),
      RGBA8C(0x14,0x2A,0x3C,0xEE), RGB8(0x1E,0x3A,0x50),
      RGB8(0x2A,0xC3,0xDE), RGB8(0x7A,0xA2,0xF7), RGB8(0x73,0xDA,0xCA),
      RGB8(0x1D,0x5F,0x8A), RGB8(0x2A,0x8F,0xA6),
      RGB8(0xC5,0xE4,0xF0), RGB8(0x5E,0x88,0xA0) },
    { "Emerald",
      RGB8(0x0D,0x17,0x12), RGB8(0x14,0x33,0x2A),
      RGBA8C(0x18,0x2E,0x26,0xEE), RGB8(0x22,0x40,0x38),
      RGB8(0x9E,0xCE,0x6A), RGB8(0x73,0xDA,0xCA), RGB8(0xE0,0xAF,0x68),
      RGB8(0x2E,0x6B,0x4F), RGB8(0x3F,0x8A,0x5A),
      RGB8(0xD2,0xEB,0xD8), RGB8(0x6E,0x93,0x7E) },
    { "Sunset",
      RGB8(0x1D,0x10,0x26), RGB8(0x4A,0x1E,0x33),
      RGBA8C(0x33,0x1E,0x3A,0xEE), RGB8(0x45,0x2B,0x4E),
      RGB8(0xFF,0x9E,0x64), RGB8(0xBB,0x9A,0xF7), RGB8(0xF7,0x76,0x8E),
      RGB8(0x7A,0x3B,0x5E), RGB8(0xA8,0x5A,0x3C),
      RGB8(0xF0,0xDC,0xE5), RGB8(0x9C,0x7A,0x96) },
    { "Slate",
      RGB8(0x15,0x17,0x1A), RGB8(0x2A,0x2E,0x36),
      RGBA8C(0x24,0x28,0x2E,0xEE), RGB8(0x33,0x38,0x3F),
      RGB8(0xAA,0xB2,0xC0), RGB8(0x8A,0x93,0xA5), RGB8(0xC9,0xD1,0xDC),
      RGB8(0x3E,0x46,0x54), RGB8(0x4C,0x55,0x68),
      RGB8(0xD5,0xDA,0xE2), RGB8(0x6E,0x76,0x84) },
    { "Sakura",
      RGB8(0x1F,0x12,0x18), RGB8(0x3A,0x1E,0x2E),
      RGBA8C(0x33,0x20,0x2B,0xEE), RGB8(0x47,0x2C,0x3A),
      RGB8(0xF4,0x8F,0xB1), RGB8(0xC7,0x9B,0xF7), RGB8(0xFF,0xD1,0xDC),
      RGB8(0x8A,0x3B,0x5E), RGB8(0xA3,0x45,0x67),
      RGB8(0xF5,0xDE,0xE7), RGB8(0x9C,0x73,0x86) },
    { "Vaporwave",
      RGB8(0x14,0x0A,0x24), RGB8(0x2B,0x11,0x52),
      RGBA8C(0x24,0x15,0x40,0xEE), RGB8(0x37,0x21,0x59),
      RGB8(0xFF,0x71,0xCE), RGB8(0x01,0xCD,0xFE), RGB8(0xB9,0x67,0xFF),
      RGB8(0x7A,0x2E,0x8F), RGB8(0x2E,0x4B,0x9E),
      RGB8(0xE8,0xDF,0xF7), RGB8(0x8B,0x7B,0xA8) },
    { "Matrix",
      RGB8(0x05,0x0D,0x06), RGB8(0x0C,0x24,0x10),
      RGBA8C(0x0E,0x1F,0x12,0xEE), RGB8(0x16,0x30,0x1C),
      RGB8(0x00,0xE6,0x76), RGB8(0x66,0xFF,0xA6), RGB8(0xB9,0xF6,0xCA),
      RGB8(0x0F,0x51,0x32), RGB8(0x1B,0x7A,0x4A),
      RGB8(0xCD,0xEF,0xD8), RGB8(0x5E,0x8A,0x6B) },
    { "Gilded",
      RGB8(0x14,0x10,0x07), RGB8(0x2E,0x24,0x10),
      RGBA8C(0x26,0x20,0x12,0xEE), RGB8(0x3A,0x31,0x1C),
      RGB8(0xE8,0xC1,0x5A), RGB8(0xF2,0xE4,0xB0), RGB8(0xC9,0x8F,0x3B),
      RGB8(0x8A,0x6A,0x1F), RGB8(0xA8,0x84,0x2E),
      RGB8(0xF2,0xE9,0xD4), RGB8(0x9C,0x8B,0x62) },
    { "Virtual",
      RGB8(0x0A,0x02,0x02), RGB8(0x1F,0x05,0x05),
      RGBA8C(0x1C,0x08,0x08,0xEE), RGB8(0x2E,0x0C,0x0C),
      RGB8(0xFF,0x3B,0x30), RGB8(0xFF,0x7A,0x6E), RGB8(0xFF,0xB3,0xAB),
      RGB8(0x6E,0x14,0x10), RGB8(0x8F,0x1E,0x16),
      RGB8(0xFF,0xD9,0xD4), RGB8(0x8F,0x5B,0x55) },
    { "Dracula",
      RGB8(0x1A,0x1B,0x23), RGB8(0x2B,0x2D,0x3F),
      RGBA8C(0x28,0x2A,0x36,0xEE), RGB8(0x34,0x36,0x4A),
      RGB8(0xBD,0x93,0xF9), RGB8(0xFF,0x79,0xC6), RGB8(0x8B,0xE9,0xFD),
      RGB8(0x44,0x47,0x5A), RGB8(0x5A,0x4E,0x8C),
      RGB8(0xF8,0xF8,0xF2), RGB8(0x62,0x72,0xA4) },
    { "Nord",
      RGB8(0x22,0x26,0x2F), RGB8(0x3B,0x42,0x52),
      RGBA8C(0x2E,0x34,0x40,0xEE), RGB8(0x3B,0x42,0x52),
      RGB8(0x88,0xC0,0xD0), RGB8(0x81,0xA1,0xC1), RGB8(0xA3,0xBE,0x8C),
      RGB8(0x4C,0x56,0x6A), RGB8(0x5E,0x81,0xAC),
      RGB8(0xEC,0xEF,0xF4), RGB8(0x7B,0x88,0xA1) },
    { "Gruvbox",
      RGB8(0x1D,0x20,0x21), RGB8(0x3C,0x38,0x36),
      RGBA8C(0x28,0x28,0x28,0xEE), RGB8(0x3C,0x38,0x36),
      RGB8(0xFE,0x80,0x19), RGB8(0xFA,0xBD,0x2F), RGB8(0xB8,0xBB,0x26),
      RGB8(0x79,0x43,0x0E), RGB8(0x9D,0x6A,0x1E),
      RGB8(0xEB,0xDB,0xB2), RGB8(0x92,0x83,0x74) },
    { "Monokai",
      RGB8(0x1E,0x1F,0x1C), RGB8(0x33,0x34,0x2E),
      RGBA8C(0x27,0x28,0x22,0xEE), RGB8(0x3E,0x3D,0x32),
      RGB8(0xF9,0x26,0x72), RGB8(0xA6,0xE2,0x2E), RGB8(0xFD,0x97,0x1F),
      RGB8(0x6E,0x1E,0x3C), RGB8(0x4E,0x5A,0x1E),
      RGB8(0xF8,0xF8,0xF2), RGB8(0x75,0x71,0x5E) },
    { "Cyberpunk",
      RGB8(0x0A,0x0A,0x12), RGB8(0x1A,0x10,0x30),
      RGBA8C(0x16,0x16,0x2A,0xEE), RGB8(0x23,0x23,0x42),
      RGB8(0xFC,0xEE,0x0A), RGB8(0x00,0xF0,0xFF), RGB8(0xFF,0x00,0x3C),
      RGB8(0x5A,0x5A,0x08), RGB8(0x08,0x50,0x5A),
      RGB8(0xF0,0xF0,0xE8), RGB8(0x78,0x78,0x90) },
    { "Gameboy",
      RGB8(0x0F,0x1B,0x0D), RGB8(0x1E,0x3A,0x1A),
      RGBA8C(0x1B,0x33,0x17,0xEE), RGB8(0x2A,0x4A,0x24),
      RGB8(0x8B,0xAC,0x0F), RGB8(0x9B,0xBC,0x0F), RGB8(0x30,0x62,0x30),
      RGB8(0x2E,0x5C,0x28), RGB8(0x3E,0x70,0x30),
      RGB8(0xD8,0xE8,0xC0), RGB8(0x6E,0x8A,0x5E) },
    { "Amber",
      RGB8(0x10,0x0A,0x02), RGB8(0x24,0x15,0x05),
      RGBA8C(0x1E,0x12,0x04,0xEE), RGB8(0x2E,0x1E,0x08),
      RGB8(0xFF,0xB0,0x00), RGB8(0xFF,0xCC,0x55), RGB8(0xCC,0x84,0x00),
      RGB8(0x6E,0x4A,0x08), RGB8(0x8A,0x5E,0x10),
      RGB8(0xFF,0xE0,0xA8), RGB8(0x8F,0x70,0x40) },
    { "Lavender",
      RGB8(0x19,0x15,0x27), RGB8(0x2E,0x25,0x47),
      RGBA8C(0x27,0x20,0x40,0xEE), RGB8(0x36,0x2D,0x54),
      RGB8(0xB7,0xA8,0xF7), RGB8(0xD6,0xBB,0xFB), RGB8(0x9B,0xB5,0xF7),
      RGB8(0x4A,0x3E,0x7A), RGB8(0x5C,0x4A,0x8F),
      RGB8(0xE8,0xE2,0xF7), RGB8(0x8A,0x7F,0xA8) },
    { "Coffee",
      RGB8(0x17,0x10,0x08), RGB8(0x2E,0x21,0x14),
      RGBA8C(0x26,0x1B,0x10,0xEE), RGB8(0x38,0x2A,0x1A),
      RGB8(0xC8,0x9F,0x70), RGB8(0xE8,0xCB,0xA8), RGB8(0xA0,0x71,0x4A),
      RGB8(0x5E,0x45,0x2A), RGB8(0x74,0x56,0x3A),
      RGB8(0xEF,0xE2,0xD0), RGB8(0x9A,0x82,0x5F) },
};
static const int NUM_THEMES = (int)(sizeof(THEMES) / sizeof(THEMES[0]));
static int g_themeIdx = 0;
#define T (THEMES[g_themeIdx])

// Fixed semantic colors, shared by every theme.
static const u32 CLR_WHITE  = RGB8(0xFF, 0xFF, 0xFF);
static const u32 CLR_GREEN  = RGB8(0x9E, 0xCE, 0x6A);  // active / success
static const u32 CLR_YELLOW = RGB8(0xE0, 0xAF, 0x68);  // warnings
static const u32 CLR_RED    = RGB8(0xF7, 0x76, 0x8E);  // errors
static const u32 CLR_ORANGE = RGB8(0xFF, 0x9E, 0x64);  // loose / caution
static const u32 CLR_DARK   = RGB8(0x16, 0x16, 0x1E);  // text on bright pills

// System-font button glyphs (3DS shared font private-use area,
// U+E000..U+E006 encoded as explicit UTF-8 so no editor can strip them).
#define G_A    "\xEE\x80\x80"
#define G_B    "\xEE\x80\x81"
#define G_X    "\xEE\x80\x82"
#define G_Y    "\xEE\x80\x83"
#define G_DPAD "\xEE\x80\x86"

// Bottom-screen list geometry.
static const int   LIST_ROWS = 6;
static const float ROW_H     = 28.0f;
static const float LIST_Y    = 30.0f;

// Global animation clock, advanced once per frame (~1/60 s).
static float g_t = 0.0f;

// ---------------------------------------------------------------------------
// Small string / filesystem helpers
// ---------------------------------------------------------------------------

// Trim trailing whitespace / line endings in place.
static void rtrim(std::string &s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t'))
        s.pop_back();
}

// True if path exists and opens as a directory.
static bool isDir(const std::string &path)
{
    DIR *d = opendir(path.c_str());
    if (d) { closedir(d); return true; }
    return false;
}

// True if the directory exists and holds at least one entry. Empty leftover
// folders in luma/titles (created by tools, never filled) are not mods.
static bool dirNonEmpty(const std::string &path)
{
    DIR *d = opendir(path.c_str());
    if (!d) return false;
    struct dirent *ent;
    bool any = false;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        any = true;
        break;
    }
    closedir(d);
    return any;
}

// True if path exists and opens as a file.
static bool fileExists(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (f) { fclose(f); return true; }
    return false;
}

// Read the first line of a file, trimmed. "" if missing/empty.
static std::string readFirstLine(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return "";
    char buf[256] = {0};
    std::string s;
    if (fgets(buf, sizeof(buf), f))
        s = buf;
    fclose(f);
    rtrim(s);
    return s;
}

// Byte-for-byte file copy (used to restore the SaltySD loader).
static bool copyFile(const std::string &src, const std::string &dst)
{
    FILE *in = fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE *out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); return false; }

    static u8 buf[64 * 1024];
    bool ok = true;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    fclose(out);
    if (!ok) remove(dst.c_str());
    return ok;
}

// True if name is exactly 16 hexadecimal characters (a 3DS Title ID).
static bool isTitleId(const char *name)
{
    if (strlen(name) != 16) return false;
    for (int i = 0; i < 16; ++i) {
        char c = name[i];
        const bool hex = (c >= '0' && c <= '9') ||
                         (c >= 'A' && c <= 'F') ||
                         (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return true;
}

// Case-insensitive string equality. FAT32 names are case-insensitive, so every
// Title ID comparison in this file must go through here (or istartsWith).
static bool iequals(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return true;
}

// Case-insensitive "does s start with prefix".
static bool istartsWith(const std::string &s, const std::string &prefix)
{
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
            return false;
    return true;
}

// If `name` belongs to a game, return its 16-hex Title ID; otherwise "".
// Recognised folder shapes (so a game stays visible regardless of its state):
//   <TitleID>            the ACTIVE mod folder (or SaltySD loader home)
//   <TitleID>_<mod>      a legacy loose stored mod
//   Disabled<TitleID>    a legacy (ModMoon-style) disabled active mod
static std::string titleIdOf(const char *name)
{
    if (isTitleId(name)) return name;

    const std::string s = name;

    if (s.size() > 17 && s[16] == '_') {
        std::string id = s.substr(0, 16);
        if (isTitleId(id.c_str())) return id;
    }

    static const std::string DIS = "Disabled";
    if (s.size() == DIS.size() + 16 && istartsWith(s, DIS)) {
        std::string id = s.substr(DIS.size());
        if (isTitleId(id.c_str())) return id;
    }

    return "";
}

// Make a display name safe as a FAT32 folder name: strip forbidden characters
// and trailing dots/spaces (FAT32 rejects names ending in either).
static std::string sanitizeName(const std::string &in)
{
    std::string out;
    for (char c : in)
        if (!strchr("\\/:*?\"<>|", c) && (unsigned char)c >= 0x20)
            out += c;
    rtrim(out);
    while (!out.empty() && out.back() == '.')
        out.pop_back();
    rtrim(out);
    if (out.empty()) out = "mod";
    return out;
}

// Replace malformed UTF-8 sequences with '?' so marker files saved in odd
// encodings can't feed garbage into the text renderer.
static std::string utf8Sanitize(const std::string &in)
{
    std::string out;
    size_t i = 0, n = in.size();
    while (i < n) {
        unsigned char c = in[i];
        int len = (c < 0x80) ? 1 :
                  ((c & 0xE0) == 0xC0) ? 2 :
                  ((c & 0xF0) == 0xE0) ? 3 :
                  ((c & 0xF8) == 0xF0) ? 4 : 0;
        bool ok = (len > 0) && (i + len <= n);
        if (len == 1) ok = (c >= 0x20 && c != 0x7F);   // drop control chars
        for (int k = 1; ok && k < len; ++k)
            ok = ((unsigned char)in[i + k] & 0xC0) == 0x80;
        if (ok) { out.append(in, i, len); i += len; }
        else    { out += '?'; ++i; }
    }
    return out;
}

// mkdir -p for an "sdmc:/a/b/c" path (creates every component, ignores EEXIST).
static void mkdirs(const std::string &path)
{
    size_t pos = path.find(":/");
    if (pos == std::string::npos) return;
    pos += 2;
    while (true) {
        size_t slash = path.find('/', pos);
        std::string sub = (slash == std::string::npos) ? path : path.substr(0, slash);
        mkdir(sub.c_str(), 0777);
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
}

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

// One swappable game, keyed by its 16-hex Title ID.
struct GameProfile {
    std::string title;     // resolved display name
    std::string titleId;   // 16-hex Title ID
    int         modCount;  // total mods known for this game (UI stats)
    bool        hasActive; // a mod currently occupies the active location
};

// A single mod discovered for a game.
struct ModEntry {
    std::string display;  // human-readable name
    std::string path;     // full path of this mod's source folder
    bool        active = false; // occupies the active location right now
    bool        loose  = false; // legacy folder (luma/titles, ModMoon, ...)
    bool        loader = false; // pseudo-entry: opens the SaltySD loader picker
};

// A legacy-location folder eligible for Tidy.
struct LooseItem {
    std::string path;      // full path of the folder
    std::string fallback;  // display name if it has no marker file
};

// Status toast shown on the bottom screen.
enum StatusKind { SK_NEUTRAL, SK_OK, SK_WARN, SK_ERR };
struct Status {
    std::string msg;
    StatusKind  kind;
};

// ---------------------------------------------------------------------------
// Path builders
// ---------------------------------------------------------------------------

// SaltySD titles keep their loader in luma/titles but their ACTIVE mod content
// in the SaltySD redirect folder.
static bool isSalty(const GameProfile &gp)
{
    for (const std::string &t : g_saltyTids)
        if (iequals(gp.titleId, t)) return true;
    return false;
}

// Extra SaltySD title IDs from sdmc:/3ds/3dsmods/saltysd.txt (one per line).
static void loadSaltyList()
{
    FILE *f = fopen(SALTY_LIST_TXT, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '))
            s.pop_back();
        if (isTitleId(s.c_str()))
            g_saltyTids.push_back(s);
    }
    fclose(f);
}

static std::string lumaPath(const GameProfile &gp)
{
    return std::string(LUMA_TITLES) + "/" + gp.titleId;
}
static std::string activePath(const GameProfile &gp)
{
    return isSalty(gp) ? std::string(SALTY_ACTIVE) : lumaPath(gp);
}
static std::string repoPath(const GameProfile &gp)
{
    return std::string(MOD_REPO) + "/" + gp.titleId;
}
static std::string repoModPath(const GameProfile &gp, const std::string &name)
{
    return repoPath(gp) + "/" + name;
}
static std::string modmoonPath(const GameProfile &gp)
{
    return std::string(MODMOON_REPO) + "/" + gp.titleId;
}

// Display name for a mod folder: modname.txt, then desc.txt, then `fallback`.
static std::string modDisplayName(const std::string &folderPath,
                                  const std::string &fallback)
{
    std::string n = readFirstLine(folderPath + "/" + MARKER_FILE);
    if (n.empty()) n = readFirstLine(folderPath + "/" + ALT_MARKER);
    if (n.empty()) n = fallback;
    return utf8Sanitize(n);
}

// Pick a repo folder name based on `base`, appending " (2)", " (3)", ... if a
// folder of that name already exists.
static std::string uniqueRepoFolder(const GameProfile &gp, const std::string &base)
{
    if (!isDir(repoModPath(gp, base))) return base;
    for (int n = 2; ; ++n) {
        std::string cand = base + " (" + std::to_string(n) + ")";
        if (!isDir(repoModPath(gp, cand))) return cand;
    }
}

// ---------------------------------------------------------------------------
// SaltySD loader upkeep
// ---------------------------------------------------------------------------

// Cached result of the last loader check, shown in the mod-menu header.
static bool g_saltyLoaderOk = true;

// Ensure luma/titles/<SmashTID>/code.ips exists: SaltySD cannot boot without
// it. Self-heals by copying a code.ips from the active folder or any repo mod
// (the user's mod folders each carry one). Returns true if the loader exists.
static bool ensureSaltyLoader(const GameProfile &gp)
{
    if (!isSalty(gp)) return true;

    const std::string loaderDir = lumaPath(gp);
    const std::string loader    = loaderDir + "/code.ips";
    if (fileExists(loader)) return true;

    std::vector<std::string> candidates;
    candidates.push_back(repoPath(gp) + "/code.ips");   // pristine copy at repo root
    candidates.push_back(std::string(SALTY_ACTIVE) + "/code.ips");
    if (DIR *dp = opendir(repoPath(gp).c_str())) {
        struct dirent *ent;
        while ((ent = readdir(dp)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            candidates.push_back(repoPath(gp) + "/" + ent->d_name + "/code.ips");
        }
        closedir(dp);
    }

    for (const std::string &c : candidates) {
        if (!fileExists(c)) continue;
        mkdirs(loaderDir);
        if (copyFile(c, loader)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// SaltySD layout adaptation
// ---------------------------------------------------------------------------
// Mods are distributed (and stored in the repo) with their game data inside a
// romfs/ subfolder, but SaltySD reads data folders (animcmd/, model/, ...)
// DIRECTLY from saltysd/smash. So the active copy must be unwrapped, and
// re-wrapped when it returns to the repo. Marker files (modname.txt etc.)
// stay at the folder root in both layouts.

// Move every child of src into dst (created if needed). Names are collected
// before any rename so the directory is not mutated mid-iteration. Returns
// true only if every entry moved.
static bool moveChildren(const std::string &src, const std::string &dst)
{
    std::vector<std::string> names;
    DIR *dp = opendir(src.c_str());
    if (!dp) return false;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        names.push_back(ent->d_name);
    }
    closedir(dp);

    mkdirs(dst);
    bool ok = true;
    for (const std::string &n : names)
        if (rename((src + "/" + n).c_str(), (dst + "/" + n).c_str()) != 0)
            ok = false;
    return ok;
}

// Active layout: hoist <folder>/romfs/* up into <folder>/ and drop the shell.
static void saltyUnwrap(const std::string &folder)
{
    const std::string wrap = folder + "/romfs";
    if (!isDir(wrap)) return;
    if (moveChildren(wrap, folder)) rmdir(wrap.c_str());
}

// Repo layout: tuck every data DIRECTORY back inside <folder>/romfs/.
// Files (markers, code.ips, readme txts) stay at the root.
static void saltyRewrap(const std::string &folder)
{
    std::vector<std::string> dirs;
    DIR *dp = opendir(folder.c_str());
    if (!dp) return;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (!strcmp(ent->d_name, "romfs")) continue;
        if (isDir(folder + "/" + ent->d_name)) dirs.push_back(ent->d_name);
    }
    closedir(dp);
    if (dirs.empty()) return;

    const std::string wrap = folder + "/romfs";
    mkdirs(wrap);
    for (const std::string &n : dirs)
        rename((folder + "/" + n).c_str(), (wrap + "/" + n).c_str());
}

// ---------------------------------------------------------------------------
// Game name resolution
// ---------------------------------------------------------------------------

// Diagnostics for the SMDH lookup: how many titles resolved, and the last
// error seen. Surfaced in the UI when nothing resolves, so a permission or
// parameter problem shows its exact code instead of failing silently.
static int    g_smdhHits   = 0;
static Result g_smdhLastRc = 0;

// Boot-time trace of every SMDH attempt, flushed to LOOKUP_LOG so failures
// can be diagnosed off-device (fetch the file over ftpd).
static std::string g_smdhLog;
static const char *LOOKUP_LOG = "sdmc:/3ds/3dsmods/namelookup.log";

static void smdhLogf(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_smdhLog.size() < 16384) g_smdhLog += buf;
}

// ---------------------------------------------------------------------------
// Game icon cache: 48x48 SMDH icons uploaded once as GPU textures, keyed by
// Title ID. SMDH stores the icon in the GPU's native tiled RGB565 layout, so
// tile rows copy straight into a 64x64 texture (textures need pow2 sides).
// ---------------------------------------------------------------------------
// The boot scan runs on a worker thread, but GPU textures must be created
// on the main thread: workers enqueue raw pixels, the main loop drains the
// queue between frames. g_gameIcons itself is main-thread-only.
struct IconEntry { std::string tid; C2D_Image img; u32 accent; };
static std::vector<IconEntry> g_gameIcons;

struct PendingIcon { std::string tid; std::vector<u16> px; };
static LightLock                 g_iconLock;
static std::vector<PendingIcon>  g_pendingIcons;
static std::vector<std::string>  g_iconTids;   // every tid ever enqueued

static const C2D_Image *gameIcon(const std::string &titleId)
{
    for (const auto &e : g_gameIcons)
        if (e.tid == titleId) return &e.img;
    return NULL;
}

// The game's dominant color, extracted from its icon (0 if unknown).
static u32 gameAccent(const std::string &titleId)
{
    for (const auto &e : g_gameIcons)
        if (e.tid == titleId) return e.accent;
    return 0;
}

// Thread-safe: remembers the pixels; the texture is built by drainIcons().
static void cacheGameIcon(const std::string &titleId, const u16 *px)
{
    LightLock_Lock(&g_iconLock);
    for (const auto &t : g_iconTids)
        if (t == titleId) { LightLock_Unlock(&g_iconLock); return; }
    g_iconTids.push_back(titleId);
    PendingIcon pi;
    pi.tid = titleId;
    pi.px.assign(px, px + 48 * 48);
    g_pendingIcons.push_back(std::move(pi));
    LightLock_Unlock(&g_iconLock);
}

// Main thread, between frames: turn queued pixels into GPU textures.
static void drainIcons()
{
    LightLock_Lock(&g_iconLock);
    std::vector<PendingIcon> batch;
    batch.swap(g_pendingIcons);
    LightLock_Unlock(&g_iconLock);

    for (const PendingIcon &pi : batch) {
        C3D_Tex *tex = (C3D_Tex *)malloc(sizeof(C3D_Tex));
        if (!tex) continue;
        if (!C3D_TexInit(tex, 64, 64, GPU_RGB565)) { free(tex); continue; }
        // 48x48 = 6 rows of 6 8x8 tiles (128 B each); a 64-wide row holds 8.
        u16 *dst = (u16 *)tex->data;
        for (int ty = 0; ty < 6; ++ty)
            memcpy(dst + ty * 64 * 8, pi.px.data() + ty * 48 * 8, 48 * 8 * 2);
        C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
        static const Tex3DS_SubTexture sub = { 48, 48, 0.0f, 1.0f,
                                               0.75f, 0.25f };

        // Saturation-weighted average color: colorful pixels dominate, so
        // the accent reads as the game's brand color instead of muddy gray.
        u64 sr = 0, sg = 0, sb = 0, sw = 0;
        for (int i = 0; i < 48 * 48; ++i) {
            const u16 v = pi.px[i];
            const int r = (v >> 11) << 3;
            const int g = ((v >> 5) & 0x3F) << 2;
            const int b = (v & 0x1F) << 3;
            const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            const int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            const u64 w  = (u64)(mx - mn) + 4;   // +4: grays still count a bit
            sr += (u64)r * w; sg += (u64)g * w; sb += (u64)b * w; sw += w;
        }
        int r = (int)(sr / sw), g = (int)(sg / sw), b = (int)(sb / sw);
        const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        if (mx > 0) {   // normalize toward a readable glow brightness
            r = r * 230 / mx; g = g * 230 / mx; b = b * 230 / mx;
        }

        IconEntry e;
        e.tid    = pi.tid;
        e.img    = { tex, &sub };
        e.accent = C2D_Color32((u8)r, (u8)g, (u8)b, 0xFF);
        g_gameIcons.push_back(e);
    }
}

// ---------------------------------------------------------------------------
// Boot cache. SMDH probing costs up to three archive opens per title every
// boot; names and icons barely ever change, so both persist on the SD card:
//   names: 3ds/3dsmods/.cache/names.txt  ("<TID>\t<name>"; "?" = unresolved.
//          Unresolved GAME titles are re-probed - a cart may have appeared -
//          but system titles never gain an SMDH, so they are not retried.)
//   icons: 3ds/3dsmods/.cache/<TID>.icn  (raw GPU-tiled 48x48 RGB565)
// ---------------------------------------------------------------------------
static const char *CACHE_DIR  = "sdmc:/3ds/3dsmods/.cache";
static const char *NAME_CACHE = "sdmc:/3ds/3dsmods/.cache/names.txt";
static std::vector<std::pair<std::string, std::string>> g_nameCache;
static bool g_nameCacheDirty = false;

static void loadNameCache()
{
    FILE *f = fopen(NAME_CACHE, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        std::string name = tab + 1;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
            name.pop_back();
        if (strlen(line) == 16 && !name.empty())
            g_nameCache.push_back(std::make_pair(std::string(line), name));
    }
    fclose(f);
}

static void saveNameCache()
{
    if (!g_nameCacheDirty) return;
    mkdirs(CACHE_DIR);
    if (FILE *f = fopen(NAME_CACHE, "w")) {
        for (const auto &e : g_nameCache)
            fprintf(f, "%s\t%s\n", e.first.c_str(), e.second.c_str());
        fclose(f);
    }
    g_nameCacheDirty = false;
}

static const std::string *cachedName(const std::string &tid)
{
    for (const auto &e : g_nameCache)
        if (iequals(e.first, tid)) return &e.second;
    return NULL;
}

static void rememberName(const std::string &tid, const std::string &name)
{
    for (auto &e : g_nameCache) {
        if (!iequals(e.first, tid)) continue;
        if (e.second != name) { e.second = name; g_nameCacheDirty = true; }
        return;
    }
    g_nameCache.push_back(std::make_pair(tid, name));
    g_nameCacheDirty = true;
}

static std::string iconCachePath(const std::string &tid)
{
    return std::string(CACHE_DIR) + "/" + tid + ".icn";
}

static bool iconKnown(const std::string &tid)
{
    LightLock_Lock(&g_iconLock);
    bool known = false;
    for (const auto &t : g_iconTids)
        if (t == tid) { known = true; break; }
    LightLock_Unlock(&g_iconLock);
    return known;
}

static bool loadCachedIcon(const std::string &tid)
{
    if (iconKnown(tid)) return true;
    FILE *f = fopen(iconCachePath(tid).c_str(), "rb");
    if (!f) return false;
    static __attribute__((aligned(128))) u16 px[48 * 48];
    const size_t n = fread(px, 1, sizeof(px), f);
    fclose(f);
    if (n != sizeof(px)) return false;
    cacheGameIcon(tid, px);
    return true;
}

static void saveIconCache(const std::string &tid, const u16 *px)
{
    mkdirs(CACHE_DIR);
    if (FILE *f = fopen(iconCachePath(tid).c_str(), "wb")) {
        fwrite(px, 1, 48 * 48 * 2, f);
        fclose(f);
    }
}

// Open a title's SMDH "icon" file on the given media (FBI's technique).
static Result openTitleIcon(u64 tid, FS_MediaType media, Handle *out)
{
    u32 archPath[4] = { (u32)(tid & 0xFFFFFFFF), (u32)(tid >> 32),
                        (u32)media, 0 };
    u32 filePath[5] = { 0, 0, 2, 0x6E6F6369 /* "icon" */, 0 };
    FS_Path aPath = { PATH_BINARY, sizeof(archPath), archPath };
    FS_Path fPath = { PATH_BINARY, sizeof(filePath), filePath };
    return FSUSER_OpenFileDirectly(out, ARCHIVE_SAVEDATA_AND_CONTENT,
                                   aPath, fPath, FS_OPEN_READ, 0);
}

// Read a title's real name from its installed SMDH (icon) metadata.
// Tries SD, then NAND (system titles), then the game card. "" if not found.
// A successful read also feeds the GPU icon cache and both SD caches.
static std::string smdhGameName(const std::string &titleIdHex)
{
    const u64 tid = strtoull(titleIdHex.c_str(), NULL, 16);
    if (tid == 0) return "";

    // Full SMDH (0x36C0): title blocks for the name plus the 48x48 icon.
    // static: FS rejects IPC read buffers on the app stack with
    // 0xE0C046F9 (InvalidArgument); .bss memory maps fine (FBI uses heap).
    // 128-byte alignment lets the ARM9 DMA write straight into the buffer
    // instead of bouncing through a kernel copy.
    static __attribute__((aligned(128))) struct {
        u32 magic;             // 'SMDH'
        u16 version, reserved;
        struct { u16 shortDesc[0x40]; u16 longDesc[0x80]; u16 publisher[0x40]; } t[16];
        u8  settings[0x30];
        u8  reserved2[0x8];
        u8  smallIcon[0x480];  // 24x24 RGB565 (unused)
        u16 largeIcon[48*48];  // 48x48 RGB565, already GPU-tiled
    } smdh;

    static const FS_MediaType MEDIA[3] = { MEDIATYPE_SD, MEDIATYPE_NAND,
                                           MEDIATYPE_GAME_CARD };
    for (int m = 0; m < 3; ++m) {
        if (MEDIA[m] == MEDIATYPE_GAME_CARD) {
            bool in = false;   // don't probe an empty card slot
            if (R_FAILED(FSUSER_CardSlotIsInserted(&in)) || !in) continue;
        }
        Handle f;
        Result rc = openTitleIcon(tid, MEDIA[m], &f);
        if (R_FAILED(rc)) {
            g_smdhLastRc = rc;
            smdhLogf("%s m%d open rc=%08lX\n", titleIdHex.c_str(),
                     (int)MEDIA[m], (unsigned long)rc);
            continue;
        }

        u32 read = 0;
        rc = FSFILE_Read(f, &read, 0, &smdh, sizeof(smdh));
        FSFILE_Close(f);   // also closes the handle; no extra svcCloseHandle
        if (R_FAILED(rc) || read < sizeof(smdh) || smdh.magic != 0x48444D53) {
            if (R_FAILED(rc)) g_smdhLastRc = rc;
            smdhLogf("%s m%d read rc=%08lX got=%lu magic=%08lX\n",
                     titleIdHex.c_str(), (int)MEDIA[m], (unsigned long)rc,
                     (unsigned long)read, (unsigned long)smdh.magic);
            continue;
        }
        smdhLogf("%s m%d OK\n", titleIdHex.c_str(), (int)MEDIA[m]);
        cacheGameIcon(titleIdHex, smdh.largeIcon);
        saveIconCache(titleIdHex, smdh.largeIcon);

        // Prefer English (block 1), fall back to Japanese (block 0).
        for (int lang = 1; lang >= 0; --lang) {
            char out[0x40 * 3 + 1] = {0};
            ssize_t n = utf16_to_utf8((u8 *)out, smdh.t[lang].shortDesc,
                                      sizeof(out) - 1);
            if (n <= 0) continue;
            out[n] = 0;
            std::string s(out);
            // SMDH short titles are sometimes two lines; keep the first.
            size_t nl = s.find('\n');
            if (nl != std::string::npos) s.erase(nl);
            rtrim(s);
            if (!s.empty()) {
                ++g_smdhHits;
                const std::string nm = utf8Sanitize(s);
                rememberName(titleIdHex, nm);
                return nm;
            }
        }
    }
    return "";
}

// Offline fallback table lookup. "" if unlisted.
static std::string gameNameFromTable(const std::string &id)
{
    for (const NamedTitle &t : TITLE_NAMES)
        if (iequals(id, t.id)) return t.name;
    return "";
}

// Last-resort label built from the Title ID's category, so leftover patch
// folders for uninstalled/system titles read as something meaningful instead
// of 16 raw hex digits. The unique-id half is kept for identification.
static std::string categoryName(const std::string &id)
{
    if (id.size() != 16) return id;
    const std::string hi = id.substr(0, 8), lo = id.substr(8);
    const char *kind = NULL;
    if      (iequals(hi, "00040010")) kind = "System app";
    else if (iequals(hi, "00040030")) kind = "System applet";
    else if (iequals(hi, "00040130")) kind = "System module";
    else if (iequals(hi, "0004000E")) kind = "Game update";
    else if (iequals(hi, "0004008C")) kind = "DLC";
    else if (iequals(hi, "00040000")) kind = "Uninstalled game";
    if (!kind) return id;
    return std::string(kind) + " " + lo;
}

// Resolve a game's display name: gamename.txt (user override), then the
// installed title's SMDH, then the offline table, then the raw Title ID.
static std::string readGameName(const GameProfile &gp)
{
    std::string n = readFirstLine(activePath(gp) + "/" + GAMENAME_FILE);
    if (n.empty()) n = readFirstLine(repoPath(gp) + "/" + GAMENAME_FILE);
    if (!n.empty()) { loadCachedIcon(gp.titleId); return utf8Sanitize(n); }

    // SD cache: a hit skips the SMDH probes entirely. "?" (never resolved)
    // is retried only for game titles - a cart may have been inserted.
    if (const std::string *c = cachedName(gp.titleId)) {
        if (*c != "?") {
            // A cached name IS a resolved name - count it, or the boot
            // warning misfires once the cache makes real probes rare.
            ++g_smdhHits;
            loadCachedIcon(gp.titleId);
            return *c;
        }
        if (!istartsWith(gp.titleId, "00040000")) {
            n = gameNameFromTable(gp.titleId);
            return n.empty() ? categoryName(gp.titleId) : n;
        }
    }

    n = smdhGameName(gp.titleId);   // remembers name + icon on success
    if (!n.empty()) return n;

    rememberName(gp.titleId, "?");
    n = gameNameFromTable(gp.titleId);
    return n.empty() ? categoryName(gp.titleId) : n;
}

// ---------------------------------------------------------------------------
// Shared luma/titles listing. Every game's legacy scan needs this directory,
// so listing it per game made boot O(games x entries) SD reads. One listing
// is kept and invalidated whenever the app itself moves folders in or out.
// ---------------------------------------------------------------------------
static std::vector<std::string> g_lumaList;
static bool g_lumaListValid = false;

static const std::vector<std::string> &lumaEntries()
{
    if (!g_lumaListValid) {
        g_lumaList.clear();
        if (DIR *lp = opendir(LUMA_TITLES)) {
            struct dirent *ent;
            while ((ent = readdir(lp)) != NULL)
                if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, ".."))
                    g_lumaList.push_back(ent->d_name);
            closedir(lp);
        }
        g_lumaListValid = true;
    }
    return g_lumaList;
}
static void invalidateLumaList() { g_lumaListValid = false; }

// ---------------------------------------------------------------------------
// Legacy-location scan, shared by the mod list and Tidy so they always agree.
// Collects, for this game:
//   - luma/titles/<TitleID>_<mod> and luma/titles/Disabled<TitleID>
//   - the bare luma folder itself, when it holds a full legacy mod for a
//     SaltySD title (romfs inside) rather than just the loader
//   - ModMoon slots: 3ds/ModMoon/<TitleID>/<any folder>
//   - stray saltysd/<Slot_N> folders (SaltySD titles only)
// ---------------------------------------------------------------------------
static std::vector<LooseItem> collectLoose(const GameProfile &gp)
{
    std::vector<LooseItem> items;

    // Loose folders in luma/titles (from the shared cached listing).
    {
        const std::string pfx = gp.titleId + "_";
        const std::string dis = "Disabled" + gp.titleId;
        for (const std::string &name : lumaEntries()) {
            std::string fb;
            if (istartsWith(name, pfx))      fb = name.substr(pfx.size());
            else if (iequals(name, dis))     fb = FALLBACK_NAME;
            else continue;
            const std::string full = std::string(LUMA_TITLES) + "/" + name;
            if (!isDir(full)) continue;
            items.push_back({ full, fb });
        }
    }

    // For SaltySD titles the bare luma folder is the loader home, not the
    // active mod. If a full legacy mod is parked there (it has a romfs/), it
    // is invisible to the game - offer it as loose so Tidy can rescue it.
    if (isSalty(gp) && isDir(lumaPath(gp) + "/romfs"))
        items.push_back({ lumaPath(gp), "Legacy active" });

    // ModMoon slots for this title.
    const std::string mm = modmoonPath(gp);
    if (DIR *mp = opendir(mm.c_str())) {
        struct dirent *ent;
        while ((ent = readdir(mp)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            const std::string full = mm + "/" + ent->d_name;
            if (!isDir(full)) continue;
            items.push_back({ full, ent->d_name });
        }
        closedir(mp);
    }

    // Stray slot folders left directly in saltysd/ by old ModMoon versions.
    if (isSalty(gp)) {
        if (DIR *sp = opendir(SALTY_PARENT)) {
            struct dirent *ent;
            while ((ent = readdir(sp)) != NULL) {
                const std::string name = ent->d_name;
                if (!istartsWith(name, "Slot_")) continue;
                const std::string full = std::string(SALTY_PARENT) + "/" + name;
                if (!isDir(full)) continue;
                items.push_back({ full, name });
            }
            closedir(sp);
        }
    }

    return items;
}

// ---------------------------------------------------------------------------
// Collect every mod for a game: the active folder (if any), each stored mod
// in the repo, and legacy loose folders. Active sorts first.
// ---------------------------------------------------------------------------
static std::vector<ModEntry> scanMods(const GameProfile &gp)
{
    std::vector<ModEntry> mods;

    // 1. The currently active mod, if present (an empty folder is not one).
    const std::string ap = activePath(gp);
    if (dirNonEmpty(ap) && !(isSalty(gp) && ap == lumaPath(gp))) {
        ModEntry e;
        e.path    = ap;
        e.active  = true;
        e.loose   = false;
        e.display = modDisplayName(ap, "(unnamed)");
        mods.push_back(e);
    }

    // 2. Stored mods in the repo.
    const std::string rp = repoPath(gp);
    if (DIR *dp = opendir(rp.c_str())) {
        struct dirent *ent;
        while ((ent = readdir(dp)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            const std::string full = rp + "/" + ent->d_name;
            if (!isDir(full)) continue;
            ModEntry e;
            e.path    = full;
            e.active  = false;
            e.loose   = false;
            e.display = modDisplayName(full, ent->d_name);
            mods.push_back(e);
        }
        closedir(dp);
    }

    // 3. Legacy loose folders (luma/titles, ModMoon, stray saltysd slots).
    for (const LooseItem &li : collectLoose(gp)) {
        ModEntry e;
        e.path    = li.path;
        e.active  = false;
        e.loose   = true;
        e.display = modDisplayName(li.path, li.fallback);
        mods.push_back(e);
    }

    std::sort(mods.begin(), mods.end(), [](const ModEntry &a, const ModEntry &b) {
        if (a.active != b.active) return a.active;   // active first
        return a.display < b.display;                 // then alphabetical
    });

    // 4. SaltySD titles get a pseudo-entry that opens the loader picker.
    if (isSalty(gp)) {
        ModEntry e;
        e.display = "SaltySD loader...";
        e.loader  = true;
        mods.push_back(e);                            // always last
    }
    return mods;
}

// ---------------------------------------------------------------------------
// Discover one GameProfile per unique Title ID found under luma/titles, the
// mod repo, or the ModMoon repo. A game stays discoverable whether it has an
// active mod, stored mods, or only legacy folders.
// ---------------------------------------------------------------------------
static std::vector<GameProfile> discoverProfiles()
{
    std::vector<std::string> ids;
    auto addId = [&ids](const std::string &id) {
        auto dup = std::find_if(ids.begin(), ids.end(),
                                [&id](const std::string &e) { return iequals(e, id); });
        if (dup == ids.end()) ids.push_back(id);
    };

    for (const std::string &name : lumaEntries()) {
        std::string id = titleIdOf(name.c_str());
        if (id.empty()) continue;
        // Empty leftover folders (browserhax/cheat-tool debris) are not
        // games worth listing; repo folders still register below.
        if (!dirNonEmpty(std::string(LUMA_TITLES) + "/" + name)) continue;
        addId(id);
    }

    for (const char *root : { MOD_REPO, MODMOON_REPO }) {
        if (DIR *rp = opendir(root)) {
            struct dirent *ent;
            while ((ent = readdir(rp)) != NULL) {
                if (!isTitleId(ent->d_name)) continue;
                if (!isDir(std::string(root) + "/" + ent->d_name)) continue;
                addId(ent->d_name);
            }
            closedir(rp);
        }
    }

    std::vector<GameProfile> profiles;
    for (const std::string &id : ids) {
        GameProfile gp;
        gp.titleId   = id;
        gp.title     = readGameName(gp);
        gp.modCount  = 0;
        gp.hasActive = false;
        profiles.push_back(gp);
    }

    // Case-insensitive sort so lowercase names don't sink below uppercase ones.
    std::sort(profiles.begin(), profiles.end(),
              [](const GameProfile &a, const GameProfile &b) {
                  const size_t n = std::min(a.title.size(), b.title.size());
                  for (size_t i = 0; i < n; ++i) {
                      int ca = tolower((unsigned char)a.title[i]);
                      int cb = tolower((unsigned char)b.title[i]);
                      if (ca != cb) return ca < cb;
                  }
                  return a.title.size() < b.title.size();
              });
    return profiles;
}

// ---------------------------------------------------------------------------
// Mod-list cache. The boot scan already reads every game's mods; keeping the
// lists means opening a game's menu costs zero SD reads. Every action that
// changes mods rescans through rescanMods(), which refreshes its cache entry.
// (External changes made over FTP mid-session appear after a relaunch.)
// ---------------------------------------------------------------------------
static std::vector<std::pair<std::string, std::vector<ModEntry>>> g_modCache;

static const std::vector<ModEntry> *cachedMods(const std::string &tid)
{
    for (const auto &e : g_modCache)
        if (e.first == tid) return &e.second;
    return NULL;
}

static std::vector<ModEntry> rescanMods(const GameProfile &gp)
{
    std::vector<ModEntry> m = scanMods(gp);
    for (auto &e : g_modCache)
        if (e.first == gp.titleId) { e.second = m; return m; }
    g_modCache.push_back(std::make_pair(gp.titleId, m));
    return m;
}

// Recompute one game's mod count / active flag (shown in the game list).
// Pseudo-entries (the loader picker row) don't count as mods.
static void refreshStats(GameProfile &gp)
{
    std::vector<ModEntry> m = rescanMods(gp);
    gp.modCount = 0;
    for (const ModEntry &e : m)
        if (!e.loader) ++gp.modCount;
    gp.hasActive = !m.empty() && m.front().active;  // active sorts first
}


// ---------------------------------------------------------------------------
// Activate a stored mod. Two metadata-only moves, no deletion:
//   1. (if a mod is active) move it into the repo under its own name.
//   2. move the selected folder into the active location.
// On failure, step 1 is rolled back. For SaltySD titles the loader is
// re-verified afterwards (a legacy folder move may have carried it away).
// ---------------------------------------------------------------------------
static bool activateMod(const GameProfile &gp, const ModEntry &target,
                        Status *st)
{
    if (target.active) {
        *st = { "That mod is already active.", SK_WARN };
        return false;
    }

    const std::string ap = activePath(gp);
    mkdirs(repoPath(gp));
    if (isSalty(gp)) mkdirs(SALTY_PARENT);   // saltysd/ may have been wiped

    std::string stashed;                 // where the old active went (rollback)
    const bool hasActive = isDir(ap);
    if (hasActive) {
        std::string name   = sanitizeName(modDisplayName(ap, FALLBACK_NAME));
        std::string folder = uniqueRepoFolder(gp, name);
        stashed = repoModPath(gp, folder);
        if (rename(ap.c_str(), stashed.c_str()) != 0) {
            *st = { "Could not stash the active mod.", SK_ERR };
            return false;
        }
    }

    if (rename(target.path.c_str(), ap.c_str()) != 0) {
        if (hasActive) rename(stashed.c_str(), ap.c_str());   // roll back
        *st = { "Could not activate the selected mod.", SK_ERR };
        return false;
    }

    // Layout fixups only once both renames are in (rollback stays exact).
    if (isSalty(gp)) {
        saltyUnwrap(ap);
        if (hasActive) saltyRewrap(stashed);
    }

    invalidateLumaList();               // folders moved in/out of luma/titles
    g_saltyLoaderOk = ensureSaltyLoader(gp);
    *st = { "Activated: " + modDisplayName(ap, "mod"), SK_OK };
    if (!g_saltyLoaderOk)
        *st = { "Activated, but SaltySD code.ips is missing!", SK_WARN };
    return true;
}

// ---------------------------------------------------------------------------
// Disable the active mod: move it back into the repo so the game boots vanilla.
// (For SaltySD titles the loader stays in luma/titles; with saltysd/smash gone
// the patch simply finds no replacement files.)
// ---------------------------------------------------------------------------
static bool disableMod(const GameProfile &gp, Status *st)
{
    const std::string ap = activePath(gp);
    if (!isDir(ap) || (isSalty(gp) && ap == lumaPath(gp))) {
        *st = { "No active mod - already vanilla.", SK_WARN };
        return false;
    }

    mkdirs(repoPath(gp));
    std::string name   = sanitizeName(modDisplayName(ap, FALLBACK_NAME));
    std::string folder = uniqueRepoFolder(gp, name);
    if (rename(ap.c_str(), repoModPath(gp, folder).c_str()) != 0) {
        *st = { "Could not disable the active mod.", SK_ERR };
        return false;
    }
    if (isSalty(gp)) saltyRewrap(repoModPath(gp, folder));

    invalidateLumaList();               // folders moved in/out of luma/titles
    g_saltyLoaderOk = ensureSaltyLoader(gp);
    *st = { "Mods disabled - game now runs vanilla.", SK_OK };
    return true;
}

// ---------------------------------------------------------------------------
// Tidy: migrate every legacy folder for this game into the repo (loose luma
// folders, ModMoon slots, stray saltysd slots). Collects paths first, then
// renames. For SaltySD titles the loader is restored afterwards if the move
// carried it away inside a legacy folder.
// ---------------------------------------------------------------------------
static bool tidyLooseMods(const GameProfile &gp, Status *st)
{
    std::vector<LooseItem> loose = collectLoose(gp);

    if (loose.empty()) {
        *st = { "Nothing to tidy - no legacy folders.", SK_WARN };
        return false;
    }

    mkdirs(repoPath(gp));
    int moved = 0, failed = 0;

    for (const LooseItem &li : loose) {
        std::string disp   = sanitizeName(modDisplayName(li.path, li.fallback));
        std::string folder = uniqueRepoFolder(gp, disp);
        if (rename(li.path.c_str(), repoModPath(gp, folder).c_str()) == 0) {
            if (isSalty(gp)) saltyRewrap(repoModPath(gp, folder));
            ++moved;
        } else {
            ++failed;
        }
    }

    invalidateLumaList();               // folders moved in/out of luma/titles
    g_saltyLoaderOk = ensureSaltyLoader(gp);

    if (failed) {
        *st = { "Tidied " + std::to_string(moved) + ", " +
                std::to_string(failed) + " failed.", SK_ERR };
        return false;
    }
    *st = { "Tidied " + std::to_string(moved) + " mod(s) into the repo.", SK_OK };
    if (!g_saltyLoaderOk)
        *st = { "Tidied " + std::to_string(moved) +
                ", but SaltySD code.ips is missing!", SK_WARN };
    return true;
}

// ---------------------------------------------------------------------------
// Game launching. Finds which media the title is installed on (by probing
// its icon file, same as the name lookup) and asks APT to jump to it. The
// actual jump happens once we fall out of the main loop and clean up.
// ---------------------------------------------------------------------------
static bool installedMedia(const std::string &titleIdHex, FS_MediaType *out)
{
    const u64 tid = strtoull(titleIdHex.c_str(), NULL, 16);
    if (tid == 0) return false;

    FS_MediaType order[3] = { MEDIATYPE_SD, MEDIATYPE_GAME_CARD,
                              MEDIATYPE_NAND };
    for (int m = 0; m < 3; ++m) {
        if (order[m] == MEDIATYPE_GAME_CARD) {
            bool in = false;
            if (R_FAILED(FSUSER_CardSlotIsInserted(&in)) || !in) continue;
        }
        Handle f;
        if (R_SUCCEEDED(openTitleIcon(tid, order[m], &f))) {
            FSFILE_Close(f);
            *out = order[m];
            return true;
        }
    }
    return false;
}

static Result doGameJump(const std::string &titleIdHex, FS_MediaType media)
{
    const u64 tid = strtoull(titleIdHex.c_str(), NULL, 16);
    u8 param[0x300] = {0};
    u8 hmac[0x20]   = {0};
    Result rc = APT_PrepareToDoApplicationJump(0, tid, media);
    if (R_FAILED(rc)) return rc;
    return APT_DoApplicationJump(param, sizeof(param), hmac);
}

// ---------------------------------------------------------------------------
// Self-updater. A worker thread checks the GitHub releases API, compares the
// latest tag against APP_VER, downloads the CIA asset, and installs it over
// this title via AM (the running copy keeps working; the update applies on
// the next launch). All network + install work stays off the UI thread.
// ---------------------------------------------------------------------------
static const char *UPDATE_API =
    "https://api.github.com/repos/Felipezwp/3ds-mod-manager/releases/latest";

enum UpdState { UPD_IDLE, UPD_CHECKING, UPD_DOWNLOADING, UPD_INSTALLING,
                UPD_DONE, UPD_UPTODATE, UPD_AVAILABLE, UPD_FAILED };
static volatile UpdState g_updState = UPD_IDLE;
static volatile int      g_updPct   = 0;
static char              g_updTag[32] = "";
static char              g_updErr[48] = "";

// Silent mode: the automatic boot check. It only ever surfaces a toast when
// an update actually exists - offline and up-to-date boots say nothing.
static volatile bool g_updSilent = false;

// When launched from the Homebrew Launcher, update our own .3dsx file on
// the SD instead of AM-installing the CIA title.
static bool        g_is3dsx = false;
static std::string g_selfPath = "sdmc:/3ds/3dsmods.3dsx";

// Record which stage failed with what code - shown in the toast and written
// to update.log so failures are diagnosable over FTP.
static void updLog(const char *fmt, ...)
{
    FILE *f = fopen("sdmc:/3ds/3dsmods/update.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "v" APP_VER " ");
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    fclose(f);
}

static void updFail(const char *stage, Result rc)
{
    snprintf(g_updErr, sizeof(g_updErr), "%s rc=%08lX", stage,
             (unsigned long)rc);
    updLog("%s%s", g_updSilent ? "(silent) " : "", g_updErr);
    // Auto-checks fail silently; only a user-triggered check shows an error.
    g_updState = g_updSilent ? UPD_IDLE : UPD_FAILED;
}

// GET with redirect following; appends the body to `out`.
// The 3DS http/ssl sysmodules can't negotiate modern TLS (GitHub requires
// >= 1.2; httpc dies with D8A0A03C), so networking goes through libcurl +
// mbedTLS over soc:U sockets instead - TLS runs in-process.
static size_t curlWrite(char *data, size_t sz, size_t n, void *ud)
{
    std::vector<u8> *out = (std::vector<u8> *)ud;
    out->insert(out->end(), (u8 *)data, (u8 *)data + sz * n);
    return sz * n;
}

static int curlProgress(void *, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t, curl_off_t)
{
    if (dltotal > 0) g_updPct = (int)(dlnow * 100 / dltotal);
    return 0;
}

static Result httpGet(const std::string &url, std::vector<u8> &out,
                      bool trackPct)
{
    CURL *c = curl_easy_init();
    if (!c) return -1;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);   // no CA bundle on 3DS
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "3dsmods/" APP_VER);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    if (trackPct) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curlProgress);
    }
    const CURLcode cc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (cc == CURLE_OK) return 0;
    // Surface the CURLcode in the description bits for updFail's log line.
    return MAKERESULT(RL_PERMANENT, RS_INTERNAL, RM_APPLICATION, (int)cc);
}

// "v3.4.1" -> {3,4,1}; missing fields are 0.
static void parseVer(const char *s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    if (*s == 'v' || *s == 'V') ++s;
    sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

static bool verNewer(const char *tag)
{
    int a[3], b[3];
    parseVer(tag, a);
    parseVer(APP_VER, b);
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

// Pull `"key":"value"` out of the (flat enough) GitHub JSON.
static std::string jsonStr(const std::string &js, const std::string &key,
                           size_t from = 0)
{
    const std::string pat = "\"" + key + "\":\"";
    const size_t p = js.find(pat, from);
    if (p == std::string::npos) return "";
    const size_t s = p + pat.size();
    const size_t e = js.find('"', s);
    return e == std::string::npos ? "" : js.substr(s, e - s);
}

// Log whether our own title sits in AM's pending/import database - the
// prime suspect for self-updates dying at am-write@0 (it survives reboots).
static void logPendingState()
{
    u32 n = 0;
    if (R_FAILED(AM_GetNumPendingTitles(&n, MEDIATYPE_SD))) return;
    updLog("pending titles on SD: %lu", (unsigned long)n);
    if (n == 0 || n > 32) return;
    u64 ids[32];
    u32 got = 0;
    if (R_FAILED(AM_GetPendingTitleList(&got, ids, n, MEDIATYPE_SD))) return;
    for (u32 i = 0; i < got; ++i)
        if (ids[i] == 0x0004000005BD3700ULL)
            updLog("OUR title is pending/importing");
}

static Result installCia(const std::vector<u8> &cia, const char **stage)
{
    logPendingState();
    *stage = "am-start";
    Handle h;
    Result rc = AM_StartCiaInstall(MEDIATYPE_SD, &h);
    if (R_FAILED(rc)) return rc;

    u64 off = 0;
    while (off < cia.size()) {
        const u32 n = (u32)std::min<size_t>(0x10000, cia.size() - off);
        u32 written = 0;
        rc = FSFILE_Write(h, &written, off, cia.data() + off, n,
                          FS_WRITE_FLUSH);   // U-U parity
        if (R_FAILED(rc)) {
            static char where[24];   // failing offset pinpoints WHAT AM hated
            snprintf(where, sizeof(where), "am-write@%06lX",
                     (unsigned long)off);
            *stage = where;
            AM_CancelCIAInstall(h);
            return rc;
        }
        off += written;
    }

    *stage = "am-finish";
    return AM_FinishCiaInstall(h);
}

// ---------------------------------------------------------------------------
// Update authenticity. TLS verification is off (the 3DS has no usable CA
// store), so a network man-in-the-middle could otherwise feed us a hostile
// CIA that we'd happily install. Every release therefore ships an RSA-2048
// SHA-256 signature (<asset>.sig) made with the maintainer's private key;
// the matching public key is baked in here and downloads that don't verify
// are refused. Unsigned releases no longer install.
// ---------------------------------------------------------------------------
static const char UPDATE_PUBKEY[] =
"-----BEGIN PUBLIC KEY-----\n"
"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAtbueUyWxFbTklknuUSAo\n"
"v63/FF/3kklAuawaoIn7z2QGf9ZANrV85Mj+Byf7wLBiDIbOKJE0jb02n+8X0dE7\n"
"k7Vk3j3itEg7CekqbvLAJ+1pBMsCdxs/mpMSZ8Xq1LtVdNF0JJN36nQNQ+tbY1Mi\n"
"WvyM5N6m0TTrg6mDh/7Ggs4Sqgpr3kvoig4QUa491DcQQQdeTuySuSKVkvucK5cv\n"
"z0r0XqN2R+pCfg6+apraKPFJwVkNRLDR63QnmHCveXRDPX/4xQTIpJyfGTji9DgH\n"
"+oel59uy63i78vEzfKc+8VHT+76IXWQ3r1Ah4afhdvJXwleD5Gx/NeHZt0L8oMN+\n"
"XQIDAQAB\n"
"-----END PUBLIC KEY-----\n";

static bool verifySignature(const std::vector<u8> &data,
                            const std::vector<u8> &sig)
{
    unsigned char hash[32];
    mbedtls_sha256(data.data(), data.size(), hash, 0);
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = false;
    if (mbedtls_pk_parse_public_key(&pk,
            (const unsigned char *)UPDATE_PUBKEY, sizeof(UPDATE_PUBKEY)) == 0)
        ok = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                               sig.data(), sig.size()) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

static bool g_netUp = false;   // soc + curl brought up on first check

// ---------------------------------------------------------------------------
// Plan B installer. AM_StartCiaInstall starts refusing (D8E08027 at the
// first write) after a title has been self-overwritten; the fine-grained
// import API with InstallTitleBeginForOverwrite is what system software
// uses to replace an installed title in place, so fall back to it: parse
// the CIA container and stream ticket -> TMD -> contents -> commit.
// ---------------------------------------------------------------------------
static u32 be32(const u8 *p) { return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3]; }
static u16 be16(const u8 *p) { return (u16)(((u16)p[0]<<8)|p[1]); }
static u64 be64(const u8 *p) { return ((u64)be32(p)<<32)|be32(p+4); }

static Result writeAll(Handle h, const u8 *data, u64 size,
                       const char **stage, const char *what)
{
    u64 off = 0;
    while (off < size) {
        const u32 n = (u32)std::min<u64>(0x10000, size - off);
        u32 written = 0;
        Result rc = FSFILE_Write(h, &written, off, data + off, n,
                                 FS_WRITE_FLUSH);
        if (R_FAILED(rc)) { *stage = what; return rc; }
        off += written;
    }
    return 0;
}

static Result installCiaOverwrite(const std::vector<u8> &cia,
                                  const char **stage)
{
    const u8 *d = cia.data();
    // CIA sections are 0x40-aligned: header, certs, ticket, TMD, content.
    u32 hdrSize, certSize, tikSize, tmdSize;
    memcpy(&hdrSize,  d + 0x00, 4);
    memcpy(&certSize, d + 0x08, 4);
    memcpy(&tikSize,  d + 0x0C, 4);
    memcpy(&tmdSize,  d + 0x10, 4);
    const u64 tikOff = ((u64)((hdrSize + 0x3F) & ~0x3Fu) + certSize + 0x3F) & ~0x3Full;
    const u64 tmdOff = (tikOff + tikSize + 0x3F) & ~0x3Full;
    const u64 cntOff = (tmdOff + tmdSize + 0x3F) & ~0x3Full;
    if (cntOff >= cia.size()) { *stage = "ov-parse"; return -1; }

    const u8 *tmd     = d + tmdOff;
    const u32 sigType = be32(tmd);
    const u32 hdrOff  = sigType == 0x00010003 ? 0x240 :
                        sigType == 0x00010004 ? 0x140 : 0x80;
    const u16 nContent = be16(tmd + hdrOff + 0x9E);
    if (nContent == 0 || nContent > 8) { *stage = "ov-tmd-count"; return -2; }

    const u64 tid = 0x0004000005BD3700ULL;
    Result rc;
    Handle h;

    *stage = "ov-ticket";
    rc = AMNET_InstallTicketBegin(&h);
    if (R_FAILED(rc)) return rc;
    rc = writeAll(h, d + tikOff, tikSize, stage, "ov-ticket-w");
    if (R_SUCCEEDED(rc)) rc = AMNET_InstallTicketFinish(h);
    else AMNET_InstallTicketAbort(h);
    if (R_FAILED(rc)) return rc;

    *stage = "ov-begin";
    rc = AMNET_InstallTitleBeginForOverwrite(tid, MEDIATYPE_SD);
    if (R_FAILED(rc)) return rc;

    *stage = "ov-tmd";
    rc = AMNET_InstallTmdBegin(&h);
    if (R_SUCCEEDED(rc)) {
        rc = writeAll(h, tmd, tmdSize, stage, "ov-tmd-w");
        if (R_SUCCEEDED(rc)) rc = AMNET_InstallTmdFinish(h, true);
    }
    if (R_FAILED(rc)) { AMNET_InstallTitleAbort(); return rc; }

    u64 dataOff = cntOff;   // contents follow in record order
    for (u16 i = 0; i < nContent; ++i) {
        const u8 *rec   = tmd + hdrOff + 0x9C4 + (u32)i * 0x30;
        const u16 index = be16(rec + 4);
        const u64 size  = be64(rec + 8);
        *stage = "ov-content";
        rc = AMNET_InstallContentBegin(&h, index);
        if (R_FAILED(rc)) break;
        rc = writeAll(h, d + dataOff, size, stage, "ov-content-w");
        if (R_SUCCEEDED(rc)) rc = AMNET_InstallContentFinish(h);
        else AMNET_InstallContentCancel(h);
        if (R_FAILED(rc)) break;
        dataOff += size;
    }
    if (R_FAILED(rc)) { AMNET_InstallTitleAbort(); return rc; }

    *stage = "ov-finish";
    rc = AMNET_InstallTitleFinish();
    if (R_FAILED(rc)) return rc;

    *stage = "ov-commit";
    u64 tids[1] = { tid };
    return AMNET_CommitImportTitles(MEDIATYPE_SD, 1, false, tids);
}

static void updWorker(void *)
{
    // Networking is initialized lazily so boot never pays for the 1 MB
    // socket buffer or curl setup - only the first update check does.
    if (!g_netUp) {
        u32 *socBuf = (u32 *)memalign(0x1000, 0x100000);
        if (!socBuf || R_FAILED(socInit(socBuf, 0x100000))) {
            updFail("soc", -1);
            return;
        }
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_netUp = true;
    }

    if (FILE *f = fopen("sdmc:/3ds/3dsmods/update.log", "w")) fclose(f);

    std::vector<u8> body;
    Result rc = httpGet(UPDATE_API, body, false);
    if (R_FAILED(rc) || body.empty()) { updFail("api", rc); return; }
    const std::string js((const char *)body.data(), body.size());

    const std::string tag = jsonStr(js, "tag_name");
    snprintf(g_updTag, sizeof(g_updTag), "%s", tag.c_str());
    if (tag.empty()) { updFail("tag", 0); return; }
    if (!verNewer(tag.c_str())) { g_updState = UPD_UPTODATE; return; }

    // The silent boot check stops here: announce, never install unasked.
    if (g_updSilent) { g_updState = UPD_AVAILABLE; return; }

    // Matching release assets: the binary (.3dsx when we ARE a 3dsx, .cia
    // otherwise) and its mandatory detached signature (<asset>.sig).
    const std::string ext    = g_is3dsx ? ".3dsx" : ".cia";
    const std::string sigExt = ext + ".sig";
    std::string url, sigUrl;
    for (size_t p = 0; (p = js.find("\"browser_download_url\":\"", p))
                       != std::string::npos; ++p) {
        std::string u = jsonStr(js, "browser_download_url", p);
        if (u.size() > sigExt.size() &&
            u.compare(u.size() - sigExt.size(), sigExt.size(), sigExt) == 0)
            sigUrl = u;
        else if (u.size() > ext.size() &&
                 u.compare(u.size() - ext.size(), ext.size(), ext) == 0)
            url = u;
    }
    if (url.empty())    { updFail("asset", 0); return; }
    if (sigUrl.empty()) { updFail("no-sig", 0); return; }

    g_updPct   = 0;
    g_updState = UPD_DOWNLOADING;
    std::vector<u8> cia;
    rc = httpGet(url, cia, true);
    if (R_FAILED(rc) || cia.size() < 0x4000) { updFail("dl", rc); return; }
    updLog("dl ok %u bytes hdr=%02X%02X%02X%02X", (unsigned)cia.size(),
           cia[0], cia[1], cia[2], cia[3]);

    // Authenticity gate: refuse anything the release key didn't sign.
    std::vector<u8> sig;
    rc = httpGet(sigUrl, sig, false);
    if (R_FAILED(rc) || sig.size() < 64) { updFail("sig-dl", rc); return; }
    if (!verifySignature(cia, sig))      { updFail("BAD-SIG", 0); return; }
    updLog("signature ok");

    g_updState = UPD_INSTALLING;
    const char *stage = "install";
    if (g_is3dsx) {
        // Replace our own .3dsx on the SD (write beside it, then swap).
        stage = "fs-write";
        const std::string tmp = g_selfPath + ".new";
        FILE *f = fopen(tmp.c_str(), "wb");
        if (!f) { updFail(stage, -1); return; }
        const bool ok = fwrite(cia.data(), 1, cia.size(), f) == cia.size();
        fclose(f);
        if (!ok) { remove(tmp.c_str()); updFail(stage, -2); return; }
        remove(g_selfPath.c_str());
        if (rename(tmp.c_str(), g_selfPath.c_str()) != 0) {
            updFail("fs-swap", -3);
            return;
        }
    } else {
        rc = installCia(cia, &stage);
        if (R_FAILED(rc)) {
            // A self-installed running title can carry a stale import
            // context (persists across reboots!) that makes the next
            // install die on its first write. Resume + abort that context,
            // then retry once.
            const Result r1 =
                AMNET_InstallTitleResume(MEDIATYPE_SD, 0x0004000005BD3700ULL);
            const Result r2 = AMNET_InstallTitleAbort();
            updLog("unwedge: resume=%08lX abort=%08lX",
                   (unsigned long)r1, (unsigned long)r2);
            rc = installCia(cia, &stage);
        }
        if (R_FAILED(rc)) {
            // Plan B: the system-updater import path (overwrite-in-place).
            updLog("plan B: overwrite import");
            rc = installCiaOverwrite(cia, &stage);
        }
        if (R_FAILED(rc)) {
            updFail(stage, rc);
            // Graceful fallback: park the CIA where FBI can install it.
            mkdirs("sdmc:/cias");
            if (FILE *f = fopen("sdmc:/cias/3dsmods-update.cia", "wb")) {
                if (fwrite(cia.data(), 1, cia.size(), f) == cia.size()) {
                    updLog("fallback cia saved");
                    snprintf(g_updErr, sizeof(g_updErr),
                             "AM refused - saved to /cias, use FBI");
                }
                fclose(f);
            }
            return;
        }
    }
    updLog("installed %s%s", g_updTag, g_is3dsx ? " (3dsx)" : "");
    g_updState = UPD_DONE;
}

static void startUpdateCheck(bool silent = false)
{
    if (g_updState == UPD_CHECKING || g_updState == UPD_DOWNLOADING ||
        g_updState == UPD_INSTALLING || g_updState == UPD_DONE)
        return;
    // Claim the state BEFORE spawning, or two quick presses double-spawn.
    g_updSilent = silent;
    g_updState  = UPD_CHECKING;
    threadCreate(updWorker, NULL, 32 * 1024, 0x31, -2, true);
}

// ---------------------------------------------------------------------------
// SaltySD loader picker. The code.ips must match the game's exact revision
// (a mismatch data-aborts at boot), and different mod packs ship different
// builds - so let the user choose among every copy on the card.
// ---------------------------------------------------------------------------
static std::vector<ModEntry> scanLoaders(const GameProfile &gp)
{
    std::vector<ModEntry> out;
    auto add = [&](const std::string &path, const std::string &name) {
        if (!fileExists(path)) return;
        ModEntry e;
        e.path    = path;
        e.display = name;
        out.push_back(e);
    };
    add(repoPath(gp) + "/code.ips", "Pristine copy (repo root)");
    add(activePath(gp) + "/code.ips",
        "From active: " + modDisplayName(activePath(gp), "mod"));
    if (DIR *dp = opendir(repoPath(gp).c_str())) {
        struct dirent *ent;
        while ((ent = readdir(dp)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
                continue;
            const std::string folder = repoPath(gp) + "/" + ent->d_name;
            if (!isDir(folder)) continue;
            add(folder + "/code.ips",
                "From mod: " + modDisplayName(folder, ent->d_name));
        }
        closedir(dp);
    }
    return out;
}

// Install the chosen loader where Luma applies it, and refresh the pristine
// repo-root copy so self-healing propagates this choice from now on.
static bool installLoader(const GameProfile &gp, const ModEntry &cand,
                          Status *st)
{
    mkdirs(lumaPath(gp));
    const bool ok = copyFile(cand.path, lumaPath(gp) + "/code.ips");
    copyFile(cand.path, repoPath(gp) + "/code.ips");
    invalidateLumaList();
    if (!ok) {
        *st = { "Could not install the loader.", SK_ERR };
        return false;
    }
    g_saltyLoaderOk = true;
    *st = { "Loader installed: " + cand.display, SK_OK };
    return true;
}

// ---------------------------------------------------------------------------
// Settings (theme persistence)
// ---------------------------------------------------------------------------
static void loadSettings()
{
    std::string line = readFirstLine(SETTINGS_TXT);
    if (istartsWith(line, "theme=")) {
        std::string name = line.substr(6);
        for (int i = 0; i < NUM_THEMES; ++i)
            if (iequals(name, THEMES[i].name)) { g_themeIdx = i; break; }
    }
}

static void saveSettings()
{
    mkdirs(MOD_REPO);
    FILE *f = fopen(SETTINGS_TXT, "w");
    if (!f) return;
    fprintf(f, "theme=%s\n", T.name);
    fclose(f);
}

// ===========================================================================
// Rendering primitives (citro2d)
// ===========================================================================

static C2D_TextBuf g_textBuf;   // dynamic glyph buffer, cleared every frame

// Replace a color's alpha channel.
static u32 withAlpha(u32 c, u8 a)
{
    return (c & 0x00FFFFFF) | ((u32)a << 24);
}

// Blend between two colors (component-wise, including alpha).
static u32 lerpColor(u32 a, u32 b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const u8 ar = a & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF, aa = a >> 24;
    const u8 br = b & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF, ba = b >> 24;
    return C2D_Color32((u8)(ar + (br - ar) * t), (u8)(ag + (bg - ag) * t),
                       (u8)(ab + (bb - ab) * t), (u8)(aa + (ba - aa) * t));
}

// Eased accent of the hovered game (its icon's dominant color): the header
// strip, card glow and selection highlight all lean toward it, so every
// game gets its own presence as you scroll. 0 = not established yet.
static u32 g_gameTint = 0;

static u32 tinted(u32 base, float amount)
{
    return g_gameTint ? lerpColor(base, withAlpha(g_gameTint, base >> 24),
                                  amount)
                      : base;
}

// Axis-aligned gradients.
static void vGrad(float x, float y, float w, float h, u32 top, u32 bottom)
{
    C2D_DrawRectangle(x, y, 0.5f, w, h, top, top, bottom, bottom);
}
static void hGrad(float x, float y, float w, float h, u32 left, u32 right)
{
    C2D_DrawRectangle(x, y, 0.5f, w, h, left, right, left, right);
}

// Measure a string's rendered width at the given scale.
static float textWidth(const std::string &s, float scale)
{
    if (s.empty()) return 0.0f;
    C2D_Text t;
    C2D_TextParse(&t, g_textBuf, s.c_str());
    float w, h;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    return w;
}

// Draw text with its top-left corner at (x, y).
static void drawText(float x, float y, float scale, u32 color, const std::string &s)
{
    if (s.empty()) return;
    C2D_Text t;
    C2D_TextParse(&t, g_textBuf, s.c_str());
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

// Draw text right-aligned against `right`.
static void drawTextRight(float right, float y, float scale, u32 color,
                          const std::string &s)
{
    drawText(right - textWidth(s, scale), y, scale, color, s);
}

// Draw text horizontally centred on `cx`.
static void drawTextCenter(float cx, float y, float scale, u32 color,
                           const std::string &s)
{
    drawText(cx - textWidth(s, scale) / 2.0f, y, scale, color, s);
}

// Truncate a string (appending an ellipsis) until it fits in `maxW` pixels.
static std::string fitText(const std::string &s, float scale, float maxW)
{
    if (textWidth(s, scale) <= maxW) return s;

    static const std::string ELL = "…";
    std::string cur = s;
    while (!cur.empty()) {
        // Drop one UTF-8 code point from the end.
        size_t i = cur.size() - 1;
        while (i > 0 && ((unsigned char)cur[i] & 0xC0) == 0x80) --i;
        cur.erase(i);
        if (textWidth(cur + ELL, scale) <= maxW) return cur + ELL;
    }
    return ELL;
}

// Filled rounded rectangle.
static void roundRect(float x, float y, float w, float h, float r, u32 c)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    C2D_DrawRectSolid(x + r, y, 0.5f, w - 2 * r, h, c);
    C2D_DrawRectSolid(x, y + r, 0.5f, w, h - 2 * r, c);
    C2D_DrawCircleSolid(x + r,     y + r,     0.5f, r, c);
    C2D_DrawCircleSolid(x + w - r, y + r,     0.5f, r, c);
    C2D_DrawCircleSolid(x + r,     y + h - r, 0.5f, r, c);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0.5f, r, c);
}

// Small rounded badge ("pill"). Returns its width so callers can lay out
// around it. When drawRight is true, `x` is treated as the RIGHT edge.
static float drawPill(float x, float y, const std::string &label, float scale,
                      u32 bg, u32 fg, bool drawRight)
{
    const float tw = textWidth(label, scale);
    const float th = 30.0f * scale;
    const float w  = tw + 12.0f;
    const float h  = th + 4.0f;
    const float px = drawRight ? x - w : x;
    roundRect(px, y, w, h, h / 2.0f, bg);
    drawText(px + 6.0f, y + 2.0f, scale, fg, label);
    return w;
}

// ---------------------------------------------------------------------------
// Animated background: vertical dusk gradient + two layers of drifting glow
// orbs. Particle hues follow the current theme.
// ---------------------------------------------------------------------------
struct Particle {
    float x, y, r, speed, phase;
    int   hue;      // 0..3, resolved against the theme each frame
    u8    alpha;
};

static u32 particleColor(const Particle &p)
{
    switch (p.hue) {
        case 0:  return withAlpha(T.accent,    p.alpha);
        case 1:  return withAlpha(T.info,      p.alpha);
        case 2:  return withAlpha(T.secondary, p.alpha);
        default: return withAlpha(CLR_GREEN,   p.alpha);
    }
}

static std::vector<Particle> makeParticles(int bigCount, int smallCount, float w)
{
    std::vector<Particle> ps;
    for (int i = 0; i < bigCount + smallCount; ++i) {
        const bool big = i < bigCount;
        Particle p;
        p.x     = (float)(rand() % (int)w);
        p.y     = (float)(rand() % 240);
        p.r     = big ? 28.0f + rand() % 34 : 1.5f + (rand() % 30) / 10.0f;
        p.speed = big ? 0.05f + (rand() % 10) / 100.0f
                      : 0.15f + (rand() % 25) / 100.0f;
        p.phase = (float)(rand() % 628) / 100.0f;
        p.hue   = rand() % 4;
        p.alpha = big ? 10 : 26;
        ps.push_back(p);
    }
    return ps;
}

static void drawBackground(std::vector<Particle> &ps, float w)
{
    vGrad(0, 0, w, 240, T.bgTop, T.bgBot);
    for (Particle &p : ps) {
        p.y -= p.speed;
        if (p.y < -p.r - 10) {                      // wrap to the bottom
            p.y = 250 + p.r;
            p.x = (float)(rand() % (int)w);
        }
        const float wob = sinf(g_t * 0.6f + p.phase) * (p.r > 10 ? 14.0f : 6.0f);
        const float px = p.x + wob;
        if (p.r > 10) {
            // Aurora blob: three concentric fades approximate a soft glow.
            const u32 base = particleColor(p) & 0x00FFFFFF;
            C2D_DrawCircleSolid(px, p.y, 0.5f, p.r * 1.8f, withAlpha(base, 4));
            C2D_DrawCircleSolid(px, p.y, 0.5f, p.r * 1.3f, withAlpha(base, 7));
            C2D_DrawCircleSolid(px, p.y, 0.5f, p.r,        withAlpha(base, 11));
        } else {
            C2D_DrawCircleSolid(px, p.y, 0.5f, p.r, particleColor(p));
        }
    }
}

// Animated two-tone accent strip (used under headers), leaning toward the
// hovered game's color.
static void drawAccentStrip(float x, float y, float w, float h)
{
    const u32 l = lerpColor(T.accent, T.secondary, 0.5f + 0.5f * sinf(g_t * 0.7f));
    const u32 r = lerpColor(T.info,   T.accent,    0.5f + 0.5f * sinf(g_t * 0.7f + 2.1f));
    hGrad(x, y, w, h, tinted(l, 0.45f), tinted(r, 0.45f));
}

// Card panel: drop shadow + soft animated border glow.
static void drawCard(float x, float y, float w, float h)
{
    const float pulse = 0.5f + 0.5f * sinf(g_t * 1.6f);
    const u32 glow = withAlpha(
        tinted(lerpColor(T.accent, T.secondary, pulse), 0.55f), 110);
    roundRect(x + 2.5f, y + 3.5f, w, h, 10.0f, C2D_Color32(0, 0, 0, 90));
    roundRect(x - 1.5f, y - 1.5f, w + 3.0f, h + 3.0f, 11.5f, glow);
    roundRect(x, y, w, h, 10.0f, T.panel);
}

// ---------------------------------------------------------------------------
// UI sounds (ndsp). Blips are synthesized at boot - square waves with a
// frequency sweep and exponential decay - so no audio assets are shipped.
// Requires a dumped DSP firmware (sdmc:/3ds/dspfirm.cdc); if ndspInit fails
// the app simply stays silent.
// ---------------------------------------------------------------------------
enum Snd { SND_MOVE, SND_CONFIRM, SND_BACK, SND_ERROR, SND_COUNT };

static bool        g_ndspUp = false;
static bool        g_sndOk  = false;
static s16        *g_sndData[SND_COUNT] = { NULL };
static ndspWaveBuf g_sndBuf[SND_COUNT];

static s16 *synthBlip(float f0, float f1, float dur, float vol, u32 *outN)
{
    const int n = (int)(32000 * dur);
    s16 *buf = (s16 *)linearAlloc(n * sizeof(s16));
    if (!buf) return NULL;
    float phase = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float t = (float)i / n;
        phase += (f0 + (f1 - f0) * t) / 32000.0f;
        const float env = expf(-t * 6.0f) * (i < 64 ? i / 64.0f : 1.0f);
        const float sq  = (phase - (int)phase) < 0.5f ? 1.0f : -1.0f;
        buf[i] = (s16)(sq * env * vol * 32767.0f);
    }
    *outN = (u32)n;
    return buf;
}

static void sndInit()
{
    if (R_FAILED(ndspInit())) return;   // no dspfirm.cdc -> silent app
    g_ndspUp = true;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, 32000);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(0, mix);

    static const struct { float f0, f1, dur, vol; } SPEC[SND_COUNT] = {
        { 1560.0f, 1560.0f, 0.045f, 0.16f },   // MOVE: short tick
        {  880.0f, 1760.0f, 0.120f, 0.22f },   // CONFIRM: rising chirp
        {  990.0f,  495.0f, 0.100f, 0.20f },   // BACK: falling chirp
        {  220.0f,  180.0f, 0.160f, 0.25f },   // ERROR: low buzz
    };
    for (int i = 0; i < SND_COUNT; ++i) {
        u32 n = 0;
        g_sndData[i] = synthBlip(SPEC[i].f0, SPEC[i].f1, SPEC[i].dur,
                                 SPEC[i].vol, &n);
        if (!g_sndData[i]) return;
        memset(&g_sndBuf[i], 0, sizeof(ndspWaveBuf));
        g_sndBuf[i].data_vaddr = g_sndData[i];
        g_sndBuf[i].nsamples   = n;
        DSP_FlushDataCache(g_sndData[i], n * sizeof(s16));
    }
    g_sndOk = true;
}

static void sndPlay(Snd s)
{
    if (!g_sndOk) return;
    ndspChnWaveBufClear(0);   // a new blip cuts the previous one
    g_sndBuf[s].status = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(0, &g_sndBuf[s]);
}

static void sndExit()
{
    if (g_ndspUp) ndspExit();
    for (int i = 0; i < SND_COUNT; ++i)
        if (g_sndData[i]) linearFree(g_sndData[i]);
}

// Threaded boot handshake (worker defined near main; the draw code only
// needs the flag to render the scanning state).
static volatile bool g_bootReady = false;

// ---------------------------------------------------------------------------
// Motion. Everything animates by interpolating draw coordinates per frame -
// the same batched quads render either way, so 60 fps costs nothing extra.
// ---------------------------------------------------------------------------
static float g_screenAnim = 1.0f;   // 0 -> 1 after each screen change
static float g_selAnim    = 0.0f;   // eased highlight slot in bottom lists
static bool  g_selSnap    = true;   // teleport the highlight next frame
static float g_statusAge  = 999.0f; // seconds since the toast text changed


static float easeOutCubic(float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// Slide-in offset for list rows, staggered top to bottom.
static float rowSlide(int slot)
{
    return (1.0f - easeOutCubic(g_screenAnim * 1.35f - slot * 0.055f)) * 46.0f;
}

// Ease the shared selection highlight toward the given viewport slot.
static void trackSelection(int slot)
{
    if (g_selSnap) { g_selAnim = (float)slot; g_selSnap = false; }
    g_selAnim += ((float)slot - g_selAnim) * 0.38f;
}

// ===========================================================================
// Screens
// ===========================================================================

static void drawTopHeader(const char *screenTitle)
{
    hGrad(0, 0, 400, 32, T.panel, withAlpha(T.panel2, 0xEE));
    drawAccentStrip(0, 32, 400, 2);
    drawText(12, 6, 0.6f, CLR_WHITE, screenTitle);

    // osGetTime: ms since 1900-01-01 in local time -> wall clock.
    const u32 daySec = (u32)((osGetTime() / 1000) % 86400);
    char clk[8];
    snprintf(clk, sizeof(clk), "%02u:%02u",
             (unsigned)(daySec / 3600), (unsigned)((daySec / 60) % 60));
    drawTextCenter(200, 11, 0.42f, T.muted, clk);

    // Battery (ptm:u), polled every ~2 s. Green > yellow > red; a slow white
    // pulse means charging.
    static u8  battLvl = 5, battChg = 0;
    static int battTick = 0;
    if (battTick-- <= 0) {
        battTick = 120;
        PTMU_GetBatteryLevel(&battLvl);
        PTMU_GetBatteryChargeState(&battChg);
    }
    const float bx = 370, by = 12;
    C2D_DrawRectSolid(bx, by, 0.5f, 18, 9, T.muted);            // shell
    C2D_DrawRectSolid(bx + 18, by + 2.5f, 0.5f, 2, 4, T.muted); // nub
    C2D_DrawRectSolid(bx + 1, by + 1, 0.5f, 16, 7, T.bgTop);    // hollow
    u32 fill = CLR_GREEN;
    if      (battLvl <= 1) fill = CLR_RED;
    else if (battLvl == 2) fill = CLR_YELLOW;
    if (battChg)
        fill = lerpColor(fill, CLR_WHITE, 0.25f + 0.25f * sinf(g_t * 3.0f));
    const u8 lvl = battLvl > 5 ? 5 : battLvl;
    if (lvl)
        C2D_DrawRectSolid(bx + 1, by + 1, 0.5f, 16.0f * lvl / 5.0f, 7, fill);

    drawTextRight(364, 11, 0.42f, T.muted, "3DS Mod Manager v" APP_VER);
}

static void drawTopFooter()
{
    C2D_DrawRectSolid(0, 222, 0.5f, 400, 18, T.panel);

    // SD free space, refreshed every ~5 s.
    static char freeTxt[32] = "";
    static int  tick = 0;
    if (tick-- <= 0) {
        tick = 300;
        FS_ArchiveResource r;
        if (R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&r)))
            snprintf(freeTxt, sizeof(freeTxt), "SD free: %.1f GB",
                     (u64)r.freeClusters * r.clusterSize /
                         (1024.0 * 1024.0 * 1024.0));
    }
    drawText(8, 225, 0.4f, T.muted, freeTxt);
    drawTextRight(392, 225, 0.4f, T.muted, "mods: 3ds/3dsmods");
}

// Top screen while browsing the game list: live details of the highlighted game.
static void drawTopGames(const std::vector<GameProfile> &profiles, int cursor)
{
    drawTopHeader("Games");

    if (profiles.empty()) {
        drawCard(20, 60, 360, 120);
        if (!g_bootReady) {
            drawTextCenter(200, 95, 0.55f, CLR_WHITE, "Scanning your games");
            drawTextCenter(200, 125, 0.42f, T.muted,
                           "Reading names, icons and mods...");
        } else {
            drawTextCenter(200, 95, 0.55f, CLR_YELLOW, "No games found");
            drawTextCenter(200, 125, 0.42f, T.muted,
                           "Add mods to 3ds/3dsmods/<TitleID>/");
        }
        drawTopFooter();
        return;
    }

    const GameProfile &gp = profiles[cursor];

    drawCard(20, 52, 360, 132);
    const C2D_Image *ic = gameIcon(gp.titleId);
    float tx = 36;
    if (ic) {
        roundRect(33, 59, 54, 54, 6, T.panel2);
        C2D_DrawImageAt(*ic, 36, 62, 0.5f, NULL, 1.0f, 1.0f);
        tx = 100;
    }
    drawText(tx, 66, 0.7f, CLR_WHITE, fitText(gp.title, 0.7f, 364 - tx));
    drawText(tx, 96, 0.45f, T.muted, "Title ID");
    drawText(tx + 74, 96, 0.45f, T.secondary, gp.titleId);

    float x = 36;
    if (gp.hasActive)
        x += drawPill(36, 122, "MODDED", 0.42f, CLR_GREEN, CLR_DARK, false) + 10;
    else
        x += drawPill(36, 122, "VANILLA", 0.42f, T.panel2, T.text, false) + 10;
    if (isSalty(gp))
        x += drawPill(x, 122, "SaltySD", 0.42f, T.info, CLR_DARK, false) + 10;
    drawText(x, 124, 0.45f, T.info,
             std::to_string(gp.modCount) + (gp.modCount == 1 ? " mod" : " mods"));

    drawText(36, 156, 0.42f, T.muted,
             G_A " manage mods    " G_X " play now");

    drawTopFooter();
}

// Top screen inside a game's mod menu.
static void drawTopMods(const GameProfile &gp, const std::vector<ModEntry> &mods)
{
    drawTopHeader("Mods");

    std::string activeName;
    int looseCount = 0;
    for (const ModEntry &m : mods) {
        if (m.active) activeName = m.display;
        if (m.loose)  ++looseCount;
    }

    drawCard(20, 52, 360, 126);
    const C2D_Image *ic = gameIcon(gp.titleId);
    float tx = 36;
    if (ic) {
        const float s = 40.0f / 48.0f;
        roundRect(33, 59, 46, 46, 5, T.panel2);
        C2D_DrawImageAt(*ic, 36, 62, 0.5f, NULL, s, s);
        tx = 90;
    }
    drawText(tx, 64, 0.62f, CLR_WHITE, fitText(gp.title, 0.62f, 364 - tx));
    drawText(tx, 92, 0.42f, T.muted, "Title ID");
    drawText(tx + 68, 92, 0.42f, T.secondary, gp.titleId);
    if (isSalty(gp))   // right-aligned at the card edge, clear of the ID text
        drawPill(364, 88, "SaltySD", 0.4f, T.info, CLR_DARK, true);

    drawText(36, 118, 0.45f, T.muted, "Active");
    if (!activeName.empty())
        drawPill(96, 115, fitText(activeName, 0.42f, 240), 0.42f, CLR_GREEN, CLR_DARK, false);
    else
        drawPill(96, 115, "none - vanilla", 0.42f, T.panel2, T.text, false);

    int nMods = 0;
    for (const ModEntry &m : mods)
        if (!m.loader) ++nMods;
    drawText(36, 150, 0.45f, T.muted, "Library");
    drawText(104, 150, 0.45f, T.info,
             std::to_string(nMods) + (nMods == 1 ? " mod" : " mods"));

    if (isSalty(gp) && !g_saltyLoaderOk) {
        roundRect(20, 188, 360, 26, 7, CLR_RED);
        drawTextCenter(200, 193, 0.42f, CLR_DARK,
                       "SaltySD loader missing: luma/titles/.../code.ips");
    } else if (looseCount > 0) {
        roundRect(20, 188, 360, 26, 7, CLR_ORANGE);
        drawTextCenter(200, 193, 0.42f, CLR_DARK,
                       std::to_string(looseCount) +
                       " legacy folder(s) found - press " G_Y " to tidy");
    }

    drawTopFooter();
}

// Top screen in the SaltySD loader picker.
static void drawTopLoader()
{
    drawTopHeader("SaltySD loader");
    drawCard(20, 52, 360, 132);
    drawText(36, 64, 0.55f, CLR_WHITE, "Pick the code.ips patch");
    drawText(36, 92, 0.42f, T.muted,
             "SaltySD only works when the loader matches your");
    drawText(36, 110, 0.42f, T.muted,
             "game's exact revision. If the game crashes at boot,");
    drawText(36, 128, 0.42f, T.muted,
             "come back here and try another copy.");
    drawText(36, 156, 0.42f, T.muted, G_A " install    " G_B " back");
    drawTopFooter();
}

// Top screen in the theme picker.
static void drawTopThemes()
{
    drawTopHeader("Themes");
    drawCard(20, 60, 360, 110);
    drawText(36, 74, 0.66f, CLR_WHITE, T.name);
    drawText(36, 106, 0.45f, T.muted, "Changes apply instantly.");
    drawText(36, 128, 0.45f, T.muted,
             "Press " G_A " or " G_B " to keep this theme.");
    drawTopFooter();
}

// Bottom-screen chrome: header bar with title + index, hint bar at the bottom.
static void drawBottomChrome(const std::string &title, int cursor, int total,
                             const std::string &hints, bool backZone = false)
{
    hGrad(0, 0, 320, 26, T.panel, withAlpha(T.panel2, 0xEE));
    drawAccentStrip(0, 26, 320, 2);
    float tx = 8;
    if (backZone) {   // tappable back corner (matches the touch hit test)
        drawText(8, 3, 0.52f, T.muted, "<");
        tx = 22;
    }
    drawText(tx, 4, 0.5f, CLR_WHITE, fitText(title, 0.5f, 252 - tx));
    if (total > 0)
        drawPill(314, 5, std::to_string(cursor + 1) + "/" + std::to_string(total),
                 0.36f, T.panel2, T.info, true);

    drawAccentStrip(0, 215, 320, 1);
    C2D_DrawRectSolid(0, 216, 0.5f, 320, 24, T.panel);
    drawText(8, 221, 0.42f, T.text, hints);
}

// Scrolling viewport over `total` items: picks [start, end) so the cursor
// stays centred once the list outgrows the window.
static void viewport(int total, int cursor, int &start, int &end)
{
    start = 0;
    if (total > LIST_ROWS) {
        start = cursor - LIST_ROWS / 2;
        if (start < 0) start = 0;
        if (start > total - LIST_ROWS) start = total - LIST_ROWS;
    }
    end = start + LIST_ROWS;
    if (end > total) end = total;
}

// Shared list navigation: Up/Down wrap one row, Left/Right jump a page,
// drag rows clamp - one implementation for games, mods and themes.
// Returns true when the cursor moved (caller plays the tick).
static bool navList(int &cursor, int n, u32 kNav, int dragRows)
{
    if (n <= 0) return false;
    const int before = cursor;
    if (kNav & KEY_DOWN) cursor = (cursor + 1) % n;
    if (kNav & KEY_UP)   cursor = (cursor - 1 + n) % n;
    int jump = dragRows;
    if (kNav & KEY_RIGHT) jump += LIST_ROWS;
    if (kNav & KEY_LEFT)  jump -= LIST_ROWS;
    if (jump) {
        cursor += jump;
        if (cursor < 0)  cursor = 0;
        if (cursor >= n) cursor = n - 1;
    }
    return cursor != before;
}

// Scrollbar along the right edge of the list area.
static void drawScrollbar(int total, int start)
{
    if (total <= LIST_ROWS) return;
    const float trackY = LIST_Y, trackH = LIST_ROWS * ROW_H;
    roundRect(314, trackY, 4, trackH, 2, withAlpha(T.panel2, 120));
    const float thumbH = trackH * (float)LIST_ROWS / total;

    // Ease the thumb toward its target so scrolling feels fluid.
    static float smoothY = -1.0f;
    const float targetY = trackY + (trackH - thumbH) * (float)start /
                          (total - LIST_ROWS);
    if (smoothY < 0) smoothY = targetY;
    smoothY += (targetY - smoothY) * 0.35f;

    roundRect(313, smoothY - 1, 6, thumbH + 2, 3, withAlpha(T.accent, 70));
    roundRect(314, smoothY, 4, thumbH, 2, T.accent);
}

// One list row with an optional right-aligned badge.
static void drawListRow(int slot, const std::string &name, bool selected,
                        const char *badge, u32 badgeBg,
                        const C2D_Image *icon = NULL,
                        const std::string &sub = "")
{
    const float y    = LIST_Y + slot * ROW_H;
    const float x    = 4 + rowSlide(slot);
    const float rowW = 306;

    if (selected) {
        // The highlight lives at the EASED slot so it glides between rows;
        // the row's own content stays put and simply brightens.
        const float hy    = LIST_Y + g_selAnim * ROW_H;
        const float pulse = 0.5f + 0.5f * sinf(g_t * 2.4f);
        roundRect(x + 2, hy + 4, rowW, ROW_H - 5, 8, C2D_Color32(0, 0, 0, 80));
        roundRect(x - 1, hy + 1, rowW + 2, ROW_H - 3, 8,
                  withAlpha(tinted(lerpColor(T.accent, T.secondary, pulse),
                                   0.40f), 95));
        roundRect(x + 1, hy + 3, rowW - 2, ROW_H - 7, 7,
                  lerpColor(T.selL, T.selR, 0.5f + 0.5f * sinf(g_t * 0.9f)));
        C2D_DrawRectSolid(x + 4, hy + 7, 0.5f, 3, ROW_H - 15,
                          lerpColor(T.accent, T.info, pulse));
    } else {
        roundRect(x, y + 2, rowW, ROW_H - 5, 7,
                  withAlpha(T.panel2, slot % 2 ? 46 : 70));
    }

    float badgeW = 0;
    if (badge && badge[0])
        badgeW = drawPill(x + rowW - 8, y + 6, badge, 0.38f, badgeBg,
                          CLR_DARK, true) + 8;

    float tx = x + 12;
    if (icon) {
        const float s = (ROW_H - 6) / 48.0f;   // 22px square
        C2D_DrawImageAt(*icon, x + 8, y + 3, 0.5f, NULL, s, s);
        tx = x + 36;
    }

    float subW = 0;
    if (!sub.empty() && ROW_H >= 26) {
        subW = textWidth(sub, 0.36f) + 8;
        drawTextRight(x + rowW - 10 - badgeW, y + 8, 0.36f,
                      selected ? withAlpha(CLR_WHITE, 0xB4) : T.muted, sub);
    }

    const float nameMax = (x + rowW - 8) - badgeW - subW - tx;
    drawText(tx, y + 6, 0.5f, selected ? CLR_WHITE : T.text,
             fitText(name, 0.5f, nameMax));
}

// Status toast: slides up when the text changes, lingers, then slides away.
static void drawStatus(const Status &st)
{
    if (st.msg.empty()) return;
    const float SHOW = 4.2f, IN = 0.22f, OUT = 0.3f;
    if (g_statusAge > SHOW + OUT) return;

    float yoff = 0;                                 // 0 = fully shown
    if (g_statusAge < IN)
        yoff = (1.0f - easeOutCubic(g_statusAge / IN)) * 20.0f;
    else if (g_statusAge > SHOW)
        yoff = easeOutCubic((g_statusAge - SHOW) / OUT) * 20.0f;

    u32 c = T.text;
    if      (st.kind == SK_OK)   c = CLR_GREEN;
    else if (st.kind == SK_WARN) c = CLR_YELLOW;
    else if (st.kind == SK_ERR)  c = CLR_RED;
    const float y = 198 + yoff;
    C2D_DrawRectSolid(0, y, 0.5f, 320, 18, lerpColor(T.panel2, c, 0.18f));
    C2D_DrawRectSolid(0, y, 0.5f, 3, 18, c);
    drawText(10, y + 3, 0.42f, c, fitText(st.msg, 0.42f, 302));
}

// Bottom screen: game list.
static void drawBottomGames(const std::vector<GameProfile> &profiles, int cursor,
                            const Status &st)
{
    drawBottomChrome("Select a game", cursor, (int)profiles.size(),
                     G_A " Open  " G_X " Play  " G_Y " Update  SEL Themes");

    if (profiles.empty()) {
        if (!g_bootReady) {
            const int dots = 1 + ((int)(g_t * 2.5f) % 3);
            drawTextCenter(160, 100, 0.5f, T.muted,
                           std::string("Scanning") + std::string(dots, '.'));
        } else {
            drawTextCenter(160, 100, 0.5f, T.muted, "No games found");
            drawTextCenter(160, 125, 0.4f, T.muted,
                           "3ds/3dsmods/<TitleID>/<mod>/");
        }
        return;
    }

    const int total = (int)profiles.size();
    int start, end;
    viewport(total, cursor, start, end);
    trackSelection(cursor - start);

    for (int i = start; i < end; ++i) {
        const GameProfile &gp = profiles[i];
        std::string sub;
        if (gp.modCount > 0)
            sub = std::to_string(gp.modCount) +
                  (gp.modCount == 1 ? " mod" : " mods");
        drawListRow(i - start, gp.title, i == cursor,
                    gp.hasActive ? "ON" : "", T.info,
                    gameIcon(gp.titleId), sub);
    }

    drawScrollbar(total, start);
    drawStatus(st);
}

// Bottom screen: mod list for the selected game.
static void drawBottomMods(const GameProfile &gp, const std::vector<ModEntry> &mods,
                           int cursor, const Status &st)
{
    drawBottomChrome(gp.title, cursor, (int)mods.size(),
                     G_A " Use  " G_X " Vanilla  " G_Y " Tidy  " G_B " Back",
                     true);

    if (mods.empty()) {
        drawTextCenter(160, 95, 0.5f, T.muted, "No mods found");
        drawTextCenter(160, 120, 0.4f, T.muted,
                       "3ds/3dsmods/" + gp.titleId + "/");
    } else {
        const int total = (int)mods.size();
        int start, end;
        viewport(total, cursor, start, end);
        trackSelection(cursor - start);

        for (int i = start; i < end; ++i) {
            const ModEntry &m = mods[i];
            const char *badge = m.active ? "ACTIVE"
                              : m.loose  ? "LOOSE"
                              : m.loader ? "SETUP" : "";
            drawListRow(i - start, m.display, i == cursor, badge,
                        m.active ? CLR_GREEN : m.loader ? T.info : CLR_ORANGE);
        }

        drawScrollbar(total, start);
    }

    drawStatus(st);
}

// Bottom screen: SaltySD loader picker.
static void drawBottomLoader(const std::vector<ModEntry> &cands, int cursor,
                             const Status &st)
{
    drawBottomChrome("Choose loader", cursor, (int)cands.size(),
                     G_A " Install  " G_B " Back", true);
    if (cands.empty()) {
        drawTextCenter(160, 100, 0.5f, T.muted, "No code.ips found");
        drawTextCenter(160, 125, 0.4f, T.muted,
                       "Put one in a mod folder or the repo root");
    } else {
        int start, end;
        viewport((int)cands.size(), cursor, start, end);
        trackSelection(cursor - start);
        for (int i = start; i < end; ++i)
            drawListRow(i - start, cands[i].display, i == cursor, "", 0);
        drawScrollbar((int)cands.size(), start);
    }
    drawStatus(st);
}

// Bottom screen: theme picker. Each row previews its palette as color dots.
static void drawBottomThemes(int cursor)
{
    drawBottomChrome("Themes", cursor, NUM_THEMES,
                     G_DPAD " Move  " G_A " Keep  " G_B " Back", true);

    int start, end;
    viewport(NUM_THEMES, cursor, start, end);
    trackSelection(cursor - start);

    for (int i = start; i < end; ++i) {
        const int slot = i - start;
        const float y  = LIST_Y + slot * ROW_H;
        drawListRow(slot, THEMES[i].name, i == cursor, "", 0);
        // palette preview dots (accent / secondary / info)
        const float dx = 4 + rowSlide(slot);
        const float cy = y + ROW_H / 2.0f;
        C2D_DrawCircleSolid(dx + 258, cy, 0.5f, 4.5f, THEMES[i].accent);
        C2D_DrawCircleSolid(dx + 274, cy, 0.5f, 4.5f, THEMES[i].secondary);
        C2D_DrawCircleSolid(dx + 290, cy, 0.5f, 4.5f, THEMES[i].info);
    }
    drawScrollbar(NUM_THEMES, start);
}

// ---------------------------------------------------------------------------
// Threaded boot: the whole discovery pipeline (DSP firmware load, name/icon
// caches, directory scans, stats) runs off the UI thread so the first frame
// renders immediately. The worker publishes the finished profile list via
// g_bootReady; icons arrive through the pending-icon queue.
// ---------------------------------------------------------------------------
static std::vector<GameProfile> g_bootProfiles;

static void bootWorker(void *)
{
    sndInit();          // reads dspfirm.cdc from SD - off the boot path
    mkdirs(MOD_REPO);   // first run: make the repo visible to FTP users
    loadSaltyList();
    loadNameCache();

    std::vector<GameProfile> p = discoverProfiles();
    for (GameProfile &gp : p)
        refreshStats(gp);
    // A folder alone doesn't make a game: leftover luma/ModMoon dirs with
    // no mods, nothing active and nothing to tidy would clutter the list.
    p.erase(std::remove_if(p.begin(), p.end(),
                [](const GameProfile &g) { return g.modCount == 0; }),
            p.end());
    saveNameCache();

    // Flush the SMDH attempt trace for off-device diagnosis.
    if (FILE *lf = fopen(LOOKUP_LOG, "w")) {
        fprintf(lf, "v" APP_VER " hits=%d lastRc=%08lX\n", g_smdhHits,
                (unsigned long)g_smdhLastRc);
        fputs(g_smdhLog.c_str(), lf);
        fclose(lf);
    }

    g_bootProfiles = std::move(p);
    g_bootReady = true;

    // Quiet background update check now that boot work is done. Says
    // nothing unless a newer release actually exists.
    startUpdateCheck(true);
}

// ---------------------------------------------------------------------------
// Program entry point.
// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    osSetSpeedupEnable(true);   // New3DS: 804MHz + L2 (no-op on old units)

    // Running from the Homebrew Launcher? Then self-updates rewrite our own
    // .3dsx (whose path HBL passes in argv[0]) instead of installing a CIA.
    g_is3dsx = envIsHomebrew();
    if (argc > 0 && argv && argv[0] &&
        strncmp(argv[0], "sdmc:/", 6) == 0)
        g_selfPath = argv[0];

    gfxInitDefault();
    ptmuInit();     // battery level for the header indicator
    amInit();       // self-updater: CIA install (cheap; net init is lazy)
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    fontEnsureMapped();   // system shared font for all text

    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_textBuf = C2D_TextBufNew(8192);

    srand((unsigned)svcGetSystemTick());
    std::vector<Particle> topParticles = makeParticles(5, 18, 400);
    std::vector<Particle> botParticles = makeParticles(4, 14, 320);

    loadSettings();   // theme must be right on the very first frame

    // Everything slow happens on the boot worker; the UI starts now.
    LightLock_Init(&g_iconLock);
    std::vector<GameProfile> profiles;
    bool bootLoaded = false;
    threadCreate(bootWorker, NULL, 64 * 1024, 0x31, -2, true);

    enum AppState { ST_GAMES, ST_MODS, ST_THEMES, ST_LOADER };
    std::vector<ModEntry> loaders;   // SaltySD loader picker candidates
    int loaderCursor = 0;
    AppState state       = ST_GAMES;
    AppState themeReturn = ST_GAMES;

    int gameCursor  = 0;
    int selected    = 0;
    int modCursor   = 0;
    int themeCursor = g_themeIdx;
    std::vector<ModEntry> mods;
    Status status = { "", SK_NEUTRAL };

    // If not a single title name resolved via SMDH, surface the last FS error
    // so the failure is diagnosable on-device (e.g. missing CIA permissions).
    if (!profiles.empty() && g_smdhHits == 0 && g_smdhLastRc != 0) {
        char rcbuf[16];
        snprintf(rcbuf, sizeof(rcbuf), "%08lX", (unsigned long)g_smdhLastRc);
        status = { std::string("Name lookup failed: 0x") + rcbuf, SK_WARN };
    }

    // Refresh the open game's mod list after an action, keeping the cursor
    // in range (the same three lines used to follow every action).
    const auto refreshModList = [&](const GameProfile &g) {
        mods = rescanMods(g);
        if (modCursor >= (int)mods.size())
            modCursor = mods.empty() ? 0 : (int)mods.size() - 1;
    };

    // Quit fade: -1 = running; >= 0 counts up to 1 while fading to black.
    // If jumpTid is set when the fade completes, we jump to that title
    // instead of just exiting.
    float        quitT = -1.0f;
    std::string  jumpTid;
    FS_MediaType jumpMedia = MEDIATYPE_SD;
    bool         jumped    = false;   // jump requested; waiting to be closed

    while (aptMainLoop()) {
        // Adopt the boot worker's results the moment they're ready, and
        // turn any queued icon pixels into GPU textures (main thread only).
        if (!bootLoaded && g_bootReady) {
            profiles.swap(g_bootProfiles);
            bootLoaded = true;
        }
        drainIcons();

        hidScanInput();
        u32 kDown      = hidKeysDown();          // one-shot: actions
        const u32 kNav = quitT < 0 ? hidKeysDownRepeat() : 0;

        if (quitT < 0 && (kDown & KEY_START))
            quitT = 0.0f;                        // start the fade-out
        if (quitT >= 0)
            kDown = 0;                           // no input while fading

        // Touchscreen: drag to scroll (the list follows the finger a row at
        // a time), release without dragging to tap - tap a row to highlight
        // it, tap it again to activate. Header's left edge is a back zone.
        // Pure input math every frame; nothing here touches the SD card.
        static touchPosition tStart;
        static int  tPrevY    = 0;
        static bool tDragging = false;
        static float tAccumY  = 0.0f;
        int  touchRow  = -1;    // tap released on this viewport slot
        bool touchBack = false; // tap released in the back zone
        int  dragRows  = 0;     // cursor rows to move from this frame's drag

        if (kDown & KEY_TOUCH) {
            hidTouchRead(&tStart);
            tPrevY    = tStart.py;
            tDragging = false;
            tAccumY   = 0.0f;
        } else if (quitT < 0 && (hidKeysHeld() & KEY_TOUCH)) {
            touchPosition tp;
            hidTouchRead(&tp);
            tAccumY += (float)(tPrevY - tp.py);   // finger up = forward
            tPrevY   = tp.py;
            const int total = tp.py - tStart.py;
            if (!tDragging && (total > 8 || total < -8)) tDragging = true;
            if (tDragging) {
                dragRows = (int)(tAccumY / ROW_H);
                tAccumY -= dragRows * ROW_H;
            }
        }
        if (quitT < 0 && (hidKeysUp() & KEY_TOUCH) && !tDragging) {
            if (tStart.px < 312 && tStart.py >= LIST_Y &&
                tStart.py < LIST_Y + LIST_ROWS * ROW_H)
                touchRow = (int)((tStart.py - LIST_Y) / ROW_H);
            else if (tStart.py < 26 && tStart.px < 70)
                touchBack = true;
        }

        // L / R cycle the theme from anywhere.
        if (kDown & (KEY_L | KEY_R)) {
            g_themeIdx = (g_themeIdx +
                          ((kDown & KEY_R) ? 1 : NUM_THEMES - 1)) % NUM_THEMES;
            themeCursor = g_themeIdx;
            saveSettings();
            status = { std::string("Theme: ") + T.name, SK_OK };
            sndPlay(SND_MOVE);
        }


        // ------------------------------ input ------------------------------
        if (state == ST_THEMES) {
            if (navList(themeCursor, NUM_THEMES, kNav, dragRows))
                sndPlay(SND_MOVE);

            if (touchRow >= 0) {
                int vs, ve;
                viewport(NUM_THEMES, themeCursor, vs, ve);
                const int idx = vs + touchRow;
                if (idx < ve) {
                    if (idx == themeCursor) touchBack = true;  // keep + close
                    else { themeCursor = idx; sndPlay(SND_MOVE); }
                }
            }
            g_themeIdx = themeCursor;   // live preview

            if ((kDown & (KEY_A | KEY_B | KEY_SELECT)) || touchBack) {
                saveSettings();
                state = themeReturn;
                sndPlay(SND_BACK);
            }
        }
        else if (state == ST_GAMES) {
            const int n = (int)profiles.size();

            if (navList(gameCursor, n, kNav, dragRows))
                sndPlay(SND_MOVE);

            bool actOpen = false;
            if (touchRow >= 0 && n > 0) {
                int vs, ve;
                viewport(n, gameCursor, vs, ve);
                const int idx = vs + touchRow;
                if (idx < ve) {
                    if (idx == gameCursor) actOpen = true;
                    else { gameCursor = idx; sndPlay(SND_MOVE); }
                }
            }

            if (kDown & KEY_SELECT) {
                themeReturn = ST_GAMES;
                themeCursor = g_themeIdx;
                state = ST_THEMES;
                sndPlay(SND_CONFIRM);
            }
            else if (actOpen || (n > 0 && (kDown & KEY_A))) {
                selected  = gameCursor;
                {   // cache hit = instant menu; miss = scan once and keep
                    const std::vector<ModEntry> *c =
                        cachedMods(profiles[selected].titleId);
                    mods = c ? *c : rescanMods(profiles[selected]);
                }
                modCursor = 0;
                status    = { "", SK_NEUTRAL };
                g_saltyLoaderOk = ensureSaltyLoader(profiles[selected]);
                state = ST_MODS;
                sndPlay(SND_CONFIRM);
            }
            else if (n > 0 && (kDown & KEY_X)) {
                // Launch the highlighted game (fade out, then APT jump).
                const GameProfile &gp = profiles[gameCursor];
                if (installedMedia(gp.titleId, &jumpMedia)) {
                    jumpTid = gp.titleId;
                    quitT   = 0.0f;
                    status  = { "Launching " + gp.title + "...", SK_OK };
                    sndPlay(SND_CONFIRM);
                } else {
                    status  = { "Not installed - can't launch.", SK_WARN };
                    sndPlay(SND_ERROR);
                }
            }
            else if (kDown & KEY_Y) {
                startUpdateCheck();
                sndPlay(SND_CONFIRM);
            }
        }
        else if (state == ST_MODS) {
            const GameProfile &gp = profiles[selected];

            if (navList(modCursor, (int)mods.size(), kNav, dragRows))
                sndPlay(SND_MOVE);

            bool actUse = false;
            if (touchRow >= 0 && !mods.empty()) {
                int vs, ve;
                viewport((int)mods.size(), modCursor, vs, ve);
                const int idx = vs + touchRow;
                if (idx < ve) {
                    if (idx == modCursor) actUse = true;
                    else { modCursor = idx; sndPlay(SND_MOVE); }
                }
            }

            if ((kDown & KEY_B) || touchBack) {
                state      = ST_GAMES;
                gameCursor = selected;
                // `mods` is already fresh (rescanned after every action), so
                // derive the stats from it - zero SD reads on backout.
                profiles[selected].modCount  = (int)mods.size();
                profiles[selected].hasActive =
                    !mods.empty() && mods.front().active;
                sndPlay(SND_BACK);
            }
            else if (kDown & KEY_SELECT) {
                themeReturn = ST_MODS;
                themeCursor = g_themeIdx;
                state = ST_THEMES;
                sndPlay(SND_CONFIRM);
            }
            else if (kDown & KEY_X) {
                sndPlay(disableMod(gp, &status) ? SND_CONFIRM : SND_ERROR);
                refreshModList(gp);
            }
            else if (kDown & KEY_Y) {
                sndPlay(tidyLooseMods(gp, &status) ? SND_CONFIRM : SND_ERROR);
                refreshModList(gp);
            }
            else if (!mods.empty()) {
                if (actUse || (kDown & KEY_A)) {
                    if (mods[modCursor].loader) {
                        // The pseudo-entry opens the loader picker.
                        loaders      = scanLoaders(gp);
                        loaderCursor = 0;
                        state        = ST_LOADER;
                        sndPlay(SND_CONFIRM);
                    } else {
                        // On success the activated mod sorts to the top;
                        // follow it so the selection tracks what you did.
                        if (activateMod(gp, mods[modCursor], &status)) {
                            modCursor = 0;
                            sndPlay(SND_CONFIRM);
                        } else {
                            sndPlay(SND_ERROR);
                        }
                        refreshModList(gp);
                    }
                }
            }
        }

        else { // ST_LOADER
            if (navList(loaderCursor, (int)loaders.size(), kNav, dragRows))
                sndPlay(SND_MOVE);

            bool actUse = false;
            if (touchRow >= 0 && !loaders.empty()) {
                int vs, ve;
                viewport((int)loaders.size(), loaderCursor, vs, ve);
                const int idx = vs + touchRow;
                if (idx < ve) {
                    if (idx == loaderCursor) actUse = true;
                    else { loaderCursor = idx; sndPlay(SND_MOVE); }
                }
            }

            if ((kDown & KEY_B) || touchBack) {
                state = ST_MODS;
                sndPlay(SND_BACK);
            }
            else if (!loaders.empty() && (actUse || (kDown & KEY_A))) {
                sndPlay(installLoader(profiles[selected],
                                      loaders[loaderCursor], &status)
                            ? SND_CONFIRM : SND_ERROR);
                state = ST_MODS;
            }
        }

        // Self-updater progress -> status toast (kept fresh every frame so
        // the download percentage ticks and the toast doesn't time out).
        switch (g_updState) {
            case UPD_CHECKING:
                if (!g_updSilent)
                    status = { "Checking for updates...", SK_NEUTRAL };
                break;
            case UPD_AVAILABLE:
                status = { "Update " + std::string(g_updTag) +
                           " available - press " G_Y "!", SK_OK };
                g_updState = UPD_IDLE;   // one-shot; Y starts the install
                sndPlay(SND_CONFIRM);
                break;
            case UPD_DOWNLOADING:
                status = { "Downloading " + std::string(g_updTag) + "... " +
                           std::to_string(g_updPct) + "%", SK_NEUTRAL };
                break;
            case UPD_INSTALLING:
                status = { "Installing update...", SK_NEUTRAL };
                break;
            case UPD_DONE:
                if (g_is3dsx) {
                    // Can't relaunch a 3dsx by title id - user restarts HBL.
                    status = { "Updated to " + std::string(g_updTag) +
                               " - restart the app!", SK_OK };
                } else {
                    // Relaunch into the new copy NOW: the APT jump is what
                    // finalizes the import (Universal-Updater's flow) - a
                    // manual HOME relaunch leaves it half-committed and the
                    // NEXT self-update dies at am-write@0.
                    status  = { "Updated to " + std::string(g_updTag) +
                                " - restarting...", SK_OK };
                    jumpTid   = "0004000005BD3700";
                    jumpMedia = MEDIATYPE_SD;
                    if (quitT < 0) quitT = 0.0f;
                }
                g_updState = UPD_IDLE;
                break;
            case UPD_UPTODATE:
                status = { "Up to date (v" APP_VER ").", SK_OK };
                g_updState = UPD_IDLE;   // one-shot toast
                break;
            case UPD_FAILED:
                status = { std::string("Update failed: ") + g_updErr, SK_ERR };
                g_updState = UPD_IDLE;
                sndPlay(SND_ERROR);
                break;
            default: break;
        }

        // ------------------------------ draw -------------------------------
        g_t += 1.0f / 60.0f;

        // Animation bookkeeping: screen slide-in restarts on state change,
        // the toast timer restarts when its text changes.
        static AppState lastState = ST_GAMES;
        if (state != lastState) {
            lastState    = state;
            g_screenAnim = 0.0f;
            g_selSnap    = true;
        }
        if (g_screenAnim < 1.0f) {
            g_screenAnim += 1.0f / 18.0f;
            if (g_screenAnim > 1.0f) g_screenAnim = 1.0f;
        }
        static std::string lastToast;
        static float toastBorn = -999.0f;
        if (status.msg != lastToast) { lastToast = status.msg; toastBorn = g_t; }
        // While the updater is actively working, its progress text changes
        // every frame - pin the toast fully shown instead of replaying the
        // slide-in per percent (and never let it time out mid-download).
        if (!g_updSilent &&
            (g_updState == UPD_CHECKING || g_updState == UPD_DOWNLOADING ||
             g_updState == UPD_INSTALLING))
            toastBorn = g_t - 0.5f;
        g_statusAge = g_t - toastBorn;

        // Ease the ambient tint toward the hovered game's icon color
        // (neutral theme accent in the theme picker / while scanning).
        {
            u32 target = T.accent;
            if (state == ST_GAMES && !profiles.empty()) {
                const u32 a = gameAccent(profiles[gameCursor].titleId);
                if (a) target = a;
            } else if (state == ST_MODS) {
                const u32 a = gameAccent(profiles[selected].titleId);
                if (a) target = a;
            }
            g_gameTint = g_gameTint ? lerpColor(g_gameTint, target, 0.10f)
                                    : target;
        }

        C2D_TextBufClear(g_textBuf);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        // Fade-to-black overlay alpha while quitting.
        u8 fadeA = 0;
        if (quitT >= 0) {
            // Launching a game fades twice as fast as quitting - handoff
            // should feel urgent.
            quitT += jumpTid.empty() ? 1.0f / 24.0f : 1.0f / 12.0f;
            fadeA = (u8)(easeOutCubic(quitT) * 255.0f);
        }

        C2D_TargetClear(top, T.bgTop);
        C2D_SceneBegin(top);
        drawBackground(topParticles, 400);
        if      (state == ST_GAMES)  drawTopGames(profiles, gameCursor);
        else if (state == ST_MODS)   drawTopMods(profiles[selected], mods);
        else if (state == ST_LOADER) drawTopLoader();
        else                         drawTopThemes();
        if (fadeA) C2D_DrawRectSolid(0, 0, 0.9f, 400, 240,
                                     C2D_Color32(0, 0, 0, fadeA));

        C2D_TargetClear(bot, T.bgTop);
        C2D_SceneBegin(bot);
        drawBackground(botParticles, 320);
        if      (state == ST_GAMES)  drawBottomGames(profiles, gameCursor, status);
        else if (state == ST_MODS)   drawBottomMods(profiles[selected], mods, modCursor, status);
        else if (state == ST_LOADER) drawBottomLoader(loaders, loaderCursor, status);
        else                         drawBottomThemes(themeCursor);
        if (fadeA) C2D_DrawRectSolid(0, 0, 0.9f, 320, 240,
                                     C2D_Color32(0, 0, 0, fadeA));

        C3D_FrameEnd(0);

        // Hold the black screen until the boot worker is done - tearing
        // down services underneath its FS calls would crash on exit.
        if (quitT >= 1.0f && !jumped && g_bootReady) {
            if (!jumpTid.empty() &&
                R_SUCCEEDED(doGameJump(jumpTid, jumpMedia))) {
                // The jump is now pending inside NS. Exiting here would
                // cancel it (and wedge NS - later launches black-screen),
                // so keep pumping aptMainLoop and let the system close us.
                jumped = true;
            } else {
                break;   // plain quit, or the jump request failed
            }
        }
    }

    C2D_TextBufDelete(g_textBuf);
    C2D_Fini();
    C3D_Fini();
    amExit();
    if (g_netUp) { curl_global_cleanup(); socExit(); }
    sndExit();
    ptmuExit();
    gfxExit();
    return 0;
}
