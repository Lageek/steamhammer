#include "ScoutManager.h"
#include "ProductionManager.h"

using namespace UAlbertaBot;

ScoutManager::ScoutManager() 
    : _workerScout(nullptr)
	, _scoutStatus("None")
	, _gasStealStatus("None")
	, _scoutCommand(MacroCommandType::None)
	, _scoutUnderAttack(false)
	, _tryGasSteal(false)
    , _didGasSteal(false)
    , _gasStealFinished(false)
    , _currentRegionVertexIndex(-1)
    , _previousScoutHP(0)
{
}

ScoutManager & ScoutManager::Instance() 
{
	static ScoutManager instance;
	return instance;
}

// Should we send out a scout when it's time? Usually, but not always.
bool ScoutManager::shouldScout()
{
	// Called by GameCommander only when scouting is asked for, so the command should never be None.
	// We check anyway.
	if (_scoutCommand == MacroCommandType::None)
	{
		return false;
	}

	if ((_scoutCommand == MacroCommandType::ScoutIfNeeded || _scoutCommand == MacroCommandType::ScoutLocation) &&
		!_tryGasSteal)
	{
		return !InformationManager::Instance().getEnemyMainBaseLocation();
	}

	return true;
}

void ScoutManager::update()
{
	// If we're not scouting now, minimum effort.
	if (!_workerScout)
	{
		return;
	}

	// If the worker scout is gone, admit it.
	if (!_workerScout->exists() || _workerScout->getHitPoints() <= 0 ||   // it died
		_workerScout->getPlayer() != BWAPI::Broodwar->self())             // it got mind controlled!
	{
		_workerScout = nullptr;
		return;
	}

	// If we only want to locate the enemy base and we have, release the scout worker.
	if (_scoutCommand == MacroCommandType::ScoutLocation && InformationManager::Instance().getEnemyMainBaseLocation())
	{
		releaseWorkerScout();
		return;
	}

    // calculate enemy region vertices if we haven't yet
    if (_enemyRegionVertices.empty())
    {
        calculateEnemyRegionVertices();
    }

	moveScout();
    drawScoutInformation(200, 320);
}

void ScoutManager::setWorkerScout(BWAPI::Unit unit)
{
    // if we have a previous worker scout, release it back to the worker manager
	releaseWorkerScout();

    _workerScout = unit;
    WorkerManager::Instance().setScoutWorker(_workerScout);
}

// Send the scout home.
void ScoutManager::releaseWorkerScout()
{
	if (_workerScout)
	{
		WorkerManager::Instance().finishedWithWorker(_workerScout);
		_workerScout = nullptr;
	}
}

void ScoutManager::setGasSteal()
{
	_tryGasSteal = true;
}

void ScoutManager::setScoutCommand(MacroCommandType cmd)
{
	UAB_ASSERT(
		cmd == MacroCommandType::Scout ||
		cmd == MacroCommandType::ScoutIfNeeded ||
		cmd == MacroCommandType::ScoutLocation ||
		cmd == MacroCommandType::ScoutOnceOnly,
		"bad scout command");

	_scoutCommand = cmd;
}

void ScoutManager::drawScoutInformation(int x, int y)
{
    if (!Config::Debug::DrawScoutInfo)
    {
        return;
    }

    BWAPI::Broodwar->drawTextScreen(x, y, "Scout info: %s", _scoutStatus.c_str());
    BWAPI::Broodwar->drawTextScreen(x, y+10, "Gas steal: %s", _gasStealStatus.c_str());
	std::string more = "and stay in the enemy base";
	if (_scoutCommand == MacroCommandType::ScoutLocation)
	{
		more = "location";
	}
	else if (_scoutCommand == MacroCommandType::ScoutOnceOnly)
	{
		more = "once around";
	}
	else if (_scoutCommand == MacroCommandType::ScoutWhileSafe)
	{
		more = "while safe";
	}
	// NOTE "go scout if needed" doesn't need to be represented here.
	BWAPI::Broodwar->drawTextScreen(x, y + 20, "Go scout: %s", more.c_str());

    for (size_t i(0); i < _enemyRegionVertices.size(); ++i)
    {
        BWAPI::Broodwar->drawCircleMap(_enemyRegionVertices[i], 4, BWAPI::Colors::Green, false);
        BWAPI::Broodwar->drawTextMap(_enemyRegionVertices[i], "%d", i);
    }
}

