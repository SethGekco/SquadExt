#include "Body.h"

#include <FootClass.h>
#include <BuildingTypeClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <CellClass.h>
#include <ScenarioClass.h>
#include <RulesClass.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>
#include <Helpers/Cast.h>

#include <unordered_set>

#include <Ext/TechnoType/Body.h>

TechnoExt::ExtContainer TechnoExt::ExtMap;

// Bounds for walking a nested squad tree during selection. A cyclic roster is
// already prevented from hanging by the visited set; these just cap the work.
static constexpr int MaxSquadTreeDepth = 64;
static constexpr size_t MaxSquadTreeNodes = 256;

namespace
{
	// Deterministic roll against the SYNCED scenario RNG. Using rand() or any
	// unsynced source here would desync online play -- the exact failure class
	// that bit the Kratos work.
	bool RollPercent(int chance)
	{
		if (chance >= 100)
			return true;
		if (chance <= 0)
			return false;
		return ScenarioClass::Instance->Random.RandomRanged(1, 100) <= chance;
	}

	// Find a free cell near the anchor suitable for a member of this type.
	// Returns false if the type cannot be a member or no cell is available.
	bool FindMemberCell(TechnoTypeClass* pType, TechnoClass* pAnchor, CoordStruct& outCoords)
	{
		if (!pType || !pAnchor)
			return false;

		// Buildings are valid anchors but never members -- placing one would need
		// full base-layout validation and is out of scope by design.
		if (pType->WhatAmI() == AbstractType::BuildingType)
		{
			Debug::Log("[SquadExt] %s is a BuildingType and cannot be a squad member.\n", pType->ID);
			return false;
		}

		auto speedType = pType->SpeedType;
		auto movementZone = pType->MovementZone;

		// Aircraft have no ground movement zone; place them as if on tracks.
		if (pType->WhatAmI() == AbstractType::AircraftType)
		{
			speedType = SpeedType::Track;
			movementZone = MovementZone::Normal;
		}

		CellStruct const anchorCell = pAnchor->GetMapCoords();
		CellStruct const placeCell = MapClass::Instance.NearByLocation(
			anchorCell, speedType, -1, movementZone, false, 1, 1,
			true, false, false, false, CellStruct::Empty, false, false);

		if (placeCell == CellStruct::Empty)
		{
			Debug::Log("[SquadExt] No free cell near %s for member %s.\n",
				pAnchor->GetTechnoType()->ID, pType->ID);
			return false;
		}

		if (!MapClass::Instance.TryGetCellAt(placeCell))
			return false;

		outCoords = CellClass::Cell2Coord(placeCell);
		return true;
	}

	void ApplyVeterancy(TechnoClass* pMember, TechnoClass* pAnchor, SquadVeterancy vet)
	{
		switch (vet)
		{
		case SquadVeterancy::Rookie:
			pMember->Veterancy.Veterancy = 0.0f;
			break;
		case SquadVeterancy::Veteran:
			pMember->Veterancy.Veterancy = 1.0f;
			break;
		case SquadVeterancy::Elite:
			pMember->Veterancy.Veterancy = 2.0f;
			break;
		case SquadVeterancy::Inherit:
			if (pAnchor)
				pMember->Veterancy.Veterancy = pAnchor->Veterancy.Veterancy;
			break;
		case SquadVeterancy::None:
		default:
			break;
		}
	}

	// Give the member a standing order so a squad with no follow behaviour still
	// does something instead of standing inert.
	void ApplyStance(TechnoClass* pMember, SquadStance stance)
	{
		Mission mission;
		switch (stance)
		{
		case SquadStance::Guard:     mission = Mission::Guard;      break;
		case SquadStance::AreaGuard: mission = Mission::Area_Guard; break;
		case SquadStance::Sticky:    mission = Mission::Sticky;     break;
		case SquadStance::Hunt:      mission = Mission::Hunt;       break;
		case SquadStance::None:
		default:
			return; // leave the engine default untouched
		}

		// Virtual calls -- QueueMission is an R0 stub in YRpp, so a qualified
		// call would silently no-op (same footgun as Select / Unlimbo).
		pMember->QueueMission(mission, false);
	}

