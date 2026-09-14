#pragma once

#include <vector>

#include <AbstractClass.h>
#include <TechnoClass.h>

#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

#include <SquadEnums.h>

// Per-instance squad state.
//
// NOTE ON DESIGN: PR #1663 kept squads in a separate global `SquadManager`
// entity array. That class had three defects we deliberately do not carry
// forward -- double ownership (ctor self-registered while callers also `new`ed
// it), an empty PointerGotInvalid (dead members left dangling TechnoClass*
// pointers -> crash), and a half-finished save/load swizzle.
//
// Instead the squad is expressed as plain links in ext data that is already
// serialized and already participates in pointer invalidation: an anchor lists
// its members, a member points back at its anchor. That removes the ownership
// problem by construction -- there is no separate object to own -- and gets
// pointer swizzling for free (Phobos swizzles raw pointers on Process).
class TechnoExt
{
public:
	using base_type = TechnoClass;

	// Unique canary -- distinct from Phobos (0x11111111) and
	// TechnoAttachmentExt (0x0A77AC77 / 0x0A77EC77).
	static constexpr DWORD Canary = 0x50DEC77;

	class ExtData final : public Extension<TechnoClass>
	{
	public:
		// If this unit is a squad member, the anchor it belongs to.
		TechnoClass* SquadAnchor;
		// If this unit is an anchor, the members it brought.
		std::vector<TechnoClass*> SquadMembers;

		// True when this unit was spawned AS a member (drives ActivateAs).
		bool IsSquadMember;
		// One-shot guard: Unlimbo fires again on undeploy / transport exit /
		// chrono-warp, and we must not re-spawn the roster each time.
		bool SquadsSpawned;
		// Set at Unlimbo, consumed on the next AI tick. Spawning is deferred
		// because at Unlimbo entry the anchor is not on the map yet, so we have
		// no valid position to place members around.
		bool PendingSquadSpawn;
		// Generation in a nested chain. 0 and 1 both mean "top level"; a member
		// spawned by a generation-N unit gets N+1. Bounds Squad.LoopLimit.
		int SquadDepth;
		// LoopLimit carried down from the ORIGINATING unit, which is
		// authoritative for the whole chain (a nested member's own LoopLimit is
		// deliberately ignored). -1 = not yet inherited.
		int SquadLoopLimit;
		// Click behaviour inherited from the entry that spawned this squad.
		SquadMemberSelection MemberSelection;

		// Follow settings, copied from the spawning entry onto each member.
		bool SquadFollow;
		int SquadFollowRange;   // cells of slack before we recall the member
		int SquadFollowDelay;   // frames between re-orders
		int SquadFollowTimer;   // counts down; 0 = may re-order

		// Anchor-death handling, copied from the spawning entry onto each member.
		SquadAnchorDeath AnchorDeathBehavior;
		// >= 0 once the anchor has died: frames left before the behaviour fires.
		// -1 = not pending. Survives save/load so a delayed reaction is not lost.
		int AnchorDeathTimer;
		TechnoTypeClass* AnchorDeathHeir;

		// Countdown for the `timer` SpawnEvent. -1 = this unit has no timer
		// entry, which is the case for essentially every unit in the game and
		// keeps the per-frame check a single negative-int test.
		int SquadEventTimer;

		ExtData(TechnoClass* OwnerObject) : Extension<TechnoClass>(OwnerObject)
			, SquadAnchor { nullptr }
			, SquadMembers {}
			, IsSquadMember { false }
			, SquadsSpawned { false }
			, PendingSquadSpawn { false }
			, SquadDepth { 0 }
			, SquadLoopLimit { -1 }
			, MemberSelection { SquadMemberSelection::Independent }
			, SquadFollow { false }
			, SquadFollowRange { 4 }
			, SquadFollowDelay { 15 }
			, SquadFollowTimer { 0 }
			, AnchorDeathBehavior { SquadAnchorDeath::Disband }
			, AnchorDeathTimer { -1 }
			, AnchorDeathHeir { nullptr }
			, SquadEventTimer { -1 }
		{ }

