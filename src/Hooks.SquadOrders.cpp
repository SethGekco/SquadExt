// Player-order refusal -- SquadN.Orderable / SquadN.OrderRange.
//
// Every order a human gives becomes a MEGAMISSION event, and EventClass::Execute
// is where that event is turned into a mission on the unit. Hooking it is what
// makes "this member does not take orders" actually true: the alternative,
// watching for the resulting mission on the unit's own tick, cannot tell a
// player's order from the engine's and is always a frame late.
//
// 0x4C7217 sits in the MEGAMISSION branch after the engine has resolved and
// validated the target techno (EDI) and before any of Mission / Target /
// Destination / Follow is applied. The six bytes there are one whole
// instruction (8A 87 81 00 00 00, mov al, [edi+0x81]), and no other DLL in the
// install references the address.
//
// 0x4C8109 is the branch's own bail-out: every validity check above it
// (dead, no health, in limbo) jumps there to abandon the event. Phobos uses the
// same address under the name DoNotExecute, so this is the engine's own way of
// dropping an order rather than a control-flow shortcut of ours.

#include <TechnoClass.h>
#include <EventClass.h>

#include <Utilities/Macro.h>

#include <Ext/Techno/Body.h>

DEFINE_HOOK(0x4C7217, EventClass_Execute_MegaMission_SquadExt, 0x6)
{
	enum { DoNotExecute = 0x4C8109 };

	GET(EventClass* const, pThis, ESI);
	GET(TechnoClass* const, pTechno, EDI);

	if (TechnoExt::RefusePlayerOrder(pTechno,
		static_cast<Mission>(pThis->MegaMission.Mission),
		pThis->MegaMission.Target,
		pThis->MegaMission.Destination))
	{
		return DoNotExecute;
	}

	return 0;
}
