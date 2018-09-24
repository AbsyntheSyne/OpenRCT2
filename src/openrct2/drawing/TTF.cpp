/*****************************************************************************
 * Copyright (c) 2014-2018 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#ifndef NO_TTF

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdocumentation"
#    include <ft2build.h>
#    include FT_FREETYPE_H
#    pragma clang diagnostic pop

#    include "../OpenRCT2.h"
#    include "../config/Config.h"
#    include "../localisation/Localisation.h"
#    include "../localisation/LocalisationService.h"
#    include "../platform/platform.h"
#    include "TTF.h"

static bool _ttfInitialised = false;

#    define TTF_SURFACE_CACHE_SIZE 256
#    define TTF_GETWIDTH_CACHE_SIZE 1024

struct ttf_cache_entry
{
    TTFSurface* surface;
    TTF_Font* font;
    utf8* text;
    uint32_t lastUseTick;
};

struct ttf_getwidth_cache_entry
{
    uint32_t width;
    TTF_Font* font;
    utf8* text;
    uint32_t lastUseTick;
};

static ttf_cache_entry _ttfSurfaceCache[TTF_SURFACE_CACHE_SIZE] = {};
static int32_t _ttfSurfaceCacheCount = 0;
static int32_t _ttfSurfaceCacheHitCount = 0;
static int32_t _ttfSurfaceCacheMissCount = 0;

static ttf_getwidth_cache_entry _ttfGetWidthCache[TTF_GETWIDTH_CACHE_SIZE] = {};
static int32_t _ttfGetWidthCacheCount = 0;
static int32_t _ttfGetWidthCacheHitCount = 0;
static int32_t _ttfGetWidthCacheMissCount = 0;

static TTF_Font* ttf_open_font(const utf8* fontPath, int32_t ptSize);
static void ttf_close_font(TTF_Font* font);
static uint32_t ttf_surface_cache_hash(TTF_Font* font, const utf8* text);
static void ttf_surface_cache_dispose(ttf_cache_entry* entry);
static void ttf_surface_cache_dispose_all();
static void ttf_getwidth_cache_dispose_all();
static bool ttf_get_size(TTF_Font* font, const utf8* text, int32_t* width, int32_t* height);
static TTFSurface* ttf_render(TTF_Font* font, const utf8* text);

bool ttf_initialise()
{
    if (!_ttfInitialised)
    {
        if (TTF_Init() != 0)
        {
            log_error("Couldn't initialise FreeType engine");
            return false;
        }

        for (int32_t i = 0; i < FONT_SIZE_COUNT; i++)
        {
            TTFFontDescriptor* fontDesc = &(gCurrentTTFFontSet->size[i]);

            utf8 fontPath[MAX_PATH];
            if (!platform_get_font_path(fontDesc, fontPath, sizeof(fontPath)))
            {
                log_verbose("Unable to load font '%s'", fontDesc->font_name);
                return false;
            }

            fontDesc->font = ttf_open_font(fontPath, fontDesc->ptSize);
            if (fontDesc->font == nullptr)
            {
                log_verbose("Unable to load '%s'", fontPath);
                return false;
            }
        }

        ttf_toggle_hinting();
        _ttfInitialised = true;
    }
    return true;
}

void ttf_dispose()
{
    if (_ttfInitialised)
    {
        ttf_surface_cache_dispose_all();
        ttf_getwidth_cache_dispose_all();

        for (int32_t i = 0; i < 4; i++)
        {
            TTFFontDescriptor* fontDesc = &(gCurrentTTFFontSet->size[i]);
            if (fontDesc->font != nullptr)
            {
                ttf_close_font(fontDesc->font);
                fontDesc->font = nullptr;
            }
        }

        TTF_Quit();
        _ttfInitialised = false;
    }
}

static TTF_Font* ttf_open_font(const utf8* fontPath, int32_t ptSize)
{
    return TTF_OpenFont(fontPath, ptSize);
}

static void ttf_close_font(TTF_Font* font)
{
    TTF_CloseFont(font);
}

static uint32_t ttf_surface_cache_hash(TTF_Font* font, const utf8* text)
{
    uint32_t hash = (uint32_t)((((uintptr_t)font * 23) ^ 0xAAAAAAAA) & 0xFFFFFFFF);
    for (const utf8* ch = text; *ch != 0; ch++)
    {
        hash = ror32(hash, 3) ^ (*ch * 13);
    }
    return hash;
}

static void ttf_surface_cache_dispose(ttf_cache_entry* entry)
{
    if (entry->surface != nullptr)
    {
        ttf_free_surface(entry->surface);
        free(entry->text);

        entry->surface = nullptr;
        entry->font = nullptr;
        entry->text = nullptr;
    }
}

static void ttf_surface_cache_dispose_all()
{
    for (int32_t i = 0; i < TTF_SURFACE_CACHE_SIZE; i++)
    {
        ttf_surface_cache_dispose(&_ttfSurfaceCache[i]);
        _ttfSurfaceCacheCount--;
    }
}

void ttf_toggle_hinting()
{
    if (!LocalisationService_UseTrueTypeFont())
    {
        return;
    }

    for (int32_t i = 0; i < FONT_SIZE_COUNT; i++)
    {
        TTFFontDescriptor* fontDesc = &(gCurrentTTFFontSet->size[i]);
        bool use_hinting = gConfigFonts.enable_hinting && fontDesc->hinting_threshold;
        TTF_SetFontHinting(fontDesc->font, use_hinting ? 1 : 0);
    }

    if (_ttfSurfaceCacheCount)
    {
        ttf_surface_cache_dispose_all();
    }
}

TTFSurface* ttf_surface_cache_get_or_add(TTF_Font* font, const utf8* text)
{
    ttf_cache_entry* entry;

    uint32_t hash = ttf_surface_cache_hash(font, text);
    int32_t index = hash % TTF_SURFACE_CACHE_SIZE;
    for (int32_t i = 0; i < TTF_SURFACE_CACHE_SIZE; i++)
    {
        entry = &_ttfSurfaceCache[index];

        // Check if entry is a hit
        if (entry->surface == nullptr)
            break;
        if (entry->font == font && strcmp(entry->text, text) == 0)
        {
            _ttfSurfaceCacheHitCount++;
            entry->lastUseTick = gCurrentDrawCount;
            return entry->surface;
        }

        // If entry hasn't been used for a while, replace it
        if (entry->lastUseTick < gCurrentDrawCount - 64)
        {
            break;
        }

        // Check if next entry is a hit
        if (++index >= TTF_SURFACE_CACHE_SIZE)
            index = 0;
    }

    // Cache miss, replace entry with new surface
    entry = &_ttfSurfaceCache[index];
    ttf_surface_cache_dispose(entry);

    TTFSurface* surface = ttf_render(font, text);
    if (surface == nullptr)
    {
        return nullptr;
    }

    _ttfSurfaceCacheMissCount++;
    // printf("CACHE HITS: %d   MISSES: %d)\n", _ttfSurfaceCacheHitCount, _ttfSurfaceCacheMissCount);

    _ttfSurfaceCacheCount++;
    entry->surface = surface;
    entry->font = font;
    entry->text = _strdup(text);
    entry->lastUseTick = gCurrentDrawCount;
    return entry->surface;
}

static void ttf_getwidth_cache_dispose(ttf_getwidth_cache_entry* entry)
{
    if (entry->text != nullptr)
    {
        free(entry->text);

        entry->width = 0;
        entry->font = nullptr;
        entry->text = nullptr;
    }
}

static void ttf_getwidth_cache_dispose_all()
{
    for (int32_t i = 0; i < TTF_GETWIDTH_CACHE_SIZE; i++)
    {
        ttf_getwidth_cache_dispose(&_ttfGetWidthCache[i]);
        _ttfGetWidthCacheCount--;
    }
}

uint32_t ttf_getwidth_cache_get_or_add(TTF_Font* font, const utf8* text)
{
    ttf_getwidth_cache_entry* entry;

    uint32_t hash = ttf_surface_cache_hash(font, text);
    int32_t index = hash % TTF_GETWIDTH_CACHE_SIZE;
    for (int32_t i = 0; i < TTF_GETWIDTH_CACHE_SIZE; i++)
    {
        entry = &_ttfGetWidthCache[index];

        // Check if entry is a hit
        if (entry->text == nullptr)
            break;
        if (entry->font == font && strcmp(entry->text, text) == 0)
        {
            _ttfGetWidthCacheHitCount++;
            entry->lastUseTick = gCurrentDrawCount;
            return entry->width;
        }

        // If entry hasn't been used for a while, replace it
        if (entry->lastUseTick < gCurrentDrawCount - 64)
        {
            break;
        }

        // Check if next entry is a hit
        if (++index >= TTF_GETWIDTH_CACHE_SIZE)
            index = 0;
    }

    // Cache miss, replace entry with new width
    entry = &_ttfGetWidthCache[index];
    ttf_getwidth_cache_dispose(entry);

    int32_t width, height;
    ttf_get_size(font, text, &width, &height);

    _ttfGetWidthCacheMissCount++;

    _ttfGetWidthCacheCount++;
    entry->width = width;
    entry->font = font;
    entry->text = _strdup(text);
    entry->lastUseTick = gCurrentDrawCount;
    return entry->width;
}

TTFFontDescriptor* ttf_get_font_from_sprite_base(uint16_t spriteBase)
{
    return &gCurrentTTFFontSet->size[font_get_size_from_sprite_base(spriteBase)];
}

bool ttf_provides_glyph(const TTF_Font* font, codepoint_t codepoint)
{
    return TTF_GlyphIsProvided(font, codepoint);
}

static bool ttf_get_size(TTF_Font* font, const utf8* text, int32_t* outWidth, int32_t* outHeight)
{
    return TTF_SizeUTF8(font, text, outWidth, outHeight);
}

static TTFSurface* ttf_render(TTF_Font* font, const utf8* text)
{
    if (TTF_GetFontHinting(font) != 0)
    {
        return TTF_RenderUTF8_Shaded(font, text, 0x000000FF, 0x000000FF);
    }
    else
    {
        return TTF_RenderUTF8_Solid(font, text, 0x000000FF);
    }
}

void ttf_free_surface(TTFSurface* surface)
{
    free((void*)surface->pixels);
    free(surface);
}

#else

#    include "TTF.h"

bool ttf_initialise()
{
    return false;
}

void ttf_dispose()
{
}

#endif // NO_TTF
