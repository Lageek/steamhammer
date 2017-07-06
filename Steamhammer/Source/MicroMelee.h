#pragma once;

#include <Common.h>
#include "MicroManager.h"

namespace UAlbertaBot
{
class MicroManager;

class MicroMelee : public MicroManager
{

public:

	MicroMelee();

	void executeMicro(const BWAPI::Unitset & targets);
	void assignTargets(const BWAPI::Unitset & targets);

	int getAttackPriority(BWAPI::Unit attacker, BWAPI::Unit unit);
	BWAPI::Unit getTarget(BWAPI::Unit meleeUnit, const BWAPI::Unitset & targets);
	BWAPI::Unit getBestTarget(BWAPI::Unit meleeUnit, const BWAPI::Unitset & targets);
	bool meleeUnitShouldRetreat(BWAPI::Unit meleeUnit, const BWAPI::Unitset & targets);
};
}