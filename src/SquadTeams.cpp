#include <SquadTeams.h>

#include <FootClass.h>
#include <HouseClass.h>
#include <TeamClass.h>
#include <TeamTypeClass.h>
#include <ScriptTypeClass.h>
#include <TaskForceClass.h>
#include <Memory.h>

#include <Utilities/Debug.h>
#include <Helpers/Cast.h>

#include <map>
#include <cstdio>

#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>

TeamClass* SquadTeams_Live(TeamClass* pTeam)
{
	if (!pTeam)
		return nullptr;

	// Membership in the global array is the ONLY safe liveness test here.
	// Reading a flag off a freed team is already undefined and can easily come
	// back looking valid, and the allocation may since have been reused by
	// something unrelated -- writing into it would corrupt that instead.
	return TeamClass::Array.FindItemIndex(pTeam) != -1 ? pTeam : nullptr;
}

namespace
{
	// Synthesised TeamTypes are cached and reused for the whole game. The key is
	// the script plus the roster it was built for, because the TaskForce is
	// derived from that roster -- two entries running the same script with
	// different compositions need different TaskForces.
	struct SynthKey
	{
		ScriptTypeClass* Script;
		bool Recruits;
		std::vector<std::pair<TechnoTypeClass*, int>> Roster;

		bool operator<(SynthKey const& other) const
		{
			if (Script != other.Script)
				return Script < other.Script;
			if (Recruits != other.Recruits)
				return Recruits < other.Recruits;
			return Roster < other.Roster;
		}
	};

	std::map<SynthKey, TeamTypeClass*> g_Synthesised;
	int g_SynthSerial = 0;

	std::vector<std::pair<TechnoTypeClass*, int>> RosterOf(SquadEntryData const* pEntry)
	{
		std::vector<std::pair<TechnoTypeClass*, int>> out;
		if (!pEntry)
			return out;

		// A TaskForce holds at most SIX entries (TaskForceClass::Entries[0x6]),
		// so a roster with more distinct types than that cannot be described in
		// full. Truncating is the honest failure: the team then asks for fewer
		// types than the squad fields, which leaves it under strength and, if it
		// is a recruiter, recruiting. Hence the warning.
		for (size_t k = 0; k < pEntry->SlotCount(); ++k)
		{
			auto const pType = pEntry->GetSlotType(k);
			if (!pType)
				continue;

			int const count = pEntry->GetSlotCount(k) > 0 ? pEntry->GetSlotCount(k) : 1;

			// Fold duplicate types together rather than spending two of the six
			// slots on the same thing.
			bool merged = false;
			for (auto& slot : out)
			{
				if (slot.first == pType)
				{
					slot.second += count;
					merged = true;
					break;
				}
			}

			if (!merged)
				out.emplace_back(pType, count);
		}

		if (out.size() > 6)
		{
			Debug::Log("[SquadExt] a squad roster has %u distinct types but a "
				"TaskForce holds only 6; the team will be described by the first "
				"6 and so will run under strength.\n", (unsigned)out.size());
			out.resize(6);
		}

		return out;
	}

