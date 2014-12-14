/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file roadstop.cpp Implementation of the roadstop base class. */

#include "stdafx.h"
#include "roadveh.h"
#include "core/pool_func.hpp"
#include "roadstop_base.h"
#include "station_base.h"
#include "map/road.h"

/** The pool of roadstops. */
template<> RoadStop::Pool RoadStop::PoolItem::pool ("RoadStop");
INSTANTIATE_POOL_METHODS(RoadStop)

/**
 * De-Initializes RoadStops.
 */
RoadStop::~RoadStop()
{
	/* When we are the head we need to free the entries */
	if (HasBit(this->status, RSSFB_BASE_ENTRY)) {
		delete this->platform;
	}

	if (CleaningPool()) return;
}

/**
 * Join this road stop to another 'base' road stop if possible;
 * fill all necessary data to become an actual drive through road stop.
 * Also update the length etc.
 */
void RoadStop::MakeDriveThrough()
{
	assert (this->platform == NULL);

	RoadStopType rst = GetRoadStopType(this->xy);
	/* AxisToDiagDir always returns the direction that heads south. */
	TileIndexDiff offset = TileOffsByDiagDir(AxisToDiagDir(GetRoadStopAxis(this->xy)));

	/* Information about the tile north of us */
	TileIndex north_tile = this->xy - offset;
	bool north = IsDriveThroughRoadStopContinuation(this->xy, north_tile);
	RoadStop *rs_north = north ? RoadStop::GetByTile(north_tile, rst) : NULL;

	/* Information about the tile south of us */
	TileIndex south_tile = this->xy + offset;
	bool south = IsDriveThroughRoadStopContinuation(this->xy, south_tile);
	RoadStop *rs_south = south ? RoadStop::GetByTile(south_tile, rst) : NULL;

	/* Amount of road stops that will be added to the 'northern' head */
	int added = 1;
	if (north && rs_north->platform != NULL) {
		/* There is a more northern one, so this can join them */
		this->platform = rs_north->platform;

		if (south && rs_south->platform != NULL) {
			/* There more southern tiles too, they must 'join' us too */
			ClrBit(rs_south->status, RSSFB_BASE_ENTRY);
			this->platform->occupied_east += rs_south->platform->occupied_east;
			this->platform->occupied_west += rs_south->platform->occupied_west;

			/* Free the now unneeded entry structs */
			delete rs_south->platform;

			/* Make all 'children' of the southern tile take the new master */
			for (; IsDriveThroughRoadStopContinuation(this->xy, south_tile); south_tile += offset) {
				rs_south = RoadStop::GetByTile(south_tile, rst);
				if (rs_south->platform == NULL) break;
				rs_south->platform = rs_north->platform;
				added++;
			}
		}
	} else if (south && rs_south->platform != NULL) {
		/* There is one to the south, but not to the north... so we become 'parent' */
		this->platform = rs_south->platform;
		SetBit(this->status, RSSFB_BASE_ENTRY);
		ClrBit(rs_south->status, RSSFB_BASE_ENTRY);
	} else {
		/* We are the only... so we are automatically the master */
		this->platform = new Platform();
		SetBit(this->status, RSSFB_BASE_ENTRY);
	}

	/* Now update the lengths */
	added *= TILE_SIZE;
	this->platform->length += added;
}

/**
 * Prepare for removal of this stop; update other neighbouring stops
 * if needed. Also update the length etc.
 */
void RoadStop::ClearDriveThrough()
{
	assert (this->platform != NULL);

	RoadStopType rst = GetRoadStopType(this->xy);
	/* AxisToDiagDir always returns the direction that heads south. */
	TileIndexDiff offset = TileOffsByDiagDir(AxisToDiagDir(GetRoadStopAxis(this->xy)));

	/* Information about the tile north of us */
	TileIndex north_tile = this->xy - offset;
	bool north = IsDriveThroughRoadStopContinuation(this->xy, north_tile);
	RoadStop *rs_north = north ? RoadStop::GetByTile(north_tile, rst) : NULL;

	/* Information about the tile south of us */
	TileIndex south_tile = this->xy + offset;
	bool south = IsDriveThroughRoadStopContinuation(this->xy, south_tile);
	RoadStop *rs_south = south ? RoadStop::GetByTile(south_tile, rst) : NULL;

	/* Must only be cleared after we determined which neighbours are
	 * part of our little entry 'queue' */
	DoClearSquare(this->xy);

	if (north) {
		/* There is a tile to the north, so we can't clear ourselves. */
		if (south) {
			/* There are more southern tiles too, they must be split;
			 * first make the new southern 'base' */
			SetBit(rs_south->status, RSSFB_BASE_ENTRY);
			rs_south->platform = new Platform();

			/* Keep track of the base because we need it later on */
			RoadStop *rs_south_base = rs_south;
			TileIndex base_tile = south_tile;

			/* Make all (even more) southern stops part of the new entry queue */
			for (south_tile += offset; IsDriveThroughRoadStopContinuation(base_tile, south_tile); south_tile += offset) {
				rs_south = RoadStop::GetByTile(south_tile, rst);
				rs_south->platform = rs_south_base->platform;
			}

			/* Find the other end; the northern most tile */
			for (; IsDriveThroughRoadStopContinuation(base_tile, north_tile); north_tile -= offset) {
				rs_north = RoadStop::GetByTile(north_tile, rst);
			}

			/* We have to rebuild the entries because we cannot easily determine
			 * how full each part is. So instead of keeping and maintaining a list
			 * of vehicles and using that to 'rebuild' the occupied state we just
			 * rebuild it from scratch as that removes lots of maintenance code
			 * for the vehicle list and it's faster in real games as long as you
			 * do not keep split and merge road stop every tick by the millions. */
			rs_south_base->Rebuild();

			assert(HasBit(rs_north->status, RSSFB_BASE_ENTRY));
			rs_north->Rebuild();
		} else {
			/* Only we left, so simple update the length. */
			rs_north->platform->length -= TILE_SIZE;
		}
	} else if (south) {
		/* There is only something to the south. Hand over the base entry */
		SetBit(rs_south->status, RSSFB_BASE_ENTRY);
		rs_south->platform->length -= TILE_SIZE;
	} else {
		/* We were the last */
		delete this->platform;
	}

	/* Make sure we don't get used for something 'incorrect' */
	ClrBit(this->status, RSSFB_BASE_ENTRY);
	this->platform = NULL;
}

