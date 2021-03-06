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

#include <stll/layouter.h>

#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

#include <fribidi/fribidi.h>

#include <linebreak.h>
#include <wordbreak.h>

#include "hyphen/hyphen.h"
#include "hyphendictionaries_internal.h"

#include <algorithm>
#include <map>

#include <cassert>

namespace STLL {

// This module contains the functions to layout one paragraph of text
// so let's see how does the whole thing work...
//
// At first we split the text into runs. A run is a section of the text that "belongs together"
// line breaks only happen between runs. Also all the text in one run uses the same font. The text
// in one run is layouted using harfbuzz.
//
// Then the runs are assembled into the paragraph either using a greedy algorithm (fill lines as
// far as possible and then start the next) or using something akin to TeX Paragraph layout
// algorithm.
//
// Because the complex hyphens are a bit hard to implement (would require creating all possible
// sub-runs) we leave them out right now
//
// A possible fault is that also soft hyphens separate runs, so if the shaping of the not broken
// word looks different from the shape of the word broken between lines, the results will be wrong
//

// We use two helper-structures for the process

// This class contains information about the text to layout
// it is mainly there to prevent too many arguments to functions
class LayoutDataView
{
  private:

    std::u32string txt32;     // the text to layout, bidi-control characters are removed

    // we don't want to copy the attribute and the embedding levels, but the txt32
    // string may miss some of the characters of the original string, the string that
    // the provided attributes refert to
    // So we use an index array to index into the original attributeIndex
    std::vector<size_t> idx;

    // original attributes and embedding levels
    const AttributeIndex_c & attr;
    const std::vector<FriBidiLevel> & embeddingLevels;

    // calculated linebreak and hyphenation position
    std::vector<char> linebreaks;
    std::vector<bool> hyphens;

    // check if a character is a bidi control character and should not go into
    // the output stream
    bool isBidiCharacter(char32_t c)
    {
      if (c == U'\U0000202A' || c == U'\U0000202B' || c == U'\U0000202C')
        return true;
      else
        return false;
    }

  public:

    // create the object, copy the string leaving out all the bidi control characters
    LayoutDataView(const std::u32string & t, const AttributeIndex_c & a, const std::vector<FriBidiLevel> & e)
      : attr(a), embeddingLevels(e)
    {
      for (size_t i = 0; i < t.size(); i++)
      {
        if (!isBidiCharacter(t[i]))
        {
          txt32 += t[i];
          idx.push_back(i);
        }
      }
      linebreaks.resize(idx.size());
    }

    // accessors for the data
    const std::u32string & txt(void) const { return txt32; }
    char32_t txt(size_t i) const { return txt32[i]; }
    size_t size(void) const { return txt32.size(); }

    const CodepointAttributes_c & att(size_t i) const { return attr[idx[i]]; }
    bool hasatt(size_t i) const { return attr.hasAttribute(idx[i]); }

    FriBidiLevel emb(size_t i) const { return embeddingLevels[idx[i]]; }

    char lnb(size_t i) const { return linebreaks[i]; }
    char * lnb(void) { return linebreaks.data(); }

