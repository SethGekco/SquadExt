#pragma once

// SquadExt enum types + their INI parsers.
//
// Phobos's detail::read<> generic handles ints/bools/type-pointers, but each
// enum needs its own explicit specialization. Any translation unit that reads a
// Valueable<T> of these enums must include THIS header (not <TemplateDef.h>
// directly) so the specialization is visible before the point of instantiation.

#include <Utilities/TemplateDef.h>

// Which roster entries fire when an anchor is created (unit-level).
enum class SquadEntrySelect
{
	First,      // first eligible entry only (default) -- predictable, order-driven
	All,        // every eligible entry
	Random,     // one eligible entry, uniform
	Weighted,   // one eligible entry, by SquadN.Weight
};

// In-game click behaviour for this entry's members (per-entry).
enum class SquadMemberSelection
{
	Independent,  // each unit selectable alone (default)
	Group,        // click one -> select the whole squad (old IsInitAsTeam)
	Anchor,       // click any member -> selects the anchor instead (carrier-like)
};

// Role gate: when is this roster entry allowed to fire? (per-entry)
enum class SquadActivateAs
{
	Any,          // no condition (default)
	Member,       // only when this unit is ITSELF someone's squad member
	Independent,  // only when this unit is NOT a squad member
};

// Standing order given to a member the moment it spawns, so a squad that has no
// follow behaviour still *does* something instead of standing inert.
enum class SquadStance
{
	None,       // leave the engine default
	Guard,      // hold position, engage what comes close (default)
	AreaGuard,  // patrol/defend a radius around where it stands
	Sticky,     // never move, ever
	Hunt,       // seek out enemies across the map
};

// Rank to grant a spawned member.
enum class SquadVeterancy
{
	None,     // engine default (default)
	Rookie,
	Veteran,
	Elite,
	Inherit,  // copy the anchor's current rank
};

namespace detail
{
	template <>
	inline bool read<SquadEntrySelect>(SquadEntrySelect& value, INI_EX& parser, const char* pSection, const char* pKey)
	{
		if (parser.ReadString(pSection, pKey))
		{
			auto const v = parser.value();
			if (_strcmpi(v, "first") == 0) { value = SquadEntrySelect::First; return true; }
			if (_strcmpi(v, "all") == 0) { value = SquadEntrySelect::All; return true; }
			if (_strcmpi(v, "random") == 0) { value = SquadEntrySelect::Random; return true; }
			if (_strcmpi(v, "weighted") == 0) { value = SquadEntrySelect::Weighted; return true; }
			Debug::INIParseFailed(pSection, pKey, v, "Expected first, all, random or weighted");
		}
		return false;
	}

	template <>
	inline bool read<SquadMemberSelection>(SquadMemberSelection& value, INI_EX& parser, const char* pSection, const char* pKey)
	{
		if (parser.ReadString(pSection, pKey))
		{
			auto const v = parser.value();
			if (_strcmpi(v, "independent") == 0) { value = SquadMemberSelection::Independent; return true; }
			if (_strcmpi(v, "group") == 0) { value = SquadMemberSelection::Group; return true; }
			if (_strcmpi(v, "anchor") == 0) { value = SquadMemberSelection::Anchor; return true; }
			// Back-compat with PR #1663's boolean tag spelling.
			if (_strcmpi(v, "yes") == 0 || _strcmpi(v, "true") == 0) { value = SquadMemberSelection::Group; return true; }
			if (_strcmpi(v, "no") == 0 || _strcmpi(v, "false") == 0) { value = SquadMemberSelection::Independent; return true; }
			Debug::INIParseFailed(pSection, pKey, v, "Expected independent, group or anchor");
		}
		return false;
	}

	template <>
	inline bool read<SquadActivateAs>(SquadActivateAs& value, INI_EX& parser, const char* pSection, const char* pKey)
	{
		if (parser.ReadString(pSection, pKey))
		{
			auto const v = parser.value();
			// "none"/"neither" are accepted synonyms of "any".
			if (_strcmpi(v, "any") == 0 || _strcmpi(v, "none") == 0 || _strcmpi(v, "neither") == 0)
			{ value = SquadActivateAs::Any; return true; }
			if (_strcmpi(v, "member") == 0) { value = SquadActivateAs::Member; return true; }
			if (_strcmpi(v, "independent") == 0) { value = SquadActivateAs::Independent; return true; }
			Debug::INIParseFailed(pSection, pKey, v, "Expected any, member or independent");
		}
		return false;
	}

	template <>
	inline bool read<SquadStance>(SquadStance& value, INI_EX& parser, const char* pSection, const char* pKey)
	{
		if (parser.ReadString(pSection, pKey))
		{
			auto const v = parser.value();
			if (_strcmpi(v, "none") == 0) { value = SquadStance::None; return true; }
			if (_strcmpi(v, "guard") == 0) { value = SquadStance::Guard; return true; }
			if (_strcmpi(v, "areaguard") == 0 || _strcmpi(v, "area_guard") == 0)
			{ value = SquadStance::AreaGuard; return true; }
			if (_strcmpi(v, "sticky") == 0) { value = SquadStance::Sticky; return true; }
			if (_strcmpi(v, "hunt") == 0) { value = SquadStance::Hunt; return true; }
			Debug::INIParseFailed(pSection, pKey, v, "Expected none, guard, areaguard, sticky or hunt");
		}
		return false;
	}

	template <>
	inline bool read<SquadVeterancy>(SquadVeterancy& value, INI_EX& parser, const char* pSection, const char* pKey)
	{
		if (parser.ReadString(pSection, pKey))
		{
			auto const v = parser.value();
			if (_strcmpi(v, "none") == 0) { value = SquadVeterancy::None; return true; }
			if (_strcmpi(v, "rookie") == 0) { value = SquadVeterancy::Rookie; return true; }
			if (_strcmpi(v, "veteran") == 0) { value = SquadVeterancy::Veteran; return true; }
			if (_strcmpi(v, "elite") == 0) { value = SquadVeterancy::Elite; return true; }
			if (_strcmpi(v, "inherit") == 0) { value = SquadVeterancy::Inherit; return true; }
			Debug::INIParseFailed(pSection, pKey, v, "Expected none, rookie, veteran, elite or inherit");
		}
		return false;
	}
}