	const char* SpawnEventName(int flag)
	{
		switch (flag)
		{
		case SquadEvent_Produced: return "produced";
		case SquadEvent_Death:    return "death";
		case SquadEvent_Timer:    return "timer";
		default:                  return "?";
		}
	}

	void ApplyHealth(TechnoClass* pMember, int percent)
	{
		if (percent >= 100 || percent <= 0)
			return;

		int const max = pMember->GetTechnoType()->Strength;
		int hp = (max * percent) / 100;
		if (hp < 1)
			hp = 1;
		pMember->Health = hp;
	}
}

// ============================================================================
// Spawn trigger
//
// Two-stage on purpose. At Unlimbo entry the anchor is not on the map yet, so
// there is no valid position to place members around; we only raise a flag
// there and do the work on the next AI tick.
//
// Deferring also removes the freeze risk entirely: a cyclic roster can no
// longer recurse unbounded inside one call stack. Each generation spawns on a
// later frame, so a runaway config produces a lot of units over time but the
// game stays responsive and interruptible.
// ============================================================================

void TechnoExt::FlagForSquadSpawn(TechnoClass* pTechno)
{
	if (!pTechno)
		return;

	auto const pType = pTechno->GetTechnoType();
	if (!pType)
		return;

	auto const pExt = TechnoExt::ExtMap.Find(pTechno);
	if (!pExt || pExt->SquadsSpawned || pExt->PendingSquadSpawn)
		return;

	// Pay-for-what-you-use: units with no roster never get flagged, so the
	// per-frame check below stays a single already-false bool test for them.
	auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
	if (!pTypeExt || !pTypeExt->HasSquads())
		return;

	pExt->PendingSquadSpawn = true;
}

