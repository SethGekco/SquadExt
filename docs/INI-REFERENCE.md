# SquadExt — INI reference (Phase 0/1)

A **squad** is an **anchor** (the tagged unit) plus the **members** it brings with
it. Tags go in the anchor's TechnoType section (`[E1]`, `[HTNK]`, `[GAPILE]`, …).

Nothing here costs anything at runtime unless you use it: a unit with no `Squad*`
tags is never flagged and never ticked.

---

## Quick start

```ini
[SREF]                      ; Prism Tank leads two GIs
Squad0.Members=E1
Squad0.Numbers=2
Squad0.MemberSelection=group
```

Build a Prism Tank → it arrives with two GIs, and clicking any of the three
selects all three.

---

## Composition

| Tag | Default | Meaning |
|---|---|---|
| `SquadN.Members` | — | Comma list of TechnoTypes. Repeats allowed (`E1,E1`). |
| `SquadN.Numbers` | `1` | Per-type counts, aligned 1:1 with `Members`. |

`Numbers` follows the shared list convention: a **scalar broadcasts**
(`Numbers=3` → 3 of every member), and a **short list pads with its last value**.

```ini
Squad0.Members=E1,IVAN
Squad0.Numbers=3,1          ; 3x E1 + 1x IVAN
```

Buildings can be **anchors** but never **members** (a building member is skipped
with a log line).

---

## Per-member cascade

Three layers that are **not** synonyms — they override in order:

```
SquadN.MemberK.*   (slot K override)
  > SquadN.Member.*  (entry-wide default)
    > compact Members/Numbers
      > engine default
```

```ini
Squad0.Members=E1,E1,IVAN
Squad0.Member.Veterancy=elite      ; whole squad elite …
Squad0.Member2.Veterancy=veteran   ; … except slot 2
```

| Tag | Meaning |
|---|---|
| `.Type` | TechnoType for this slot (expanded form; overrides `Members[K]`) |
| `.Count` | How many of this slot |
| `.Chance` | 0–100 roll **per unit** |
| `.Veterancy` | `none` / `rookie` / `veteran` / `elite` / `inherit` (copies the anchor's rank) |
| `.Health` | Percent of `Strength` to spawn at (1–100) |
| `.Facing` | 0–255 game facing |
| `.RequiredHouses` | Country whitelist **for this member only** |
| `.ForbiddenHouses` | Country blacklist for this member only |

The per-member house filter is the big one — it collapses what would otherwise be
several entries into one:

```ini
Squad0.Members=E1,IVAN
Squad0.Member1.RequiredHouses=Russia   ; everyone gets a GI; only Russia gets Ivan
```

---

## Eligibility

Evaluated against the **anchor's owner** when the squad forms.

```ini
Squad0.Prerequisite=GAPILE             ; list 0 — AND within a list
Squad0.Prerequisite.1=NAHAND,NAWEAP    ; OR-alternative: satisfy list 0 OR list 1
Squad0.Prerequisite.2=...              ; …read contiguously; first empty index ends it
Squad0.Prerequisite.Negative=GAPOWR    ; must NOT have any of these
Squad0.RequiredHouses=Russia,Iraq      ; country whitelist
Squad0.ForbiddenHouses=Americans       ; country blacklist
```

An entry with no gates at all is always eligible.

---

## Which entries fire

```ini
Squad.EntrySelect=first     ; first | all | random | weighted   (unit-level)
Squad0.Weight=3             ; for weighted
Squad0.Chance=50            ; 0-100 roll for the whole entry
```

- `first` *(default)* — the first eligible entry only. Order entries specific → general.
- `all` — every eligible entry fires.
- `random` — one eligible entry, uniform.
- `weighted` — one eligible entry, by `Weight`.

This is how one anchor fields different squads per country:

```ini
[CONS]
Squad0.Members=SEAL
Squad0.RequiredHouses=Americans
Squad1.Members=IVAN
Squad1.RequiredHouses=Russia
```

---

## Selection behaviour

```ini
Squad0.MemberSelection=independent   ; independent | group | anchor
```

| Value | Behaviour |
|---|---|
| `independent` *(default)* | Each unit selects alone. |
| `group` | Clicking any squad mate selects the whole squad. |
| `anchor` | Clicking a member selects its **anchor** instead (carrier-like). |

`SquadN.IsInitAsTeam=yes` from PR #1663 still parses and maps to `group`.

> **Known conflict:** this replaces the four `Select` vtable slots, which
> TechnoAttachmentExt also does for its `PassSelection`. If both DLLs are loaded,
> whichever loads last wins. Defaults are `independent`, so nothing changes
> unless you opt in.

---

## Role gate

```ini
Squad0.ActivateAs=any        ; any | member | independent
```

- `any` *(default)* — no condition. (`none` / `neither` are accepted synonyms.)
- `independent` — fires only when this unit is **not** itself somebody's member.
  Use it so a sergeant brings riflemen when built directly, but rides as a plain
  body when a captain brings *him*.
- `member` — fires only when this unit **is** already a member.

---

## Nesting and its guards

A member that itself carries `Squad*` tags brings its own roster — squads of
squads, chains, centipedes. This is deliberate and unbounded in config.

```ini
Squad.LoopLimit=5              ; generations deep; the 5th is sterile. 0 = unlimited (default)
Squad0.LoopLimit=3             ; per-entry override
Squad.MaxSpawnPerProduction=999 ; width cap: max members ONE unit's roster may spawn
```

`LoopLimit` is read from the **originating** unit and governs the whole chain — a
nested member's own `LoopLimit` is ignored. `LoopLimit=5` means
initial → 2nd → 3rd → 4th → 5th, with the 5th unable to reproduce.

Because each generation spawns on a **later frame**, a cyclic roster (`A→B→A`)
cannot freeze the match the way synchronous recursion would — it just keeps
producing units while the game stays responsive. `LoopLimit` is still the right
way to bound it deliberately.

---

## Determinism

Every `Chance` / `random` / `weighted` roll uses the game's **synced** scenario
RNG, so multiplayer stays in lockstep. Spawning happens inside game logic, not
render code.

---

## Not in this build

Deferred to later phases (documented in `DESIGN.md`): `SpawnEvent` (death /
deploy / promotion / timer triggers), member regen & `MaxActive`, Peer/Retinue
behaviour families, formations, `OrderEcho`, economy tags, `AnchorDeath.Behavior`.
