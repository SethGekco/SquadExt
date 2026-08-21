# SquadExt — Design

Standalone Syringe DLL (Antares/Phobos base) that lets a produced unit come out of
its factory together with a roster of other units, group them for selection, and
(eventually) lead them. Evolution of Phobos PR
[#1663](https://github.com/Phobos-developers/Phobos/pull/1663) (**closed, not merged**),
rebuilt around one primitive and a proper type layer.

Sits alongside — not fused with — [TechnoAttachmentExt] and the Kratos GiftBox/Host
extraction. See **Relationship to sibling DLLs** below.

---

## 1. What PR #1663 gave us, and what we keep

The PR is a ~356-line prototype. It is worth reading once, then treating as a
reference sketch rather than a base to extend.

Kept ideas:
- Co-spawn extra units when a unit is kicked out of a factory.
- A `SquadManager` that bags the resulting `TechnoClass*`s so selecting one selects all.
- Verified hook sites (below).

Discarded / rewritten:
- Flat `Squad.Members` / `Squad.IsInitAsTeam` directly on the TechnoType. Replaced by
  an **indexed `SquadN.*` roster** (§3).
- The `SquadManager` ownership model. It is broken and must be rebuilt (§5).
- Infantry-only, barracks-only production path. Generalized in Phase 2.

Verified hook sites from the PR (gamemd.exe):
- `0x444DDF` — `BuildingClass::KickOutUnit`, infantry exit (co-spawn point).
- `0x6FBFD0` — `TechnoClass::Select` (group-select fan-out).
- `0x737BBE` — `UnitClass::Unlimbo`, passenger squad (Ares initial-payload path).

---

## 2. The primitive: the Roster

One concept does all the front-end work: a **Roster** — an indexed list of entries,
each of which is a set of member types plus a house/prereq filter that decides whether
that entry is eligible for a given owner.

```
Roster
├─ entry 0 : { Members=[...], Prerequisite, RequiredHouses, ForbiddenHouses, MemberSelection }
├─ entry 1 : { ... }
└─ entry N : { ... }
```

At spawn time: walk the entries, pick the first whose filter the producing house
satisfies, spawn its Members near the **anchor**, optionally group them.

**Terminology.** The unit that owns a roster is the **anchor** — renameable, but chosen to
avoid rank words like *leader*/*officer*; the units it brings are **members**. A member can
itself be an anchor of its own roster (nesting, §3).

This is deliberately the **same shape** as `Attachment0..N` in TechnoAttachmentExt and
the GiftBox contents list. We do **not** share an implementation (see §7) — we reuse the
base engine's prerequisite/house checks and duplicate only the ~40 lines of "walk the
indexed list, pick an eligible entry" glue.

---

## 3. Tag schema (inline, indexed — matches the Attachment0 mental model)

On any anchor TechnoType (`[SOMEUNIT]`):

```ini
; --- roster entry 0 ---
Squad0.Members=E1,E1,DOG          ; list of TechnoTypes; repeats allowed (two E1)
Squad0.Prerequisite=GAPILE         ; standard prereq list (base-engine semantics)
Squad0.Prerequisite.1=             ; OR-group: satisfy Prerequisite= (list 0) OR this (see §9)
Squad0.RequiredHouses=             ; house/country whitelist for this entry
Squad0.ForbiddenHouses=            ; house/country blacklist for this entry
Squad0.MemberSelection=group       ; independent | group | anchor  (click behavior; see §9)

; --- roster entry 1: same anchor, different squad for a specific house ---
Squad1.Members=CONS,CONS
Squad1.RequiredHouses=Russia
Squad1.MemberSelection=group
```

**Semantics**
- Entries evaluated in ascending index order; **first eligible entry wins** (single
  squad per production). Rationale: predictable, and lets modders order specific→general.
- Eligibility = `Prerequisite` **and** `RequiredHouses` satisfied, and `ForbiddenHouses`
  not matched — all evaluated against the **producing building's owner** (full stack in §9).
- Empty `SquadN.Members` with a satisfied filter = "produce this anchor with no extras"
  (a deliberate no-op override for some houses).
- Filter keys reuse the **base engine's** prereq/house evaluation (§7); we do not define
  new prereq semantics.

Member entries are plain types in v1. Per-member count/veterancy/facing deferred to a
later phase if demanded.

**Nesting — squads of squads (by design).**
Spawning is **recursive**: a member type that itself carries `SquadN.*` tags brings its
own roster, which spawns in turn. This is intentional and deliberately unbounded in the
config — it enables chains / centipede-like constructs. If a modder wires something janky,
that's their call; we do not hardcode a nesting-depth or member-type boundary.

Two guards keep a cyclic roster (`A→B→A`, `A→A`) from becoming a non-terminating spawn
loop that freezes the match. They bound **different axes** and are both kept:

**`Squad.LoopLimit` (anchor tag — the modder's dial).** A *depth* budget: the number of
generations the recursion runs down each branch. Read from the **originating** produced
unit only, then carried down the recursion as state and decremented per generation —
nested members' own `LoopLimit` is ignored so the initial's value is authoritative.
Rule: a unit spawns its roster only while `depth < LoopLimit` (top unit = depth 1).

```ini
[ANCHOR]
Squad.LoopLimit=5   ; initial→2nd→3rd→4th→5th; the 5th is sterile. 0 = unlimited (default)
```

So `LoopLimit=5` yields five generations with the last unable to reproduce — exactly the
intended chain length. A cycle is just a branch that keeps going, so this bounds cycles too.

**`Squad.MaxSpawnPerProduction` (engine backstop — always on).** `LoopLimit` bounds depth
but **not width**: `LoopLimit=5` where each unit spawns 10 members is 10⁴ units — still an
explosion that hangs the match even though it terminates. So a single engine-wide cap on
the *total* units one production event may spawn (high default, e.g. 999) is the hard
safety net. Hit it → stop. Set it huge for a thousand-segment centipede; it exists only so
a forgotten or oversized `LoopLimit` with wide fan-out still can't freeze the game.

Neither bounds a feature — depth is the modder's to choose (`LoopLimit`), width degrades
gracefully instead of locking (`MaxSpawnPerProduction`).

Nesting + selection: when `MemberSelection=group` chains down the tree, nested members
**flatten into the top-level anchor's** selection group, so one click selects the whole chain (the
centipede case). Separate per-level sub-squads are a later opt-in if anyone wants them.
The PR's per-member `BuildLimit` check is kept and naturally throttles recursive fan-out.

**Member-side control — `SquadN.ActivateAs` (rank-free activation gate).**
`LoopLimit` is set on the anchor and governs the whole chain. This gate flips control to
the unit itself: a roster entry activates only when the unit is in the required role. It
supersedes the earlier `OfficerOnly` sketch and, per the terminology decision, uses no rank
words:

```ini
Squad0.ActivateAs=any        ; any | member | independent   (default any)
```
- **`any`** — no condition (the *none/neither* case). Always eligible.
- **`independent`** — fires only when the unit is *not* itself a squad member (built/exists
  standalone). Equivalent to the old `OfficerOnly=yes`.
- **`member`** — fires only when the unit *is* already a member of another unit's squad —
  e.g. a unit that grows an escort *only* while embedded in a larger formation.

Maps 1:1 to both sketch schemes: `member`↔`SquadMember=yes`, `independent`↔`SquadMember=no`,
`any`↔`none/neither`; and to `RoleRequirement` with *leader* renamed to *independent*.

Requires one per-instance flag, **`IsSquadMember`**, set true when a unit is spawned *as a
member* and serialized on the techno ext — one bool beside the PR's existing `HasSquad`.
Production-time, deterministic, no desync risk. Per-entry, so one unit can gate `Squad0`
and `Squad1` differently.

---

## 4. Behavior families: Peer vs. Retinue (Phase 3, the ambitious part)

Everything above is *composition* — who spawns. What the squad then *does* splits into two
genuinely different runtime engines that share the same roster front-end. Chosen per entry:

```ini
Squad0.Behavior=peer     ; peer | retinue   (default peer; inferred as retinue if any
                         ;                    Regen/Reload/Evasive tag is present)
```

**Peer family** — members are autonomous peers of the anchor: they move and fight as a
group, the anchor fights alongside them, and cohesion is a soft leash (below). This is the
PR's direction and the "charge in as a squad" case.

**Retinue family** — the anchor *owns* the members the way an aircraft carrier owns its
craft or a Slaver owns its slaves. **Key realization: the Reload/Regen/Evasive cluster is
the engine's Spawner (aircraft-carrier) mechanic generalized to any technotype.** Members
sortie to attack within range and return to the anchor to reload/regen; the anchor can be
evasive and never fight itself. Because this maps onto the existing `SpawnManager`
codepath, the Retinue family should **lean on / mirror that proven, already-deterministic
engine code** rather than hand-roll new AI — which both saves work and cuts desync risk.

### Tag groups

*Composition (both families)* — Slaver-style counts replace the repeat-list:
```ini
Squad0.Members=E1,IVAN        ; TechnoTypes
Squad0.Numbers=3,1            ; per-type counts → 3×E1 + 1×IVAN  (replaces E1,E1,E1)
```

*Retinue only (mirrors `Spawns=`/`SpawnRegenRate=`/`SpawnReloadRate=`)* —
```ini
Squad0.RegenRate=500          ; regrow lost members over time, up to Numbers (single, or per-type list)
Squad0.AnchorEvasive=no       ; yes → anchor avoids combat and sends members instead
Squad0.EvasiveRange=0         ; cells the anchor tries to keep from the enemy
```

*Cohesion / leash (both, flavored per family)* —
```ini
Squad0.StrayScan=0            ; how far a member may wander from the anchor
Squad0.Stance=Guard           ; Guard | Aggressive | …  members' area stance
Squad0.OverStrayBehavior=recall ; charge (anchor advances) | recall (members fall back) | none
```

Mixing families on one anchor is exactly the intended example: `Squad0.AnchorEvasive=yes` +
`Squad1.AnchorEvasive=no` → the anchor charges in with `Squad1` as peers, while `Squad0`
sorties as an evasive retinue and the anchor hangs back honoring `EvasiveRange` — both still
obeying their own `StrayScan`/`OverStrayBehavior`.

### Hard constraint (unchanged)

All of this is per-frame behavior inside **synced game logic** and must be fully
deterministic — the same desync failure class as the Kratos RNG work. Grouped *selection*
(Phases 0–1) is client-side and safe; *behavior* is not. The Retinue family's `SpawnManager`
reuse is the lowest-risk path; custom Peer formation AI is the highest. Buildings can be
anchors (Phase 2); buildings are never members.

---

## 5. Rebuilding SquadManager (Phase 0 hardening)

The PR's manager has three defects that must not be carried forward:

1. **Double ownership.** Ctor does `Array.emplace_back(this)` while callers do
   `new SquadManager` and keep a raw pointer; `Allocate()` would then register twice.
   → Fix: one allocation path. `Allocate()` returns the raw pointer; the vector owns it;
   callers never `new` directly. Ctor does **not** self-register.
2. **Empty `PointerGotInvalid`.** Dead members leave dangling `TechnoClass*` in the
   roster → crash. → Fix: wire into the base's `AnnounceInvalidPointer` dispatch so
   every manager scrubs the invalidated techno; auto-dissolve when ≤1 member remains.
3. **Half-finished swizzle.** `RegisterChange` is commented out in `Serialize`.
   → Fix: real save/load with pointer swizzle for `Squad_Members` and the anchor ref.

Manager holds generic `TechnoClass*`, so it already supports mixed-type squads for free
— the generalization cost in Phase 2 is entirely on the **spawn/placement** side, not
the grouping side.

---

## 6. Phase plan

| Phase | Scope | Size | Risk |
|---|---|---|---|
| **0 — Harden** | Rebuild `SquadManager` (§5). Ship infantry co-spawn + group-select done right (PR scope, correct). | S | Low |
| **1 — Roster** | Indexed `SquadN.*` lists + Prerequisite/RequiredHouses/ForbiddenHouses filtering via base helpers. One anchor, many house-specific squads. | M | Low |
| **2 — Any technotype** | Generalize anchor + member types across factory paths (barracks / war factory / helipad / building grand-opening). Safe cell placement for vehicle/aircraft members. Buildings as anchors only. | M | Med (placement/pathing) |
| **2.5 — Summon engine** | `SpawnEvent` (death/deploy/promotion/timer) + member regen (Count/RegenRate/MaxActive) + Ammo-budget summons. Gated behind tag presence → zero cost when unused (§9 Performance). | M | Med (event hooks + interval tick, no per-frame polling) |
| **3a — Retinue** | Spawner-style members (Numbers/Regen/Reload/Evasive) by mirroring the engine `SpawnManager`. | M–L | Med (reuses proven synced code) |
| **3b — Peer formation** | Custom group AI: follow, formation shape, StrayScan/OverStray/Stance, acquire-attack in range. | L | **High (desync)** |

Ship 0→1 first; they deliver most of the practical value (Bellum Æternum's use case is
essentially Phase 1, plus the upgrade/country-specific member case which is pure Phase 1
composition). Treat 2, 2.5, 3a, 3b as independent follow-ups; 3a is materially lower-risk
than 3b because it reuses `SpawnManager` rather than new AI, and 2.5 costs nothing at
runtime for units that don't use it.

---

## 7. Relationship to sibling DLLs (Attachment, GiftBox/Host)

**Decision: three independent DLLs. No shared primitive DLL/lib. Duplicate the thin glue.**

Why this is safe and correct:
- **Composition is through the game object, not the DLLs.** Each DLL reads its own
  ext-data off `TechnoClass`/`TechnoTypeClass`. A type that is simultaneously an
  Attachment, a GiftBox host, and a Squad anchor just works — each DLL processes it
  independently. Nothing needs fusing.
- **The dangerous shared code isn't ours.** Prerequisite/DynamicPrerequisite/
  RequiredHouses/ForbiddenHouses semantics live in the base engine (Antares/Phobos, the same
  checks buildability uses). All three DLLs *call* that — single source of truth for the
  player-facing meaning of those tags. Verify the exact base helper before Phase 1.
- **What's left to duplicate is ~40 lines** of "walk `FooN.*`, pick an eligible entry,
  spawn near a coord." Cheaper to copy than to couple three independently-shipped DLLs
  to a shared lib with its own versioning. Requirements will drift (GiftBox: scatter/
  velocity; Squad: rally point/formation; Attachment: anchor offset), so a single
  abstraction would leak. Extract later only if all three end up editing the same glue
  for the same reason.

**The only real cross-DLL concern is mechanical:**
- Hook-address collisions — if two DLLs `DEFINE_HOOK` the same site, be deliberate about
  return values / clobbered registers so they don't fight.
- Serialization ordering — no DLL may assume another's ext-data is restored first.

Neither is solved by merging; both are solved by coordination.

---

## 8. Open questions

- Rally-point handling for non-infantry members (PR sends members to `ArchiveTarget`;
  vehicles/aircraft may need staged pathing).
- **Resolved:** anchor-death handling is `SquadN.AnchorDeath.Behavior=` (§9) —
  Disband/Promote/Kill/Vanish/Sell/Neutral/ToKiller, mirroring Phobos `AutoDeath.Behavior`.
- **Resolved:** inline `SquadN.*` only; no named `[SquadType]` sections. Nesting (a member
  type carrying its own roster) gives roster reuse for free, so named types aren't needed.
- Nested selection defaults to **flatten into the top group**; per-level sub-squads are a
  future opt-in. Confirm this default survives contact with the centipede/formation work.
- Tune defaults for `Squad.LoopLimit` (0/unlimited) and `Squad.MaxSpawnPerProduction`
  (engine backstop) once Phase 2 exists and real fan-out is measurable.
- Interaction with Ares Unit Delivery SW / map triggers (PR explicitly excludes these).
- **`Squad.MemberOnly=`** (separate from `ActivateAs`): a unit that may *only* exist as a
  squad member and can't be built standalone — a buildability/sidebar restriction, not
  recursion control. Keep the two tags distinct so they aren't conflated. Add if wanted.
- Confirm final term for the roster-owner (**anchor** placeholder) and the `ActivateAs`
  value words (`any`/`member`/`independent`).
- `Squad.Behavior` explicit tag vs. pure inference from which tags are present (default:
  support both — explicit wins, else infer retinue when any Regen/Reload/Evasive tag exists).
- `RegenRate`/`ReloadRate` shape: single value vs. per-type list — likely Regen per-type
  list, Reload single (matches Slave semantics). Decide when 3a is built.

### Other mechanics worth folding in (brainstorm)

- **Formation shapes** — `Squad0.Formation=column|wedge|line|box` + spacing. `column` is
  the centipede; gives the "moves like a chain" visual the recursion alone doesn't.
- **Anchor succession** — on anchor death, promote a member to anchor vs. just dissolve
  (already an open question; Retinue family may prefer dissolve/vanish).
- **Veterancy / kill-credit routing** — members inherit the anchor's rank, or the squad
  shares a veterancy pool; kills credit the anchor. Ties into [[payloadext-project]]'s
  veterancy-index idea.
- **Death coupling** — anchor→members handled by `AnchorDeath.Behavior` (§9, resolved). The
  reverse, `AnchorDiesWithLastMember=yes`, is still open.
- **Focus fire** — members prioritize the anchor's current target.
- **Weighted random entry** — `Squad0.Chance=` for random roster selection instead of
  strict first-match; ties into [[traitext-project]]'s random primitive.
- **Transport interplay** — a squad boards a transport as one unit; ties into
  [[payloadext-project]]'s Bay.

---

## 9. Flexibility levers (refined)

Flexibility comes from a few **orthogonal mechanisms**, not a flat tag pile. Two selection
axes were conflated earlier and are now split: **entry selection** (which roster entry
spawns) vs. **member selection** (in-game click behavior).

### Lever 1 — per-member sub-blocks, with a cascade
Three layers, **not synonyms** — they override in order:
`MemberK.*` (slot K) > `Member.*` (entry-wide default) > compact `Members`/`Numbers` > engine default.
```ini
Squad0.Members=E1,IVAN            ; compact form (ergonomic)
Squad0.Numbers=3,1
Squad0.Member.Veterancy=elite     ; ENTRY DEFAULT — applies to every slot
Squad0.Member2.Veterancy=veteran  ; SLOT OVERRIDE — demotes just slot 2
Squad0.Member0.Type=E1            ; full per-slot control
Squad0.Member0.Count=3            ; target alive count for this slot (drives regen)
Squad0.Member0.Health=50%
Squad0.Member0.Facing=64
Squad0.Member0.Offset=-1,0        ; formation cell offset from anchor
Squad0.Member0.RegenRate=500
Squad0.Member0.RequiredHouses=Russia   ; per-member house filter — one entry, IVAN only for Russia
Squad0.Member0.ForbiddenHouses=
Squad0.Member0.Prerequisite=           ; per-member prereq gate
```

### Lever 2 — entry selection `Squad.EntrySelect` (which entry spawns)
```ini
Squad.EntrySelect=first  ; first | all | random | weighted   (default first)
Squad0.Weight=3          ; random/weighted; ties into TraitExt random
```
`all` = field several squads at once; `weighted`/`random` = randomized loadouts.

### Lever 3 — member selection `SquadN.MemberSelection` (in-game click behavior)
Folds in the old `IsInitAsTeam`.
```ini
Squad0.MemberSelection=independent   ; independent | group | anchor
```
- `independent` — each unit selectable alone (peer default).
- `group` — click one → select all (the old `IsInitAsTeam`).
- `anchor` — click any member → **selects the anchor instead** (carrier-like; retinue default).

### Lever 4 — entry inheritance (DRY, no named types)
```ini
Squad1.InheritFrom=0     ; copy entry 0's tags, then override below
Squad1.RequiredHouses=Russia
```

### Lever 5 — trigger events `SpawnEvent` (general summon engine)
```ini
Squad0.SpawnEvent=produced   ; produced | deploy | death | promotion | timer | manual  (list)
Squad0.SpawnEvent.Interval=225 ; cadence for 'timer' (walking barracks) — this is elapsed-time
Squad0.SpawnEvent.Chance=    ; roll before spawning
Squad0.SpawnEvent.Delay=30   ; event-level: wait N ticks after the trigger, THEN spawn (e.g. delayed deathsquad)
```
`timer` = elapsed-time trigger (this is what "periodic" meant); `death` = anchor dies
(deathsquad); `deploy` = deploy-to-summon; `promotion` = veterancy reinforcements.
**Default (no `SpawnEvent` tag): spawn once with the anchor at production, and register no
runtime checks** — see Performance below.

### Member replacement / regen (distinct from SpawnEvent)
Two member-level delays, separate from `SpawnEvent.Delay`:
```ini
Squad0.RegenRate=500              ; ticks to REPLACE a lost member (per-member overridable); 0 = no regen
Squad0.RegenDelay=               ; optional extra initial delay before first replacement (default = RegenRate)
Squad0.RegenRequiresEligible=yes ; regen re-checks Prerequisite/RequiredHouses — lose the prereq, regen halts
Squad0.MaxActive=8               ; HARD cap on total alive members from this anchor (across slots/entries)
```
- **Maintain mode** (default when `RegenRate` set): keep each slot at its `Count`, replacing
  deaths after `RegenRate`. Steady-state retinue that heals losses.
- **Accumulate mode** (`SpawnEvent=timer`): add new members each `Interval` up to `MaxActive`
  — an army that grows to a ceiling.
- `MaxActive` bounds both, so neither floods the board.

### Ammo integration (reuse the vanilla Ammo/Reload subsystem)
```ini
Squad0.SpawnUsesAmmo=no       ; yes → each member spawn/regen consumes the anchor's Ammo
Squad0.SpawnAmmoCost=1        ; ammo per member; the anchor's normal Reload refills the summon budget
```
`SpawnUsesAmmo` turns the anchor's `Ammo=N` into a limited-charge summon budget (a carrier
with N craft out), driven entirely by the engine's already-deterministic Ammo/Reload code —
the same "lean on proven engine systems" trick as the `SpawnManager` reuse. **Mechanically
uncertain — cross that bridge when we reach it; worst case the idea is dropped.**

> **Back burner (deferred, not in scope now):** member self-reload at the anchor —
> `ReloadRate` / `ReloadBehavior=none|vanish|idle` / `ReloadRange`. Documented so it isn't
> lost; revisit when the Retinue family is actually built.

### Economy & Grinder
```ini
Squad0.Cost=                  ; extra credits when the squad forms (0 = free members)
Squad0.Refund=                ; % back when the ANCHOR is sold/dies
Squad0.MemberValue=inherit    ; inherit | zero — 'zero' makes members worthless to Grinder/Soylent/refund
Squad0.OrderEcho=move         ; none | move | all — members mirror the anchor's orders, copied at
                              ; command-time so they keep executing even after the anchor dies
```
- `Refund` (anchor sold/dies) and the **Grinder** (pays a unit's own Soylent value on
  consumption) are **orthogonal systems**. Free-but-real members + Grinder = infinite-money
  exploit → `MemberValue=zero` closes it.
- `OrderEcho` enables "order the whole squad by ordering the anchor," incl. marching the
  entire squad into a Grinder that survives the anchor's death.

### Anchor-death behavior — what happens to members when the anchor dies
Per-entry (with a unit-level `Squad.AnchorDeath.Behavior=` default entries inherit), so one
anchor can vanish `Squad0` while `Squad1` goes independent. Values **mirror Phobos
`AutoDeath.Behavior`** where they overlap, so the vocabulary is already familiar.
```ini
Squad0.AnchorDeath.Behavior=Disband  ; Disband | Promote | Kill | Vanish | Sell | Neutral | ToKiller
Squad0.AnchorDeath.Delay=0           ; ticks members linger before the behavior fires (last-stand / grace)
Squad0.AnchorDeath.Heir=             ; for Promote: which member becomes anchor (else first survivor)
```
- **`Disband`** *(default)* — members survive, ungroup, same owner; dependent (retinue)
  members gain autonomy. (Merges the sketch's *None* + *Independent* — one outcome.)
- **`Promote`** — a surviving member becomes the new anchor; the rest re-parent under it
  (the complex one; `Heir=` picks who, else first survivor).
- **`Kill`** — members die: counts as a loss, fires death weapons/anims. (Phobos parlance.)
- **`Vanish`** — members disappear: no loss, no death effects. (Phobos parlance.)
- **`Sell`** — members refunded as if sold; ties into `Refund`/`MemberValue`. (Phobos parlance.)
- **`Neutral`** — members transfer to the neutral/civilian house.
- **`ToKiller`** — members transfer to the house that killed the anchor (falls back to
  `Neutral` if there was no killer — cliff, self-destruct, etc.).

Subsumes the earlier `MembersDieWithAnchor` brainstorm (that's just `Kill`/`Vanish`) and
resolves the open question of promote-vs-ungroup. Runs off the anchor's death hook — an event,
not a poll, so it honors the pay-for-what-you-use rule.

### Other tuning tags
```ini
Squad0.TechLevel= / Squad0.RequiredHealth=       ; more eligibility gates
Squad0.TargetFilter= / Squad0.Scatter= / Squad0.GuardRange= / Squad0.Aggression=   ; behavior
Squad0.Formation=column / Squad0.Formation.Spacing=1   ; none|line|column|wedge|box|custom (custom → Offset)
```

### Full eligibility stack (per-entry; key ones per-member)
```ini
Squad0.RequiredHouses=Russia,Iraq
Squad0.ForbiddenHouses=
Squad0.Prerequisite=GAPILE            ; list 0 — AND within one list
Squad0.Prerequisite.1=NAWEAP,NARADR   ; OR-groups (dotted numeric): satisfy list 0 OR list 1
Squad0.Prerequisite.2=                 ; …OR list 2, etc.
Squad0.Prerequisite.Negative=         ; must NOT have
```
- `Owner` is dropped entirely — `RequiredHouses`/`ForbiddenHouses` are the single house/country
  filter vocabulary at every level. Reuses the base engine's prereq/house checks (§7).
- OR-group syntax is **dotted numeric** (`Prerequisite.1`), not `$` — the bare
  `Prerequisite=` is list 0; satisfy any list.
- Prereqs are checked against the **producing house**. The old Ares
  `Prerequisite.RequiredHouses` (which houses' buildings *count* toward the prereq: self/ally)
  is deliberately dropped as a rare case; if ever needed it's a separate future tag, not this.

### Performance — pay-for-what-you-use (first-class rule)
- Composition/filter-only units (the common case: upgrades, country-specific members) do
  their work **once at production and then cost zero** — no update tick, no polling.
- An update tick is registered **only** for `SpawnEvent=timer` or regen, and ticks on its
  `Interval`, not every frame.
- Event triggers (`death`/`deploy`/`promotion`) hang off existing engine hooks — no polling.

### Foundational — list conventions (define once, all list tags obey)
- Scalar broadcasts (`Numbers=3` → all); list aligns 1:1 with `Members`.
- Short list pads with last value; `*`/`-` = keep default / skip slot.
- Range `2-4` = random count (deterministic RNG).
- **Keep these conventions byte-identical across Squad / Attachment / GiftBox** even though
  code is duplicated (§7): duplicated code is fine, divergent modder syntax is not.

### Resolved this pass
- `EntrySelect` default `first`; ship compact + expanded (cascading, not synonymous).
- `Owner` → `RequiredHouses`/`ForbiddenHouses` everywhere. `SpawnOn` → `SpawnEvent`;
  `periodic` → `timer`. `IsInitAsTeam` → `MemberSelection=group`. Added `MemberSelection=anchor`.
- `SpawnEvent` gated behind tag presence so the default path has zero runtime cost.
- Prereq OR-groups: dotted numeric `Prerequisite.1` (not `$`). `Prerequisite.RequiredHouses`
  dropped/merged — prereqs check the producing house; `RequiredHouses` is the sole house filter.
- Anchor-death handled by `AnchorDeath.Behavior` (Phobos-aligned values); subsumes
  `MembersDieWithAnchor`.
- Member self-reload (`ReloadRate`/`ReloadBehavior`/`ReloadRange`) → back burner, deferred.

### Still open
- Whether `SpawnEvent=timer`/regen behavior is its own phase (2.5) between composition and
  full Peer/Retinue AI. Likely yes — it's the summon engine, lighter than formation AI.
- `RegenRate` list vs scalar shape (per list conventions above).
- `AnchorDeath.Behavior=ToKiller` needs a defined "killer" source + no-killer fallback (→ `Neutral`).

[TechnoAttachmentExt]: ../TechnoAttachmentExt
