# espaudiocore local patch

`espaudiocore` is a separate repository, included here as a submodule pinned to
commit `e3965a6` (`feat(audio_source_fs): add POSIX file source mode`).

The firmware in this repository was developed against a working tree that also
carries a few uncommitted espaudiocore changes. They are intentionally kept out
of the espaudiocore repository and preserved here as a patch:

```powershell
git submodule update --init espaudiocore
git -C espaudiocore apply ..\patches\espaudiocore-local-changes.patch
```

The patch adds an `espaudiocore_probe_file()` metadata probe used by the
storage/library screens.