// Core spawn pass, shared by every trigger.
//
//   eventFlag    -- only entries whose SpawnEvent list contains this flag fire.
//   linkToAnchor -- false for deathsquads: the anchor is being destroyed, so
//                   linking members to it would create an instantly-dangling
//                   parent and a squad with no leader. They spawn independent.
void TechnoExt::SpawnEntriesForEvent(TechnoClass* pAnchor, int eventFlag, bool linkToAnchor)
{
	if (!pAnchor)
		return;

	auto const pExt = TechnoExt::ExtMap.Find(pAnchor);
	auto const pType = pAnchor->GetTechnoType();
	auto const pTypeExt = pType ? TechnoTypeExt::ExtMap.Find(pType) : nullptr;
	if (!pExt || !pTypeExt || !pTypeExt->HasSquads())
		return;

	// Generation of this unit; 0 and 1 both mean top-level.
	int const myGen = pExt->SquadDepth > 0 ? pExt->SquadDepth : 1;

	// LoopLimit comes from the originating unit and governs the whole chain.
	int const loopLimit = pExt->SquadLoopLimit >= 0
		? pExt->SquadLoopLimit
		: pTypeExt->LoopLimit;

	// A unit at generation G spawns only while G < LoopLimit, so LoopLimit=5
	// yields five generations with the fifth sterile. 0 = unlimited.
	if (loopLimit > 0 && myGen >= loopLimit)
	{
		// Logged because "sterile generation" and "roster never fired" are
		// indistinguishable in game -- in both cases nothing appears.
		Debug::Log("[SquadExt] %s: %s suppressed, generation %d has reached LoopLimit %d (sterile).\n",
			pType->ID, SpawnEventName(eventFlag), myGen, loopLimit);
		return;
	}

	auto const pOwner = pAnchor->Owner;

	// ---- pick which roster entries fire ----
	std::vector<const SquadEntryData*> eligible;
	for (auto const& entry : pTypeExt->SquadData)
	{
		// Only entries that answer to THIS trigger.
		if (!(entry.SpawnEvents & eventFlag))
			continue;

		// Per-firing roll, separate from the entry's own Chance so a repeating
		// timer entry can be "usually fires" without affecting its one-off use.
		if (!RollPercent(entry.SpawnEventChance))
			continue;

		switch (entry.ActivateAs)
		{
		case SquadActivateAs::Member:
			if (!pExt->IsSquadMember) continue;
			break;
		case SquadActivateAs::Independent:
			if (pExt->IsSquadMember) continue;
			break;
		case SquadActivateAs::Any:
		default:
			break;
		}

		if (!entry.EligibleFor(pOwner))
			continue;

		if (!RollPercent(entry.Chance))
			continue;

		eligible.push_back(&entry);
	}

	if (eligible.empty())
		return;

	std::vector<const SquadEntryData*> chosen;
	switch (pTypeExt->EntrySelect)
	{
	case SquadEntrySelect::All:
		chosen = eligible;
		break;

	case SquadEntrySelect::Random:
		chosen.push_back(eligible[ScenarioClass::Instance->Random.RandomRanged(
			0, static_cast<int>(eligible.size()) - 1)]);
		break;

	case SquadEntrySelect::Weighted:
	{
		int total = 0;
		for (auto const pEntry : eligible)
			total += (pEntry->Weight > 0 ? pEntry->Weight : 0);

		if (total <= 0)
		{
			chosen.push_back(eligible.front());
			break;
		}

		int roll = ScenarioClass::Instance->Random.RandomRanged(1, total);
		for (auto const pEntry : eligible)
		{
			roll -= (pEntry->Weight > 0 ? pEntry->Weight : 0);
			if (roll <= 0) { chosen.push_back(pEntry); break; }
		}
		if (chosen.empty())
			chosen.push_back(eligible.back());
		break;
	}

	case SquadEntrySelect::First:
	default:
		chosen.push_back(eligible.front());
		break;
	}

	// ---- spawn ----
	int budget = pTypeExt->MaxSpawnPerProduction;
	int spawned = 0;

	for (auto const pEntry : chosen)
	{
		// Per-entry LoopLimit override, else the inherited chain value.
		int const childLimit = pEntry->LoopLimit.isset()
			? pEntry->LoopLimit.Get()
			: loopLimit;

		size_t const slots = pEntry->SlotCount();

		for (size_t k = 0; k < slots; ++k)
		{
			auto const pMemberType = pEntry->GetSlotType(k);
			if (!pMemberType)
				continue;

			if (!pEntry->SlotAllowedFor(k, pOwner))
				continue;

			int const count = pEntry->GetSlotCount(k);
			int const chance = pEntry->GetSlotChance(k);
			auto const vet = pEntry->GetSlotVeterancy(k);
			int const health = pEntry->GetSlotHealth(k);
			int const facing = pEntry->GetSlotFacing(k);

			for (int n = 0; n < count; ++n)
			{
				// Width cap for this unit's own roster.
				if (budget <= 0)
				{
					Debug::Log("[SquadExt] %s hit Squad.MaxSpawnPerProduction; stopping.\n",
						pType->ID);
					return;
				}

				if (!RollPercent(chance))
					continue;

				CoordStruct coords {};
				if (!FindMemberCell(pMemberType, pAnchor, coords))
					continue;

				--budget;

				auto const pMember = abstract_cast<TechnoClass*>(pMemberType->CreateObject(pOwner));
				if (!pMember)
				{
					Debug::Log("[SquadExt] CreateObject failed for member %s.\n", pMemberType->ID);
					continue;
				}

				// Link member -> anchor BEFORE placing it. Unlimbo raises the
				// member's own pending-spawn flag, and the nested pass that
				// follows must already see the correct IsSquadMember state and
				// generation for ActivateAs / LoopLimit to resolve right.
				if (auto const pMemberExt = linkToAnchor ? TechnoExt::ExtMap.Find(pMember) : nullptr)
				{
					pMemberExt->IsSquadMember = true;
					pMemberExt->SquadAnchor = pAnchor;
					pMemberExt->MemberSelection = pEntry->MemberSelection;
					pMemberExt->SquadDepth = myGen + 1;
					pMemberExt->SquadLoopLimit = childLimit;
				}

				DirType const dir = facing >= 0
					? static_cast<DirType>(static_cast<unsigned char>(facing & 0xFF))
					: DirType::North;

				// Virtual call -- reaches the game's real Unlimbo. A *qualified*
				// call (pMember->ObjectClass::Unlimbo) would bind to YRpp's R0
				// stub and silently no-op: the same footgun as ObjectClass::Select.
				if (!pMember->Unlimbo(coords, dir))
				{
					Debug::Log("[SquadExt] Unlimbo failed for member %s.\n", pMemberType->ID);
					pMember->UnInit();
					continue;
				}

				ApplyVeterancy(pMember, pAnchor, vet);
				ApplyHealth(pMember, health);
				ApplyStance(pMember, pEntry->Stance);
				++spawned;

				if (auto const pMemberExt = TechnoExt::ExtMap.Find(pMember))
				{
					pMemberExt->SquadFollow = pEntry->Follow;
					pMemberExt->SquadFollowRange = pEntry->FollowRange;
					pMemberExt->SquadFollowDelay = pEntry->FollowDelay;
					pMemberExt->SquadFollowTimer = 0;
					pMemberExt->AnchorDeathBehavior = pEntry->AnchorDeath;
					pMemberExt->AnchorDeathHeir = pEntry->AnchorDeathHeir.isset()
						? pEntry->AnchorDeathHeir.Get() : nullptr;
					pMemberExt->AnchorDeathTimer = -1;
				}

				if (linkToAnchor)
				{
					pExt->SquadMembers.push_back(pMember);
					pExt->MemberSelection = pEntry->MemberSelection;
				}
			}
		}
	}

	// Runtime liveness. The rules-parse line only proves the TAGS parsed; it
	// cannot distinguish "the timer armed and fired" from "the timer never
	// armed", nor "fired but every placement failed" from "never fired" -- and
	// those look identical in game. Logging the count separates all three.
	Debug::Log("[SquadExt] %s: %s fired, %d of %d entr%s spawned %d member%s%s.\n",
		pType->ID,
		SpawnEventName(eventFlag),
		static_cast<int>(chosen.size()),
		static_cast<int>(eligible.size()),
		eligible.size() == 1 ? "y" : "ies",
		spawned,
		spawned == 1 ? "" : "s",
		linkToAnchor ? "" : " (unlinked)");
}

