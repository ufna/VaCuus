# The host project, the two working trees, and what removes the duplication

Repo-internal. This page does not ship in the plugin package (`Config/FilterPlugin.ini`
includes `docs/buyer/...` and nothing else under `docs/`), which is why it may name
machine paths — a buyer's README must not.

It used to live in `README.md`, where it was the bulk of the page and where a Fab buyer
would have read absolute paths from the author's dev box.

## Building and testing

Development and testing happen in a separate host project that is **not** part of this
repository, so the repo stays a clean plugin folder. On this machine that host is
`/w/Unreal/VcHost`, and the plugin reaches it as a **git clone** at
`VcHost/Plugins/VaCuus` whose `origin` is the canonical checkout `/w/Unreal/VaCuus`.

Build the editor target:

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh \
    VcHostEditor Linux Development -project=/w/Unreal/VcHost/VcHost.uproject
```

Run the automation suite (read the results from `VcHost/Saved/Logs/VcHost.log`; stdout is
unreliable at the end of a run, and note that `-ExecCmds` splits on **commas**, not
semicolons):

```bash
/w/Unreal/UnrealEngine/Engine/Binaries/Linux/UnrealEditor-Cmd /w/Unreal/VcHost/VcHost.uproject \
    -ExecCmds="Automation RunTests VaCuus, Quit" -unattended -nullrhi -nosplash
```

No editor may be running while you build — it holds the module `.so` files open.

`CLAUDE.md` carries the rest of the dev-loop hazards; this page is the part about the
tree layout itself.

## The two-working-trees hazard

**Read this before concluding that live reload is broken.** The canonical checkout and the
copy the editor loads are two separate directories, so they are two sets of **inodes** —
and `inotify` watches inodes, not paths. A watch registered on
`VcHost/Plugins/VaCuus/Content/DevUI` therefore cannot see an edit made to
`/w/Unreal/VaCuus/Content/DevUI/…`, and the failure is completely silent: no event, no
reload, no log line, not even `reloaded 0 view(s)`.

Two things now make it loud rather than silent, both at editor startup in `LogVaCuus`:

* the watcher's startup line names the watched roots **and states the consequence**;
* if the plugin directory is a git checkout whose remote is a local path that also has a
  `Content/DevUI`, a warning names that second tree explicitly.

The second check reads a fact off disk (`.git/config`), so it cannot see two clones that
both point upstream, or a `cp -r` copy. If live reload does nothing, grep the log for
`Live reload watching` and confirm that the file you saved is under one of the roots it
names.

## Arrangements that do and do not remove the duplication

Measured on UE 5.8.1 at `/w/Unreal/UnrealEngine`, 2026-07-30:

| Arrangement | Result |
| --- | --- |
| **Clone into `Plugins/`** (current) | Builds with UBA. Two inode sets — the hazard above. |
| **Symlink into `Plugins/`** | **Build fails.** UBA's file detours track the symlinked path and the real path as two different files and abort renaming `.o.tmp`: `ASSERT: rename: cross-process rename-while-open … UbaDetoursSharedPosix.inl:1158`. Only the plugin's own modules fail. `-NoUBA`, or `bAllowUBAExecutor=false` in `<Project>/Saved/UnrealBuildTool/BuildConfiguration.xml` (the only project-scoped config UBT reads — `UnrealBuildTool/Configuration/Xml/XmlConfig.cs:123-133`, and it is under `Saved/`, so a `.gitignore` rule must whitelist it), builds fine — so this arrangement costs UBA but does remove the duplication. |
| **`git worktree`** | No help. A linked worktree is a separate checkout with its own files, so it is still two inode sets. It fixes divergence in git, not live reload. |
| **Hard-linked `Content/`** | No help, and worse than it looks. Editors save by write-to-temp-then-rename — the shape the watcher's own filter is built around — which replaces the directory entry and breaks the link, so the two copies silently diverge after the first save. `git checkout` does the same. |
| **`AdditionalPluginDirectories` in the `.uproject`** | **Works, with UBA on.** Verified end to end: full plugin rebuild, plugin mounted, `IPlugin::GetContentDir()` and the watcher both resolved to the external tree, whole automation suite green. This removes the duplication entirely — there is only one checkout and no symlink. |

If you take the last option, note the trap: the entry must name a **parent** of the plugin
directory, not the plugin directory itself. UBT's `EnumeratePlugins` only looks for
`.uplugin` files inside *subfolders* of the search root
(`EpicGames.Build/System/EnumeratePlugins.cs:81-107`, and its doc comment says so), while
the runtime `FPluginManager::FindPluginsInDirectory` seeds its queue with the search root
itself (`Runtime/Projects/Private/PluginManager.cpp:1177-1203`). Point it straight at the
plugin and you get the worst outcome of the two: the editor mounts the plugin and UBT
never builds its modules.
