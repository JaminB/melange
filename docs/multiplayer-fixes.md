# Multiplayer fixes

Melange fixes a set of bugs in Worms Ultimate Mayhem's online play: freezes, matches that end with "This session is no longer available", and crashes. Each fix runs on your own machine and needs nothing from the other players: none of them changes the format of what the game sends, and none of them checks whether the other players have Melange. So a fix only covers a bug that starts on a machine running Melange; a player without Melange can still run into the bug on their side. Whether a match between Melange and non-Melange players stays in sync with every fix on is not documented; Melange's [desync detection](wormsign.md#desync-detection) only compares with players on the same Melange version. All fixes are on by default unless a section below says otherwise, and they work with the Steam build #1077 only. To turn one off, set its key to `0` in `Melange.ini`, in the section named below, and restart the game.

## Freezes after a lost packet

**In the game:** the game resends a lost packet only for the first two packets of a connection. A packet lost later, just before both machines wait on each other, froze the match on both sides.

**What Melange does:** when the other player acknowledges a packet, Melange moves the game's resend window forward and keeps its resend timer running while newer packets are still unacknowledged (every 250 ms). The packets themselves are unchanged.

**Switch:** `[NetTransport] FixRetransmit=1`

## Freezes after a lost acknowledgement

**In the game:** when your acknowledgement of a packet got lost, the other player kept resending it, and the game ignored the copies without acknowledging them again, so the other player waited.

**What Melange does:** when a packet arrives that was already delivered, Melange acknowledges it again.

**Switch:** `[NetTransport] ReAckDuplicates=1`

## One player's dropped connection closing everyone's

**In the game:** when Steam reported that the connection to one player had failed, every connection closed itself, without checking whose it was. A late timeout for a player who had already left (for example, the old host after the host changed) tore down the connections to the players still in the match.

**What Melange does:** a connection ignores a failure that belongs to another player. When Melange can't tell which player a connection belongs to (while it is being set up or torn down), the game handles the failure as before.

**Switch:** `[NetTransport] IgnoreOtherPeerFails=1`

## Leftover surrender in the next match

**In the game:** a "surrender at the next turn" flag from one match could survive into the next match in the same lobby. Only some machines then surrendered that player, and the match went out of sync.

**What Melange does:** clears those flags when a match starts and when everyone is back in the lobby.

**Switch:** `[NetSession] FixStaleSurrender=1`

## Next match pausing every tick after the host changed

**In the game:** after the host changed, the game's network throttle forgot which message types it had received, and nothing restored them, so it paused the match on every tick.

**What Melange does:** restores them when a match starts and when everyone is back in the lobby.

**Switch:** `[NetSession] FixThrottleMask=1`

## Next match failing to start after the host left

**In the game:** after the host was lost, a value the session uses to check that it can still run stayed at `-1`, and that check failed from then on.

**What Melange does:** puts the value back to `0` when a match starts and when everyone is back in the lobby.

**Switch:** `[NetSession] FixViabilityOffset=1`

## Next match frozen by a pause from the last one

**In the game:** a network pause that started late in a match carried over into the lobby, and the next match never ran.

**What Melange does:** when everyone is back in the lobby, releases that pause the way the game does when it aborts a match.

**Switch:** `[NetSession] FixStuckPause=1`

## Inputs played twice after the host changed

**In the game:** after the host changed, the player whose turn it was resent the whole turn's inputs, then sent the most recent ones a second time. The other machines applied those twice, and the match went out of sync.

**What Melange does:** after the resend, drops the inputs it already carried from the queue, but only when they match it exactly; otherwise it leaves the queue alone and says so in `Melange.log`.

**Switch:** `[NetSession] FixResendDuplicates=1`

## Desync after closing the weapon panel with the girder

**In the game:** closing the weapon panel while the girder was selected re-aimed the girder camera on that machine only. The camera could land in another position than on the other machines, and the turn ended with "This session is no longer available". A spectator who right-clicked while the girder was selected could set it off too.

**What Melange does:** during an online match, the panel close no longer switches the camera. The girder controls still come back as before. Offline, the game re-aims as it always did.

**Switch:** `[NetSession] FixGirderPanelCamera=1`

## Desync after an explosion when a spectator changes view

**In the game:** after a projectile exploded, the camera that followed it could hold for about a second, and the turn waited for that hold. It happened on one machine only, for example on a spectator who pressed `E` (blimp view) just after a Super Sheep exploded. That machine ended its turn later than the others, and the match ended with an out-of-sync error.

**What Melange does:** during an online match, the hold no longer delays the end of the turn. The camera still pauses. Offline, the game behaves as before.

**Switch:** `[NetSession] FixCameraHold=1`

## Crash joining a lobby on a map you don't have

**In the game:** joining a host whose map is not in your game's level list crashed the game.

**What Melange does:** keeps your current map instead, and logs the map's code in `Melange.log`.

**Switch:** `[NetSession] FixLobbyLevelNull=1`

## Crash joining a host with a game style you don't have

**In the game:** joining a host whose game style is one your game lacks (for example, one added by a mod) read past the end of your list of game styles.

**What Melange does:** shows that style as "Unknown game style" and logs it in `Melange.log`.

**Switch:** `[Fixes] SchemeCode=1`

## Crash closing the game after an online or LAN session

**In the game:** closing the game after playing online or on a LAN could crash it on the way out.

**What Melange does:** skips the step that crashed (the game set a flag on a part of itself it had already shut down).

**Switch:** `[Fixes] NetServiceExit=1`

## Off by default: weapon picked just before opening the pause menu

**In the game:** when the player whose turn it is picks a weapon and opens the pause menu (or another menu layer) before the pick takes effect, their machine keeps the old weapon while the others switch, so the match can go out of sync.

**What Melange does:** with the switch on, during an online match, the pick is applied on that machine too, as the other machines already do.

**Switch:** `[NetSession] FixPanelSelectUnderMenu=0` (set it to `1` to turn it on). It is off until this desync is reproduced online.

## Off by default: forfeit instead of abort when the other player drops

**What Melange does:** with the switch on, when the only other player in a two-player match drops, the match is meant to end with that player forfeiting instead of "This session is no longer available".

**Switch:** `[NetSession] DeadPeerForfeit=0` (set it to `1` to turn it on). Beyond that, how it behaves is not documented; leave it off unless you want to try it.

## Warning: game files differ

This one is a warning, not a fix. Third-party mods that replace game files or turn off the game's file check (MMP, Renewation HD, a `Data2` folder) can make a match go out of sync with no sign of why. Each Melange player shares a short fingerprint of their game program and game data files with the lobby. When another Melange player's fingerprint differs from yours, the lobby shows "Game files differ between players: this match may desync" with what differs. It never stops a match from starting, and players without Melange are not checked.

**Switch:** `[Handshake] PeerIntegrity=1`

## Reporting a desync or crash

Export the last game's logs right after it happens and attach the zip to your report: see [Reporting a bug](../README.md#reporting-a-bug). The zip has that game's logs, match recordings and desync bundles, and most fixes above write a line to `Melange.log` when they step in. Say who in the match had Melange and which version.