// ============================================================================
// Follow
//
// Keeps a member trailing its anchor. Deliberately minimal: no formation, no
// slots, no pathfinding of our own -- just "if you have drifted too far and you
// are not busy, walk back". The engine does the actual movement, so this stays
// deterministic and costs nothing for units that are not following members.
//
// Anything more (formation shapes, follow offsets, order inheritance) is the
// Peer/Retinue behaviour work and is deliberately NOT attempted here.
// ============================================================================

void TechnoExt::ProcessSquadFollow(TechnoClass* pMember)
{
	auto const pExt = TechnoExt::ExtMap.Find(pMember);

	// Cheapest possible rejection for the overwhelming majority of units.
	if (!pExt || !pExt->SquadFollow)
		return;

	if (pExt->SquadFollowTimer > 0)
	{
		--pExt->SquadFollowTimer;
		return;
	}

	auto const pAnchor = pExt->SquadAnchor;
	if (!pAnchor || !pAnchor->IsAlive || pAnchor->InLimbo)
		return;

	if (!pMember->IsAlive || pMember->InLimbo)
		return;

	auto const pFoot = abstract_cast<FootClass*>(pMember);
	if (!pFoot)
		return; // buildings cannot follow anything

	// Do not override a player's own order, or an attack in progress. We only
	// take over when the member is idling.
	switch (pFoot->CurrentMission)
	{
	case Mission::Guard:
	case Mission::Area_Guard:
	case Mission::Sleep:
	case Mission::None:
	case Mission::Stop:
		break;
	default:
		return; // busy: moving, attacking, entering, harvesting...
	}

	// Leptons -> cells. 256 leptons per cell.
	int const rangeCells = pExt->SquadFollowRange > 0 ? pExt->SquadFollowRange : 4;
	if (pMember->DistanceFrom(pAnchor) <= rangeCells * 256)
		return; // close enough

	auto const pCell = MapClass::Instance.TryGetCellAt(pAnchor->GetMapCoords());
	if (!pCell)
		return;

	// Virtual calls -- SetDestination is RX and QueueMission is R0 in YRpp, so
	// qualified calls would silently no-op.
	pFoot->SetDestination(pCell, true);
	pFoot->QueueMission(Mission::Move, false);

	pExt->SquadFollowTimer = pExt->SquadFollowDelay > 0 ? pExt->SquadFollowDelay : 15;
}

