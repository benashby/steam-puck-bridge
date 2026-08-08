<!--
Thanks for the patch. Keep it to one logical change; see CONTRIBUTING.md.
-->

## What this changes

<!-- One or two sentences. Link the issue if there is one: Fixes #123 -->

## Why

<!-- What was broken or missing. For protocol changes, cite where the constant
     or offset came from: the SDL3 symbol name, or your own --dump capture. -->

## Tested on

- **Hardware:** <!-- e.g. 28de:1304 Proteus dongle -->
- **Distro / kernel:** <!-- e.g. Bazzite 42, 6.16.3-201.fc42.x86_64 -->
- **How:** <!-- what you actually exercised — buttons, rumble, hotplug, Steam handoff -->

## Checklist

- [ ] `make check` passes (gcc and clang, `-Werror`)
- [ ] No new dependencies beyond libc
- [ ] `docs/protocol.md` updated if wire-format constants changed
- [ ] `CHANGELOG.md` `[Unreleased]` entry added for user-visible changes
- [ ] I agree to license this contribution under the MIT License
