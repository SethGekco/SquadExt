// Squad selection behaviour (SquadN.MemberSelection).
//
//   independent -- default, no behavioural change: each unit selects alone.
//   group       -- clicking any squad mate selects the whole squad.
//   anchor      -- clicking a member selects its anchor instead (carrier-like).
//
// Implemented by wrapping ObjectClass::Select through the Unit / Infantry /
// Building / Aircraft vtable slots, all of which point at 0x6FBFA0 by default.
//
// !! ENCYCLOPEDIA FOOTGUN !! YRpp declares ObjectClass::Select() with the R0
// stub macro ({ return 0; }) -- unlike most virtuals it has NO JMP_THIS
// trampoline. A qualified non-virtual call (pThis->TechnoClass::Select()) binds
// to that stub and silently no-ops: no crash, no except.txt, just dead
// selection for every unit in the game. We must reach the previous handler by
// address. See YR-Hook-Encyclopedia/encyclopedia/Selection-Mouse.md.
//
// ---------------------------------------------------------------------------
// VTABLE CHAINING
//
// DEFINE_HOOK breakpoints chain -- Syringe runs every consumer at an address.
// DEFINE_FUNCTION_JUMP(VTABLE, ...) does NOT: it blindly overwrites the slot,
// so the last DLL to patch wins and every earlier wrapper is silently lost.
// TechnoAttachmentExt wraps these same four slots for its PassSelection
// feature, so a blind overwrite means the two DLLs cannot coexist -- whichever
// loads second erases the other's selection behaviour.
//
// Fix: read each slot BEFORE patching it, cache whatever was there, and have
// the wrapper tail-call that cached pointer instead of a hardcoded 0x6FBFA0.
// That turns a destructive overwrite into a proper chain:
//
//   TAExt first:  Squad -> TAExt -> real Select
//   Squad first:  TAExt -> Squad -> real Select   (once TAExt does the same)
//
// Either order composes, and the chain terminates at the real function because
// the first DLL to patch caches the genuine 0x6FBFA0.
// ---------------------------------------------------------------------------

#include <TechnoClass.h>
#include <ObjectClass.h>

#include <Utilities/Macro.h>
#include <Utilities/Patch.h>
#include <Utilities/Debug.h>

#include <Ext/Techno/Body.h>

namespace
{
	using SelectFn = bool(__thiscall*)(TechnoClass*);

	// The genuine ObjectClass::Select. Only used as the terminal fallback.
	constexpr DWORD RealSelectAddr = 0x6FBFA0;

	// Select vtable slots. None of Unit/Infantry/Building/Aircraft override
	// Select, so all four hold the same pointer in a clean game.
	constexpr DWORD VTable_Unit     = 0x7F5DBC;
	constexpr DWORD VTable_Infantry = 0x7EB1A4;
	constexpr DWORD VTable_Building = 0x7E4008;
	constexpr DWORD VTable_Aircraft = 0x7E23F0;

	// Whatever occupied each slot before we patched it: the real function, or
	// another DLL's wrapper if it got there first.
	SelectFn g_PrevUnit = nullptr;
	SelectFn g_PrevInfantry = nullptr;
	SelectFn g_PrevBuilding = nullptr;
	SelectFn g_PrevAircraft = nullptr;

	bool g_Installed = false;

	// Dispatch on the object's actual class so each chains through its own
	// slot's predecessor. They are usually identical, but only usually.
	SelectFn PrevFor(TechnoClass* pThis)
	{
		switch (pThis->WhatAmI())
		{
		case AbstractType::Unit:      return g_PrevUnit;
		case AbstractType::Infantry:  return g_PrevInfantry;
		case AbstractType::Building:  return g_PrevBuilding;
		case AbstractType::Aircraft:  return g_PrevAircraft;
		default:                      return nullptr;
		}
	}

	// Continue down the chain. Falls back to the real function if a slot was
	// never captured (unknown class, or install ran late).
	bool CallPrev(TechnoClass* pThis)
	{
		if (auto const prev = PrevFor(pThis))
			return prev(pThis);

		return reinterpret_cast<SelectFn>(RealSelectAddr)(pThis);
	}
}

// __fastcall with a single pointer arg matches __thiscall's ECX convention for
// a no-argument method, which is what a vtable slot expects here. (A dummy EDX
// param is only needed once the wrapped method takes stack arguments.)
bool __fastcall TechnoClass_Select_Wrapper_SquadExt(TechnoClass* pThis)
{
	if (!pThis)
		return false;

	// anchor: redirect the click to the squad's anchor. Routed through the full
	// virtual call so any other DLL's wrapper still gets to see the anchor.
	if (auto const pTarget = TechnoExt::ResolveSelectionTarget(pThis))
	{
		if (pTarget != pThis)
			return pTarget->Select();
	}

	// group: select this unit, then append the rest of the squad. Mates go
	// through CallPrev rather than the virtual so we do not re-enter this
	// wrapper and re-expand the group once per member.
	std::vector<TechnoClass*> group;
	TechnoExt::CollectSelectionGroup(pThis, group);

	bool const result = CallPrev(pThis);

	for (auto const pMate : group)
		CallPrev(pMate);

	return result;
}

// Called from ExeRun, after Patch::ApplyStatic. Deliberately NOT a
// DEFINE_FUNCTION_JUMP -- see the chaining note above.
void SquadExt_InstallSelectWrappers()
{
	if (g_Installed)
		return;
	g_Installed = true;

	auto const self = reinterpret_cast<void*>(&TechnoClass_Select_Wrapper_SquadExt);

	// Read first, patch second. Anything already in the slot becomes our tail.
	auto const capture = [self](DWORD slot, SelectFn& out)
	{
		auto const current = *reinterpret_cast<SelectFn*>(slot);

		// Guard against capturing ourselves (a double install would otherwise
		// build an infinite self-recursive chain and blow the stack).
		out = (reinterpret_cast<void*>(current) == self)
			? reinterpret_cast<SelectFn>(RealSelectAddr)
			: current;

		Patch::Apply_VTABLE(slot, self);
	};

	capture(VTable_Unit, g_PrevUnit);
	capture(VTable_Infantry, g_PrevInfantry);
	capture(VTable_Building, g_PrevBuilding);
	capture(VTable_Aircraft, g_PrevAircraft);
}

// Reported by the rules-parse log line, which is a logging path we have
// VERIFIED reaches debug.log.
//
// Why not just log from inside the install: we tried, twice, and the line never
// appeared even when the install provably ran immediately before a log call that
// did appear. Rather than keep guessing at the logger's readiness, the status is
// now piggybacked onto a message already known to work. Lesson: a liveness probe
// is only worth anything on a channel whose liveness you have already proven.
bool SquadExt_SelectWrappersInstalled()
{
	return g_Installed;
}

// The handler each slot chained to, for diagnostics. 0x6FBFA0 means we were
// first; anything else means another DLL had already wrapped that slot.
unsigned int SquadExt_PrevSelectHandler()
{
	return reinterpret_cast<unsigned int>(g_PrevUnit);
}