// ---- trigger entry points -------------------------------------------------

void TechnoExt::ProcessPendingSpawn(TechnoClass* pAnchor)
{
	if (!pAnchor || !pAnchor->IsAlive || pAnchor->InLimbo)
		return;

	auto const pExt = TechnoExt::ExtMap.Find(pAnchor);
	if (!pExt || !pExt->PendingSquadSpawn)
		return;

	pExt->PendingSquadSpawn = false;

	if (pExt->SquadsSpawned)
		return;

	// The `produced` trigger is one-shot per unit: Unlimbo fires again on
	// undeploy / transport exit / chrono-warp.
	pExt->SquadsSpawned = true;

	TechnoExt::SpawnEntriesForEvent(pAnchor, SquadEvent_Produced, true);

	// Arm the repeating trigger, but ONLY if some entry actually asks for it --
	// that keeps the per-frame tick a single already-negative int test for every
	// other unit in the game (pay-for-what-you-use).
	auto const pType = pAnchor->GetTechnoType();
	auto const pTypeExt = pType ? TechnoTypeExt::ExtMap.Find(pType) : nullptr;
	if (!pTypeExt)
		return;

	for (auto const& entry : pTypeExt->SquadData)
	{
		if ((entry.SpawnEvents & SquadEvent_Timer) && entry.SpawnEventInterval > 0)
		{
			pExt->SquadEventTimer = entry.SpawnEventInterval;
			break;
		}
	}
}

void TechnoExt::ProcessSpawnTimer(TechnoClass* pAnchor)
{
	auto const pExt = TechnoExt::ExtMap.Find(pAnchor);

	// Cheapest possible rejection for the overwhelming majority of units.
	if (!pExt || pExt->SquadEventTimer < 0)
		return;

	if (!pAnchor->IsAlive || pAnchor->InLimbo)
		return;

	if (pExt->SquadEventTimer > 0)
	{
		--pExt->SquadEventTimer;
		return;
	}

	TechnoExt::SpawnEntriesForEvent(pAnchor, SquadEvent_Timer, true);

	// Re-arm from the first timer entry's interval.
	auto const pType = pAnchor->GetTechnoType();
	auto const pTypeExt = pType ? TechnoTypeExt::ExtMap.Find(pType) : nullptr;
	pExt->SquadEventTimer = -1;

	if (!pTypeExt)
		return;

	for (auto const& entry : pTypeExt->SquadData)
	{
		if ((entry.SpawnEvents & SquadEvent_Timer) && entry.SpawnEventInterval > 0)
		{
			pExt->SquadEventTimer = entry.SpawnEventInterval;
			break;
		}
	}
}

void TechnoExt::SpawnDeathSquad(TechnoClass* pAnchor)
{
	// Runs from the 0x702050 death site, where the unit is still present and its
	// coords/owner are valid -- the encyclopedia documents on-death spawning here
	// as verified from a standalone DLL. Members spawn UNLINKED: their would-be
	// anchor is about to be freed.
	TechnoExt::SpawnEntriesForEvent(pAnchor, SquadEvent_Death, false);
}

