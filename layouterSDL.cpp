/*
 * STLL Simple Text Layouting Library
 *
 * STLL is the legal property of its developers, whose
 * names are listed in the COPYRIGHT file, which is included
 * within the source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "layouterSDL.h"

#include "layouter.h"

#include <ft2build.h>
#include FT_FREETYPE_H

// TODO we need some kind of caching here...

namespace STLL {

typedef struct
{
  // set to the origin of the pixel
  uint32_t *pixels;

  // final bounding check
  uint32_t *first_pixel, *last_pixel;

  // format information
  int32_t pitch;
  uint32_t rshift;
  uint32_t gshift;
  uint32_t bshift;
  uint32_t ashift;

  color_c c;

} spanInfo;

void spanner(int y, int count, const FT_Span* spans, void *user)
{
  spanInfo *baton = reinterpret_cast<spanInfo*>(user);

  uint32_t *scanline = baton->pixels - y * baton->pitch;

  if (scanline >= baton->first_pixel)
  {
    for (int i = 0; i < count; i++)
    {
      uint32_t *start = scanline + spans[i].x;

      if (start + spans[i].len >= baton->last_pixel)
        return;

      uint8_t alpha = (spans[i].coverage * baton->c.a()) / 255;

      for (int x = 0; x < spans[i].len; x++)
      {
        uint8_t pr = *start >> baton->rshift;
        uint8_t pg = *start >> baton->gshift;
        uint8_t pb = *start >> baton->bshift;

        pr = ((255-alpha)*pr + (alpha*baton->c.r()))/255;
        pg = ((255-alpha)*pg + (alpha*baton->c.g()))/255;
        pb = ((255-alpha)*pb + (alpha*baton->c.b()))/255;

        *start = (pr << baton->rshift) | (pg << baton->gshift) | (pb << baton->bshift);
        start++;

        if (start >= baton->last_pixel) return;
      }
    }
  }
}

void showLayoutSDL(const textLayout_c & l, int sx, int sy, SDL_Surface * s)
{
  /* set up rendering via spanners */
  spanInfo span;
  span.pixels = NULL;
  span.first_pixel = reinterpret_cast<uint32_t*>(s->pixels);
  span.last_pixel = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(s->pixels) + s->pitch*s->h);
  span.pitch = s->pitch/4;
  span.rshift = s->format->Rshift;
  span.gshift = s->format->Gshift;
  span.bshift = s->format->Bshift;

  FT_Raster_Params ftr_params;
  ftr_params.target = 0;
  ftr_params.flags = FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_AA;
  ftr_params.user = &span;
  ftr_params.black_spans = 0;
  ftr_params.bit_set = 0;
  ftr_params.bit_test = 0;
  ftr_params.gray_spans = spanner;

  SDL_Rect r;

  /* render */
  for (auto & i : l.data)
  {
    switch (i.command)
    {
      case textLayout_c::commandData::CMD_GLYPH:

        span.pixels = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(s->pixels) + ((sy+i.y+32)/64) * s->pitch) + ((sx+i.x+32)/64);
        span.c = i.c;

        i.font->outlineRender(i.glyphIndex, &ftr_params);

        break;

      case textLayout_c::commandData::CMD_RECT:

        r.x = (i.x+sx+32)/64;
        r.y = (i.y+sy+32)/64;
        r.w = (i.x+sx+i.w+32)/64-r.x;
        r.h = (i.y+sy+i.h+32)/64-r.y;
        SDL_FillRect(s, &r, SDL_MapRGBA(s->format, i.c.r(), i.c.g(), i.c.b(), i.c.a()));
        break;

      case textLayout_c::commandData::CMD_IMAGE:

        // TODO this is just a placeholder for now

        r.x = (i.x+sx+32)/64;
        r.y = (i.y+sy+32)/64;
        r.w = 10;
        r.h = 10;
        SDL_FillRect(s, &r, SDL_MapRGBA(s->format, 255, 255, 255, SDL_ALPHA_OPAQUE));
        break;
    }
  }
}

}
