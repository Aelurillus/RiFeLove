# Offline desktop localization

The desktop contains four complete local catalogs: Simplified Chinese (`zh_CN`), Traditional Chinese (`zh_TW`), English (`en`) and Japanese (`ja`). Simplified Chinese is the source language; the other three catalogs are compiled to Qt `.qm` files and embedded in `RifeLove.exe`. Runtime language selection does not access the network.

At first launch, the app maps the Windows locale to a supported language and saves that choice in `data/language.ini` for the portable build, or in the local application data folder for a regular build. The Home page can change the saved language. The current process keeps its active language until restart, and the page reports that requirement. Unsupported system locales fall back to Simplified Chinese.

To update the catalogs after changing QML or translated C++ source strings, run these commands from the repository root with the pinned Qt SDK:

```powershell
& 'C:\itzming\dependences\qt-6.12.0-local\bin\lupdate.exe' apps\desktop ui -recursive -no-obsolete -ts ui\translations\rifelove_en.ts ui\translations\rifelove_zh_TW.ts ui\translations\rifelove_ja.ts
python tools\development\update_translations.py
cmake --build build\windows-qt-release --target rifelove-desktop rifelove-translation-check
```

`update_translations.py` fills the maintained translations and rejects missing strings, empty entries or changed `%N` placeholders. The catalogs currently cover 457 source messages each. The `Desktop.Translations` check loads the compiled catalogs and verifies representative Home, Studio and queue messages. `Desktop.LocalizationSmoke` launches the actual QML app once per language with isolated local settings and checks that each locale renders successfully.

Language coverage does not certify complete keyboard navigation or screen-reader accessibility; those remain separate release work.
