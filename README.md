# QQ Music in English

An add-on that shows the QQ Music Windows desktop app (QQ音乐) in English. It
translates the app's menus, settings, buttons, tooltips, pop-ups and built-in web pages
(Home, Music Hall, VIP) while they are on screen. QQ Music's own files are not changed.

Tested with QQ Music 22.76 on Windows 11. It ships with 5,300+ translations.

Not affiliated with or endorsed by Tencent. Use at your own risk.

## Install

1. Download `QQMusicEnglish.zip` from [Releases](../../releases) and unzip it.
2. Close QQ Music completely: right-click its tray icon and choose 退出 (Exit).
3. Double-click `install.cmd` and approve the admin prompt.
4. Start QQ Music.

Run `install.cmd` again after QQ Music updates itself.

If QQ Music is installed somewhere unusual and isn't found, run this in PowerShell from
the unzipped folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1 -Path "D:\Apps\QQMusic"
```

### Uninstall

Close QQ Music and double-click `uninstall.cmd`. That removes `version.dll` from the
QQ Music folder and the translation files from `%LOCALAPPDATA%\QQMusicEN`.

## What stays in Chinese

- Song, artist and album names, lyrics, playlists and comments from other users.
  These change all the time, and machine-translating names would mangle them.
- Text that is part of an image: some banners, badges and the sound-effect tiles.
- Text written live by QQ Music's AI features.

## How it works

The add-on is a `version.dll` placed next to `QQMusic.exe`. Windows loads it with
QQ Music in place of its own `version.dll`, and it passes every real `version.dll`
call through to the Windows copy.

- **App interface** ([src/qqmusic_en.cpp](src/qqmusic_en.cpp)): QQ Music draws all of
  its own text with DirectWrite. The add-on hooks `IDWriteFactory::CreateTextLayout` and
  gives it the English text, so the same English is measured and drawn and stays put on
  every redraw. If the English needs more room than the Chinese had, the font is shrunk
  to fit (down to 72%). Text is matched exactly, then as printf-style templates
  (`播放 %s`), then piece by piece; counts such as `2494.0万` become `24.9M`.
- **Web pages** ([src/webbridge.cpp](src/webbridge.cpp), [web/web.js](web/web.js)):
  Home, Music Hall, Radio and the VIP pages run in QQ Music's own Chromium browser
  (`qmbrowser.exe`). The add-on starts it with a DevTools port and injects `web.js`,
  which translates page text as it appears.
- **Windows dialogs** ([src/native.cpp](src/native.cpp), [src/iat.cpp](src/iat.cpp)):
  message boxes, window titles and classic dialog windows.

### Privacy and security

- The add-on collects nothing and connects to nothing on the internet. Its only network
  connection is to QQ Music's own browser on `127.0.0.1`.
- The web-page part opens a Chromium DevTools port on `127.0.0.1` (a random port each
  time QQ Music starts). Only programs on your own PC can reach it, but while QQ Music
  runs, such a program could use it to control QQ Music's web pages. To turn the
  web-page translation off, create an empty file named `web.off` in
  `%LOCALAPPDATA%\QQMusicEN`.
- Antivirus programs sometimes flag unsigned files like this one, because placing a
  `version.dll` next to a program is also a trick malware uses. The source is all here,
  and each release is built from it by GitHub Actions, so you can check it or build it
  yourself.

## Fixing or adding translations

All translations are in [translations/zh-en.json](translations/zh-en.json), as
`"Chinese": "English"` pairs. Keys are the exact Chinese text. In keys with `%s` or `%d`,
those parts are filled in by the app, and the English should keep the same ones in the
same order.

The add-on writes what it could not translate to `%LOCALAPPDATA%\QQMusicEN`:

- `missing.txt`: app text that reached the screen untranslated
- `missing_web.txt`: untranslated web-page text
- `overflow.txt`: English that still doesn't fit its space, even after shrinking
- `status.txt`: whether the hooks are active

To try a change without rebuilding the add-on:

```powershell
python -X utf8 tools\build_tables.py "$env:LOCALAPPDATA\QQMusicEN"
```

then restart QQ Music. For personal tweaks you don't want to send in, add a file such as
`translations\zz-local.json`. Files are merged in name order, so later files win.

Pull requests with translation fixes are welcome. Short labels matter: many buttons
and tabs only have room for a word or two.

After a QQ Music update, `tools\find_untranslated.py` lists Chinese strings in its
program files that aren't translated yet. Many of these are internal messages that are
never shown, so compare the list with a previous run, or with `missing.txt`:

```powershell
python -X utf8 tools\find_untranslated.py "C:\Program Files (x86)\Tencent\QQMusic" new.json
```

## Building

Needs Visual Studio 2019 or later (or Build Tools) with *Desktop development with C++*,
and Python 3.

```powershell
.\build.ps1        # -> dist\QQMusicEnglish
.\build.ps1 -Zip   # also dist\QQMusicEnglish.zip
```

Then run `dist\QQMusicEnglish\install.cmd`. Pushing a `v*` tag builds a release with
the zip attached.

## License

MIT, see [LICENSE](LICENSE). QQ Music and its text belong to Tencent. This repository
contains none of QQ Music's files.