/**
 * Find a roadstop at given tile
 * @param tile tile with roadstop
 * @param type roadstop type
 * @return pointer to RoadStop
 * @pre there has to be roadstop of given type there!
 */
/* static */ RoadStop *RoadStop::GetByTile(TileIndex tile, RoadStopType type)
{
	const Station *st = Station::GetByTile(tile);

	for (RoadStop *rs = st->GetPrimaryRoadStop(type);; rs = rs->next) {
		if (rs->xy == tile) return rs;
		assert(rs->next != NULL);
	}
}

/**
 * Checks whether the 'next' tile is still part of the road same drive through
 * stop 'rs' in the same direction for the same vehicle.
 * @param rs   the road stop tile to check against
 * @param next the 'next' tile to check
 * @return true if the 'next' tile is part of the road stop at 'next'.
 */
/* static */ bool RoadStop::IsDriveThroughRoadStopContinuation(TileIndex rs, TileIndex next)
{
	return IsStationTile(next) &&
			GetStationIndex(next) == GetStationIndex(rs) &&
			GetStationType(next) == GetStationType(rs) &&
			IsDriveThroughStopTile(next) &&
			GetRoadStopAxis(next) == GetRoadStopAxis(rs);
}

/**
 * Rebuild, from scratch, the vehicles and other metadata on this platform.
 * @param tile The northernmost tile of the platform
 */
void RoadStop::Platform::Rebuild (TileIndex tile)
{
	typedef std::list<const RoadVehicle *> RVList; ///< A list of road vehicles

	DiagDirection dir = GetRoadStopDir (tile);
	TileIndexDiff offset = abs(TileOffsByDiagDir(dir));
	Direction dir_east = DiagDirToDir (dir);

	int length = 0;
	RVList list_east, list_west;
	for (TileIndex t = tile; IsDriveThroughRoadStopContinuation (tile, t); t += offset) {
		length += TILE_SIZE;
		VehicleTileIterator iter (t);
		while (!iter.finished()) {
			Vehicle *v = iter.next();

			/* Not a RV or not primary or crashed :( */
			if (v->type != VEH_ROAD || !v->IsPrimaryVehicle() || (v->vehstatus & VS_CRASHED) != 0) continue;

			RoadVehicle *rv = RoadVehicle::From(v);
			/* Don't add ones not in a road stop */
			if (rv->state < RVSB_IN_ROAD_STOP) continue;

			assert (v->direction == dir_east || v->direction == ReverseDir (dir_east));
			RVList *list = (v->direction == dir_east) ? &list_east : &list_west;

			/* Do not add duplicates! */
			RVList::iterator it;
			for (it = list->begin(); it != list->end(); it++) {
				if (rv == *it) break;
			}

			if (it == list->end()) list->push_back(rv);
		}
	}

	this->length = length;

	this->occupied_east = 0;
	for (RVList::iterator it = list_east.begin(); it != list_east.end(); it++) {
		this->occupied_east += (*it)->gcache.cached_total_length;
	}

	this->occupied_west = 0;
	for (RVList::iterator it = list_west.begin(); it != list_west.end(); it++) {
		this->occupied_west += (*it)->gcache.cached_total_length;
	}
}

/**
 * Check the integrity of the data in this drive-through road stop.
 */
void RoadStop::CheckIntegrity (void) const
{
	if (!HasBit (this->status, RSSFB_BASE_ENTRY)) return;

	/* The tile 'before' the road stop must not be part of this 'line' */
	assert (!IsDriveThroughRoadStopContinuation (this->xy, this->xy - TileOffsByDiagDir (AxisToDiagDir (GetRoadStopAxis (this->xy)))));

	Platform temp;
	temp.Rebuild (this->xy);

	assert (this->platform->length == temp.length);
	assert (this->platform->occupied_east == temp.occupied_east);
	assert (this->platform->occupied_west == temp.occupied_west);
}
