# hdtext - the game's text redrawn with a TrueType font

From Alien Odyssey: all six programs share one glyph blitter (plain + colour-table variant).
hdtext.c reads the blitter's globals from its code bytes (offsets listed at the top of hdtext.c),
captures the screen under each glyph before it is drawn, records glyph + colours after, and the
presenter (video.c, HAVE_HDTEXT) erases still-visible glyphs and redraws them in a smooth font.

For another game: find its glyph routine (string drawing -> per-character call; Ghidra decomp),
add `<addr> N_glyph_draw wrap` (and the variant) to tools/native_<tag>.txt, and adapt blit_setup()
and the glyph header reading (w, h, pixels) to that routine's globals / arguments. Character code =
glyph index + 0x21 in Alien Odyssey's fonts - check yours. Tiny fonts (cell height < 8) are skipped.