void ScoutManager::moveScout()
{
    int scoutHP = _workerScout->getHitPoints() + _workerScout->getShields();
    
    gasSteal();

	// get the enemy base location, if we have one
	// Note: In case of an enemy proxy or weird map, this might be our own base. Roll with it.
	BWTA::BaseLocation * enemyBaseLocation = InformationManager::Instance().getEnemyMainBaseLocation();

    int scoutDistanceThreshold = 30;

    // if we initiated a gas steal and the worker isn't idle, 
    bool finishedConstructingGasSteal = _workerScout->isIdle() || _workerScout->isCarryingGas();
    if (!_gasStealFinished && _didGasSteal && !finishedConstructingGasSteal)
    {
        return;
    }
    // check to see if the gas steal is completed
    else if (_didGasSteal && finishedConstructingGasSteal)
    {
        _gasStealFinished = true;
    }
    
	// if we know where the enemy region is and where our scout is
	if (_workerScout && enemyBaseLocation)
	{
        int scoutDistanceToEnemy = MapTools::Instance().getGroundTileDistance(_workerScout->getPosition(), enemyBaseLocation->getPosition());
        bool scoutInRangeOfenemy = scoutDistanceToEnemy <= scoutDistanceThreshold;
        
        // we only care if the scout is under attack within the enemy region
        // this ignores if their scout worker attacks it on the way to their base
        if (scoutHP < _previousScoutHP)
        {
	        _scoutUnderAttack = true;
        }

        if (!_workerScout->isUnderAttack() && !enemyWorkerInRadius())
        {
	        _scoutUnderAttack = false;
        }

		// if the scout is in the enemy region
		if (scoutInRangeOfenemy)
		{
			if (_scoutUnderAttack)
			{
				_scoutStatus = "Under attack, fleeing";
				followPerimeter();
			}
			else
			{
				BWAPI::Unit closestWorker = enemyWorkerToHarass();

				// If configured and reasonable, harass an enemy worker.
				if (Config::Strategy::ScoutHarassEnemy && closestWorker && (!_tryGasSteal || _gasStealFinished))
				{
                    _scoutStatus = "Harass enemy worker";
                    _currentRegionVertexIndex = -1;
					Micro::SmartAttackUnit(_workerScout, closestWorker);
				}
				// otherwise keep circling the enemy region
				else
				{
                    _scoutStatus = "Following perimeter";
                    followPerimeter();  
                }
			}
		}
		// if the scout is not in the enemy region
		else if (_scoutUnderAttack)
		{
            _scoutStatus = "Under attack, fleeing";

            followPerimeter();
		}
		else
		{
            _scoutStatus = "Enemy located, going there";

			// move to the enemy region
			followPerimeter();
        }
		
	}

	// for each start location in the level
	if (!enemyBaseLocation)
	{
        _scoutStatus = "Seeking enemy base";

		for (BWTA::BaseLocation * startLocation : BWTA::getStartLocations()) 
		{
			// if we haven't explored it yet
			if (!BWAPI::Broodwar->isExplored(startLocation->getTilePosition())) 
			{
				// assign a unit to go scout it
				Micro::SmartMove(_workerScout, BWAPI::Position(startLocation->getTilePosition()));			
				return;
			}
		}
	}

    _previousScoutHP = scoutHP;
}

void ScoutManager::followPerimeter()
{
	int previousIndex = _currentRegionVertexIndex;

    BWAPI::Position fleeTo = getFleePosition();

	if (Config::Debug::DrawScoutInfo)
	{
		BWAPI::Broodwar->drawCircleMap(fleeTo, 5, BWAPI::Colors::Red, true);
	}

	// We've been told to circle the enemy base only once.
	if (_scoutCommand == MacroCommandType::ScoutOnceOnly)
	{
		// NOTE previousIndex may be -1 if we're just starting the loop.
		if (_currentRegionVertexIndex < previousIndex)
		{
			releaseWorkerScout();
			return;
		}
	}

	Micro::SmartMove(_workerScout, fleeTo);
}

void ScoutManager::gasSteal()
{
    if (!_tryGasSteal)
    {
        _gasStealStatus = "Not planned";
        return;
    }

    if (_didGasSteal)
    {
        return;
    }

    if (!_workerScout)
    {
        _gasStealStatus = "No worker scout";
        return;
    }

    BWTA::BaseLocation * enemyBaseLocation = InformationManager::Instance().getEnemyMainBaseLocation();
    if (!enemyBaseLocation)
    {
        _gasStealStatus = "Enemy base not found";
        return;
    }

    BWAPI::Unit enemyGeyser = getEnemyGeyser();
    if (!enemyGeyser)
    {
        _gasStealStatus = "Not exactly 1 enemy geyser";
        return;
    }

    if (!_didGasSteal)
    {
        ProductionManager::Instance().queueGasSteal();
        _didGasSteal = true;
        Micro::SmartMove(_workerScout, enemyGeyser->getPosition());
        _gasStealStatus = "Stealing gas";
    }
}

