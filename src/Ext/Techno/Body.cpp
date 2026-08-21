#include "Body.h"

#include <FootClass.h>
#include <BuildingTypeClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <CellClass.h>
#include <ScenarioClass.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>
#include <Helpers/Cast.h>

#include <Ext/TechnoType/Body.h>

TechnoExt::ExtContainer TechnoExt::ExtMap;

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

	auto const pType = pAnchor->GetTechnoType();
	auto const pTypeExt = pType ? TechnoTypeExt::ExtMap.Find(pType) : nullptr;
	if (!pTypeExt || !pTypeExt->HasSquads())
		return;

	// Handled exactly once per unit.
	pExt->SquadsSpawned = true;

	// Generation of this unit; 0 and 1 both mean top-level.
	int const myGen = pExt->SquadDepth > 0 ? pExt->SquadDepth : 1;

	// LoopLimit comes from the originating unit and governs the whole chain.
	int const loopLimit = pExt->SquadLoopLimit >= 0
		? pExt->SquadLoopLimit
		: pTypeExt->LoopLimit;

	// A unit at generation G spawns only while G < LoopLimit, so LoopLimit=5
	// yields five generations with the fifth sterile. 0 = unlimited.
	if (loopLimit > 0 && myGen >= loopLimit)
		return;

	auto const pOwner = pAnchor->Owner;

	// ---- pick which roster entries fire ----
	std::vector<const SquadEntryData*> eligible;
	for (auto const& entry : pTypeExt->SquadData)
	{
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
				if (auto const pMemberExt = TechnoExt::ExtMap.Find(pMember))
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

				pExt->SquadMembers.push_back(pMember);
				pExt->MemberSelection = pEntry->MemberSelection;
			}
		}
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

	// Resolve to the anchor, then collect it plus every living member. Works
	// whether the click landed on the anchor or on one of its members.
	TechnoClass* pAnchor = pExt->SquadAnchor ? pExt->SquadAnchor : pTechno;
	if (!pAnchor->IsAlive)
		return;

	auto const pAnchorExt = TechnoExt::ExtMap.Find(pAnchor);
	if (!pAnchorExt)
		return;

	if (pAnchor != pTechno)
		out.push_back(pAnchor);

	for (auto const pMember : pAnchorExt->SquadMembers)
	{
		if (pMember && pMember != pTechno && pMember->IsAlive && !pMember->InLimbo)
			out.push_back(pMember);
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
