AnyPS5 Desktop 0.5.0 — Windows x64

START THE APPLICATION
Open dist\AnyPS5 Desktop-win32-x64\AnyPS5 Desktop.exe.
Keep the entire application folder: the executable needs its adjacent files.
The app starts in windowed mode. F11 switches between fullscreen and windowed mode. The app can be operated with arrows, Enter and Esc.
The application interface, messages and date formatting are in English.

ANIMATED BACKGROUND
The home screen recreates the classic PS5 ambience with original procedural light,
gold particles and soft bokeh, with visible drift at different depths.
The library fits the window without outer scrollbars in both display modes. Motion pauses while the window is hidden. The Windows reduced-motion preference
slows the requested live background instead of freezing it. The effect dims behind game artwork.
It uses local code, capped rendering resolution and no video/network downloads.

MENU AUDIO
A warm ambient background with eight evolving harmonies, sparse soft melodies,
uneven phrase lengths and gentle stereo movement. Soft navigation, selection,
back, opening, success and
error sounds accompany the menu. Settings > Audio provides an enable switch and
separate Background music / Menu sounds volumes, saved on this PC. Changes preview
immediately; Save settings keeps them, and closing Settings restores saved levels.
Audio fades out while the app is hidden or loses focus, and resumes on return.
On a volume slider, Enter starts editing, arrows adjust, Enter confirms, and Esc
restores the previous value. These sounds are synthesized locally; no audio download
or original PlayStation recording is required.

BROWSE LOCATIONS
Each file/folder picker remembers its last confirmed location across app restarts.
Cancel keeps the previous location. If a remembered folder no longer exists, the
nearest available parent folder is used. Browse history is saved automatically.

KEYBOARD CONTROLS
Use the arrow keys to move the visible focus between controls. On the game shelf,
Left/Right previews games and Enter launches the focused game. Enter activates
buttons and toggles checkboxes. Hidden and disabled controls are skipped.
In Settings, Enter edits a player choice; arrows change it; Enter confirms the
choice and Esc restores its previous value. Choose Save settings to persist changes.
For text fields reached with arrows, press Enter to edit. Arrows then move the caret;
Enter or Esc returns to navigation. Mouse and Tab-based text editing remain available.
Esc closes menus, search and Settings, or moves back through the conversion wizard.
From the main library, Esc opens the Exit menu. Choose Stay or press Esc to return;
choose Exit and press Enter to close the app.
During conversion, Esc focuses Cancel; press Enter to explicitly stop conversion.
Native Windows file dialogs keep their standard keyboard controls.

CONTROLLER MENU NAVIGATION
Mouse, keyboard and a standard-mapped controller can be used interchangeably.
D-pad or left stick moves focus; A/Cross selects; B/Circle goes back. Settings
choices and sliders use A/Cross to start/confirm editing and B/Circle to revert.
Numeric input settings can be adjusted with the D-pad/stick. Text entry and native
Windows file dialogs still use the keyboard/mouse. Button hints follow the last
input device; supported PlayStation pads show Cross/Circle instead of A/B.
Hold a direction to repeat navigation. Confirm/Back fire once per press. A stick
deadzone filters drift. Controllers never navigate an unfocused or hidden app;
release held controls after returning to the launcher or reconnecting the pad.
Unknown raw gamepad button orders are not guessed: menu navigation requires the
browser's standard gamepad mapping. SDL game input remains separate.

PLAYER SETTINGS
Open Settings > Players to configure Player 1, 2, 3 and 4.
Each player can be Disabled, use Keyboard & mouse, or use Controller 1, 2 or 3.
Only one player can use the keyboard and mouse. Each controller can be assigned
only once, with a maximum of three controllers. Disable an existing assignment
before moving that input to another player. Settings are saved across restarts.

Connect a supported Xbox, PlayStation or generic controller over USB or Bluetooth.
Press a button while Settings is focused to make it visible to the browser. These
slots follow browser discovery order; they do not select the SDL device used by games.
The four assignments remain saved launcher profiles, not four-player engine routing.

INPUT MAPPING AT LAUNCH
Settings > Input mapping controls left/right stick deadzones (0-90%) and mouse
sensitivity (greater than 0, up to 10). Official defaults are 0%, 0%, and 1.
When launching from this app, a complete profile is supplied through the child's
ANYPS5_INPUT_CONFIG environment variable. Generated profiles are stored under
%APPDATA%\AnyPS5 Desktop\input-profiles; game files are not modified.
A custom anyps5-input.ini beside the executable takes precedence and is preserved.
Direct executable/old desktop-shortcut launches use AnyPS5's own config discovery;
launch through AnyPS5 Desktop to apply the saved launcher profile.