// Choose an enemy worker to harass, or none.
BWAPI::Unit ScoutManager::enemyWorkerToHarass() const
{
	// First look for any enemy worker that is building.
	for (const auto unit : BWAPI::Broodwar->enemy()->getUnits())
	{
		if (unit->getType().isWorker() && unit->isConstructing())
		{
			return unit;
		}
	}

	BWAPI::Unit enemyWorker = nullptr;
	int maxDist = 600;    // ignore any beyond this range

	BWAPI::Unit geyser = getEnemyGeyser();

	// Failing that, find the enemy worker closest to the gas.
	for (const auto unit : BWAPI::Broodwar->enemy()->getUnits())
	{
		if (unit->getType().isWorker())
		{
			int dist = unit->getDistance(geyser);

			if (dist < maxDist)
			{
				maxDist = dist;
				enemyWorker = unit;
			}
		}
	}

	return enemyWorker;
}

// If there is exactly 1 geyser in the enemy base, return it.
// If there's 0 we can't steal it, and if >1 then it's no use to steal it.
BWAPI::Unit ScoutManager::getEnemyGeyser() const
{
	BWTA::BaseLocation * enemyBaseLocation = InformationManager::Instance().getEnemyMainBaseLocation();

	BWAPI::Unitset geysers = enemyBaseLocation->getGeysers();
	if (geysers.size() == 1)
	{
		return *(geysers.begin());
	}

	return nullptr;
}

bool ScoutManager::enemyWorkerInRadius()
{
	for (const auto unit : BWAPI::Broodwar->enemy()->getUnits())
	{
		if (unit->getType().isWorker() && (unit->getDistance(_workerScout) < 300))
		{
			return true;
		}
	}

	return false;
}

int ScoutManager::getClosestVertexIndex(BWAPI::Unit unit)
{
    int closestIndex = -1;
    int closestDistance = 10000000;

    for (size_t i(0); i < _enemyRegionVertices.size(); ++i)
    {
        int dist = unit->getDistance(_enemyRegionVertices[i]);
        if (dist < closestDistance)
        {
            closestDistance = dist;
            closestIndex = i;
        }
    }

    return closestIndex;
}

BWAPI::Position ScoutManager::getFleePosition()
{
    UAB_ASSERT_WARNING(!_enemyRegionVertices.empty(), "should have enemy region vertices");
    
    BWTA::BaseLocation * enemyBaseLocation = InformationManager::Instance().getEnemyMainBaseLocation();

    // if this is the first flee, we will not have a previous perimeter index
    if (_currentRegionVertexIndex == -1)
    {
        // so return the closest position in the polygon
        int closestPolygonIndex = getClosestVertexIndex(_workerScout);

        UAB_ASSERT_WARNING(closestPolygonIndex != -1, "Couldn't find a closest vertex");

        if (closestPolygonIndex == -1)
        {
            return BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation());
        }

        // set the current index so we know how to iterate if we are still fleeing later
        _currentRegionVertexIndex = closestPolygonIndex;
        return _enemyRegionVertices[closestPolygonIndex];
    }

    // if we are still fleeing from the previous frame, get the next location if we are close enough
    double distanceFromCurrentVertex = _enemyRegionVertices[_currentRegionVertexIndex].getDistance(_workerScout->getPosition());

    // keep going to the next vertex in the perimeter until we get to one we're far enough from to issue another move command
    while (distanceFromCurrentVertex < 128)
    {
        _currentRegionVertexIndex = (_currentRegionVertexIndex + 1) % _enemyRegionVertices.size();

        distanceFromCurrentVertex = _enemyRegionVertices[_currentRegionVertexIndex].getDistance(_workerScout->getPosition());
    }

    return _enemyRegionVertices[_currentRegionVertexIndex];
}