    void sethyp(size_t i) { hyphens.resize(idx.size()); hyphens[i] = true; }
    bool hyp(size_t i) const { return i < hyphens.size() && hyphens[i]; }
};


// this structure contains the information of a run finished and ready or paragraph assembly
typedef struct
{
  // the commands to output this run including the layer, the larger the number, the more in front to draw
  std::vector<std::pair<size_t, CommandData_c>> run;

  // the advance information of this run
  int dx = 0;
  int dy = 0;

  // the embedding level (text direction) of this run
  FriBidiLevel embeddingLevel = 0;

  // line-break information for AFTER this run, for values see liblinebreak
  char linebreak = LINEBREAK_NOBREAK;

  // the font used for this run... will probably be identical to
  // the fonts in the run
  std::shared_ptr<FontFace_c> font;

  // is this run a space run? Will be removed at line ends
  bool space = false;

  // is this a soft hyphen?? will only be shown at line ends
  bool shy = false;

  // ascender and descender of this run
  int32_t ascender = 0;
  int32_t descender = 0;

  // link boxes for this run
  std::vector<TextLayout_c::LinkInformation_c> links;

#ifndef NDEBUG
  // the text of this run, useful for debugging to see what is going on
  std::u32string text;
#endif

} runInfo;


// the following functions gather additional information about the text to layout

// create the text direction information using libfribidi
// txt32 and base_dir go in, embedding_levels comes out
static std::vector<FriBidiLevel> getBidiEmbeddingLevels(const std::u32string & txt32, const LayoutProperties_c & prop)
{
  std::vector<FriBidiCharType> bidiTypes(txt32.length());
  fribidi_get_bidi_types(reinterpret_cast<const uint32_t*>(txt32.c_str()), txt32.length(), bidiTypes.data());

  std::vector<FriBidiLevel> embedding_levels(txt32.length());
  FriBidiParType base_dir = prop.ltr ? FRIBIDI_TYPE_LTR_VAL : FRIBIDI_TYPE_RTL_VAL;

  if (fribidi_get_par_embedding_levels(bidiTypes.data(), txt32.length(), &base_dir, embedding_levels.data()) == 0)
  {
    // throw an exception
    throw LayoutException_c("unable to calculate embedding levels, possible out of memory");
  }

  return embedding_levels;
}

// calculate positions of potential line-breaks using liblinebreak
static void getLinebreaks(LayoutDataView & view)
{
  size_t length = view.size();

  size_t runstart = 0;

  while (runstart < length)
  {
    size_t runpos = runstart+1;

    // accumulate text that uses the same language and is no bidi character
    while (runpos < length && view.att(runstart).lang == view.att(runpos).lang)
    {
      runpos++;
    }

    // when calculating the length for the function call below, we need to keep in mind
    // that the function will always force a line-break at the end of the string, to avoid
    // this when the string really goes on, we include the next character in the string
    // to line-break (except of course when the string really ends here), that way we get
    // a real line-break and the wrongly written break is overwritten in the next call
    set_linebreaks_utf32(reinterpret_cast<const utf32_t*>(view.txt().c_str()+runstart),
                         runpos-runstart+(runpos < length ? 1 : 0),
                         view.att(runstart).lang.c_str(), view.lnb()+runstart);

    runstart = runpos;
  }
}

// find possible hyphenation places, of the returned positions
// we right now ignore complex hyphenations and only take those
// that correspond to simply adding a soft-hyphen
static void getHyphens(LayoutDataView & view)
{
  std::vector<internal::HyphenDict<char32_t>::Hyphens> hyphens;

  size_t sectionstart = 0;

  while (sectionstart < view.size())
  {
    if (view.hasatt(sectionstart) && !view.att(sectionstart).lang.empty())
    {
      // initial stuff: separate words on spaces, find English words
      std::string curLang = view.att(0).lang;

      // find end of current language section
      size_t i = sectionstart + 1;
      while (i < view.size() && view.hasatt(i) && view.att(i).lang == curLang) i++;

      auto dict = internal::getHyphenDict(curLang);

      if (dict)
      {
        std::vector<char> breaks(i-sectionstart+1);

        set_wordbreaks_utf32(reinterpret_cast<const utf32_t*>(view.txt().c_str()+sectionstart),
                             i-sectionstart+1, curLang.c_str(), breaks.data());

        breaks.push_back(WORDBREAK_BREAK);

        // now find the words and feed them to the hyphenator
        size_t wordstart = 0;
        for (size_t j = 1; j < breaks.size(); j++)
        {
          if (breaks[j-1] == WORDBREAK_BREAK)
          {
            // only hyphen, when the user has not done so manually
            if (view.txt().find_first_of(U'\u00AD', wordstart) >= j)
            {
              // assume a word from wordstart to j
              dict->hyphenate(view.txt().substr(wordstart, j-wordstart), hyphens);

              for (size_t l = 0; l < j-wordstart+1; l++)
              {
                if ((hyphens[l].hyphens % 2) && (hyphens[l].rep->length() == 0))
                  view.sethyp(sectionstart+wordstart+l+1);
              }
            }
            wordstart = j;
          }
        }
      }
      sectionstart = i;
    }
    else
    {
      sectionstart++;
    }
  }
}

// ok now we come to the generation of the runs

// This function adds the required commands for an underline to the run
// gx is the xposition, where to start
// gw is the width of the object to put the underline below
static void addUnderline(runInfo & run, int32_t gx, int32_t gw, const LayoutProperties_c & prop, const CodepointAttributes_c & a)
{
  // create underline commands
  if (a.flags & CodepointAttributes_c::FL_UNDERLINE)
  {
    int32_t gh, gy;

    if (prop.underlineFont)
    {
      gy = -((prop.underlineFont.getUnderlinePosition()+prop.underlineFont.getUnderlineThickness()/2));
      gh = std::max(64, prop.underlineFont.getUnderlineThickness());
    }
    else
    {
      gy = -((a.font.getUnderlinePosition()+a.font.getUnderlineThickness()/2));
      gh = std::max(64, a.font.getUnderlineThickness());
    }

    for (size_t j = 0; j < a.shadows.size(); j++)
    {
      run.run.push_back(std::make_pair(a.shadows.size()-j,
          CommandData_c(gx+a.shadows[j].dx, gy+a.shadows[j].dy, gw, gh, a.shadows[j].c, a.shadows[j].blurr)));
    }

    run.run.push_back(std::make_pair(0, CommandData_c(gx, gy, gw, gh, a.c, 0)));
  }
}

// create a fun for the texte between runstart and spos
static runInfo createRun(const LayoutDataView & view, size_t spos, size_t runstart,
                         const LayoutProperties_c & prop,
                         std::shared_ptr<FontFace_c> & font,
                         hb_font_t * hb_ft_font
                        )
{
  // the resulting run
  runInfo run;

  // check, if this is a space run, on line ends space runs will be removed
  run.space = view.txt(spos-1) == U' ' || view.txt(spos-1) == U'\n';

  // check, if this run is a soft hyphen. Soft hyphens are ignored and not output, except on line endings
  run.shy = view.txt(runstart) == U'\u00AD';

  // we may only have this one caracter in the text, when the first character is a shy
  assert(!run.shy || spos-runstart == 1);

  // Create a harfbuzz buffer
  hb_buffer_t *buf = hb_buffer_create();

  // setup the language for the harfbuzz shaper
  std::string language = view.att(runstart).lang;
  if (!language.empty())
  {
    size_t i = language.find_first_of('-');

    if (i != std::string::npos)
    {
      hb_script_t scr = hb_script_from_iso15924_tag(HB_TAG(language[i+1], language[i+2], language[i+3], language[i+4]));
      hb_buffer_set_script(buf, scr);
      hb_buffer_set_language(buf, hb_language_from_string(language.c_str(), i-1));
    }
    else
    {
      hb_buffer_set_language(buf, hb_language_from_string(language.c_str(), language.length()));
    }
  }

  if (!run.shy)
  {
    // copy the text to layout into the harfbuzz buffer
    hb_buffer_add_utf32(buf, reinterpret_cast<const uint32_t*>(view.txt().c_str()), -1, runstart, spos-runstart);
  }
  else
  {
    // we want to append a hyphen, in that case we only append a hyphen,
    // not all fonts contain the proper character for this symbol, so we
    // first try the proper one, and if that is not available
    // we use hyphen-minus, which all should have
    if (font->containsGlyph(U'\u2010'))
    {
      hb_buffer_add_utf32(buf, reinterpret_cast<const uint32_t*>(U"\u2010"), 1, 0, 1);
    }
    else
    {
      hb_buffer_add_utf32(buf, reinterpret_cast<const uint32_t*>(U"\u002D"), 1, 0, 1);
    }
  }

  run.embeddingLevel = view.emb(runstart);

  // set text direction for this run
  if (run.embeddingLevel % 2 == 0)
  {
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
  }
  else
  {
    hb_buffer_set_direction(buf, HB_DIRECTION_RTL);
  }

  // get the right font for this run and do the shaping
  if (hb_ft_font)
    hb_shape(hb_ft_font, buf, NULL, 0);

  // get the output
  unsigned int         glyph_count;
  hb_glyph_info_t     *glyph_info   = hb_buffer_get_glyph_infos(buf, &glyph_count);
  hb_glyph_position_t *glyph_pos    = hb_buffer_get_glyph_positions(buf, &glyph_count);

  // fill in some of the run information
  run.font = font;
  if (view.att(runstart).inlay)
  {
    // for inlays the ascender and descender depends on the size of the inlay
    run.ascender = view.att(runstart).inlay->getHeight()+view.att(runstart).baseline_shift;
    run.descender = view.att(runstart).inlay->getHeight()-run.ascender;
  }
  else
  {
    // for normal text ascender and descender are taken from the font
    run.ascender = run.font->getAscender()+view.att(runstart).baseline_shift;
    run.descender = run.font->getDescender()+view.att(runstart).baseline_shift;
  }

#ifndef NDEBUG
  run.text = view.txt().substr(runstart, spos-runstart);
#endif

  run.linebreak = view.lnb(spos-1);

  // information for a hyperlink within the text
  size_t curLink = 0;
  TextLayout_c::Rectangle_c linkRect;
  int linkStart = 0;

  // off we go creating the drawing commands
  // BUT, we need to make sure we keep the logical order here
  // harfbuzz will reverse the order of commands, when set to RTL
  // direction
  // BUT again harfbuzz has made the glyph_pos output dependent on the order
  // so make a first pass through this array to make absolute positions out
  // of the relative ones
  for (size_t j=0; j < glyph_count; ++j)
  {
    // get the attribute for the current character
    auto a = view.att(glyph_info[j].cluster);

    if (!a.inlay)
    {
      // when a new link is started, we save the current x-position within the run
      if ((!curLink && a.link) || (curLink != a.link))
      {
        linkStart = run.dx;
      }

      glyph_pos[j].x_offset += run.dx;
      run.dx += glyph_pos[j].x_advance;

      // if we have a link, we include that information within the run
      if (a.link)
      {
        // link has changed
        if (curLink && curLink != a.link)
        {
          // store information for current link
          run.links.emplace_back(TextLayout_c::LinkInformation_c(prop.links[curLink-1], linkRect));
          curLink = 0;
        }

        if (!curLink)
        {
          // start new link
          linkRect.x = linkStart;
          linkRect.y = -run.ascender;
          linkRect.w = run.dx-linkStart;
          linkRect.h = run.ascender-run.descender;
          curLink = a.link;
        }
        else
        {
          // make link box for current link wider
          linkRect.w = run.dx-linkStart;
        }
      }
    }
  }

  // now output using these absolute positions
  for (size_t jj=0; jj < glyph_count; ++jj)
  {
    size_t j = jj;

    if (run.embeddingLevel % 2 != 0)
    {
      j = glyph_count-1-jj;
    }

    // get the attribute for the current character
    auto a = view.att(glyph_info[j].cluster);

    if (a.inlay)
    {
      // copy the drawing commands from the inlay, shift them
      // to the right position
      for (auto in : a.inlay->getData())
      {
        // if ascender is 0 we want to be below the baseline
        // but if we leave the inlay where is is the top line of them
        // image will be _on_ the baseline, which is not what we want
        // so we actually need to go one below
        in.y -= (run.ascender-1);
        in.x += run.dx;

        run.run.push_back(std::make_pair(0, in));
      }

      addUnderline(run, run.dx, a.inlay->getRight(), prop, a);

      // advance
      run.dx += a.inlay->getRight();
    }
    else
    {
      // output the glyph
      glyphIndex_t gi = glyph_info[j].codepoint;

      int32_t gx = glyph_pos[j].x_offset;
      int32_t gy = run.dy - (glyph_pos[j].y_offset)-view.att(runstart).baseline_shift;

      // output all shadows of the glyph
      for (size_t j = 0; j < view.att(runstart).shadows.size(); j++)
      {
        run.run.push_back(std::make_pair(view.att(runstart).shadows.size()-j,
            CommandData_c(font, gi, gx+a.shadows[j].dx, gy+a.shadows[j].dy, a.shadows[j].c, a.shadows[j].blurr)));
      }

      // output the final glyph
      run.run.push_back(std::make_pair(0, CommandData_c(font, gi, gx, gy, a.c, 0)));

      addUnderline(run, gx, glyph_pos[j].x_advance+64, prop, a);

      // we only support line wise scripts
      if (glyph_pos[j].y_advance != 0)
        throw LayoutException_c("STLL only supports line based scripts and this text appears to be something else");
    }
  }

  // finalize an open link
  if (curLink)
  {
    run.links.emplace_back(TextLayout_c::LinkInformation_c(prop.links[curLink-1], linkRect));
    curLink = 0;
  }

  // free harfbuzz buffer and fonts
  hb_buffer_destroy(buf);

  return run;
}

// split the text to layout into runs
static std::vector<runInfo> createTextRuns(const LayoutDataView & view, const LayoutProperties_c & prop)
{
  // Get harfbuzz font structs for all required fonts within the text
  std::map<const std::shared_ptr<FontFace_c>, hb_font_t *> hb_ft_fonts;
  for (size_t i = 0; i < view.size(); i++)
    for (auto f : view.att(i).font)
      if (hb_ft_fonts.find(f) == hb_ft_fonts.end())
        hb_ft_fonts[f] = hb_ft_font_create(f->getFace(), NULL);

  // runstart always contains the first character for the current run
  size_t runstart = 0;

  // the result
  std::vector<runInfo> runs;

  // as long as there is something left in the text
  while (runstart < view.size())
  {
    // pos points at the first character AFTER the current run
    size_t spos = runstart+1;

    // the font for this run
    auto font = view.att(runstart).font.get(view.txt(runstart));

    // Find end of current run. This run continues, as long as
    while (   (spos < view.size())                                   // there is text left in our string
           && (view.emb(runstart) == view.emb(spos))                 // text direction has not changed
           && (view.att(runstart).lang == view.att(spos).lang)       // text still has the same language
           && (font == view.att(spos).font.get(view.txt(spos)))      // and the same font
           && (view.att(runstart).baseline_shift == view.att(spos).baseline_shift)           //  and the same baseline
           && (!view.att(spos).inlay)                                // and next char is not an inlay
           && (!view.att(spos-1).inlay)                              // and we are an not inlay
           && (   (view.lnb(spos-1) == LINEBREAK_NOBREAK)            // and line-break is not requested
               || (view.lnb(spos-1) == LINEBREAK_INSIDEACHAR)
              )
           && (view.txt(spos) != U' ')                               // and there is no space (needed to adjust width for justification)
           && (view.txt(spos-1) != U' ')
           && (view.txt(spos) != U'\n')                              // also end run on forced line-breaks
           && (view.txt(spos-1) != U'\n')
           && (view.txt(spos) != U'\u00AD')                          // and on soft hyphen
           && (!view.hyp(spos))
          )
    {
      spos++;
    }

    // get the harbuzz foont structure
    // inlays don't have fonts, but still need a run, that is why it might be
    // possible to provide nullptrs to the run creator
    hb_font_t * hbfont = nullptr;
    if (font) hbfont = hb_ft_fonts[font];

    // create and add the run
    runs.emplace_back(createRun(view, spos, runstart, prop, font, hbfont));

    // the manually insert soft hyphens are recognized and separated into single runs with
    // the condition of the look above (before is recognized by the character
    // comparison, behin is recognized by the linebreak condition
    //
    // but the automatically inserted hyphenation positions are just marked
    // with the hyphenation flag, here we need to manually
    // insert a run containing a soft hyphen character
    // TODO it might be better to change the string
    if (view.hyp(spos))
    {
      AttributeIndex_c attra(view.att(runstart));
      std::vector<FriBidiLevel> embedding_levelsa {view.emb(runstart)};
      LayoutDataView viewa(U"\u00AD", attra, embedding_levelsa);
      viewa.lnb()[0] = LINEBREAK_ALLOWBREAK;

      runs.emplace_back(createRun(viewa, 1, 0, prop, font, hbfont));
    }

    runstart = spos;
  }

  // free harfbuzz font structures
  for (auto & a : hb_ft_fonts)
    hb_font_destroy(a.second);

  return runs;
}

// merge links into a text layout, shifting the link boxes by dx and dy
static void mergeLinks(TextLayout_c & txt, const std::vector<TextLayout_c::LinkInformation_c> & links, int dx, int dy)
{
  // go over all links that we want to add
  for (const auto & l : links)
  {
    // try to find the link to insert in the already existing links within txt
    auto i = std::find_if(txt.links.begin(), txt.links.end(),
                          [&l] (TextLayout_c::LinkInformation_c l2) { return l.url == l2.url; }
                         );

    // when not found create it
    if (i == txt.links.end())
    {
      txt.links.emplace_back(TextLayout_c::LinkInformation_c(l.url));
      i = txt.links.end()-1;
    }

    // copy over the rectangles from the link and offset them
    for (auto r : l.areas)
    {
      r.x += dx;
      r.y += dy;

      i->areas.push_back(r);
    }
  }
}

#define LF_FIRST 1
#define LF_LAST 2
#define LF_SMALL_SPACE 4

// add a single line to the result layout
// take the runs from the runs argument, start with runstart and end before spos
// add to l
// add at ypos between left and right
// curWidth contains the sum of all the runs to add, curWidth already contains indent, if any
// numSpace the number of spaces within all those runs
static void addLine(int runstart, size_t spos, std::vector<runInfo> & runs, TextLayout_c & l,
                    int ypos, int curWidth, int32_t left, int32_t right, int lineflags,
                    int numSpace, const LayoutProperties_c & prop
                   )
{
  // first find out the order in which the runs must be
  // this is fribidi terrain, start with the logical order
  // and then use the embedding levels to reverse the
  // ranges with the same level
  std::vector<size_t> runorder(spos-runstart);
  std::iota(runorder.begin(), runorder.end(), runstart);

  // find the maximum level
  FriBidiLevel max_level = 0;
  for (auto ri : runorder)
    max_level = std::max(max_level, runs[ri].embeddingLevel);

  // reorder runs for current line
  for (int i = max_level-1; i >= 0; i--)
  {
    // find starts of regions to reverse
    for (size_t j = 0; j < runorder.size(); j++)
    {
      if (runs[runorder[j]].embeddingLevel > i)
      {
        // find the end of the current regions
        size_t k = j+1;
        while (k < runorder.size() && runs[runorder[k]].embeddingLevel > i)
        {
          k++;
        }

        std::reverse(runorder.begin()+j, runorder.begin()+k);
        j = k;
      }
    }
  }

  // calculate how much space is left on the line (for justification)
  int32_t spaceLeft = right - left - curWidth;

  // output of runs is always left to right, so depending on the paragraph alignment
  // settings we calculate where at the x-axis we start with the runs and how many
  // additional pixels we add to each space
  //
  // TODO later on we want to output in logical order instead of left to right
  int32_t xpos;
  double spaceadder = 0;

  switch (prop.align)
  {
    default:
    case LayoutProperties_c::ALG_LEFT:
      xpos = left;
      if (lineflags & LF_FIRST) xpos += prop.indent;
      break;

    case LayoutProperties_c::ALG_RIGHT:
      xpos = left + spaceLeft;
      break;

    case LayoutProperties_c::ALG_CENTER:
      xpos = left + spaceLeft/2;
      break;

    case LayoutProperties_c::ALG_JUSTIFY_LEFT:
      xpos = left;
      // don't justify last paragraph
      if (numSpace > 0 && !(lineflags & LF_LAST))
        spaceadder = 1.0 * spaceLeft / numSpace;

      if (lineflags & LF_FIRST) xpos += prop.indent;

      break;

    case LayoutProperties_c::ALG_JUSTIFY_RIGHT:
      // don't justify last paragraph
      if (numSpace > 0 && !(lineflags & LF_LAST))
      {
        xpos = left;
        spaceadder = 1.0 * spaceLeft / numSpace;
      }
      else
      {
        xpos = left + spaceLeft;
      }
      break;
  }

  int32_t xpos2 = xpos;
  numSpace = 0;

  // place all elements of the line according to alignment
  for (auto ri : runorder)
  {
    // output soft hyphen runs only, when the last in line
    if (!runs[ri].shy || ri == spos-1)
    {
      // output only non-space runs
      if (!runs[ri].space)
      {
        for (auto & cc : runs[ri].run)
        {
          cc.second.x += xpos2+spaceadder*numSpace;
          cc.second.y += ypos;
        }
      }
      else
      {
        // in space runs, there may be an rectangular command that represents
        // the underline, make that underline longer by spaceadder
        // to accommodate larger spaces due to justification
        for (auto & cc : runs[ri].run)
        {
          if (cc.second.command == CommandData_c::CMD_RECT)
          {
            cc.second.w += spaceadder;
            cc.second.x += xpos2+spaceadder*numSpace;
            cc.second.y += ypos;
          }
        }

        // the link rectangle in spaces also needs to get longer
        if (!runs[ri].links.empty() && !runs[ri].links[0].areas.empty())
        {
          runs[ri].links[0].areas[0].w += spaceadder;
        }
      }

      // merge in the links, but only do this once, for the layer 0
      mergeLinks(l, runs[ri].links, xpos2+spaceadder*numSpace, ypos);

      // count the spaces
      if (runs[ri].space) numSpace++;

      // advance the x-position
      if (!runs[ri].space)
        xpos2 += runs[ri].dx;
      else if (lineflags & LF_SMALL_SPACE)
        xpos2 += 9*runs[ri].dx/10;
      else
        xpos2 += runs[ri].dx;
    }
  }

  // find the number of layers that we need to output
  size_t maxlayer = 0;
  for (auto ri : runorder)
    for (auto & r : runs[ri].run)
      maxlayer = std::max(maxlayer, r.first+1);

  // output all the layers one after the other starting with maximal layer index
  // and working down to layer 0
  for (uint32_t layer = 0; layer < maxlayer; layer++)
  {
    // output runs of current layer
    for (size_t i = runstart; i < spos; i++)
    {
      // output soft hyphen runs only, when the last in line
      if (!runs[i].shy || i == spos-1)
      {
        // output only non-space runs
        if (!runs[i].space)
        {
          for (auto & cc : runs[i].run)
            if (cc.first == maxlayer-layer-1)
              l.addCommand(cc.second);
        }
        else
        {
          // in space runs, there may be an rectangular command that represents
          // the underline, make that underline longer by spaceadder
          // to accommodate larger spaces due to justification
          for (auto & cc : runs[i].run)
          {
            if (   (cc.first == maxlayer-layer-1)
                && (cc.second.command == CommandData_c::CMD_RECT)
               )
            {
              l.addCommand(cc.second);
            }
          }
        }
      }
    }
  }
}

// do the line breaking using the runs created before
static TextLayout_c breakLines(std::vector<runInfo> & runs,
                               const Shape_c & shape,
                               const LayoutProperties_c & prop, int32_t ystart)
{
  // layout a paragraph line by line
  size_t runstart = 0;
  int32_t ypos = ystart;
  TextLayout_c l;
  bool firstline = true;
  bool forcebreak = false;

  // while there are runs left to do
  while (runstart < runs.size())
  {
    // accumulate enough runs to fill the line, this is done by accumulating runs
    // until we come to a place where we might break the line
    // then we check if the line would be too long with the new set of runs

    // skip initial spaces
    while (runstart < runs.size() && runs[runstart].space) runstart++;

    // these variables contain the current line information
    // which run it starts at, when the first run that will go onto the next
    // line, how many spaces there are in the line (for justification)
    // and what the width of the runs is
    int32_t curAscend = 0;
    int32_t curDescend = 0;
    int32_t curWidth = 0;
    size_t spos = runstart;
    size_t numSpace = 0;
    forcebreak = false;

    // if it is a first line, we add the indent first
    if (firstline && prop.align != LayoutProperties_c::ALG_CENTER) curWidth = prop.indent;

    // now go through the remaining runs and add them
    while (spos < runs.size())
    {
      // calculate the line information including the added runs
      // we start with the current line settings
      int32_t newAscend = curAscend;
      int32_t newDescend = curDescend;
      int32_t newWidth = curWidth;
      size_t newspos = spos;
      size_t newSpace = numSpace;

      // now add runs, until we get to a new point where we can break
      // the line, or we run out of runs
      while (newspos < runs.size())
      {
        // update line hight and width with the new run
        newAscend = std::max(newAscend, runs[newspos].ascender);
        newDescend = std::min(newDescend, runs[newspos].descender);
        newWidth += runs[newspos].dx;
        if (runs[newspos].space) newSpace++;

        // if we come to a point where we can break the line, we stop and
        // evaluate if these new added runs still fits
        //
        // liblinebreak inserts the breaks AFTER the spaces, but we don't want to
        // include the spaces at line ends, that is why we have 2 break conditions here:
        // 1) when liblinebreak as inserted a break after the current run
        // 2) when the next run is a space run and liblinebreak has inserted a break after that run
        if (  (    (newspos+1) < runs.size()
                && (runs[newspos+1].space)
                && (   (runs[newspos+1].linebreak == LINEBREAK_ALLOWBREAK)
                    || (runs[newspos+1].linebreak == LINEBREAK_MUSTBREAK))
              )
            ||(    (!runs[newspos].space)
                && (   (runs[newspos].linebreak == LINEBREAK_ALLOWBREAK)
                    || (runs[newspos].linebreak == LINEBREAK_MUSTBREAK))
              )
           )
        {
          break;
        }

        // next run has to go in as well
        newspos++;
      }

      // we have included runs up to newspos, we will continue with the one after that
      // so increment once more
      newspos++;

      // check, if the line still fits in the available space
      // if not break out and don't take over the new additional runs
      // but even if it doesn't fit, we need to take over when we have
      // not yet anything in our line, this might happen when there is one
      // run that is longer than the available space
      if (   (spos > runstart)
          && (shape.getLeft(ypos, ypos+newAscend-newDescend)+newWidth >
              shape.getRight(ypos, ypos+newAscend-newDescend))
         )
      {
        // next run would overrun
        break;
      }

      // when the final character at the end of the line (prior to the latest
      // additions) is a shy, remove that one from the width, because the
      // shy will only be output at the end of the line
      if ((spos > runstart) && runs[spos-1].shy) newWidth -= runs[spos-1].dx;

      // additional run fits, so take over the new line
      curAscend = newAscend;
      curDescend = newDescend;
      curWidth = newWidth;
      spos = newspos;
      numSpace = newSpace;

      // the current end of the line forces a break, or the next character is a space and forces a break
      if (  (runs[spos-1].linebreak == LINEBREAK_MUSTBREAK)
          ||((spos < runs.size()) && runs[spos].space && runs[spos].linebreak == LINEBREAK_MUSTBREAK)
         )
      {
        forcebreak = true;
        break;
      }
    }

    // force break also when end of paragraph is reached
    forcebreak |= (spos == runs.size());

    addLine(runstart, spos, runs, l, ypos+curAscend, curWidth,
        shape.getLeft(ypos, ypos+curAscend-curDescend),
        shape.getRight(ypos, ypos+curAscend-curDescend),
        (firstline ? LF_FIRST : 0) + (forcebreak ? LF_LAST : 0), numSpace, prop);
    if (firstline) l.setFirstBaseline(ypos+curAscend);
    ypos = ypos + curAscend - curDescend;

    // set the runstart at the next run and skip space runs
    runstart = spos;
    firstline = false;
  }

  // set the final shape of the paragraph
  l.setHeight(ypos);
  l.setLeft(shape.getLeft2(ystart, ypos));
  l.setRight(shape.getRight2(ystart, ypos));

  return l;
}

// do the line breaking using the runs created before, using an optimizing
// paragraph layouting algorithm
static TextLayout_c breakLinesOptimize(std::vector<runInfo> & runs,
                                       const Shape_c & shape,
                                       const LayoutProperties_c & prop, int32_t ystart)
{
  // layout a paragraph line by line
  TextLayout_c l;

  // for details look into the TeX documentation...
  // This is a very similar method

  typedef struct
  {
    size_t from; // optimal line starting position
    float demerits; // penalty when coming from there

    // properties of the line, when coming from there
    int ascend;
    int descend;
    int width;
    int spaces;
    int32_t ypos;
    bool forcebreak;
    int linetype; // line categorisation 0: tight, 1: decent, 2: loose, 3: very loose
    bool hypen;
    bool start;
  } lineinfo;

  std::vector<lineinfo> li (runs.size()+1);

  li[0].from = 0;
  li[0].demerits = 0;
  li[0].ypos = ystart;
  li[0].start = true;

  // find the best paths to all the line break positions
  for (size_t i = 1; i < runs.size()+1; i++)
  {
    li[i].demerits = std::numeric_limits<int>::max();

    if (runs[i-1].linebreak == LINEBREAK_ALLOWBREAK || runs[i-1].linebreak == LINEBREAK_MUSTBREAK)
    {
      for (size_t start = i; start > 0; start--)
      {
        // no need to look at lines with this infinite penalty
        if (li[start-1].demerits == std::numeric_limits<int>::max())
          continue;

        int32_t Ascend = 0;
        int32_t Descend = 0;
        int32_t Width = 0;
        size_t Space = 0;
        bool force = false;
        int optimalStretch = 0;
        int spaceWidth = 0;

        if (start == 1)
        {
          if (prop.align != LayoutProperties_c::ALG_CENTER)
            Width = prop.indent;
        }

        // ignore spaces at the start end end of the line
        int s1 = start-1;
        int s2 = i;
        while (runs[s1].space) s1++;
        while (runs[s2-1].space) s2--;

        for (int j = s1; j < s2; j++)
        {
          if (!runs[j].shy || j == s2-1)
          {
            Ascend = std::max(Ascend, runs[j].ascender);
            Descend = std::min(Descend, runs[j].descender);
            if (runs[j].space)
            {
              Space++;
              Width += runs[j].dx*9/10;
              optimalStretch += runs[j].dx/10;
              spaceWidth += runs[j].dx;
            }
            else
            {
              Width += runs[j].dx;
            }
          }
        }

        // line has become too long, no need to go further back from here
        if (shape.getLeft(li[start-1].ypos, li[start-1].ypos+Ascend-Descend)+Width >
          shape.getRight(li[start-1].ypos, li[start-1].ypos+Ascend-Descend))
          break;

        //  how much do we need to stretch the line
        float fillin = shape.getRight(li[start-1].ypos, li[start-1].ypos+Ascend-Descend) -
        shape.getLeft(li[start-1].ypos, li[start-1].ypos+Ascend-Descend) - Width;

        // what would be the optimum fillin to get exactly the right space size
        float optimalFillin = spaceWidth-Width;

        float fillinDifference = fabs(fillin-optimalFillin);

        float badness = 100.0*pow(1.0*fillinDifference/optimalFillin, 3);

        int linetype = 1;

        if (badness >= 100) { linetype = 3; }
        else if (badness >= 13)
        {
          if (fillin > optimalFillin)
            linetype = 2;
          else
            linetype = 0;
        }

        float demerits = (10+badness)*(10+badness);

        // hypen demerits
        if (runs[s2-1].shy && li[start-1].hypen)
        {
          demerits += 10000;
        }

        if (abs(linetype - li[start-1].linetype) > 1) demerits += 10000;
        if (linetype != li[start-1].linetype) demerits += 5000;

        if (runs[i-1].linebreak == LINEBREAK_MUSTBREAK || i == runs.size())
        {
          if (Width > (shape.getRight(li[start-1].ypos, li[start-1].ypos+Ascend-Descend) -
                       shape.getLeft(li[start-1].ypos, li[start-1].ypos+Ascend-Descend))/3)
          {
            demerits = 0;
          }
          else
          {
            demerits = 100000;
          }
          force = true;
        }

        demerits += li[start-1].demerits;

        if (demerits < li[i].demerits)
        {
          li[i].from = start-1;
          li[i].demerits = demerits;

          li[i].ascend = Ascend;
          li[i].descend = Descend;
          li[i].width = Width;
          li[i].spaces = Space;
          li[i].ypos = li[start-1].ypos + Ascend - Descend;
          li[i].forcebreak = force;
          li[i].linetype = linetype;
          li[i].hypen = runs[s2-1].shy;
          li[i].start = false;
        }
      }
    }

    if (runs[i-1].linebreak == LINEBREAK_MUSTBREAK || i == runs.size())
    {
      // store breaking points
      size_t ii = i;
      std::vector<size_t> breaks;

      while (!li[ii].start)
      {
        breaks.push_back(ii);
        ii = li[ii].from;
      }
      breaks.push_back(ii);

      for (ii = breaks.size()-1; ii > 0; ii--)
      {
        auto & bb = li[breaks[ii-1]];
        auto & cc = li[breaks[ii]];

        size_t s1 = breaks[ii];
        size_t s2 = breaks[ii-1];
        while (runs[s1].space) s1++;
        while (runs[s2-1].space) s2--;
        addLine(s1, s2, runs, l, cc.ypos + bb.ascend, bb.width,
                shape.getLeft(cc.ypos, cc.ypos+bb.ascend-bb.descend),
                shape.getRight(cc.ypos, cc.ypos+bb.ascend-bb.descend),
                (ii == breaks.size()-1 ? LF_FIRST : 0) + (ii == 1 ? LF_LAST : 0) + LF_SMALL_SPACE, bb.spaces, prop);
        if (ii == breaks.size()-1) l.setFirstBaseline(cc.ypos+bb.ascend);
        cc.ypos = cc.ypos + bb.ascend - bb.descend;
      }

      runs.erase(runs.begin(), runs.begin()+i);
      li[0].ypos = li[i].ypos;

      i = 0;
    }
  }

  l.setHeight(li[0].ypos);
  l.setLeft(shape.getLeft2(ystart, li[0].ypos));
  l.setRight(shape.getRight2(ystart, li[0].ypos));

  return l;
}


TextLayout_c layoutParagraph(const std::u32string & txt32, const AttributeIndex_c & attr,
                             const Shape_c & shape, const LayoutProperties_c & prop, int32_t ystart)
{
  // calculate embedding types for the text
  auto embedding_levels = getBidiEmbeddingLevels(txt32, prop);

  LayoutDataView view(txt32, attr, embedding_levels);

  // calculate the possible line-break positions
  getLinebreaks(view);

  // add hyphenation information, when requested
  if (prop.hyphenate) getHyphens(view);

  // create runs of layout text. Each run is a cohesive set, e.g. a word with a single font, ...
  auto runs = createTextRuns(view, prop);

  // layout the runs into lines
  if (prop.optimizeLinebreaks)
    return breakLinesOptimize(runs, shape, prop, ystart);
  else
    return breakLines(runs, shape, prop, ystart);
}


}