// ============================================================================
// Anchor death
//
// Two-stage, like spawning, and for the same reason: the death hook sits inside
// TechnoClass::ReceiveDamage, and killing / re-owning / removing other units
// from inside the engine's damage path invites re-entrancy. So the hook only
// ARMS each member, and the member's own tick does the work a frame or more
// later. That also gives AnchorDeath.Delay for free.
// ============================================================================

void TechnoExt::ArmAnchorDeath(TechnoClass* pAnchor)
{
	auto const pExt = TechnoExt::ExtMap.Find(pAnchor);
	if (!pExt || pExt->SquadMembers.empty())
		return;

	auto const pType = pAnchor->GetTechnoType();
	auto const pTypeExt = pType ? TechnoTypeExt::ExtMap.Find(pType) : nullptr;

	// Delay comes from the type's first entry; the per-member behaviour was
	// already copied down at spawn time, so a mixed-entry anchor still reacts
	// per entry.
	int delay = 0;
	if (pTypeExt && !pTypeExt->SquadData.empty())
		delay = pTypeExt->SquadData[0].AnchorDeathDelay;

	// Elect the heir HERE, while the member list is still intact.
	//
	// This cannot be deferred to the reaction: pointer invalidation nulls every
	// member's SquadAnchor the instant the anchor is freed, so by the time the
	// reaction runs there is no way left to enumerate the squad. Resolving it
	// late is exactly why promote previously degraded into plain disband --
	// the sibling walk found a null anchor and fell through.
	TechnoClass* pHeir = nullptr;
	{
		TechnoClass* pFirstAlive = nullptr;
		for (auto const pMember : pExt->SquadMembers)
		{
			if (!pMember || !pMember->IsAlive || pMember->InLimbo)
				continue;

			auto const pMemberExt = TechnoExt::ExtMap.Find(pMember);
			if (!pMemberExt || pMemberExt->AnchorDeathBehavior != SquadAnchorDeath::Promote)
				continue;

			if (!pFirstAlive)
				pFirstAlive = pMember;

			// A configured Heir type wins; otherwise the first survivor does.
			// Either way the choice is tick-order deterministic, never RNG.
			if (pMemberExt->AnchorDeathHeir
				&& pMemberExt->AnchorDeathHeir == pMember->GetTechnoType())
			{
				pHeir = pMember;
				break;
			}
		}

		if (!pHeir)
			pHeir = pFirstAlive;
	}

	for (auto const pMember : pExt->SquadMembers)
	{
		if (!pMember || !pMember->IsAlive)
			continue;

		auto const pMemberExt = TechnoExt::ExtMap.Find(pMember);
		if (!pMemberExt || pMemberExt->AnchorDeathTimer >= 0)
			continue; // already armed

		pMemberExt->AnchorDeathTimer = delay > 0 ? delay : 0;
		pMemberExt->AnchorDeathNewAnchor = pHeir;
	}

	if (pHeir)
	{
		Debug::Log("[SquadExt] %s died: electing %s as the new anchor for %d member(s).\n",
			pType ? pType->ID : "?",
			pHeir->GetTechnoType()->ID,
			static_cast<int>(pExt->SquadMembers.size()));
	}
}

