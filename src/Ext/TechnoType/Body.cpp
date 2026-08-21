#include "Body.h"

#include <BuildingTypeClass.h>
#include <HouseTypeClass.h>
#include <HouseClass.h>
#include <Utilities/Macro.h>

TechnoTypeExt::ExtContainer TechnoTypeExt::ExtMap;

// Longest key we format is roughly "Squad12.Member12.ForbiddenHouses" (32) --
// TechnoAttachmentExt hit a real crash by sizing this buffer too tightly:
// _snprintf_s overflow invokes the CRT invalid-parameter handler and kills the
// process instantly with no exception dump. 96 leaves plenty of headroom.
static constexpr size_t KeyBufferSize = 96;

// How far past the compact Members list we keep probing for expanded
// SquadN.MemberK.* blocks before concluding there are none.
static constexpr size_t MemberProbeLookahead = 8;
static constexpr size_t MemberProbeHardCap = 128;

// ============================================================================
// SquadEntryData -- eligibility
// ============================================================================

bool SquadEntryData::EligibleFor(HouseClass* pOwner) const
{
	bool const hasBuildingReq = !this->Prerequisite.empty() || !this->PrerequisiteLists.empty();

	if (!hasBuildingReq && this->PrerequisiteNegative.empty()
		&& this->RequiredHouses.empty() && this->ForbiddenHouses.empty())
	{
		return true; // no gating at all
	}

	if (!pOwner)
		return false;

	// Building requirement: satisfy the primary Prerequisite (list 0) OR any
	// numbered alternative list. Each list is AND-within.
	if (hasBuildingReq)
	{
		auto const listPresent = [pOwner](const ValueableVector<BuildingTypeClass*>& list) -> bool
		{
			if (list.empty())
				return false; // empty = not a real alternative
			for (auto const pBld : list)
			{
				if (pBld && pOwner->CountOwnedAndPresent(pBld) <= 0)
					return false;
			}
			return true;
		};

		bool met = listPresent(this->Prerequisite);
		if (!met)
		{
			for (auto const& list : this->PrerequisiteLists)
			{
				if (listPresent(list)) { met = true; break; }
			}
		}
		if (!met)
			return false;
	}

	// Negative buildings: NONE may be present.
	for (auto const pBld : this->PrerequisiteNegative)
	{
		if (pBld && pOwner->CountOwnedAndPresent(pBld) > 0)
			return false;
	}

	// Required houses: owner's country must be listed.
	if (!this->RequiredHouses.empty())
	{
		bool listed = false;
		for (auto const pHT : this->RequiredHouses)
		{
			if (pHT && pHT == pOwner->Type) { listed = true; break; }
		}
		if (!listed)
			return false;
	}

	// Forbidden houses: owner's country must NOT be listed.
	for (auto const pHT : this->ForbiddenHouses)
	{
		if (pHT && pHT == pOwner->Type)
			return false;
	}

	return true;
}

// ============================================================================
// SquadEntryData -- member cascade resolution
// ============================================================================

size_t SquadEntryData::SlotCount() const
{
	return this->Members.size() > this->MemberOverrides.size()
		? this->Members.size()
		: this->MemberOverrides.size();
}

TechnoTypeClass* SquadEntryData::GetSlotType(size_t k) const
{
	if (auto const pOv = this->Override(k))
	{
		if (pOv->Type.isset())
			return pOv->Type.Get();
	}

	return k < this->Members.size() ? this->Members[k] : nullptr;
}

int SquadEntryData::GetSlotCount(size_t k) const
{
	if (auto const pOv = this->Override(k))
	{
		if (pOv->Count.isset())
			return pOv->Count.Get();
	}

	if (this->MemberDefault.Count.isset())
		return this->MemberDefault.Count.Get();

	// Compact Numbers list: aligns 1:1 with Members; a short list pads with its
	// last value (so Numbers=3 means "3 of every member").
	if (!this->Numbers.empty())
	{
		return k < this->Numbers.size()
			? this->Numbers[k]
			: this->Numbers[this->Numbers.size() - 1];
	}

	return 1;
}

int SquadEntryData::GetSlotChance(size_t k) const
{
	if (auto const pOv = this->Override(k))
	{
		if (pOv->Chance.isset())
			return pOv->Chance.Get();
	}
	if (this->MemberDefault.Chance.isset())
		return this->MemberDefault.Chance.Get();
	return 100;
}

