# CS2 Full Reset

Portable Windows utility for backing up and rebuilding Counter-Strike 2's Steam
userdata/config state. One native x64 executable, no installer or external runtime.
The console interface uses Russian on Russian Windows and English otherwise.

[Download v0.1.0 (preview)](https://github.com/alextmsv/FullCS2ConfigCleaner/releases/tag/v0.1.0)

## What it does

The utility finds Steam, selects one account, backs up the entire
`userdata/<SteamID32>/730` directory, and attempts a two-phase reset followed by
launching CS2 through Steam. It is built with C++20 and Win32; no administrator
privileges are requested.

**A Steam Cloud conflict is not guaranteed.** If Steam launches the game without
a conflict, the utility stops before the final deletion phase and retains the
original backup. Automatic conflict detection is conservative and may require
manual confirmation and Local Save selection.

## What it does NOT claim

> There is currently no controlled measurement in this project proving that this reset reduces end-to-end input latency. The utility exists to reproduce a clean CS2 userdata/config reset reliably and safely.

This is not an optimizer. It makes no FPS, frame-time, subtick or mouse-accuracy
claims. It does not change Windows/GPU settings, launch options, autoexec,
sensitivity, shader caches, process priorities or power plans. There is no
telemetry, advertising, updater or network service.

## Why a userdata reset can be useful

CS2 uses Steam AppID `730`. Its user configuration includes files under
`userdata/<SteamID32>/730/local/cfg`, such as:

- `cs2_user_convars_0_slot0.vcfg`
- `cs2_user_keys_0_slot0.vcfg`
- `cs2_machine_convars.vcfg`
- `cs2_video.txt`

The exact files depend on the game version and account state. A subsequent launch
can recreate missing configuration files. This describes observable file behavior,
not a performance benefit. The documented cases below report recovery from broken
input or display settings; they do not establish a cause or prove reduced latency.

## Safety / automatic backups

Keep the EXE in a writable directory. All storage is relative to the **EXE**,
not the working directory:

```text
cs2_backup/
  settings.json
  logs/last.log
  profile_<sanitized nickname>_<SteamID32>/
    YYYY-MM-DD_HH-mm-ss-mmm/
      730/                 # complete original directory
      complete.txt         # relative paths and sizes
```

- Two completed backups are retained per account. A new backup is copied and
  verified before an older unprotected backup is retired. Recovery snapshots
  remain pinned while reset or restore is incomplete.
- Verification compares names, directory structure and primary-stream sizes,
  not content hashes. Same-size corruption is not detected. Windows file-copy
  APIs also copy alternate data streams; those streams are not independently
  covered by the manifest.
- Backup failure, insufficient space or inability to write beside the EXE blocks
  reset. Incomplete `.partial` snapshots are never selected for restoration.
- Operations require the discovered Steam root, a numeric account directory and
  AppID `730`. Traversal, reparse points (including symlinks/junctions), hard-linked
  files, UNC paths and paths longer than 240 characters are rejected.
- Directory handles prevent path renames during operations; truncation/deletion
  use validated file handles. Neighboring accounts and AppIDs are not reset.
- CS2 must be closed. Process checks run before mutations. They cannot eliminate
  the race with a separately starting game; do not launch CS2 independently while
  the tool works.
- Steam is never forcibly closed. Closing it before starting is recommended; the
  utility launches it after Phase A if needed. It must sign in to the selected
  account. The account is checked again before Phase B and Local Save invocation.
- Do not edit completed backups or remove recovery markers. Interrupted cleanup
  may leave excluded `.deleting` directories; these and `.partial` directories
  can occupy additional disk space.

## Usage

Close CS2, launch `CS2FullReset.exe`, select an account on first use, and choose
**Reset & Launch CS2**. Use Up/Down and Enter; Escape cancels a menu.

Accounts are read from Steam's local `loginusers.vdf` and matched to existing
numeric `userdata/<SteamID32>/730` directories. The list shows IDs and cached
nicknames, newest login first; `MostRecent` breaks timestamp ties. Missing profile
data falls back to IDs and an unknown nickname.

The chosen ID is saved in `cs2_backup/settings.json` and validated on every run.
Missing or invalid selection always opens the selector, even with one account.
Only one account is processed.

```powershell
.\CS2FullReset.exe --run
.\CS2FullReset.exe --backup-only
.\CS2FullReset.exe --restore-latest
.\CS2FullReset.exe --select-account
.\CS2FullReset.exe --diagnose
.\CS2FullReset.exe --help
```

`--run` skips the main menu, but account selection, errors and conflict questions
can still appear. Add `--account 123456` for explicit selection for this run;
combine it with `--select-account` to save that choice. Redirected input cannot
operate the interactive selector.

The menu also opens backups and the log. `--diagnose` reads Steam/account/process
state and probes UI Automation without resetting anything. Exit codes: `0` for a
completed action, `1` for an error, `2` for an incomplete reset/manual action.
One normal instance is allowed per Windows session.

## Restore

Close CS2 and cancel any pending launch in Steam. Closing Steam is also recommended
to avoid concurrent synchronization. Run `--restore-latest` for the same account.
The tool first backs up the current entire `730`, then restores the previously
selected snapshot, removing extra files from the current tree.

After an interrupted reset, the pinned pre-reset snapshot takes precedence over
newer partial-state backups. Restore records its target and safety backup so a
retry does not replace the original safety copy with partially restored data.
Restore is blocked while CS2 runs.

If Local Save was chosen after the final launch timeout, the marker remains
conservative: restore is required before another reset. The utility does not infer
success retrospectively from an unrelated game launch.

## How the reset works

1. Validate account, paths and access; copy and verify the full `730` backup.
2. **Phase A:** recursively truncate ordinary files in `remote` and `local` to
   zero bytes without deleting files/directories. Verify the resulting sizes.
3. Launch CS2 through `steam.exe -applaunch 730` and observe for up to 60 seconds.
   A game launch without conflict or a detected sync error stops the procedure.
4. If automatic recognition fails, ask whether the Steam Cloud config conflict
   appeared. Answer Yes only when CS2's local/cloud alternatives are visible.
   Keep the dialog open and do not choose a version yet. No stops the reset.
5. **Phase B:** after automatic or explicit human confirmation, clear the contents
   of `remote` and `local`, remove `remotecache.vdf`, and create empty `local/cfg`.
6. Invoke Local Save only when its visible, enabled, unique button belongs to a
   recognized CS2 conflict in a Steam process. Otherwise follow the always-on-top
   instruction to select **LOCAL SAVE** yourself.
7. Wait another 60 seconds for `cs2.exe`. Only an observed launch after Phase B is
   reported as complete; this does not assert that all game files regenerated.

UI Automation checks the executable path, dialog title, CS2 context, conflict
wording and both local/cloud alternatives. Conservative English/Russian labels
are supported. Bounded child processes of the same EXE prevent an unresponsive
UIA provider from hanging the workflow indefinitely. There is no OCR, coordinate
clicking, screenshot matching or blind keyboard navigation. Internal `--uia-*`
options are not supported user commands.

A network error, Play Anyway prompt or timeout is not a confirmed conflict. The
tool does not disable Steam Cloud or edit Steam's internal configuration.

## Validation and known limitations

- 32 filesystem/workflow fixture cases plus the CLI help check pass. Tests use
  temporary directories and a fake Steam adapter, not the installed game's files.
- A real backup/reset attempt reached Phase A, but Steam launched CS2 without a
  conflict. Phase B was correctly blocked. After the game closed, Restore returned
  all 22 files to their original contents, verified independently with SHA-256.
- The complete conflict-driven flow and automatic Local Save invocation have
  **not** been demonstrated on that Steam installation. Manual fallback may be
  needed because Steam exposes little accessibility information.
- The release was built/tested on Windows x64. Windows 10 and a fresh standard-user
  account have not been separately tested. The binary requests `asInvoker` and is
  unsigned.

## Sources / documented cases

- **Official behavior:** [Steam Support — Steam Cloud](https://help.steampowered.com/en/faqs/view/68D2-35AB-09A9-7678)
  explains local/cloud conflicts when versions cannot be resolved automatically.
  It does not guarantee that truncating files will produce a conflict.
- **Documented user case:** [Lei Mao — CS2 Mouse Fix, January 14, 2025](https://leimao.github.io/blog/CS2-Mouse-Fix/)
  describes recovering mouse clicks and display behavior on Ubuntu after deleting
  `730` following a monitor change. It is not a latency benchmark.
- **Community report:** [Steam Community — CS2 Mouse not reacting after loading cfg](https://steamcommunity.com/app/730/discussions/0/3823033163000303420/)
  includes reports about recovering input through configuration fixes, including
  resetting `local/cfg`. It is not controlled measurement.
- **Third-party procedure:** [ndunnett/cs2-config](https://github.com/ndunnett/cs2-config)
  documents disabling Cloud, removing `730` contents, running/closing CS2,
  enabling Cloud and selecting Local Save. That different sequence does not prove
  that this utility will always generate a conflict. Its optimization claims are
  not adopted here.
- **Anecdotal community report:** [r/GlobalOffensive — How to reset settings on CS2?](https://www.reddit.com/r/GlobalOffensive/comments/145mf8j/how_to_reset_settings_on_cs2/)
  discusses reinstalling versus resetting userdata with Cloud disabled. It does
  not establish a technical cause or performance benefit.

## Building from source

Requirements: Windows x64, Visual Studio 2022 C++ Build Tools, Windows SDK and
CMake 3.24+. No package downloads are needed.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The binary is `build/Release/CS2FullReset.exe`. Alternatively, run
`powershell -File scripts/build.ps1` to build, test and copy it to `dist`.
The CRT is linked statically; imported DLLs are Windows components. The symlink
test requires Developer Mode or symbolic-link creation privilege. The application
itself does not require either.

Source layout: `safe_fs` handles guarded operations; `backup` handles snapshots
and recovery; `accounts` handles VDF/settings; `workflow` coordinates phases;
`win_steam` handles Windows/Steam/UIA; `main` provides CLI, localization and logging.

## License

Licensed under [GNU General Public License version 3](LICENSE).
Steam and Counter-Strike are Valve trademarks. This is an independent utility.
