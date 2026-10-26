// sandbox_selftest cases for the Melange 0.12 wum.input functions (owner: C2). Included in the test file's anonymous
// namespace after the helpers; defines TestModInput(), run before the docs check. The real mod input core runs on the
// fake Host of sandbox_fakes_input.inc; window messages are fed to it the way input.cpp's window procedure does.
namespace mi_case {
const char kRecorder[] = R"(
ev = {}
for _, k in ipairs({'mouse', 'wheel', 'key', 'char', 'paste'}) do
  wum.events.on('melange.input.' .. k, function(p) p.kind = k ev[#ev + 1] = p end)
end
function dump()
  local out = {}
  for _, e in ipairs(ev) do
    if e.kind == 'mouse' then
      out[#out + 1] = string.format('mouse:%s:%s:%s:%s', e.region, e.button, tostring(e.down), tostring(e.double))
    elseif e.kind == 'wheel' then
      out[#out + 1] = string.format('wheel:%s:%g', e.region, e.delta)
    elseif e.kind == 'key' then
      out[#out + 1] = string.format('key:%s:%s', e.key, tostring(e.down))
    else
      out[#out + 1] = e.kind .. ':' .. e.text
    end
  end
  ev = {}
  return table.concat(out, ' ')
end
)";

bool Mouse(UINT msg, int x = 0, int y = 0, WPARAM wp = 0) {
    return render::modinput::RouteMouse(msg, wp, MAKELPARAM(x, y));
}
bool Key(UINT msg, WPARAM vk, uint8_t scan) {
    return render::modinput::RouteKey(msg, vk, static_cast<LPARAM>(scan) << 16 | 1);
}
void At(float x, float y) {
    fake_input::g_x = x;
    fake_input::g_y = y;
}
}  // namespace mi_case

void TestModInput() {
    using namespace mi_case;
    namespace mi = render::modinput;
    for (const char* f : {"cursor", "region", "hover", "pointer", "pointerActive", "focus"})
        ExpectEq(Eval(nullptr, std::string("return type(wum.input.") + f + ")"), "function", std::string("wum.input.") + f + " exists");
    fake_input::Install();
    SetMod("inA", kRecorder);
    SetMod("inB", kRecorder);
    Expect(sandbox::LoadMod("inA") && sandbox::LoadMod("inB"), "mod input test mods load");

    // ---- arguments
    Expect(Eval("inA", "return wum.input.region('', 0, 0, 1, 1)").rfind("ERR:", 0) == 0, "region: an empty id raises");
    Expect(Eval("inA", "return wum.input.region(string.rep('x', 65), 0, 0, 1, 1)").rfind("ERR:", 0) == 0, "region: a 65-byte id raises");
    Expect(Eval("inA", "return wum.input.region('a', 0, 0, 1, 1, {cursor = 'cross'})").rfind("ERR:", 0) == 0, "region: unknown cursor raises");
    Expect(Eval("inA", "return wum.input.pointer('yes')").rfind("ERR:", 0) == 0, "pointer: a non-boolean raises");
    Expect(Eval("inA", "return wum.input.pointer(true, {cursor = 12345})").rfind("ERR:", 0) == 0, "pointer: a foreign texture raises");
    Expect(Eval("inA", "return wum.input.pointer(true, {release = 'never'})").rfind("ERR:", 0) == 0, "pointer: unknown release raises");
    ExpectEq(Eval(nullptr, "return tostring(select(2, wum.input.region('c', 0, 0, 1, 1)))"), "unavailable", "the console has no regions");

    // ---- frontend cursor, regions, hover, clicks
    At(100, 100);
    ExpectEq(Eval("inA", "local x, y, s = wum.input.cursor() return s .. ' ' .. x .. ' ' .. y"), "frontend 100.0 100.0", "frontend cursor");
    ExpectEq(Eval("inA", "return tostring(wum.input.region('btn', 50, 50, 150, 150, {cursor = 'hand'}))"), "true", "region claimed");
    ExpectEq(Eval("inA", "return tostring(wum.input.hover())"), "btn", "hover finds the region");
    ExpectEq(Eval("inB", "return tostring(wum.input.hover())"), "nil", "hover is per mod");
    Expect(!Mouse(WM_LBUTTONDOWN) && !Mouse(WM_LBUTTONUP), "a click inside a region is kept from the game");
    Expect(Mouse(WM_MOUSEMOVE, 5, 5), "moves still reach the game");
    Frames(1);
    ExpectEq(Eval("inA", "return dump()"), "mouse:btn:left:true:false mouse:btn:left:false:false", "the owner gets press and release");
    ExpectEq(Eval("inB", "return dump()"), "", "events are owner-only");
    Expect(input::gesture::Recent("inA", 2000, false), "a delivered click marks a gesture");
    Expect(!input::gesture::Recent("inA", 2000, true), "a region claimed for under 500 ms is not consent-grade");
    fake_input::g_now += 600;
    Eval("inA", "wum.input.region('btn', 50, 50, 150, 150)");
    Mouse(WM_LBUTTONDOWN);
    Mouse(WM_LBUTTONUP);
    Expect(input::gesture::Recent("inA", 2000, true), "a small region held for 500 ms gives a consent-grade click");
    Frames(1);
    Eval("inA", "dump()");
    // the game saw the press: its release always reaches the game, even inside a region
    At(500, 500);
    Expect(Mouse(WM_RBUTTONDOWN), "a press outside every region reaches the game");
    At(100, 100);
    Expect(Mouse(WM_RBUTTONUP), "...and so does its release, now over a region");
    // a press a mod took keeps its release from the game, wherever the cursor is
    Mouse(WM_MBUTTONDOWN);
    At(500, 500);
    Expect(!Mouse(WM_MBUTTONUP), "the release of a press a mod took is kept from the game outside the region");
    At(100, 100);
    Expect(!Mouse(WM_MOUSEWHEEL, 0, 0, MAKEWPARAM(0, WHEEL_DELTA)), "the wheel over a region is kept from the game");
    Frames(1);
    ExpectEq(Eval("inA", "return dump()"), "mouse:btn:middle:true:false mouse:btn:middle:false:false wheel:btn:1",
             "middle click and wheel delivered");
    // priority: a later-loaded mod's region wins where they overlap; the older mod's overlapping claim is busy
    ExpectEq(Eval("inB", "return tostring(wum.input.region('top', 90, 90, 200, 200))"), "true", "a newer mod claims over it");
    ExpectEq(Eval("inA", "return tostring(select(2, wum.input.region('btn', 50, 50, 150, 150)))"), "busy",
             "an older mod's overlapping claim is busy");
    Mouse(WM_LBUTTONDOWN);
    Mouse(WM_LBUTTONUP);
    Frames(1);
    ExpectEq(Eval("inB", "return dump()"), "mouse:top:left:true:false mouse:top:left:false:false", "the newer mod gets the click");
    // regions lapse unless renewed
    fake_input::g_frame += 3;
    mi::Tick();
    Expect(Mouse(WM_LBUTTONDOWN) && Mouse(WM_LBUTTONUP), "lapsed regions no longer take clicks");
    ExpectEq(Eval("inA", "return tostring(wum.input.hover())"), "nil", "no hover after the region lapsed");

    // ---- pointer mode
    ExpectEq(Eval("inA", "return tostring(select(2, wum.input.pointer(true)))"), "unavailable", "pointer outside a match");
    fake_input::g_match = true;
    fake_input::g_fe = false;
    ExpectEq(Eval("inA", "return tostring(wum.input.cursor())"), "nil", "no cursor in a match without pointer mode");
    At(900, 600);
    Expect(Mouse(WM_RBUTTONDOWN), "the game holds the right button");
    ExpectEq(Eval("inA", "return tostring(wum.input.pointer(true))"), "true", "pointer on in a match");
    Expect(fake_input::g_started == 1 && fake_input::g_held == std::vector<int>{1}, "the held button gets a synthetic release");
    ExpectEq(Eval("inA", "return tostring(wum.input.pointerActive())"), "true", "pointerActive");
    ExpectEq(Eval("inB", "return tostring(wum.input.pointerActive())"), "false", "pointerActive is per mod");
    ExpectEq(Eval("inB", "return tostring(select(2, wum.input.pointer(true)))"), "busy", "one pointer owner at a time");
    ExpectEq(Eval("inB", "return tostring(select(2, wum.input.region('x', 0, 0, 10, 10)))"), "busy", "other mods' regions wait");
    Expect(mi::OwnsMouse(), "core sees the mouse owned");
    Expect(Mouse(WM_RBUTTONUP) && !Mouse(WM_RBUTTONUP), "exactly one release of the held button reaches the game");
    Expect(!Mouse(WM_MOUSEMOVE, 10, 10), "pointer mode keeps moves from the game");
    ExpectEq(Eval("inA", "local x, y, s = wum.input.cursor() return s .. ' ' .. x"), "pointer 900.0", "cursor source pointer");
    Eval("inA", "wum.input.region('bar', 800, 500, 1000, 700, {cursor = 'ibeam'})");
    Expect(mi::CurrentPointer().shape == mi::Shape::IBeam, "the region's cursor shape shows in pointer mode");
    Expect(!Mouse(WM_LBUTTONDOWN) && !Mouse(WM_LBUTTONUP), "a click on the owner's region is kept from the game");
    At(10, 10);
    Expect(!Mouse(WM_LBUTTONDOWN), "a click outside is consumed");
    ExpectEq(Eval("inA", "return tostring(wum.input.pointerActive())"), "false", "...and ends pointer mode (click-outside)");
    Expect(fake_input::g_ended == 1, "the exit runs the zero-delta rebase");
    Expect(!Mouse(WM_LBUTTONUP), "the release of that consuming click is kept from the game too");
    Expect(!Mouse(WM_MOUSEMOVE, 20, 20) && Mouse(WM_MOUSEMOVE, 640, 360) && Mouse(WM_MOUSEMOVE, 20, 20),
           "after the exit, moves away from the centre are dropped until one at the centre arrives");
    Frames(1);
    ExpectEq(Eval("inA", "return dump()"), "mouse:bar:left:true:false mouse:bar:left:false:false", "pointer click delivered");
    // Esc ends it and is consumed, the rest of the press too
    Eval("inA", "wum.input.pointer(true, {release = 'manual'})");
    At(10, 10);
    Expect(!Mouse(WM_LBUTTONDOWN) && !Mouse(WM_LBUTTONUP) && mi::PointerActive("inA"), "manual: a click outside does not end it");
    Expect(mi::EatEsc(), "DirectInput eats a bare Esc while pointer mode is on");
    Expect(Key(WM_KEYDOWN, 'W', 0x11), "the keyboard still reaches the game in pointer mode");
    Expect(!Key(WM_KEYDOWN, VK_ESCAPE, 0x01) && !mi::PointerActive("inA"), "Esc ends pointer mode and is consumed");
    Expect(!Key(WM_KEYUP, VK_ESCAPE, 0x01) && mi::EatEsc(), "the rest of that Esc press is eaten too");
    fake_input::g_now += 1500;
    Expect(!mi::EatEsc() && Key(WM_KEYDOWN, VK_ESCAPE, 0x01), "a later Esc reaches the game");
    // Esc first seen by DirectInput
    Eval("inA", "wum.input.pointer(true)");
    mi::EscFromDirectInput();
    Expect(!mi::PointerActive("inA") && !Key(WM_KEYDOWN, VK_ESCAPE, 0x01), "a DirectInput Esc ends it; its window message is eaten");
    // a turn of mine, the end of the match, the overlay and focus loss end it
    fake_input::g_now += 1500;
    Eval("inA", "wum.input.pointer(true)");
    fake_input::g_turn = true;
    mi::Tick();
    Expect(!mi::PointerActive("inA"), "a turn of this machine's player ends pointer mode");
    Eval("inA", "wum.input.pointer(true)");
    Expect(!fake_input::g_turn, "the turn watch is reset at pointer start");
    fake_input::g_match = false;
    mi::Tick();
    Expect(!mi::PointerActive("inA"), "the end of the match ends pointer mode");
    fake_input::g_match = true;
    Eval("inA", "wum.input.pointer(true)");
    fake_input::g_overlay = true;
    mi::Tick();
    Expect(!mi::PointerActive("inA"), "the overlay taking input ends pointer mode");
    ExpectEq(Eval("inA", "return tostring(select(2, wum.input.pointer(true)))"), "busy", "pointer is busy while the overlay captures");
    fake_input::g_overlay = false;
    Eval("inA", "wum.input.pointer(true)");
    mi::FocusLost();
    Expect(!mi::PointerActive("inA"), "focus loss ends pointer mode");
    fake_input::g_hookOk = false;
    ExpectEq(Eval("inA", "return tostring(select(2, wum.input.pointer(true)))"), "unavailable", "no warp hook: unavailable");
    fake_input::g_hookOk = true;
    const int endedBefore = fake_input::g_ended;
    Eval("inA", "wum.input.pointer(true)");
    sandbox::UnloadMod("inA");
    Expect(!mi::OwnsMouse() && fake_input::g_ended == endedBefore + 1, "unloading the owner ends pointer mode with the exit");
    Expect(!input::gesture::Recent("inA", 60000, false), "unloading forgets the mod's gestures");
    Expect(sandbox::LoadMod("inA"), "inA reloads");
    fake_input::g_match = false;

    // ---- keyboard focus and paste
    ExpectEq(Eval("inA", "return tostring(wum.input.focus(true))"), "true", "focus taken");
    ExpectEq(Eval("inB", "return tostring(select(2, wum.input.focus(true)))"), "busy", "one focus owner at a time");
    Expect(mi::OwnsKeyboard() && mi::SuppressBareFKeys(), "core sees the keyboard owned; bare F-keys suppressed");
    Expect(!Key(WM_KEYDOWN, 'A', 0x1E) && !render::modinput::RouteKey(WM_CHAR, 'a', 0) && !Key(WM_KEYUP, 'A', 0x1E),
           "keys and characters are kept from the game");
    Expect(!render::modinput::RouteKey(WM_CHAR, 0xD83D, 0) && !render::modinput::RouteKey(WM_CHAR, 0xDE00, 0),
           "a surrogate pair is kept from the game");
    Expect(render::modinput::RouteKey(WM_SYSKEYDOWN, VK_F4, 0x3E << 16), "system keys still reach the window (Alt+F4)");
    fake_input::g_mods = render::kModCtrl;
    Expect(!Key(WM_KEYDOWN, 'V', 0x2F), "Ctrl+V is kept from the game");
    fake_input::g_mods = 0;
    render::modinput::RouteKey(WM_CHAR, 0x16, 0);  // the control character Ctrl+V types: no char event
    Expect(!Key(WM_KEYDOWN, VK_ESCAPE, 0x01), "Esc is kept from the game");
    Expect(!mi::OwnsKeyboard() && mi::EatEsc(), "Esc ends focus; DirectInput eats the rest of it");
    Frames(1);
    ExpectEq(Eval("inA", "return dump()"),
             "key:A:true char:a key:A:false char:\xF0\x9F\x98\x80 key:F4:true key:V:true paste:https://youtu.be/dQw4w9WgXcQ "
             "key:ESCAPE:true",
             "focus delivers keys, characters and the paste, Esc first");
    fake_input::g_now += 1500;
    Eval("inA", "wum.input.focus(true)");
    fake_input::g_overlay = true;
    mi::Tick();
    Expect(!mi::OwnsKeyboard(), "the overlay taking input ends focus");
    ExpectEq(Eval("inA", "return tostring(select(2, wum.input.region('q', 0, 0, 1, 1)))"), "busy", "regions are busy under the overlay");
    fake_input::g_overlay = false;
    Eval("inA", "wum.input.focus(true)");
    ExpectEq(Eval("inA", "return tostring(wum.input.focus(false))"), "true", "focus released");
    Expect(!mi::OwnsKeyboard() && Key(WM_KEYDOWN, 'A', 0x1E), "keys reach the game again");

    // ---- hot reload: a failed reload keeps the running generation's pointer; a good one ends it
    fake_input::g_match = true;
    Eval("inA", "wum.input.pointer(true)");
    SetMod("inA", "error('broken on purpose')");
    Expect(!sandbox::ReloadMod("inA") && mi::PointerActive("inA"), "a failed reload leaves pointer mode on");
    SetMod("inA", kRecorder);
    Expect(sandbox::ReloadMod("inA") && !mi::PointerActive("inA"), "a reload ends the old generation's pointer mode");
    fake_input::g_match = false;

    // ---- [Input] AllowModInput=0
    fake_input::g_allowed = false;
    fake_input::g_fe = true;
    ExpectEq(Eval("inA", "return tostring(select(2, wum.input.region('btn', 0, 0, 9, 9)))"), "unavailable", "disabled: region");
    ExpectEq(Eval("inA", "return tostring(select(2, wum.input.focus(true)))"), "unavailable", "disabled: focus");
    ExpectEq(Eval("inA", "return tostring(wum.input.cursor())"), "nil", "disabled: no cursor");
    fake_input::g_allowed = true;
    sandbox::UnloadMod("inA");
    sandbox::UnloadMod("inB");
}
