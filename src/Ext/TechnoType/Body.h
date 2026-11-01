#pragma once

#include <string>

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
	// Can a member be put into a selection at all? no = clicks and band-select
	// skip it entirely, which also makes it unorderable by the player: there is
	// no way to issue an order to a unit you cannot select.
	//
	// The reason this exists: YR resolves the action cursor across the WHOLE
	// selection, so one GI in the box downgrades a CHRONO MINER's ore-harvest
	// cursor to a plain move cursor. Keeping members out of the selection
	// leaves the anchor's own cursor intact.
	Valueable<bool> Selectable;
	// May AI team recruitment take this member? no = it is released again on
	// its next tick.
	//
	// The reason this exists: a PRISM TOWER with a timer entry became an
	// infinite GI fountain -- the AI recruited each guard into an attack wave,
	// the guard died at the enemy base, the live count dropped, and the timer
	// refilled it. 5499 spawns in one session, MaxActive working perfectly the
	// whole time, because the cap bounds how many are ALIVE and not how many
	// the AI may march off and lose.
	Valueable<bool> Recruitable;
	// Does a member obey orders from OUTSIDE the squad? no = it reverts to its
	// Stance whenever something else tries to walk it somewhere. Independent of
	// Selectable: a member can be selectable and still ignore orders, which is
	// not expressible if the two are the same tag.
	Valueable<bool> Orderable;
	// Leash, in cells. 0 = none. A member further than this from its anchor is
	// walked back, enforced continuously rather than only once it goes idle --
	// so the player may order it about, but only inside this radius.
	Valueable<int> OrderRange;
	// Any engine mission, BY NAME, resolved through MissionControlClass so the
	// set of legal values is whatever Mission Control actually defines -- and
	// stays correct if a mod redefines the table. -1 = unset, in which case
	// Stance decides. Stance stays as the short convenience form; this is the
	// general one, and it is what makes a spawned member able to hold any
	// mission the engine has rather than the five Stance exposes.
	int Mission { -1 };
	// Re-assert that mission whenever the member lapses back to idling. Without
	// it a mission is a one-shot nudge at spawn: a hunter that runs out of
	// targets reverts to Guard and never resumes, which is the difference
	// between a squad and a spawner.
	Valueable<bool> MissionHold;

	// -- AI team / script attachment --
	//
	// ONE team per squad entry, not per member: a team is a formation, and a
	// team per member would mean an unbounded number of one-unit teams for an
	// entry that spawns on a timer.
	//
	// Stored as IDs and resolved LAZILY, because TeamTypes and ScriptTypes come
	// from aimd.ini, which the engine reads AFTER rulesmd.ini -- at parse time
	// neither lookup can succeed yet.
	//
	// Team= uses the modder's own TeamType, with its own TaskForce and its own
	// Recruiter= setting, so recruiting behaviour stays entirely in aimd.ini.
	// Script= synthesises a TeamType whose TaskForce is built from THIS entry's
	// roster, which is what keeps it at full strength and therefore quiet.
	std::string TeamID;
	std::string ScriptID;
	// Synthesised teams only: may the team pull in units that are not ours? no
	// keeps it to the squad; yes lets it replace losses from the owner's army,
	// which is the "building that interacts with the player's army" case.
	// Ignored for a named Team=, whose TeamType we must not mutate -- it is
	// shared with every other user of that team in aimd.ini.
	Valueable<bool> TeamRecruits;
	// What the synthesised TaskForce ASKS FOR, parallel to Members=. Defaults to
	// Numbers=, i.e. exactly what the squad brings, which is what keeps a
	// non-recruiting team at full strength.
	//
	// Setting it HIGHER than Numbers= is how recruiting becomes deliberate
	// rather than incidental: the team is short from the moment it is created,
	// so with TeamRecruits=yes it fills the gap out of the owner's existing
	// army straight away instead of only as members die. "Field one of mine and
	// conscript three of yours" is expressible; without it, recruiting could
	// only ever be observed by losing a member first.
	ValueableVector<int> TeamNumbers;

	// -- when this entry fires (bitmask of SquadSpawnEventFlags) --
	int SpawnEvents;
	Valueable<int> SpawnEventInterval; // frames, for the timer trigger
	Valueable<int> SpawnEventChance;   // 0-100 roll each time the event fires

	// -- order echo: members mirror the anchor's orders --
	Valueable<SquadOrderEcho> OrderEcho;
	// FootClass::Destination is the LIVE movement target, not a record of the
	// player's order -- the engine rewrites it constantly while pathing, chasing
	// a target or scattering. Echoing every change re-ordered members 691 times
	// in one session and reset their pathing each time. These two filters
	// approximate "a new order" from that noisy signal.
	Valueable<int> OrderEchoDelay;     // min frames between echoes
	Valueable<int> OrderEchoMinChange; // min cells the destination must move

	// -- upkeep: replace losses and cap the live population --
	Valueable<int> RegenRate;             // frames to replace a lost member; 0 = off
	Valueable<int> RegenDelay;            // first replacement delay; <0 = use RegenRate
	Valueable<bool> RegenRequiresEligible;// re-check Prerequisite/Houses before regen
	Valueable<int> MaxActive;             // cap on LIVE members from this anchor; 0 = none

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
		, Selectable { true }
		, Recruitable { true }
		, Orderable { true }
		, OrderRange { 0 }
		, MissionHold { false }
		, TeamID {}
		, ScriptID {}
		, TeamRecruits { false }
		, TeamNumbers {}
		, OrderEcho { SquadOrderEcho::None }
		, OrderEchoDelay { 30 }
		, OrderEchoMinChange { 3 }
		, RegenRate { 0 }
		, RegenDelay { -1 }
		, RegenRequiresEligible { true }
		, MaxActive { 0 }
		, SpawnEvents { SquadEvent_Produced }
		, SpawnEventInterval { 0 }
		, SpawnEventChance { 100 }
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
		// Umbrella cap on LIVE members from this anchor, across EVERY entry.
		// Unlike the per-entry SquadN.MaxActive it also binds entries that set
		// no cap of their own, which is the gap it exists to close. 0 = none.
		Valueable<int> MaxActive;

		// Unit-level team attachment, independent of any squad.
		//
		// This is what lets a unit or building carry a team WITHOUT spawning
		// anything: the team is created when the unit appears and, if its
		// TaskForce asks for units, recruits them from the owner's existing
		// army. A building has no FootClass to add, so for a building the team
		// starts empty and recruiting is the entire point.
		std::string SquadTeamID;
		std::string SquadScriptID;
		Valueable<bool> SquadTeamRecruits;

		ExtData(TechnoTypeClass* OwnerObject) : Extension<TechnoTypeClass>(OwnerObject)
			, SquadData {}
			, EntrySelect { SquadEntrySelect::First }
			, LoopLimit { 0 }
			, MaxSpawnPerProduction { 999 }
			, MaxActive { 0 }
			, SquadTeamID {}
			, SquadScriptID {}
			, SquadTeamRecruits { false }
		{ }

		virtual ~ExtData() = default;

		virtual void LoadFromINIFile(CCINIClass* pINI) override;
		virtual void Initialize() override { }
		virtual void InvalidatePointer(void* ptr, bool bRemoved) override { }

		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;

		// True when this type needs the per-frame squad machinery at all. A
		// unit-level team counts: a building can carry Squad.Team= with no
		// roster whatsoever, and without this it would never be flagged and the
		// team would never be created.
		bool HasSquads() const
		{
			return !this->SquadData.empty()
				|| !this->SquadTeamID.empty()
				|| !this->SquadScriptID.empty();
		}

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