void TechnoExt::ProcessAnchorDeath(TechnoClass* pMember)
{
	auto const pExt = TechnoExt::ExtMap.Find(pMember);

	// Cheapest possible rejection for the overwhelming majority of units.
	if (!pExt || pExt->AnchorDeathTimer < 0)
		return;

	if (pExt->AnchorDeathTimer > 0)
	{
		--pExt->AnchorDeathTimer;
		return;
	}

	pExt->AnchorDeathTimer = -1; // fire once

	if (!pMember->IsAlive || pMember->InLimbo)
		return;

	auto const behavior = pExt->AnchorDeathBehavior;

	// Every path ends with this member no longer belonging to a dead anchor.
	auto const detach = [pExt]()
	{
		pExt->SquadAnchor = nullptr;
		pExt->IsSquadMember = false;
		pExt->SquadFollow = false;
		pExt->MemberSelection = SquadMemberSelection::Independent;
	};

	switch (behavior)
	{
	case SquadAnchorDeath::Kill:
	{
		// A real death: counts as a loss and fires death weapons/anims.
		int damage = pMember->Health * 2 + 1000;
		pMember->ReceiveDamage(&damage, 0, RulesClass::Instance->C4Warhead,
			nullptr, true, false, nullptr);
		detach();
		break;
	}

	case SquadAnchorDeath::Vanish:
		// Gone with no loss and no death effects. Limbo first so the engine
		// unregisters it from its cell before the object is torn down.
		detach();
		pMember->Limbo();
		pMember->UnInit();
		break;

	case SquadAnchorDeath::Sell:
		detach();
		pMember->Sell(-1); // -1 = always sell
		break;

	case SquadAnchorDeath::Neutral:
		if (auto const pNeutral = HouseClass::FindNeutral())
			pMember->SetOwningHouse(pNeutral, false);
		detach();
		break;

	case SquadAnchorDeath::Promote:
	{
		auto const pNewAnchor = pExt->AnchorDeathNewAnchor;

		// The heir died between the election and now (or there never was one:
		// every member gone, or this squad had no promote members). Nothing to
		// re-parent to, so fall back to the safe outcome.
		if (!pNewAnchor || !pNewAnchor->IsAlive || pNewAnchor->InLimbo)
		{
			detach();
			break;
		}

		pExt->AnchorDeathNewAnchor = nullptr;

		if (pNewAnchor == pMember)
		{
			// I am the heir: stop being a member, start being an anchor. My
			// SquadMembers fills in as each sibling processes its own tick.
			pExt->SquadAnchor = nullptr;
			pExt->IsSquadMember = false;
			pExt->SquadFollow = false; // nothing left to follow

			Debug::Log("[SquadExt] %s promoted to anchor.\n",
				pMember->GetTechnoType()->ID);
			break;
		}

		// I am a survivor: re-parent under the heir and keep following it.
		pExt->SquadAnchor = pNewAnchor;
		pExt->IsSquadMember = true;

		if (auto const pHeirExt = TechnoExt::ExtMap.Find(pNewAnchor))
			pHeirExt->SquadMembers.push_back(pMember);

		break;
	}

	case SquadAnchorDeath::Disband:
	default:
		detach();
		break;
	}
}

// ============================================================================
// Selection
// ============================================================================

TechnoClass* TechnoExt::ResolveSelectionTarget(TechnoClass* pTechno)
{
	auto const pExt = TechnoExt::ExtMap.Find(pTechno);
	if (!pExt)
		return pTechno;

	if (pExt->MemberSelection == SquadMemberSelection::Anchor
		&& pExt->SquadAnchor
		&& pExt->SquadAnchor->IsAlive)
	{
		return pExt->SquadAnchor;
	}

	return pTechno;
}

void TechnoExt::CollectSelectionGroup(TechnoClass* pTechno, std::vector<TechnoClass*>& out)
{
	out.clear();

	auto const pExt = TechnoExt::ExtMap.Find(pTechno);
	if (!pExt || pExt->MemberSelection != SquadMemberSelection::Group)
		return;

	// Nested squads FLATTEN into one selection group: walk up to the top-level
	// anchor, then collect that whole tree. Previously this only looked one
	// level up, so a Prism Cannon anchoring a SEAL selected the SEAL but none of
	// the SEAL's own GIs -- the chain looked half-connected in game.
	TechnoClass* pRoot = pTechno;
	for (int guard = 0; guard < MaxSquadTreeDepth; ++guard)
	{
		auto const pCurExt = TechnoExt::ExtMap.Find(pRoot);
		auto const pUp = pCurExt ? pCurExt->SquadAnchor : nullptr;
		if (!pUp || !pUp->IsAlive || pUp->InLimbo)
			break;
		pRoot = pUp;
	}

	// Breadth-first over the tree. `seen` makes a cyclic roster terminate rather
	// than hang, and the size cap bounds a pathological chain.
	std::vector<TechnoClass*> queue;
	std::unordered_set<TechnoClass*> seen;
	queue.push_back(pRoot);
	seen.insert(pRoot);

	for (size_t i = 0; i < queue.size() && queue.size() < MaxSquadTreeNodes; ++i)
	{
		auto const pCur = queue[i];

		if (pCur != pTechno)
			out.push_back(pCur);

		auto const pCurExt = TechnoExt::ExtMap.Find(pCur);
		if (!pCurExt)
			continue;

		for (auto const pMember : pCurExt->SquadMembers)
		{
			if (pMember && pMember->IsAlive && !pMember->InLimbo
				&& seen.insert(pMember).second)
			{
				queue.push_back(pMember);
			}
		}
	}
}

