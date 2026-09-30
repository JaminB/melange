#pragma once
// Starting Quick Game from code at the frontend, so `level.test` can play a Test level itself instead of
// asking the user to press the button. #1077 only; refuses on any other build or off the main thread's frontend.
namespace melange::erg::quickstart {
bool Available();    // FnOk() would pass: known build, the bus registry is ready, the post site's bytes match
bool PostQuickGame();  // posts WXMsg.StartGame "QuickStartHvC"; true only if the post itself did not fault
}  // namespace melange::erg::quickstart
