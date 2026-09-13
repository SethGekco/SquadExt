#pragma once

#include <vector>

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
	};

	static ExtContainer ExtMap;

	// Called from the Unlimbo hook: flag the unit so its roster is processed on
	// the next AI tick (when it is actually on the map). Cheap and side-effect
	// free for units with no roster.
	static void FlagForSquadSpawn(TechnoClass* pTechno);

	// Called from the per-frame AI hooks: if flagged, spawn the roster now.
	static void ProcessPendingSpawn(TechnoClass* pAnchor);

	// Called from the per-frame AI hook: keep a member near its anchor. No-op
	// for anything that is not a following squad member, so units that never
	// use the feature pay one bool test.
	static void ProcessSquadFollow(TechnoClass* pMember);

	// The unit a click on pTechno should actually select, honouring
	// MemberSelection=anchor. Returns pTechno when no redirection applies.
	static TechnoClass* ResolveSelectionTarget(TechnoClass* pTechno);

	// Every unit that should be selected together with pTechno (Group mode).
	// Empty when this unit is not part of a grouped squad.
	static void CollectSelectionGroup(TechnoClass* pTechno, std::vector<TechnoClass*>& out);
};
