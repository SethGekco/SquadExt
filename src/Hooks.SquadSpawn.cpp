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

#include <Ext/Techno/Body.h>
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

	return 0;
}

DEFINE_HOOK(0x43FE69, BuildingClass_AI_SquadExt, 0xA)
{
	GET(BuildingClass*, pThis, ESI);

	TechnoExt::ProcessPendingSpawn(pThis);

	return 0;
}