SquadVeterancy SquadEntryData::GetSlotVeterancy(size_t k) const
{
	if (auto const pOv = this->Override(k))
	{
		if (pOv->Veterancy.isset())
			return pOv->Veterancy.Get();
	}
	if (this->MemberDefault.Veterancy.isset())
		return this->MemberDefault.Veterancy.Get();
	return SquadVeterancy::None;
}

int SquadEntryData::GetSlotHealth(size_t k) const
{
	if (auto const pOv = this->Override(k))
	{
		if (pOv->Health.isset())
			return pOv->Health.Get();
	}
	if (this->MemberDefault.Health.isset())
		return this->MemberDefault.Health.Get();
	return 100;
}

int SquadEntryData::GetSlotFacing(size_t k) const
{
	if (auto const pOv = this->Override(k))
	{
		if (pOv->Facing.isset())
			return pOv->Facing.Get();
	}
	if (this->MemberDefault.Facing.isset())
		return this->MemberDefault.Facing.Get();
	return -1; // -1 = leave engine default
}

bool SquadEntryData::SlotAllowedFor(size_t k, HouseClass* pOwner) const
{
	auto const check = [pOwner](const ValueableVector<HouseTypeClass*>& req,
		const ValueableVector<HouseTypeClass*>& forb) -> bool
	{
		if (req.empty() && forb.empty())
			return true;
		if (!pOwner)
			return false;

		if (!req.empty())
		{
			bool listed = false;
			for (auto const pHT : req)
			{
				if (pHT && pHT == pOwner->Type) { listed = true; break; }
			}
			if (!listed)
				return false;
		}

		for (auto const pHT : forb)
		{
			if (pHT && pHT == pOwner->Type)
				return false;
		}
		return true;
	};

	// Entry-wide member default gate, then the slot's own gate. Both must pass.
	if (!check(this->MemberDefault.RequiredHouses, this->MemberDefault.ForbiddenHouses))
		return false;

	if (auto const pOv = this->Override(k))
	{
		if (!check(pOv->RequiredHouses, pOv->ForbiddenHouses))
			return false;
	}

	return true;
}

// ============================================================================
// INI parsing
// ============================================================================

// Read one SquadN.Member[K].* block. Returns true if ANY key was present, which
// is how we detect that an expanded slot exists at all.
static bool ReadMemberBlock(SquadMemberData& out, INI_EX& exINI, const char* pSection,
	int entry, int slot /* -1 = the entry-wide "Member." default */)
{
	char key[KeyBufferSize];
	char prefix[KeyBufferSize];

	if (slot < 0)
		_snprintf_s(prefix, sizeof(prefix), "Squad%d.Member", entry);
	else
		_snprintf_s(prefix, sizeof(prefix), "Squad%d.Member%d", entry, slot);

	bool any = false;

	_snprintf_s(key, sizeof(key), "%s.Type", prefix);
	if (out.Type.Read(exINI, pSection, key)) any = true;

	_snprintf_s(key, sizeof(key), "%s.Count", prefix);
	if (out.Count.Read(exINI, pSection, key)) any = true;

	_snprintf_s(key, sizeof(key), "%s.Chance", prefix);
	if (out.Chance.Read(exINI, pSection, key)) any = true;

	_snprintf_s(key, sizeof(key), "%s.Veterancy", prefix);
	if (out.Veterancy.Read(exINI, pSection, key)) any = true;

	_snprintf_s(key, sizeof(key), "%s.Health", prefix);
	if (out.Health.Read(exINI, pSection, key)) any = true;

	_snprintf_s(key, sizeof(key), "%s.Facing", prefix);
	if (out.Facing.Read(exINI, pSection, key)) any = true;

	_snprintf_s(key, sizeof(key), "%s.RequiredHouses", prefix);
	out.RequiredHouses.Read(exINI, pSection, key);
	if (!out.RequiredHouses.empty()) any = true;

	_snprintf_s(key, sizeof(key), "%s.ForbiddenHouses", prefix);
	out.ForbiddenHouses.Read(exINI, pSection, key);
	if (!out.ForbiddenHouses.empty()) any = true;

	return any;
}

