# SquadExt

A standalone [Syringe](https://github.com/Ares-Developers/Syringe) DLL for
Yuri's Revenge that lets a produced unit arrive with a roster of other units —
and group, filter, and nest those squads.

Evolves [Phobos PR #1663](https://github.com/Phobos-developers/Phobos/pull/1663)
("Infantry Squad Production", closed/unmerged) into a general, technotype-agnostic
system built around one primitive: the **Roster**.

Runs alongside Phobos/Ares/Antares — it claims no extension pointer slot and all
of its breakpoints are Syringe-chainable. See `docs/INI-REFERENCE.md` for tags
and `DESIGN.md` for the full design and roadmap.

## What works today (Phase 0/1)

- **Any technotype as anchor** — infantry, vehicles, aircraft, buildings.
  (Buildings can lead; they are never members.)
- **Indexed rosters** `Squad0.*`, `Squad1.*`, … so one unit fields different
  squads per country, prerequisite, or roll.
- **Per-member cascade** — `MemberK.*` overrides `Member.*` overrides the compact
  `Members`/`Numbers` form, incl. per-member veterancy, health, facing and
  house filters.
- **Full eligibility stack** — `Prerequisite` with numbered OR-alternatives,
  `Prerequisite.Negative`, `RequiredHouses`, `ForbiddenHouses`.
- **Entry selection** — `first` / `all` / `random` / `weighted`.
- **Selection behaviour** — `independent` / `group` / `anchor`.
- **Role gating** — `ActivateAs=any|member|independent`.
- **Nesting** with `LoopLimit` (depth) and `MaxSpawnPerProduction` (width).
- Deterministic (synced RNG), save/load safe, crash-safe pointer invalidation.

## Building

CI (`.github/workflows/build.yml`) builds `DevBuild|x86` on `windows-latest`.
Clone with submodules:

```
git clone --recursive https://github.com/SethGekco/SquadExt.git
```

## Credits

Squad production concept from Phobos PR #1663. Built against
[YRpp](https://github.com/Phobos-developers/YRpp) and
[Phobos](https://github.com/Phobos-developers/Phobos) utilities.
