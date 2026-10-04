# Melange

**A modding framework for Worms Ultimate Mayhem.**

Melange fixes long-standing multiplayer bugs and lets you install mods: new weapons, graphics effects, shaders, scripts and tools. It works with the Steam version of the game (build #1077) and switches itself off on any other build.

## Install Melange

1. Download the latest `melange-*.zip` from [Releases](https://github.com/JaminB/melange/releases) and extract it.
2. Run `Melange.exe`. It will guide you to your game folder, check the build, and install or update Melange.
3. Start the game. The window title shows `[Melange x.y.z]`.

**Undo:** Delete the `Melange.exe` copy in your game folder, `melange.asi`, `Melange.ini` and the `Melange` and `Mods` folders. Delete `dinput8.dll` too, unless other ASI mods still need it. Melange keeps a backup of anything it replaces on Home › Settings › Melange › Backups.

Releases are code-signed; see [Code signing](CODE_SIGNING.md).

![The in-game overlay, opened with the grave key](docs/images/overlay/main-overlay.png)

## Install a mod

1. Put the mod's folder in `<game>\Mods\`, so that you have `<game>\Mods\<mod>\spice.json`.
2. Start the game and press `` ` `` to open the overlay.
3. On the *Thumper/Mods* page, switch the mod on. Mods that change gameplay take effect the next time you start the game.

Or install it from the Store: on the *Thumper/Mods* page press *Store* (or open the **Store** panel in Oasis), pick a plugin and press *Install*. The Store contacts GitHub only when you open it, and sends nothing about you or your game. Some plugins, like Caravan, import maps from a download onto your PC.

Good to know:

- **Online play:** everyone in a match needs the same gameplay mods. Mods that only change your screen, such as effects and UI, don't matter to other players.
- **Permissions:** a mod that asks for raw access to the game's memory ("Deep Desert") shows a consent dialog first. Only allow mods you trust.
- **Samples:** `dist\Mods\` has example mods, all switched off. Copy one into `<game>\Mods\` to try it.

## Uninstall

Delete `melange.asi`, `Melange.ini` and the `Melange` and `Mods` folders from the game folder. Delete `dinput8.dll` too, unless other `.asi` mods still need it.

## Reporting a bug

Press `Ctrl+Shift+F11` in the game (or *File > Save logs as...* in the overlay) and attach the zip it saves. User names are removed, and Steam IDs and IP addresses are hashed.

## For developers

- [Creating a plugin](docs/creating-plugins.md): write your first mod or C++ module.
- [Developer guide](docs/developer-guide.md): settings, logs, the SDK and every subsystem, and building from source.
- [Map editor (Erg)](docs/erg.md): build, test and share your own Versus maps from Oasis.

## License

[MIT](LICENSE). Melange is an unofficial fan project, not affiliated with or endorsed by Team17. You need your own copy of Worms Ultimate Mayhem.