// ============================================================================
// Pointer invalidation -- without this, a dead member leaves a dangling
// TechnoClass* and the next selection/AI touch calls into freed memory. This is
// exactly the defect PR #1663 shipped with (its PointerGotInvalid was empty).
// ============================================================================

void TechnoExt::ExtData::InvalidatePointer(void* ptr, bool bRemoved)
{
	if (this->SquadAnchor == ptr)
		this->SquadAnchor = nullptr;

	if (this->AnchorDeathNewAnchor == ptr)
		this->AnchorDeathNewAnchor = nullptr;

	for (auto it = this->SquadMembers.begin(); it != this->SquadMembers.end(); )
	{
		if (*it == ptr)
			it = this->SquadMembers.erase(it);
		else
			++it;
	}
}

// ============================================================================
// Serialization
// ============================================================================

template <typename T>
void TechnoExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(this->SquadAnchor)
		.Process(this->SquadMembers)
		.Process(this->IsSquadMember)
		.Process(this->SquadsSpawned)
		.Process(this->PendingSquadSpawn)
		.Process(this->SquadDepth)
		.Process(this->SquadLoopLimit)
		.Process(this->MemberSelection)
		.Process(this->SquadFollow)
		.Process(this->SquadFollowRange)
		.Process(this->SquadFollowDelay)
		.Process(this->SquadFollowTimer)
		.Process(this->AnchorDeathBehavior)
		.Process(this->AnchorDeathTimer)
		.Process(this->AnchorDeathHeir)
		.Process(this->AnchorDeathNewAnchor)
		.Process(this->SquadEventTimer)
		;
}

void TechnoExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<TechnoClass>::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void TechnoExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<TechnoClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

// ============================================================================
// Container
// ============================================================================

TechnoExt::ExtContainer::ExtContainer()
	: Container("TechnoClass")
{ }

TechnoExt::ExtContainer::~ExtContainer() = default;

DEFINE_HOOK(0x6F3260, TechnoClass_CTOR_SquadExt, 0x5)
{
	GET(TechnoClass*, pItem, ESI);
	TechnoExt::ExtMap.TryAllocate(pItem);
	return 0;
}

DEFINE_HOOK(0x6F4500, TechnoClass_DTOR_SquadExt, 0x5)
{
	GET(TechnoClass*, pItem, ECX);
	TechnoExt::ExtMap.Remove(pItem);
	return 0;
}

DEFINE_HOOK_AGAIN(0x70C250, TechnoClass_SaveLoad_Prefix_SquadExt, 0x8)
DEFINE_HOOK(0x70BF50, TechnoClass_SaveLoad_Prefix_SquadExt, 0x5)
{
	GET_STACK(TechnoClass*, pItem, 0x4);
	GET_STACK(IStream*, pStm, 0x8);
	TechnoExt::ExtMap.PrepareStream(pItem, pStm);
	return 0;
}

DEFINE_HOOK(0x70C249, TechnoClass_Load_Suffix_SquadExt, 0x5)
{
	TechnoExt::ExtMap.LoadStatic();
	return 0;
}

DEFINE_HOOK(0x70C264, TechnoClass_Save_Suffix_SquadExt, 0x5)
{
	TechnoExt::ExtMap.SaveStatic();
	return 0;
}