SDL automatically uses the first recognized controller. Xbox A/B/X/Y map to
Cross/Circle/Square/Triangle; LB/RB to L1/R1; LT/RT to analog L2/R2; Start to Options.
Sticks, D-pad and stick clicks are passed through. Keyboard and mouse retain the
engine defaults. These settings do not remap controller buttons or enable multiplayer.
The ambient music channel is boosted by about 12 dB versus 0.4.6; effects retain
their independent gain, and the output compressor remains active.

CONVERSION
1. In Settings > Engine, select relinker.exe compiled for Windows.
2. Select the libraries in build\core\libs\libs (not unpatched).
3. If the three MinGW DLLs are not in libs, select the bin folder of the
   toolchain used to compile those libraries.
4. Choose the game data folder or an ISO image and its decrypted ELF executable. A single outer
   wrapper folder is detected automatically. For a recognized decrypted subfolder,
   the original game resources are combined with matching decrypted ELF binaries
   in a temporary installation folder; source files stay unchanged.
5. Choose the destination library and start conversion.
6. When complete, optionally create a desktop shortcut and launch the game.

The progress bar counts five completed stages, not elapsed time. Copying reports
its own percentage of bytes. Relinker conversion stays marked Running until the
process exits and its output is checked; the bar fills only after finalization.

Installation creates <library>\<game>\app.exe, app0, libs, anyps5-game.json
and conversion.log. Existing destination folders are not overwritten; the source
stays intact. Additional ELF modules are discovered in subdirectories and passed
to the relinker through --module-dir. AnyPS5 engine binaries are not downloaded,
compiled or bundled by this application.

ISO SOURCES
Choose ISO image opens a complete .iso through Windows read-only mounting.
The image must contain the extracted game folder, resources and decrypted ELF
executable/modules, directly or in a recognized wrapper/decrypted layout.
Choose an executable from the detected paths inside the image. The app releases
its temporary mount after inspection, checking or conversion. Existing mounts
opened by you remain attached. The ISO picker remembers its own last location.
PKG-only images and encrypted SELF executables are not converted or decrypted.
The Windows AnyPS5 engine and libraries are still configured in Settings.

LIBRARY
Converted games are added automatically. Previously converted games can be
imported from a folder containing the Windows executable, app0 and libs.
Removing a game from the library does not delete its files.
Application data is saved in %APPDATA%\AnyPS5 Desktop\library.json.
Game covers/backgrounds use app0\sce_sys\icon0.png, pic1.png or pic0.png.
Without artwork, the interface uses the animated ambience and the game's initials.

DEVELOPMENT
npm ci
npm start
npm test
npm run test:ui
npm run test:browse
npm run test:audio
npm run test:audio-settings
npm run test:iso-ui
npm run test:iso-native
npm run test:iso-close
npm run test:progress
npm run test:keyboard
npm run test:controller
npm run test:background
npm run package

UI tests use an isolated library and simulated relinker. They verify desktop
conversion orchestration, player settings, Windows shortcuts and a test program's
launch. They do not prove conversion or compatibility of a real PS5 game.
Native ISO tests create small temporary images, verify read-only Windows mounts,
mount ownership and cleanup, and run a simulated conversion through Electron.
They do not test compatibility of a real game or unpack PKG containers.
Controller detection tests simulate Gamepad API devices; physical USB/Bluetooth
controllers require separate hardware verification.
The UI test compiles two fixture executables with GCC from the sibling AnyPS5
repository. ANYPS5_TEST_GCC can select another gcc.exe. ANYPS5_TEST_APP can select
the packaged application executable. Test screenshots are stored in artifacts.

LIMITATIONS
The interface recreates the PS5 game-menu layout with original components and
Windows fonts. It does not include PSN, Store, trophies, DRM, ISO/PKG/SELF decryption,
automatic engine updates or game compatibility verification.
A successful conversion means that the relinker completed and files were arranged;
missing engine functionality may still prevent a game from running.

References:
https://github.com/boykopovar/AnyPS5/blob/main/docs/user/USAGE.md
https://blog.playstation.com/2020/10/15/first-look-playstation-5s-next-generation-user-experience/