	TeamTypeClass* SynthesiseTeam(ScriptTypeClass* pScript,
		std::vector<std::pair<TechnoTypeClass*, int>> const& roster, bool recruits)
	{
		if (!pScript || roster.empty())
			return nullptr;

		SynthKey key { pScript, recruits, roster };

		auto const it = g_Synthesised.find(key);
		if (it != g_Synthesised.end())
			return it->second;

		// The TaskForce must be REAL, never nullptr. Creating a team walks its
		// TaskForce entries unconditionally, so a null one is an immediate
		// access violation the moment CreateTeam runs (observed in FreeUnitExt
		// at 0x6EA6B4, inside the member-consideration path).
		char tfId[0x18] = { 0 };
		std::snprintf(tfId, sizeof(tfId), "SQXTF%d", g_SynthSerial);

		auto const pTaskForce = GameCreate<TaskForceClass>(tfId);
		if (!pTaskForce)
			return nullptr;

		pTaskForce->Group = -1;
		pTaskForce->IsGlobal = false;
		pTaskForce->CountEntries = (int)roster.size();

		for (size_t i = 0; i < roster.size(); ++i)
		{
			// Amount must be at least 1. Zero was tried in FreeUnitExt to stop
			// the team ever being under strength: it does prevent recruiting,
			// and it also stops the script from ever running, because a team
			// with nothing to field never starts. The roster supplies real
			// counts, so describing it exactly is what keeps the team at full
			// strength AND running.
			pTaskForce->Entries[i].Type = roster[i].first;
			pTaskForce->Entries[i].Amount = roster[i].second > 0 ? roster[i].second : 1;
		}

		char id[0x18] = { 0 };
		std::snprintf(id, sizeof(id), "SQX%d_%.12s", g_SynthSerial, pScript->ID);
		++g_SynthSerial;

		auto const pType = GameCreate<TeamTypeClass>(id);
		if (!pType)
			return nullptr;

		pType->ScriptType = pScript;
		pType->TaskForce = pTaskForce;
		pType->Group = -1;

		// Max is how many TEAMS of this type may exist at once, NOT how many
		// members one has (members come from the TaskForce amounts above). It
		// has to cover every squad alive at once that uses this script, so a
		// small value would silently refuse teams to all but the first squad.
		pType->Max = 1000;
		pType->Priority = 5;
		pType->TechLevel = 0;
		pType->VeteranLevel = 1;

		// Must never be picked up by the AI's own team logic: these exist only
		// to carry a script for a squad we created.
		pType->Autocreate = false;
		pType->Prebuild = false;
		pType->Reinforce = false;
		pType->Loadable = false;

		// The whole of TeamRecruits. With the TaskForce describing the roster
		// exactly, a non-recruiting team simply runs its script with the members
		// we give it; a recruiting one replaces losses out of the owner's army.
		pType->Recruiter = recruits;

		g_Synthesised[key] = pType;
		return pType;
	}

	// Resolve the TeamType an attachment asks for. Named teams win over scripts.
	TeamTypeClass* ResolveTeamType(std::string const& teamId, std::string const& scriptId,
		std::vector<std::pair<TechnoTypeClass*, int>> const& roster, bool recruits)
	{
		if (!teamId.empty())
		{
			if (auto const pType = TeamTypeClass::Find(teamId.c_str()))
				return pType;

			Debug::Log("[SquadExt] unknown TeamType '%s' (%d loaded) -- is it in "
				"aimd.ini?\n", teamId.c_str(), TeamTypeClass::Array.Count);
			return nullptr;
		}

		if (scriptId.empty())
			return nullptr;

		auto const pScript = ScriptTypeClass::Find(scriptId.c_str());
		if (!pScript)
		{
			Debug::Log("[SquadExt] unknown ScriptType '%s' (%d loaded) -- is it "
				"in aimd.ini?\n", scriptId.c_str(), ScriptTypeClass::Array.Count);
			return nullptr;
		}

		return SynthesiseTeam(pScript, roster, recruits);
	}
}

void SquadTeams_Retire(TeamClass* pTeam)
{
	if (auto const pLive = SquadTeams_Live(pTeam))
		pLive->NeedsToDisappear = true;
}

