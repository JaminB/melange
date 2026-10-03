# Code signing

Melange releases from 0.3.1 on are code-signed. You can check the signature in the file's *Properties > Digital Signatures* tab, or with `Get-AuthenticodeSignature <file>` in PowerShell.

- **What is signed:** `melange.asi`, `oasis.exe` and `tools\xomtool.exe` in the release zip. `dinput8.dll` is the unmodified [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) and is not signed by Melange.
- **How:** the [`release` workflow](.github/workflows/release.yml) builds the binaries from this repository's public source on a GitHub-hosted runner when a version tag is pushed, signs them with Azure Artifact Signing (SHA-256, timestamped), checks the signatures, and attaches the zip and its SHA-256 to the GitHub release. No locally built binary is ever signed.
- **By whom:** the certificate is issued to the maintainer, JaminB, who is the only committer, reviewer and approver of releases.
- **Where:** https://github.com/JaminB/melange/releases is the only source of signed Melange releases. Forks and mirrors cannot produce them.
- **Privacy:** signing transfers no user data. Melange collects no telemetry; it contacts GitHub only when you open the Store, and sends nothing about you or your game.