// NOTE This algorithm sometimes produces bizarre paths when unbuildable tiles
//      are found deep inside the enemy base.
// TODO Eventually replace with real-time pathing that tries to see as much as possible
//      while remaining safe.
void ScoutManager::calculateEnemyRegionVertices()
{
    BWTA::BaseLocation * enemyBaseLocation = InformationManager::Instance().getEnemyMainBaseLocation();

    if (!enemyBaseLocation)
    {
        return;
    }

    BWTA::Region * enemyRegion = enemyBaseLocation->getRegion();

    if (!enemyRegion)
    {
        return;
    }

    const BWAPI::Position basePosition = BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation());
    const std::vector<BWAPI::TilePosition> & closestTobase = MapTools::Instance().getClosestTilesTo(basePosition);

    std::set<BWAPI::Position> unsortedVertices;

    // check each tile position
	for (size_t i(0); i < closestTobase.size(); ++i)
	{
		const BWAPI::TilePosition & tp = closestTobase[i];

		if (BWTA::getRegion(tp) != enemyRegion)
		{
			continue;
		}

		// a tile is 'on an edge' unless
		// 1) in all 4 directions there's a tile position in the current region
		// 2) in all 4 directions there's a buildable tile
		bool edge =
			   BWTA::getRegion(BWAPI::TilePosition(tp.x + 1, tp.y)) != enemyRegion || !BWAPI::Broodwar->isBuildable(BWAPI::TilePosition(tp.x + 1, tp.y))
			|| BWTA::getRegion(BWAPI::TilePosition(tp.x, tp.y + 1)) != enemyRegion || !BWAPI::Broodwar->isBuildable(BWAPI::TilePosition(tp.x, tp.y + 1))
			|| BWTA::getRegion(BWAPI::TilePosition(tp.x - 1, tp.y)) != enemyRegion || !BWAPI::Broodwar->isBuildable(BWAPI::TilePosition(tp.x - 1, tp.y))
			|| BWTA::getRegion(BWAPI::TilePosition(tp.x, tp.y - 1)) != enemyRegion || !BWAPI::Broodwar->isBuildable(BWAPI::TilePosition(tp.x, tp.y - 1));

		// push the tiles that aren't surrounded
		if (edge && BWAPI::Broodwar->isBuildable(tp))
		{
			if (Config::Debug::DrawScoutInfo)
			{
				int x1 = tp.x * 32 + 2;
				int y1 = tp.y * 32 + 2;
				int x2 = (tp.x + 1) * 32 - 2;
				int y2 = (tp.y + 1) * 32 - 2;

				BWAPI::Broodwar->drawTextMap(x1 + 3, y1 + 2, "%d", MapTools::Instance().getGroundTileDistance(BWAPI::Position(tp), basePosition));
				BWAPI::Broodwar->drawBoxMap(x1, y1, x2, y2, BWAPI::Colors::Green, false);
			}

			unsortedVertices.insert(BWAPI::Position(tp) + BWAPI::Position(16, 16));
		}
	}

    std::vector<BWAPI::Position> sortedVertices;
    BWAPI::Position current = *unsortedVertices.begin();

    _enemyRegionVertices.push_back(current);
    unsortedVertices.erase(current);

    // while we still have unsorted vertices left, find the closest one remaining to current
    while (!unsortedVertices.empty())
    {
        double bestDist = 1000000;
        BWAPI::Position bestPos;

        for (const BWAPI::Position & pos : unsortedVertices)
        {
            double dist = pos.getDistance(current);

            if (dist < bestDist)
            {
                bestDist = dist;
                bestPos = pos;
            }
        }

        current = bestPos;
        sortedVertices.push_back(bestPos);
        unsortedVertices.erase(bestPos);
    }

    // let's close loops on a threshold, eliminating death grooves
    int distanceThreshold = 100;

    while (true)
    {
        // find the largest index difference whose distance is less than the threshold
        int maxFarthest = 0;
        int maxFarthestStart = 0;
        int maxFarthestEnd = 0;

        // for each starting vertex
        for (int i(0); i < (int)sortedVertices.size(); ++i)
        {
            int farthest = 0;
            int farthestIndex = 0;

            // only test half way around because we'll find the other one on the way back
            for (size_t j(1); j < sortedVertices.size()/2; ++j)
            {
                int jindex = (i + j) % sortedVertices.size();
            
                if (sortedVertices[i].getDistance(sortedVertices[jindex]) < distanceThreshold)
                {
                    farthest = j;
                    farthestIndex = jindex;
                }
            }

            if (farthest > maxFarthest)
            {
                maxFarthest = farthest;
                maxFarthestStart = i;
                maxFarthestEnd = farthestIndex;
            }
        }
        
        // stop when we have no long chains within the threshold
        if (maxFarthest < 4)
        {
            break;
        }

        std::vector<BWAPI::Position> temp;

        for (size_t s(maxFarthestEnd); s != maxFarthestStart; s = (s+1) % sortedVertices.size())
        {
            temp.push_back(sortedVertices[s]);
        }

        sortedVertices = temp;
    }

    _enemyRegionVertices = sortedVertices;
}