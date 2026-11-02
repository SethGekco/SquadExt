// Squad spawn trigger + pointer invalidation.
//
// All hooks return 0 (pass-through, Syringe-chainable). Every address here is
// also hooked by Phobos and/or TechnoAttachmentExt; size-matched breakpoints at
// one address chain safely as long as each consumer returns "continue", which
// all of ours do.
//
// Encyclopedia-checked: 0x7258D0 (AnnounceInvalidPointer) and 0x6F6F20
// (TechnoClass::Unlimbo) are established multi-consumer chain points.

#include <TechnoClass.h>
#include <FootClass.h>
#include <BuildingClass.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <Ext/Techno/Body.h>
#include <SquadTeams.h>
#include <Ext/TechnoType/Body.h>

// ============================================================================
// Pointer invalidation -- CRITICAL. When any AbstractClass is freed the game
// broadcasts it here so everyone drops references. Our anchor/member links are
// raw TechnoClass* pointers; without this a killed member leaves a dangling
// pointer and the next selection or AI touch dereferences freed memory.
// ============================================================================
DEFINE_HOOK(0x7258D0, SquadExt_AnnounceInvalidPointer, 0x6)
{
	GET(void* const, pInvalid, ECX);
	GET(bool const, removed, EDX);

	TechnoExt::ExtMap.PointerGotInvalid(pInvalid, removed);
	TechnoTypeExt::ExtMap.PointerGotInvalid(pInvalid, removed);

	return 0;
}

// ============================================================================
// Unlimbo -- raise the pending-spawn flag. We deliberately do NOT spawn here:
// at Unlimbo entry the unit is not on the map yet, so there is no valid
// position to place members around. The AI hooks below do the work one tick
// later, when the anchor is really placed.
//
// This fires for every unlimbo (factory exit, map placement, transport exit,
// undeploy, chrono-warp), which is why ExtData::SquadsSpawned makes it a
// one-shot per unit.
// ============================================================================
DEFINE_HOOK(0x6F6F20, TechnoClass_Unlimbo_SquadExt, 0x6)
{
	GET(TechnoClass*, pThis, ESI);

	TechnoExt::FlagForSquadSpawn(pThis);

	return 0;
}

// ============================================================================
// Per-frame drivers. The body is a single already-false bool test for any unit
// without a roster, so units that never use the feature pay effectively
// nothing (pay-for-what-you-use).
// ============================================================================
DEFINE_HOOK(0x4DA8A0, FootClass_Update_SquadExt, 0x6)
{
	GET(FootClass* const, pThis, ESI);

	TechnoExt::ProcessPendingSpawn(pThis);
	TechnoExt::ProcessSpawnTimer(pThis);
	TechnoExt::ProcessSquadRegen(pThis);
	TechnoExt::ProcessOrderEcho(pThis);
	TechnoExt::ProcessAnchorDeath(pThis);
	// Before Follow: a member released from a team this frame should be walking
	// back to its anchor on the same frame, not one frame behind the AI.
	TechnoExt::ProcessSquadConscription(pThis);
	// Before Follow: refusing a foreign order releases the member back to its
	// stance, and Follow may then legitimately recall it in the same frame.
	TechnoExt::ProcessSquadOrders(pThis);
	TechnoExt::ProcessSquadFollow(pThis);

	return 0;
}

// ============================================================================
// Anchor death -- SquadN.AnchorDeath.Behavior
//
// Encyclopedia-checked: 0x702050 is the "destroyed by damage" site inside
// TechnoClass::ReceiveDamage. ESI = TechnoClass*, the unit is still present
// (coords and owner valid) but is about to be removed, and it is an established
// multi-consumer address (Antares + Kratos + Phobos all hook it, all 0x6, all
// return 0) so we chain cleanly.
//
// Deliberately NOT the destructor (0x6F4500): that fires for every removal path
// -- selling, undeploying, transforming, script removal -- which would make a
// sold anchor look like a killed one, and it is too late to read position.
//
// We only ARM the members here. Killing or re-owning units from inside the
// engine's own damage path invites re-entrancy, so the effect runs from each
// member's tick above.
// ============================================================================
DEFINE_HOOK(0x702050, TechnoClass_ReceiveDamage_AnchorDeath_SquadExt, 0x6)
{
	GET(TechnoClass* const, pThis, ESI);

	// Order matters: spawn the deathsquad BEFORE arming the survivors' reaction,
	// so freshly spawned deathsquad members (which are unlinked) are never
	// mistaken for members of the squad that is being dissolved.
	TechnoExt::SpawnDeathSquad(pThis);
	TechnoExt::ArmAnchorDeath(pThis);
	// Retire AFTER the deathsquad has spawned, so a deathsquad is not denied by
	// a team teardown, and here rather than inside ArmAnchorDeath, which returns
	// early for an anchor with no members -- a unit-level team has none. A team
	// left running would go recruiting replacements for a formation that no
	// longer exists.
	SquadTeams_RetireAllFor(pThis);

	return 0;
}

DEFINE_HOOK(0x43FE69, BuildingClass_AI_SquadExt, 0xA)
{
	GET(BuildingClass*, pThis, ESI);

	// Liveness probe for the BUILDING tick.
	//
	// Five DLLs in this install reference 0x43FE69, and Syringe only chains
	// handlers while each returns 0. So "our handler never ran" and "it ran and
	// found nothing to do" are indistinguishable from the log -- which is
	// exactly the ambiguity that let me mistake a silent log for an absent
	// squad. One line per session, raised the first time this tick sees a
	// building that actually has a roster.
	if (!TechnoExt::BuildingTickReported)
	{
		if (auto const pType = pThis->GetTechnoType())
		{
			auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
			if (pTypeExt && pTypeExt->HasSquads())
			{
				TechnoExt::BuildingTickReported = true;
				Debug::Log("[SquadExt] building tick ALIVE (first roster building: %s).\n",
					pType->ID);
			}
		}
	}

	TechnoExt::ProcessPendingSpawn(pThis);
	TechnoExt::ProcessSpawnTimer(pThis);
	TechnoExt::ProcessSquadRegen(pThis);
	TechnoExt::ProcessOrderEcho(pThis);
	TechnoExt::ProcessAnchorDeath(pThis);

	return 0;
}
