#pragma once

// AI team / script attachment for squads.
//
// Separated from Ext/Techno/Body.cpp because every function here is about the
// engine's Team subsystem rather than about squad state, and because all of the
// non-obvious constraints live in one place where they can be read together.
//
// Everything in here is deliberately defensive about team pointers. A TeamClass
// is not covered by our pointer-invalidation filter, and a team can be
// destroyed by the engine at any time, so a stored pointer is validated against
// TeamClass::Array before it is ever dereferenced -- membership in that array is
// a liveness test that never touches the pointee.

class TechnoClass;
class TeamClass;
struct SquadEntryData;

// nullptr unless pTeam is still a live team. Never dereferences pTeam.
TeamClass* SquadTeams_Live(TeamClass* pTeam);

// Get-or-create the one team belonging to this anchor's entry, and add pMember
// to it. entryIndex identifies which of the anchor's entries owns the team, so
// members of Squad0 and Squad1 do not end up sharing one.
//
// Returns the team the member joined, or nullptr if the entry wants no team,
// the lookup failed, or the member could not be added.
TeamClass* SquadTeams_JoinForEntry(TechnoClass* pAnchor, TechnoClass* pMember,
	SquadEntryData const* pEntry, int entryIndex);

// Create the unit-level team (Squad.Team= / Squad.Script=) for a unit that has
// just appeared, adding the unit itself when it is a FootClass. A building has
// nothing to add, so its team starts empty and populates by recruiting -- which
// is the whole point of attaching a team to a building.
void SquadTeams_AttachUnitTeam(TechnoClass* pTechno);

// Mark a team to disappear. Safe to call with a stale or null pointer.
void SquadTeams_Retire(TeamClass* pTeam);

// Retire every team this unit owns (its per-entry teams and its unit team).
// Called when the anchor dies, so a squad's team does not outlive the squad and
// start recruiting replacements for a formation that no longer exists.
void SquadTeams_RetireAllFor(TechnoClass* pTechno);