void TechnoTypeExt::ExtData::LoadFromINIFile(CCINIClass* const pINI)
{
	const char* pSection = this->OwnerObject()->ID;

	if (!pINI->GetSection(pSection))
		return;

	INI_EX exINI(pINI);

	this->EntrySelect.Read(exINI, pSection, "Squad.EntrySelect");
	this->LoopLimit.Read(exINI, pSection, "Squad.LoopLimit");
	this->MaxSpawnPerProduction.Read(exINI, pSection, "Squad.MaxSpawnPerProduction");

	// Iterate size + 1 so vector contents can be overridden via scenario rules.
	for (size_t i = 0; i <= this->SquadData.size(); ++i)
	{
		char key[KeyBufferSize];
		int const idx = static_cast<int>(i);

		// An entry exists if it declares members either compactly or expanded.
		ValueableVector<TechnoTypeClass*> members;
		_snprintf_s(key, sizeof(key), "Squad%d.Members", idx);
		members.Read(exINI, pSection, key);

		SquadMemberData memberDefault;
		bool const hasDefault = ReadMemberBlock(memberDefault, exINI, pSection, idx, -1);

		std::vector<SquadMemberData> overrides;
		{
			size_t const compact = members.size();
			for (size_t k = 0; k < MemberProbeHardCap; ++k)
			{
				SquadMemberData slot;
				bool const any = ReadMemberBlock(slot, exINI, pSection, idx, static_cast<int>(k));

				if (any)
				{
					if (overrides.size() <= k)
						overrides.resize(k + 1);
					overrides[k] = slot;
				}
				else if (k >= compact + MemberProbeLookahead)
				{
					break; // past the compact list and nothing found -- done
				}
			}
		}

		if (members.empty() && !hasDefault && overrides.empty())
			continue; // no such entry

		SquadEntryData entry;
		entry.Members = members;
		entry.MemberDefault = memberDefault;
		entry.MemberOverrides = overrides;

		_snprintf_s(key, sizeof(key), "Squad%d.Numbers", idx);
		entry.Numbers.Read(exINI, pSection, key);

		_snprintf_s(key, sizeof(key), "Squad%d.Prerequisite", idx);
		entry.Prerequisite.Read(exINI, pSection, key);

		// Numbered OR-alternatives: Squad0.Prerequisite.1, .2, ... read
		// contiguously; the first unset (empty) index ends the list.
		entry.PrerequisiteLists.clear();
		for (int n = 1; ; ++n)
		{
			_snprintf_s(key, sizeof(key), "Squad%d.Prerequisite.%d", idx, n);
			ValueableVector<BuildingTypeClass*> list;
			list.Read(exINI, pSection, key);
			if (list.empty())
				break;
			entry.PrerequisiteLists.emplace_back(std::move(list));
		}

		_snprintf_s(key, sizeof(key), "Squad%d.Prerequisite.Negative", idx);
		entry.PrerequisiteNegative.Read(exINI, pSection, key);

		_snprintf_s(key, sizeof(key), "Squad%d.RequiredHouses", idx);
		entry.RequiredHouses.Read(exINI, pSection, key);

		_snprintf_s(key, sizeof(key), "Squad%d.ForbiddenHouses", idx);
		entry.ForbiddenHouses.Read(exINI, pSection, key);

		_snprintf_s(key, sizeof(key), "Squad%d.Weight", idx);
		entry.Weight.Read(exINI, pSection, key);

		_snprintf_s(key, sizeof(key), "Squad%d.Chance", idx);
		entry.Chance.Read(exINI, pSection, key);

		_snprintf_s(key, sizeof(key), "Squad%d.MemberSelection", idx);
		entry.MemberSelection.Read(exINI, pSection, key);

		// Back-compat with PR #1663's tag name.
		_snprintf_s(key, sizeof(key), "Squad%d.IsInitAsTeam", idx);
		entry.MemberSelection.Read(exINI, pSection, key);

		_snprintf_s(key, sizeof(key), "Squad%d.ActivateAs", idx);
		entry.ActivateAs.Read(exINI, pSection, key);

		_snprintf_s(key, sizeof(key), "Squad%d.LoopLimit", idx);
		entry.LoopLimit.Read(exINI, pSection, key);

		if (i == this->SquadData.size())
			this->SquadData.push_back(entry);
		else
			this->SquadData[i] = entry;
	}
}

