// Squad selection behaviour (SquadN.MemberSelection).
//
//   independent -- default, no hook effect: each unit selects alone.
//   group       -- clicking any squad mate selects the whole squad.
//   anchor      -- clicking a member selects its anchor instead (carrier-like).
//
// Implemented by wrapping ObjectClass::Select through the Unit / Infantry /
// Building / Aircraft vtable slots, all of which point at 0x6FBFA0.
//
// !! ENCYCLOPEDIA FOOTGUN !! YRpp declares ObjectClass::Select() with the R0
// stub macro ({ return 0; }) -- unlike most virtuals it has NO JMP_THIS
// trampoline. A qualified non-virtual call (pThis->TechnoClass::Select()) binds
// to that stub and silently no-ops: no crash, no except.txt, just dead
// selection for every unit in the game. We must call the address directly.
// TechnoAttachmentExt shipped this exact bug once; see
// YR-Hook-Encyclopedia/encyclopedia/Selection-Mouse.md.
//
// !! KNOWN CROSS-DLL CONFLICT !! DEFINE_FUNCTION_JUMP(VTABLE, ...) *replaces* a
// vtable slot rather than chaining like DEFINE_HOOK. TechnoAttachmentExt wraps
// these same four slots for its PassSelection feature. If both DLLs are loaded,
// whichever writes last wins and the other's selection feature is silently
// lost. Defaults here are `independent` so nothing changes unless a modder opts
// in, but the two features cannot currently coexist. Resolving that needs a
// shared wrapper or a merge -- tracked in DESIGN.md.

#include <TechnoClass.h>
#include <ObjectClass.h>

#include <Utilities/Macro.h>

#include <Ext/Techno/Body.h>

namespace
{
	// The real ObjectClass::Select, invoked by address (see footgun note above).
	inline bool RealSelect(TechnoClass* pTechno)
	{
		return reinterpret_cast<bool(__thiscall*)(TechnoClass*)>(0x6FBFA0)(pTechno);
	}
}

bool __fastcall TechnoClass_Select_Wrapper_SquadExt(TechnoClass* pThis)
{
	if (!pThis)
		return false;

	// anchor: redirect the click to the squad's anchor.
	if (auto const pTarget = TechnoExt::ResolveSelectionTarget(pThis))
	{
		if (pTarget != pThis)
			return RealSelect(pTarget);
	}

	// group: select this unit, then append the rest of the squad. The mates are
	// selected via RealSelect (not the virtual) so we do not re-enter this
	// wrapper and recurse through the group for each member.
	std::vector<TechnoClass*> group;
	TechnoExt::CollectSelectionGroup(pThis, group);

	bool const result = RealSelect(pThis);

	for (auto const pMate : group)
		RealSelect(pMate);

	return result;
}

DEFINE_FUNCTION_JUMP(VTABLE, 0x7F5DBC, TechnoClass_Select_Wrapper_SquadExt); // UnitClass
DEFINE_FUNCTION_JUMP(VTABLE, 0x7EB1A4, TechnoClass_Select_Wrapper_SquadExt); // InfantryClass
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E4008, TechnoClass_Select_Wrapper_SquadExt); // BuildingClass
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E23F0, TechnoClass_Select_Wrapper_SquadExt); // AircraftClass
