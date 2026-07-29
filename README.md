VaCuus Plugin
=============

VaCuus plugin for Unreal Engine.

> **Status: early scaffold.** The modules build and load, but no functionality is implemented yet.

Current engine version: **UE 5.8**

## Installation

Copy (or symlink, or add as a submodule) this repository into your project's `Plugins` folder, so that
`VaCuus.uplugin` ends up at `<YourProject>/Plugins/VaCuus/VaCuus.uplugin`. Then enable the plugin in
`Edit -> Plugins`, or add it to your `.uproject`:

```json
"Plugins": [
    {
        "Name": "VaCuus",
        "Enabled": true
    }
]
```

The repository root *is* the plugin folder — there is no extra directory level to strip.

## Layout

```
VaCuus.uplugin              Plugin descriptor
Config/FilterPlugin.ini     Extra files to include when packaging the plugin
Content/                    Plugin content (CanContainContent is enabled)
Resources/Icon128.png       Icon shown in the editor plugin browser
Source/VaCuus/              Runtime module (loading phase: Default)
Source/VaCuusEditor/        Editor module (loading phase: PostEngineInit)
```

Both modules log on startup/shutdown, and `UVaCuusWorldSubsystem` prints a "Hello world" line to the log and to the
screen when a game session begins play — enough to confirm the plugin is loaded and running. Runtime code uses the
`LogVaCuus` category declared in `Source/VaCuus/Public/VaCuusDefines.h`.

## Development

Development and testing happen in a separate demo project that is **not** part of this repository, so that the repo
stays a clean plugin folder. See `VaCuusDemo` (a UE 5.8 Third Person C++ project) next to this one; it links the
plugin in via `VaCuusDemo/Plugins/VaCuus -> ../../VaCuus`.

The demo is a throwaway copy of the stock Third Person template and deliberately keeps the template's own names: the
game module is `TP_ThirdPerson` and its classes are `TP_ThirdPersonCharacter`, `TP_ThirdPersonGameMode`, and so on.
Only the two build targets are named after the project (`VaCuusDemo` / `VaCuusDemoEditor`), since target names are not
referenced by content. Because nothing is renamed, the template's assets resolve natively and the project needs no
`CoreRedirects` at all. Recreate it by copying `Templates/TP_ThirdPerson` plus the `LevelPrototyping`, `Characters`,
and `Input` packs from `Templates/TemplateResources/High`.

Build the editor target:

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh \
    VaCuusDemoEditor Linux Development /w/Unreal/VaCuusDemo/VaCuusDemo.uproject -NoUBA
```

`-NoUBA` is required while the plugin is symlinked into the project: Unreal Build Accelerator's file detours treat
the symlinked path (`VaCuusDemo/Plugins/VaCuus/...`) and the real path (`VaCuus/...`) as two different files, and abort
compiling the plugin modules with `cross-process rename-while-open`. The demo project also disables UBA persistently in
`VaCuusDemo/Saved/UnrealBuildTool/BuildConfiguration.xml` (`bAllowUBAExecutor=false`), so a plain `Build.sh` works too —
but that file lives under `Saved/` and is not committed, so re-add it after a clean checkout.

Then launch the editor:

```bash
/w/Unreal/UnrealEngine/Engine/Binaries/Linux/UnrealEditor /w/Unreal/VaCuusDemo/VaCuusDemo.uproject
```

## License

Not decided yet.