		virtual ~ExtData() override = default;

		virtual void InvalidatePointer(void* ptr, bool bRemoved) override;
		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;

	private:
		template <typename T>
		void Serialize(T& Stm);
	};

	class ExtContainer final : public Container<TechnoExt>
	{
	public:
		ExtContainer();
		~ExtContainer();

		// MUST be overridden or ExtData::InvalidatePointer is NEVER called.
		//
		// Container::PointerGotInvalid gates the whole invalidation pass behind
		// InvalidateExtDataIgnorable, and the base implementation returns true
		// (= ignore everything). Without this override our anchor/member raw
		// TechnoClass* pointers are never scrubbed, so a member outliving its
		// anchor dereferences freed memory.
		//
		// That is not hypothetical: it crashed in-game at 0x5F6467, 0x27 bytes
		// into AbstractClass::DistanceFrom (0x5F6440), reached from the follow
		// tick via FootClass::Update. Checking pAnchor->IsAlive does not help --
		// reading the flag off freed memory is already undefined and can easily
		// come back non-zero.
		virtual bool InvalidateExtDataIgnorable(void* const ptr) const override
		{
			switch (static_cast<AbstractClass*>(ptr)->WhatAmI())
			{
			case AbstractType::Unit:
			case AbstractType::Infantry:
			case AbstractType::Building:
			case AbstractType::Aircraft:
				return false; // we hold raw pointers to these
			default:
				return true;
			}
		}
	};

	static ExtContainer ExtMap;

	// Called from the Unlimbo hook: flag the unit so its roster is processed on
	// the next AI tick (when it is actually on the map). Cheap and side-effect
	// free for units with no roster.
	static void FlagForSquadSpawn(TechnoClass* pTechno);

	// Core spawn pass shared by every trigger. Only entries whose SpawnEvent
	// list contains eventFlag fire. linkToAnchor=false spawns them independent
	// (used for deathsquads, whose anchor is being destroyed).
	static void SpawnEntriesForEvent(TechnoClass* pAnchor, int eventFlag, bool linkToAnchor);

	// Called from the per-frame AI hooks: if flagged, run the `produced`
	// trigger now (one-shot) and arm the repeating trigger if any entry wants it.
	static void ProcessPendingSpawn(TechnoClass* pAnchor);

	// Called from the per-frame AI hook: run the `timer` trigger on its
	// interval. No-op unless this unit has a timer entry.
	static void ProcessSpawnTimer(TechnoClass* pAnchor);

	// Called from the death hook: run the `death` trigger (deathsquad).
	static void SpawnDeathSquad(TechnoClass* pAnchor);

	// Called from the per-frame AI hook: keep a member near its anchor. No-op
	// for anything that is not a following squad member, so units that never
	// use the feature pay one bool test.
	static void ProcessSquadFollow(TechnoClass* pMember);

	// Called from the death hook on a dying ANCHOR: arm every member's
	// AnchorDeath reaction. Nothing is mutated here beyond our own ext data --
	// killing or re-owning units inside the engine's damage path is unsafe, so
	// the actual effect runs from the member's own tick.
	static void ArmAnchorDeath(TechnoClass* pAnchor);

	// Called from the per-frame AI hook: run an armed AnchorDeath reaction once
	// its delay expires. No-op unless armed.
	static void ProcessAnchorDeath(TechnoClass* pMember);

	// The unit a click on pTechno should actually select, honouring
	// MemberSelection=anchor. Returns pTechno when no redirection applies.
	static TechnoClass* ResolveSelectionTarget(TechnoClass* pTechno);

	// Every unit that should be selected together with pTechno (Group mode).
	// Empty when this unit is not part of a grouped squad.
	static void CollectSelectionGroup(TechnoClass* pTechno, std::vector<TechnoClass*>& out);
};