// ============================================================================
// Serialization
// ============================================================================

template <typename T>
void TechnoTypeExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(this->SquadData)
		.Process(this->EntrySelect)
		.Process(this->LoopLimit)
		.Process(this->MaxSpawnPerProduction)
		;
}

void TechnoTypeExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<TechnoTypeClass>::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void TechnoTypeExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<TechnoTypeClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

bool SquadMemberData::Load(PhobosStreamReader& stm, bool registerForChange)
{
	return this->Serialize(stm);
}

bool SquadMemberData::Save(PhobosStreamWriter& stm) const
{
	return const_cast<SquadMemberData*>(this)->Serialize(stm);
}

template <typename T>
bool SquadMemberData::Serialize(T& stm)
{
	return stm
		.Process(this->Type)
		.Process(this->Count)
		.Process(this->Chance)
		.Process(this->Veterancy)
		.Process(this->Health)
		.Process(this->Facing)
		.Process(this->RequiredHouses)
		.Process(this->ForbiddenHouses)
		.Success();
}

bool SquadEntryData::Load(PhobosStreamReader& stm, bool registerForChange)
{
	return this->Serialize(stm);
}

bool SquadEntryData::Save(PhobosStreamWriter& stm) const
{
	return const_cast<SquadEntryData*>(this)->Serialize(stm);
}

template <typename T>
bool SquadEntryData::Serialize(T& stm)
{
	return stm
		.Process(this->Members)
		.Process(this->Numbers)
		.Process(this->Prerequisite)
		.Process(this->PrerequisiteLists)
		.Process(this->PrerequisiteNegative)
		.Process(this->RequiredHouses)
		.Process(this->ForbiddenHouses)
		.Process(this->Weight)
		.Process(this->Chance)
		.Process(this->MemberSelection)
		.Process(this->ActivateAs)
		.Process(this->LoopLimit)
		.Process(this->MemberDefault)
		.Process(this->MemberOverrides)
		.Success();
}

// ============================================================================
// Container
// ============================================================================

TechnoTypeExt::ExtContainer::ExtContainer()
	: Container("TechnoTypeClass")
{ }

TechnoTypeExt::ExtContainer::~ExtContainer() = default;

// Container lifecycle -- addresses verified against Phobos (develop) and in use
// by TechnoAttachmentExt. All return 0 so Syringe chains us with the others.

DEFINE_HOOK(0x711835, TechnoTypeClass_CTOR_SquadExt, 0x5)
{
	GET(TechnoTypeClass*, pItem, ESI);
	TechnoTypeExt::ExtMap.TryAllocate(pItem);
	return 0;
}

DEFINE_HOOK(0x711AE0, TechnoTypeClass_DTOR_SquadExt, 0x5)
{
	GET(TechnoTypeClass*, pItem, ECX);
	TechnoTypeExt::ExtMap.Remove(pItem);
	return 0;
}

DEFINE_HOOK_AGAIN(0x716DC0, TechnoTypeClass_SaveLoad_Prefix_SquadExt, 0x5)
DEFINE_HOOK(0x7162F0, TechnoTypeClass_SaveLoad_Prefix_SquadExt, 0x6)
{
	GET_STACK(TechnoTypeClass*, pItem, 0x4);
	GET_STACK(IStream*, pStm, 0x8);
	TechnoTypeExt::ExtMap.PrepareStream(pItem, pStm);
	return 0;
}

DEFINE_HOOK(0x716DAC, TechnoTypeClass_Load_Suffix_SquadExt, 0xA)
{
	TechnoTypeExt::ExtMap.LoadStatic();
	return 0;
}

DEFINE_HOOK(0x717094, TechnoTypeClass_Save_Suffix_SquadExt, 0x5)
{
	TechnoTypeExt::ExtMap.SaveStatic();
	return 0;
}

DEFINE_HOOK(0x716123, TechnoTypeClass_LoadFromINI_SquadExt, 0x5)
{
	GET(TechnoTypeClass*, pItem, EBP);
	GET_STACK(CCINIClass*, pINI, 0x380);
	TechnoTypeExt::ExtMap.LoadFromINI(pItem, pINI);
	return 0;
}