TeamClass* SquadTeams_JoinForEntry(TechnoClass* pAnchor, TechnoClass* pMember,
	SquadEntryData const* pEntry, int entryIndex)
{
	if (!pAnchor || !pMember || !pEntry || entryIndex < 0)
		return nullptr;

	if (pEntry->TeamID.empty() && pEntry->ScriptID.empty())
		return nullptr;

	auto const pFoot = abstract_cast<FootClass*>(pMember);
	if (!pFoot)
		return nullptr; // a building member cannot belong to a team

	auto const pAnchorExt = TechnoExt::ExtMap.Find(pAnchor);
	if (!pAnchorExt)
		return nullptr;

	auto& slots = pAnchorExt->SquadTeams;
	if ((int)slots.size() <= entryIndex)
		slots.resize(entryIndex + 1, nullptr);

	// ONE team per entry. Revalidated every time rather than trusted: the
	// engine may have destroyed it since the last member joined, in which case
	// a fresh one is created for the rest of the squad.
	auto pTeam = SquadTeams_Live(slots[entryIndex]);

	if (!pTeam)
	{
		auto const pOwner = pAnchor->Owner;
		if (!pOwner)
			return nullptr;

		auto const pType = ResolveTeamType(pEntry->TeamID, pEntry->ScriptID,
			RosterOf(pEntry), pEntry->TeamRecruits);

		if (!pType)
			return nullptr;

		pTeam = pType->CreateTeam(pOwner);
		if (!pTeam)
		{
			Debug::Log("[SquadExt] %s: could not create a team from '%s' "
				"(Max reached?).\n",
				pAnchor->GetTechnoType()->ID, pType->ID);
			return nullptr;
		}

		slots[entryIndex] = pTeam;

		Debug::Log("[SquadExt] %s(%p): created team '%s' for Squad%d "
			"(recruiter=%s).\n",
			pAnchor->GetTechnoType()->ID, pAnchor, pType->ID, entryIndex,
			pType->Recruiter ? "yes" : "no");
	}

	// bForce=true: we are placing a member we just created into the team we just
	// made for it, so the engine's own suitability check is not the authority.
	if (!pTeam->AddMember(pFoot, true))
	{
		Debug::Log("[SquadExt] %s: AddMember refused for member %s.\n",
			pAnchor->GetTechnoType()->ID, pMember->GetTechnoType()->ID);
		return nullptr;
	}

	if (auto const pMemberExt = TechnoExt::ExtMap.Find(pMember))
		pMemberExt->SquadTeam = pTeam;

	return pTeam;
}

void SquadTeams_AttachUnitTeam(TechnoClass* pTechno)
{
	if (!pTechno)
		return;

	auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pTechno->GetTechnoType());
	if (!pTypeExt)
		return;

	if (pTypeExt->SquadTeamID.empty() && pTypeExt->SquadScriptID.empty())
		return;

	auto const pExt = TechnoExt::ExtMap.Find(pTechno);
	if (!pExt)
		return;

	if (SquadTeams_Live(pExt->UnitTeam))
		return; // already has one

	auto const pOwner = pTechno->Owner;
	if (!pOwner)
		return;

	// A unit-level attachment has no squad roster to describe, so a synthesised
	// TaskForce is built from the unit's OWN type: the team then asks for one of
	// this unit, which the unit itself satisfies. A building cannot satisfy it,
	// which is exactly why a building's team recruits.
	std::vector<std::pair<TechnoTypeClass*, int>> roster;
	roster.emplace_back(pTechno->GetTechnoType(), 1);

	auto const pType = ResolveTeamType(pTypeExt->SquadTeamID, pTypeExt->SquadScriptID,
		roster, pTypeExt->SquadTeamRecruits);

	if (!pType)
		return;

	auto const pTeam = pType->CreateTeam(pOwner);
	if (!pTeam)
	{
		Debug::Log("[SquadExt] %s: could not create unit team from '%s' "
			"(Max reached?).\n", pTechno->GetTechnoType()->ID, pType->ID);
		return;
	}

	pExt->UnitTeam = pTeam;

	if (auto const pFoot = abstract_cast<FootClass*>(pTechno))
	{
		pTeam->AddMember(pFoot, true);
		pExt->SquadTeam = pTeam;
	}

	Debug::Log("[SquadExt] %s(%p): attached unit team '%s' (recruiter=%s, "
		"self %s a team member).\n",
		pTechno->GetTechnoType()->ID, pTechno, pType->ID,
		pType->Recruiter ? "yes" : "no",
		abstract_cast<FootClass*>(pTechno) ? "is" : "cannot be");
}

void SquadTeams_RetireAllFor(TechnoClass* pTechno)
{
	auto const pExt = pTechno ? TechnoExt::ExtMap.Find(pTechno) : nullptr;
	if (!pExt)
		return;

	for (auto& pTeam : pExt->SquadTeams)
	{
		SquadTeams_Retire(pTeam);
		pTeam = nullptr;
	}

	SquadTeams_Retire(pExt->UnitTeam);
	pExt->UnitTeam = nullptr;
}
