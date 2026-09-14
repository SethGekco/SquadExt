#pragma once

#include <vector>

#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

#include <TechnoTypeClass.h>

#include <SquadEnums.h>

class BuildingTypeClass;
class HouseTypeClass;
class HouseClass;

// Per-member settings. The SAME struct is used for two different layers, which
// are NOT synonyms -- they cascade:
//
//   SquadN.MemberK.*  (slot K override)  >  SquadN.Member.*  (entry default)
//     >  compact SquadN.Members/Numbers  >  engine default
//
// Every field is Nullable so "unset" is distinguishable from "set to the
// default value", which is what makes the cascade work.
struct SquadMemberData
{
	// Nullable<T*> (not NullableIdx<T>, which stores an int index and whose
	// Get() would not yield a pointer). detail::read<T*> resolves via
	// TechnoTypeClass::Find, so this parses a type ID string directly.
	Nullable<TechnoTypeClass*> Type;     // MemberK.Type (expanded form)
	Nullable<int> Count;                 // how many of this slot
	Nullable<int> Chance;                // 0-100 roll per unit; unset = always
	Nullable<SquadVeterancy> Veterancy;
	Nullable<int> Health;                // percent of Strength, 1-100
	Nullable<int> Facing;                // 0-255 game facing
	// Per-member house gate: one entry can hold members that only appear for
	// certain countries, instead of duplicating the whole entry.
	ValueableVector<HouseTypeClass*> RequiredHouses;
	ValueableVector<HouseTypeClass*> ForbiddenHouses;

	bool Load(PhobosStreamReader& stm, bool registerForChange);
	bool Save(PhobosStreamWriter& stm) const;

private:
	template <typename T>
	bool Serialize(T& stm);
};

// One roster entry: SquadN.*
struct SquadEntryData
{
	// -- composition (compact form) --
	ValueableVector<TechnoTypeClass*> Members;
	ValueableVector<int> Numbers;

	// -- eligibility, evaluated against the anchor's owner --
	ValueableVector<BuildingTypeClass*> Prerequisite;             // list 0
	std::vector<ValueableVector<BuildingTypeClass*>> PrerequisiteLists; // .1 .2 ... (OR)
	ValueableVector<BuildingTypeClass*> PrerequisiteNegative;
	ValueableVector<HouseTypeClass*> RequiredHouses;
	ValueableVector<HouseTypeClass*> ForbiddenHouses;

	// -- selection / gating --
	Valueable<int> Weight;                        // for EntrySelect=weighted
	Valueable<int> Chance;                        // 0-100 roll for the whole entry
	Valueable<SquadMemberSelection> MemberSelection;
	Valueable<SquadActivateAs> ActivateAs;
	Nullable<int> LoopLimit;                      // per-entry nesting override

	// -- cohesion / behaviour --
	Valueable<SquadStance> Stance;   // standing order given at spawn
	Valueable<bool> Follow;          // members trail a moving anchor
	Valueable<int> FollowRange;      // cells of slack before a member is recalled
	Valueable<int> FollowDelay;      // frames between follow re-orders (anti-spam)

	// -- what happens to members when the anchor dies --
	Valueable<SquadAnchorDeath> AnchorDeath;
	Valueable<int> AnchorDeathDelay;        // frames members linger before it fires
	Nullable<TechnoTypeClass*> AnchorDeathHeir; // Promote: which member type inherits

	// -- member cascade --
	SquadMemberData MemberDefault;                // SquadN.Member.*
	std::vector<SquadMemberData> MemberOverrides; // SquadN.MemberK.*

	SquadEntryData()
		: Members {}
		, Numbers {}
		, Prerequisite {}
		, PrerequisiteLists {}
		, PrerequisiteNegative {}
		, RequiredHouses {}
		, ForbiddenHouses {}
		, Weight { 1 }
		, Chance { 100 }
		, MemberSelection { SquadMemberSelection::Independent }
		, ActivateAs { SquadActivateAs::Any }
		, LoopLimit {}
		, Stance { SquadStance::Guard }
		, Follow { true }
		, FollowRange { 4 }
		, FollowDelay { 15 }
		, AnchorDeath { SquadAnchorDeath::Disband }
		, AnchorDeathDelay { 0 }
		, AnchorDeathHeir {}
		, MemberDefault {}
		, MemberOverrides {}
	{ }

	// True if the owner satisfies every gate on this entry. Mirrors the proven
	// evaluation in TechnoAttachmentExt's AttachmentClass::PrerequisitesMet.
	bool EligibleFor(HouseClass* pOwner) const;

	// How many slots this entry actually defines (compact list vs expanded).
	size_t SlotCount() const;

	// Cascade resolution for slot K.
	TechnoTypeClass* GetSlotType(size_t k) const;
	int GetSlotCount(size_t k) const;
	int GetSlotChance(size_t k) const;
	SquadVeterancy GetSlotVeterancy(size_t k) const;
	int GetSlotHealth(size_t k) const;
	int GetSlotFacing(size_t k) const;
	bool SlotAllowedFor(size_t k, HouseClass* pOwner) const;

	bool Load(PhobosStreamReader& stm, bool registerForChange);
	bool Save(PhobosStreamWriter& stm) const;

private:
	const SquadMemberData* Override(size_t k) const
	{
		return k < this->MemberOverrides.size() ? &this->MemberOverrides[k] : nullptr;
	}

	template <typename T>
	bool Serialize(T& stm);
};

// Standalone TechnoTypeExt carrying ONLY squad roster data. Container<T> runs in
// unordered_map mode (Canary defined, no ExtPointerOffset) so we claim no
// pointer slot inside TechnoTypeClass and never collide with Phobos/Ares/other
// standalone DLLs' extension storage.
class TechnoTypeExt
{
public:
	using base_type = TechnoTypeClass;

	// Unique canary -- distinct from Phobos (0x11111111) and
	// TechnoAttachmentExt (0x0A77AC77 / 0x0A77EC77).
	static constexpr DWORD Canary = 0x50DAC77;

	class ExtData final : public Extension<TechnoTypeClass>
	{
	public:
		ValueableVector<SquadEntryData> SquadData;

		// Unit-level knobs.
		Valueable<SquadEntrySelect> EntrySelect;
		Valueable<int> LoopLimit;              // generations of nesting; 0 = unlimited
		Valueable<int> MaxSpawnPerProduction;  // hard anti-hang cap on total spawns

		ExtData(TechnoTypeClass* OwnerObject) : Extension<TechnoTypeClass>(OwnerObject)
			, SquadData {}
			, EntrySelect { SquadEntrySelect::First }
			, LoopLimit { 0 }
			, MaxSpawnPerProduction { 999 }
		{ }

		virtual ~ExtData() = default;

		virtual void LoadFromINIFile(CCINIClass* pINI) override;
		virtual void Initialize() override { }
		virtual void InvalidatePointer(void* ptr, bool bRemoved) override { }

		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;

		bool HasSquads() const { return !this->SquadData.empty(); }

	private:
		template <typename T>
		void Serialize(T& Stm);
	};

	class ExtContainer final : public Container<TechnoTypeExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;
};
