# sandbox_selftest additions for the Melange 0.12 wum.draw functions (owner: C5). Fakes go in tests/sandbox_fakes_draw.inc;
# the fake LoadFont validates metrics with the real holo-font-1 parser.
target_sources(sandbox_selftest PRIVATE src/render/mirage/draw_font_core.cpp)
