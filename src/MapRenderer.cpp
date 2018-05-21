/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Stefan Beller
Copyright © 2013-2014 Henrik Andersson
Copyright © 2013 Kurt Rinnert
Copyright © 2012-2016 Justin Jacobs

This file is part of FLARE.

FLARE is free software: you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.

FLARE is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
FLARE.  If not, see http://www.gnu.org/licenses/
*/

#include "Avatar.h"
#include "CampaignManager.h"
#include "CombatText.h"
#include "CommonIncludes.h"
#include "CursorManager.h"
#include "Enemy.h"
#include "EnemyGroupManager.h"
#include "EnemyManager.h"
#include "EventManager.h"
#include "Hazard.h"
#include "HazardManager.h"
#include "InputState.h"
#include "MapRenderer.h"
#include "MenuDevConsole.h"
#include "MenuManager.h"
#include "PowerManager.h"
#include "RenderDevice.h"
#include "SharedGameResources.h"
#include "SharedResources.h"
#include "SoundManager.h"
#include "StatBlock.h"
#include "UtilsFileSystem.h"
#include "UtilsMath.h"
#include "WidgetTooltip.h"

#include <stdint.h>
#include <limits>
#include <math.h>

MapRenderer::MapRenderer()
	: Map()
	, tip(new WidgetTooltip())
	, tip_pos()
	, show_tooltip(false)
	, shakycam()
	, cam()
	, map_change(false)
	, teleportation(false)
	, teleport_destination()
	, respawn_point()
	, cutscene(false)
	, cutscene_file("")
	, shaky_cam_ticks(0)
	, stash(false)
	, stash_pos()
	, enemies_cleared(false)
	, save_game(false)
	, npc_id(-1)
	, show_book("")
	, index_objectlayer(0)
{
}

void MapRenderer::clearQueues() {
	Map::clearQueues();
	loot.clear();
}

bool MapRenderer::enemyGroupPlaceEnemy(float x, float y, Map_Group &g) {
	if (collider.is_empty(x, y)) {
		Enemy_Level enemy_lev = enemyg->getRandomEnemy(g.category, g.levelmin, g.levelmax);
		if (!enemy_lev.type.empty()) {
			Map_Enemy group_member = Map_Enemy(enemy_lev.type, FPoint(x, y));

			group_member.direction = (g.direction == -1 ? rand()%8 : g.direction);
			group_member.wander_radius = g.wander_radius;
			group_member.requires_status = g.requires_status;
			group_member.requires_not_status = g.requires_not_status;
			group_member.invincible_requires_status = g.invincible_requires_status;
			group_member.invincible_requires_not_status = g.invincible_requires_not_status;

			if (g.area.x == 1 && g.area.y == 1) {
				// this is a single enemy
				group_member.waypoints = g.waypoints;
			}

			enemies.push(group_member);
		}
		return true;
	}
	return false;
}

void MapRenderer::pushEnemyGroup(Map_Group &g) {
	// activate at all?
	float activate_chance = static_cast<float>(rand() % 100) / 100.0f;
	if (activate_chance > g.chance) {
		return;
	}

	// The algorithm tries to place the enemies at random locations.
	// However if a location is not possible (unwalkable or there is already an entity),
	// then try again.
	// This could result in an infinite loop if there were more enemies than
	// actual places, so have an upper bound of tries.

	// random number of enemies
	int enemies_to_spawn = randBetween(g.numbermin, g.numbermax);

	// pick an upper bound, which is definitely larger than threetimes the enemy number to spawn.
	int allowed_misses = 5 * g.numbermax;

	while (enemies_to_spawn > 0 && allowed_misses > 0) {

		float x = (g.area.x == 0) ? (static_cast<float>(g.pos.x) + 0.5f) : (static_cast<float>(g.pos.x + (rand() % g.area.x))) + 0.5f;
		float y = (g.area.y == 0) ? (static_cast<float>(g.pos.y) + 0.5f) : (static_cast<float>(g.pos.y + (rand() % g.area.y))) + 0.5f;

		if (enemyGroupPlaceEnemy(x, y, g))
			enemies_to_spawn--;
		else
			allowed_misses--;
	}
	if (enemies_to_spawn > 0) {
		// now that the fast method of spawning enemies doesn't work, but we
		// still have enemies to place, do not place them randomly, but at the
		// first free spot
		for (int x = g.pos.x; x < g.pos.x + g.area.x && enemies_to_spawn > 0; x++) {
			for (int y = g.pos.y; y < g.pos.y + g.area.y && enemies_to_spawn > 0; y++) {
				float xpos = static_cast<float>(x) + 0.5f;
				float ypos = static_cast<float>(y) + 0.5f;
				if (enemyGroupPlaceEnemy(xpos, ypos, g))
					enemies_to_spawn--;
			}
		}

	}
	if (enemies_to_spawn > 0) {
		logError("MapRenderer: Could not spawn all enemies in group at %s (x=%d,y=%d,w=%d,h=%d), %d missing (min=%d max=%d)",
				filename.c_str(), g.pos.x, g.pos.y, g.area.x, g.area.y, enemies_to_spawn, g.numbermin, g.numbermax);
	}
}

/**
 * No guarantee that maps will use all layers
 * Clear all tile layers (e.g. when loading a map)
 */
void MapRenderer::clearLayers() {
	Map::clearLayers();
	index_objectlayer = 0;
}

int MapRenderer::load(const std::string& fname) {
	// unload sounds
	snd->reset();
	while (!sids.empty()) {
		snd->unload(sids.back());
		sids.pop_back();
	}

	// clear enemy spawn queue
	while (!powers->map_enemies.empty()) {
		powers->map_enemies.pop();
	}

	// clear combat text
	comb->clear();

	show_tooltip = false;

	parallax_filename = "";

	background_color = Color(0,0,0,0);

	Map::load(fname);

	loadMusic();

	for (unsigned i = 0; i < layers.size(); ++i) {
		if (layernames[i] == "collision") {
			short width = static_cast<short>(layers[i].size());
			if (width == 0) {
				logError("MapRenderer: Map width is 0. Can't set collision layer.");
				break;
			}
			short height = static_cast<short>(layers[i][0].size());
			collider.setmap(layers[i], width, height);
			removeLayer(i);
		}
	}
	for (unsigned i = 0; i < layers.size(); ++i)
		if (layernames[i] == "object")
			index_objectlayer = i;

	while (!enemy_groups.empty()) {
		pushEnemyGroup(enemy_groups.front());
		enemy_groups.pop();
	}

	tset.load(this->tileset);

	std::vector<unsigned> corrupted;
	for (unsigned i = 0; i < layers.size(); ++i) {
		for (unsigned x = 0; x < layers[i].size(); ++x) {
			for (unsigned y = 0; y < layers[i][x].size(); ++y) {
				const unsigned tile_id = layers[i][x][y];
				if (tile_id > 0 && (tile_id >= tset.tiles.size() || tset.tiles[tile_id].tile == NULL)) {
					if (std::find(corrupted.begin(), corrupted.end(), tile_id) == corrupted.end()) {
						corrupted.push_back(tile_id);
					}
					layers[i][x][y] = 0;
				}
			}
		}
	}

	if (!corrupted.empty()) {
		logError("MapRenderer: Tileset or Map corrupted. A tile has a larger id than the tileset allows or is undefined.");
		while (!corrupted.empty()) {
			logError("MapRenderer: Removing offending tile id %d.", corrupted.back());
			corrupted.pop_back();
		}
	}

	map_parallax.load(parallax_filename);
	map_parallax.setMapCenter(w/2, h/2);

	render_device->setBackgroundColor(background_color);

	return 0;
}

void MapRenderer::loadMusic() {
	if (!AUDIO) return;

	if (MUSIC_VOLUME > 0) {
		// load and play music
		snd->loadMusic(music_filename);
	}
	else {
		snd->stopMusic();
	}
}

void MapRenderer::logic(bool paused) {

	// handle tile set logic e.g. animations
	tset.logic();

	// TODO there's a bit too much "logic" here for a class that's supposed to be dedicated to rendering
	// some of these timers should be moved out at some point
	if (paused)
		return;

	// handle camera shaking timer
	if (shaky_cam_ticks > 0) shaky_cam_ticks--;

	if (shaky_cam_ticks == 0) {
		shakycam.x = cam.x;
		shakycam.y = cam.y;
	}
	else {
		shakycam.x = cam.x + static_cast<float>((rand() % 16 - 8)) * 0.0078125f;
		shakycam.y = cam.y + static_cast<float>((rand() % 16 - 8)) * 0.0078125f;
	}


	// handle statblock logic for map powers
	for (unsigned i=0; i<statblocks.size(); ++i) {
		statblocks[i].logic();
	}

	// handle event cooldowns
	std::vector<Event>::iterator it;
	for (it = events.begin(); it < events.end(); ++it) {
		if ((*it).cooldown_ticks > 0) (*it).cooldown_ticks--;
	}

	// handle delayed events
	for (it = delayed_events.end(); it != delayed_events.begin(); ) {
		--it;

		if (it->delay_ticks > 0) {
			it->delay_ticks--;
		}
		else {
			EventManager::executeEvent(*it);
			it = delayed_events.erase(it);
		}
	}
}

bool priocompare(const Renderable &r1, const Renderable &r2) {
	return r1.prio < r2.prio;
}

/**
 * Sort in the same order as the tiles are drawn
 * Depends upon the map implementation
 */
void calculatePriosIso(std::vector<Renderable> &r) {
	for (std::vector<Renderable>::iterator it = r.begin(); it != r.end(); ++it) {
		const unsigned tilex = static_cast<unsigned>(floorf(it->map_pos.x));
		const unsigned tiley = static_cast<unsigned>(floorf(it->map_pos.y));
		const int commax = static_cast<int>((it->map_pos.x - static_cast<float>(tilex)) * (2<<16));
		const int commay = static_cast<int>((it->map_pos.y - static_cast<float>(tiley)) * (2<<16));
		it->prio += (static_cast<uint64_t>(tilex + tiley) << 54) + (static_cast<uint64_t>(tilex) << 42) + (static_cast<uint64_t>(commax + commay) << 16);
	}
}

void calculatePriosOrtho(std::vector<Renderable> &r) {
	for (std::vector<Renderable>::iterator it = r.begin(); it != r.end(); ++it) {
		const unsigned tilex = static_cast<unsigned>(floorf(it->map_pos.x));
		const unsigned tiley = static_cast<unsigned>(floorf(it->map_pos.y));
		const int commay = static_cast<int>(1024 * it->map_pos.y);
		it->prio += (static_cast<uint64_t>(tiley) << 48) + (static_cast<uint64_t>(tilex) << 32) + (static_cast<uint64_t>(commay) << 16);
	}
}

void MapRenderer::render(std::vector<Renderable> &r, std::vector<Renderable> &r_dead) {

	map_parallax.render(shakycam, "");

	if (TILESET_ORIENTATION == TILESET_ORTHOGONAL) {
		calculatePriosOrtho(r);
		calculatePriosOrtho(r_dead);
		std::sort(r.begin(), r.end(), priocompare);
		std::sort(r_dead.begin(), r_dead.end(), priocompare);
		renderOrtho(r, r_dead);
	}
	else {
		calculatePriosIso(r);
		calculatePriosIso(r_dead);
		std::sort(r.begin(), r.end(), priocompare);
		std::sort(r_dead.begin(), r_dead.end(), priocompare);
		renderIso(r, r_dead);
	}
}

void MapRenderer::drawRenderable(std::vector<Renderable>::iterator r_cursor) {
	if (r_cursor->image != NULL) {
		Rect dest;
		Point p = map_to_screen(r_cursor->map_pos.x, r_cursor->map_pos.y, shakycam.x, shakycam.y);
		dest.x = p.x - r_cursor->offset.x;
		dest.y = p.y - r_cursor->offset.y;
		render_device->render(*r_cursor, dest);
	}
}

void MapRenderer::renderIsoLayer(const Map_Layer& layerdata) {
	int_fast16_t i; // first index of the map array
	int_fast16_t j; // second index of the map array
	Point dest;
	const Point upperleft = FPointToPoint(screen_to_map(0, 0, shakycam.x, shakycam.y));
	const int_fast16_t max_tiles_width =   static_cast<int_fast16_t>((VIEW_W / TILE_W) + 2*tset.max_size_x);
	const int_fast16_t max_tiles_height = static_cast<int_fast16_t>((2 * VIEW_H / TILE_H) + 2*(tset.max_size_y+1));

	j = static_cast<int_fast16_t>(upperleft.y - tset.max_size_y/2 + tset.max_size_x);
	i = static_cast<int_fast16_t>(upperleft.x - tset.max_size_y/2 - tset.max_size_x);

	for (uint_fast16_t y = max_tiles_height ; y; --y) {
		int_fast16_t tiles_width = 0;

		// make sure the isometric corners are not rendered:
		// corner north west, upper left  (i < 0)
		if (i < -1) {
			j = static_cast<int_fast16_t>(j + i + 1);
			tiles_width = static_cast<int_fast16_t>(tiles_width - (i + 1));
			i = -1;
		}
		// corner north east, upper right (j > mapheight)
		const int_fast16_t d = static_cast<int_fast16_t>(j - h);
		if (d >= 0) {
			j = static_cast<int_fast16_t>(j - d);
			tiles_width = static_cast<int_fast16_t>(tiles_width + d);
			i = static_cast<int_fast16_t>(i + d);
		}

		// lower right (south east) corner is covered by (j+i-w+1)
		// lower left (south west) corner is caught by having 0 in there, so j>0
		const int_fast16_t j_end = std::max(static_cast<int_fast16_t>(j+i-w+1),	std::max(static_cast<int_fast16_t>(j - max_tiles_width), static_cast<int_fast16_t>(0)));

		Point p = map_to_screen(float(i), float(j), shakycam.x, shakycam.y);
		p = centerTile(p);

		// draw one horizontal line
		while (j > j_end) {
			--j;
			++i;
			++tiles_width;
			p.x += TILE_W;

			if (const uint_fast16_t current_tile = layerdata[i][j]) {
				const Tile_Def &tile = tset.tiles[current_tile];
				dest.x = p.x - tile.offset.x;
				dest.y = p.y - tile.offset.y;
				// no need to set w and h in dest, as it is ignored
				// by SDL_BlitSurface
				tile.tile->setDest(dest);
				render_device->render(tile.tile);
			}
		}
		j = static_cast<int_fast16_t>(j + tiles_width);
		i = static_cast<int_fast16_t>(i - tiles_width);
		// Go one line deeper, the starting position goes zig-zag
		if (y % 2)
			i++;
		else
			j++;
	}
}

void MapRenderer::renderIsoBackObjects(std::vector<Renderable> &r) {
	std::vector<Renderable>::iterator it;
	for (it = r.begin(); it != r.end(); ++it)
		drawRenderable(it);
}

void MapRenderer::renderIsoFrontObjects(std::vector<Renderable> &r) {
	Point dest;

	const Point upperleft = FPointToPoint(screen_to_map(0, 0, shakycam.x, shakycam.y));
	const int_fast16_t max_tiles_width = static_cast<int_fast16_t>((VIEW_W / TILE_W) + 2 * tset.max_size_x);
	const int_fast16_t max_tiles_height = static_cast<int_fast16_t>(((VIEW_H / TILE_H) + 2 * tset.max_size_y)*2);

	std::vector<Renderable>::iterator r_cursor = r.begin();
	std::vector<Renderable>::iterator r_end = r.end();

	// object layer
	int_fast16_t j = static_cast<int_fast16_t>(upperleft.y - tset.max_size_y + tset.max_size_x);
	int_fast16_t i = static_cast<int_fast16_t>(upperleft.x - tset.max_size_y - tset.max_size_x);

	while (r_cursor != r_end && (static_cast<int>(r_cursor->map_pos.x) + static_cast<int>(r_cursor->map_pos.y) < i + j || static_cast<int>(r_cursor->map_pos.x) < i)) // implicit floor
		++r_cursor;

	if (index_objectlayer >= layers.size())
		return;

	std::queue<std::vector<Renderable>::iterator> render_behind_SW;
	std::queue<std::vector<Renderable>::iterator> render_behind_NE;
	std::queue<std::vector<Renderable>::iterator> render_behind_none;

	Map_Layer drawn_tiles(w, std::vector<unsigned short>(h, 0));

	for (uint_fast16_t y = max_tiles_height ; y; --y) {
		int_fast16_t tiles_width = 0;

		// make sure the isometric corners are not rendered:
		if (i < -1) {
			j = static_cast<int_fast16_t>(j + i + 1);
			tiles_width = static_cast<int_fast16_t>(tiles_width - (i + 1));
			i = -1;
		}
		const int_fast16_t d = static_cast<int_fast16_t>(j - h);
		if (d >= 0) {
			j = static_cast<int_fast16_t>(j - d);
			tiles_width = static_cast<int_fast16_t>(tiles_width + d);
			i = static_cast<int_fast16_t>(i + d);
		}
		const int_fast16_t j_end = std::max(static_cast<int_fast16_t>(j+i-w+1), std::max(static_cast<int_fast16_t>(j - max_tiles_width), static_cast<int_fast16_t>(0)));

		// draw one horizontal line
		Point p = map_to_screen(float(i), float(j), shakycam.x, shakycam.y);
		p = centerTile(p);
		const Map_Layer &current_layer = layers[index_objectlayer];
		bool is_last_NE_tile = false;
		while (j > j_end) {
			--j;
			++i;
			++tiles_width;
			p.x += TILE_W;

			bool draw_tile = true;

			std::vector<Renderable>::iterator r_pre_cursor = r_cursor;
			while (r_pre_cursor != r_end) {
				int r_cursor_x = static_cast<int>(r_pre_cursor->map_pos.x);
				int r_cursor_y = static_cast<int>(r_pre_cursor->map_pos.y);

				if ((r_cursor_x-1 == i && r_cursor_y+1 == j) || (r_cursor_x+1 == i && r_cursor_y-1 == j)) {
					draw_tile = false;
					break;
				}
				else if (r_cursor_x+1 > i || r_cursor_y+1 > j) {
					break;
				}
				++r_pre_cursor;
			}

			if (draw_tile && !drawn_tiles[i][j]) {
				if (const uint_fast16_t current_tile = current_layer[i][j]) {
					const Tile_Def &tile = tset.tiles[current_tile];
					dest.x = p.x - tile.offset.x;
					dest.y = p.y - tile.offset.y;
					tile.tile->setDest(dest);
					render_device->render(tile.tile);
					drawn_tiles[i][j] = 1;
				}
			}

			if (r_cursor == r_end)
				continue;

do_last_NE_tile:
			// some renderable entities go in this layer

			// calculate south/south-west tile bounds
			Rect tile_SW_bounds, tile_S_bounds;
			Point tile_SW_center, tile_S_center;
			getTileBounds(i-2, j+2, current_layer, tile_SW_bounds, tile_SW_center);
			getTileBounds(i-1, j+2, current_layer, tile_S_bounds, tile_S_center);

			// calculate east/north-east tile bounds
			Rect tile_NE_bounds, tile_E_bounds;
			Point tile_NE_center, tile_E_center;
			getTileBounds(i, j, current_layer, tile_NE_bounds, tile_NE_center);
			getTileBounds(i, j+1, current_layer, tile_E_bounds, tile_E_center);

			bool draw_SW_tile = false;
			bool draw_NE_tile = false;

			while (r_cursor != r_end) {
				// implicit floor by int cast
				int r_cursor_x = static_cast<int>(r_cursor->map_pos.x);
				int r_cursor_y = static_cast<int>(r_cursor->map_pos.y);

				if (r_cursor_x+1 == i && r_cursor_y-1 == j) {
					draw_SW_tile = true;
					draw_NE_tile = !is_last_NE_tile;

					// r_cursor left/right side
					Point r_cursor_left = map_to_screen(r_cursor->map_pos.x, r_cursor->map_pos.y, shakycam.x, shakycam.y);
					r_cursor_left.y -= r_cursor->offset.y;
					Point r_cursor_right = r_cursor_left;
					r_cursor_left.x -= r_cursor->offset.x;
					r_cursor_right.x += r_cursor->src.w - r_cursor->offset.x;

					bool is_behind_SW = false;
					bool is_behind_NE = false;

					// check left of r_cursor
					if (isWithinRect(tile_S_bounds, r_cursor_right) && isWithinRect(tile_SW_bounds, r_cursor_left)) {
						is_behind_SW = true;
					}

					// check right of r_cursor
					if (draw_NE_tile && isWithinRect(tile_E_bounds, r_cursor_left) && isWithinRect(tile_NE_bounds, r_cursor_right)) {
						is_behind_NE = true;
					}

					if (is_behind_SW)
						render_behind_SW.push(r_cursor);
					else if (!is_behind_SW && is_behind_NE)
						render_behind_NE.push(r_cursor);
					else
						render_behind_none.push(r_cursor);

					++r_cursor;
				}
				else {
					break;
				}
			}

			while (!render_behind_SW.empty()) {
				drawRenderable(render_behind_SW.front());
				render_behind_SW.pop();
			}

			// draw the south-west tile
			if (draw_SW_tile && i-2 >= 0 && j+2 < h && !drawn_tiles[i-2][j+2]) {
				if (const uint_fast16_t current_tile = current_layer[i-2][j+2]) {
					const Tile_Def &tile = tset.tiles[current_tile];
					dest.x = tile_SW_center.x - tile.offset.x;
					dest.y = tile_SW_center.y - tile.offset.y;
					tile.tile->setDest(dest);
					render_device->render(tile.tile);
					drawn_tiles[i-2][j+2] = 1;
				}
			}

			while (!render_behind_NE.empty()) {
				drawRenderable(render_behind_NE.front());
				render_behind_NE.pop();
			}

			// draw the north-east tile
			if (draw_NE_tile && !draw_tile && !drawn_tiles[i][j]) {
				if (const uint_fast16_t current_tile = current_layer[i][j]) {
					const Tile_Def &tile = tset.tiles[current_tile];
					dest.x = tile_NE_center.x - tile.offset.x;
					dest.y = tile_NE_center.y - tile.offset.y;
					tile.tile->setDest(dest);
					render_device->render(tile.tile);
					drawn_tiles[i][j] = 1;
				}
			}

			while (!render_behind_none.empty()) {
				drawRenderable(render_behind_none.front());
				render_behind_none.pop();
			}

			// Okay, this is a bit of a HACK
			// In order to properly render the first row and last column of the map, we need to advance to an imaginary tile
			// Care must be taken in the code after the "do_last_NE_tile" goto label to avoid accessing map data with these coordinates
			if (is_last_NE_tile) {
				++j;
				--i;
				is_last_NE_tile = false;
			}
			else if (i == w-1 || j == 0) {
				--j;
				++i;
				is_last_NE_tile = true;
				goto do_last_NE_tile;
			}
		}
		j = static_cast<int_fast16_t>(j + tiles_width);
		i = static_cast<int_fast16_t>(i - tiles_width);
		if (y % 2)
			i++;
		else
			j++;

		while (r_cursor != r_end && (static_cast<int>(r_cursor->map_pos.x) + static_cast<int>(r_cursor->map_pos.y) < i + j || static_cast<int>(r_cursor->map_pos.x) <= i)) // implicit floor by int cast
			++r_cursor;
	}
}

void MapRenderer::renderIso(std::vector<Renderable> &r, std::vector<Renderable> &r_dead) {
	size_t index = 0;
	while (index < index_objectlayer) {
		renderIsoLayer(layers[index]);
		map_parallax.render(shakycam, layernames[index]);
		index++;
	}

	renderIsoBackObjects(r_dead);
	renderIsoFrontObjects(r);
	map_parallax.render(shakycam, layernames[index]);

	index++;
	while (index < layers.size()) {
		renderIsoLayer(layers[index]);
		map_parallax.render(shakycam, layernames[index]);
		index++;
	}

	checkTooltip();

	drawDevHUD();
	drawDevCursor();
}

void MapRenderer::renderOrthoLayer(const Map_Layer& layerdata) {

	Point dest;
	const Point upperleft = FPointToPoint(screen_to_map(0, 0, shakycam.x, shakycam.y));

	short int startj = static_cast<short int>(std::max(0, upperleft.y));
	short int starti = static_cast<short int>(std::max(0, upperleft.x));
	const short max_tiles_width =  std::min(w, static_cast<short unsigned int>(starti + (VIEW_W / TILE_W) + 2 * tset.max_size_x));
	const short max_tiles_height = std::min(h, static_cast<short unsigned int>(startj + (VIEW_H / TILE_H) + 2 * tset.max_size_y));

	short int i;
	short int j;

	for (j = startj; j < max_tiles_height; j++) {
		Point p = map_to_screen(starti, j, shakycam.x, shakycam.y);
		p = centerTile(p);
		for (i = starti; i < max_tiles_width; i++) {

			if (const unsigned short current_tile = layerdata[i][j]) {
				const Tile_Def &tile = tset.tiles[current_tile];
				dest.x = p.x - tile.offset.x;
				dest.y = p.y - tile.offset.y;
				tile.tile->setDest(dest);
				render_device->render(tile.tile);
			}
			p.x += TILE_W;
		}
	}
}

void MapRenderer::renderOrthoBackObjects(std::vector<Renderable> &r) {
	// some renderables are drawn above the background and below the objects
	std::vector<Renderable>::iterator it;
	for (it = r.begin(); it != r.end(); ++it)
		drawRenderable(it);
}

void MapRenderer::renderOrthoFrontObjects(std::vector<Renderable> &r) {

	short int i;
	short int j;
	Point dest;
	std::vector<Renderable>::iterator r_cursor = r.begin();
	std::vector<Renderable>::iterator r_end = r.end();

	const Point upperleft = FPointToPoint(screen_to_map(0, 0, shakycam.x, shakycam.y));

	short int startj = static_cast<short int>(std::max(0, upperleft.y));
	short int starti = static_cast<short int>(std::max(0, upperleft.x));
	const short max_tiles_width  = std::min(w, static_cast<short unsigned int>(starti + (VIEW_W / TILE_W) + 2 * tset.max_size_x));
	const short max_tiles_height = std::min(h, static_cast<short unsigned int>(startj + (VIEW_H / TILE_H) + 2 * tset.max_size_y));

	while (r_cursor != r_end && static_cast<int>(r_cursor->map_pos.y) < startj)
		++r_cursor;

	if (index_objectlayer >= layers.size())
		return;

	for (j = startj; j < max_tiles_height; j++) {
		Point p = map_to_screen(starti, j, shakycam.x, shakycam.y);
		p = centerTile(p);
		for (i = starti; i<max_tiles_width; i++) {

			if (const unsigned short current_tile = layers[index_objectlayer][i][j]) {
				const Tile_Def &tile = tset.tiles[current_tile];
				dest.x = p.x - tile.offset.x;
				dest.y = p.y - tile.offset.y;
				tile.tile->setDest(dest);
				render_device->render(tile.tile);
			}
			p.x += TILE_W;

			while (r_cursor != r_end && static_cast<int>(r_cursor->map_pos.y) == j && static_cast<int>(r_cursor->map_pos.x) < i) // implicit floor
				++r_cursor;

			// some renderable entities go in this layer
			while (r_cursor != r_end && static_cast<int>(r_cursor->map_pos.y) == j && static_cast<int>(r_cursor->map_pos.x) == i) // implicit floor
				drawRenderable(r_cursor++);
		}
		while (r_cursor != r_end && static_cast<int>(r_cursor->map_pos.y) <= j) // implicit floor
			++r_cursor;
	}
}

void MapRenderer::renderOrtho(std::vector<Renderable> &r, std::vector<Renderable> &r_dead) {
	unsigned index = 0;
	while (index < index_objectlayer) {
		renderOrthoLayer(layers[index]);
		map_parallax.render(shakycam, layernames[index]);
		index++;
	}

	renderOrthoBackObjects(r_dead);
	renderOrthoFrontObjects(r);
	map_parallax.render(shakycam, layernames[index]);

	index++;
	while (index < layers.size()) {
		renderOrthoLayer(layers[index]);
		map_parallax.render(shakycam, layernames[index]);
		index++;
	}

	checkTooltip();

	drawDevHUD();
	drawDevCursor();
}

void MapRenderer::executeOnLoadEvents() {
	// if set from the command-line, execute a given script if this is our first map load
	if (!LOAD_SCRIPT.empty() && filename != "maps/spawn.txt") {
		Event evnt;
		Event_Component ec;

		ec.type = EC_SCRIPT;
		ec.s = LOAD_SCRIPT;
		LOAD_SCRIPT.clear();

		evnt.components.push_back(ec);
		EventManager::executeEvent(evnt);

		return;
	}

	std::vector<Event>::iterator it;

	// loop in reverse because we may erase elements
	for (it = events.end(); it != events.begin(); ) {
		--it;

		// skip inactive events
		if (!EventManager::isActive(*it)) continue;

		if ((*it).activate_type == EVENT_ON_LOAD) {
			if (EventManager::executeEvent(*it))
				it = events.erase(it);
		}
	}
}

void MapRenderer::executeOnMapExitEvents() {
	std::vector<Event>::iterator it;

	// We're leaving the map, so the events of this map are removed anyway in
	// the next frame (Reminder: We're about to load a new map ;),
	// so we will ignore the events keep_after_trigger value and do not delete
	// any event in this loop
	for (it = events.begin(); it != events.end(); ++it) {

		// skip inactive events
		if (!EventManager::isActive(*it)) continue;

		if ((*it).activate_type == EVENT_ON_MAPEXIT)
			EventManager::executeEvent(*it); // ignore repeat value
	}
}

void MapRenderer::checkEvents(const FPoint& loc) {
	Point maploc;
	maploc.x = int(loc.x);
	maploc.y = int(loc.y);
	std::vector<Event>::iterator it;

	// loop in reverse because we may erase elements
	for (it = events.end(); it != events.begin(); ) {
		--it;

		// skip inactive events
		if (!EventManager::isActive(*it)) continue;

		// static events are run every frame without interaction from the player
		if ((*it).activate_type == EVENT_STATIC) {
			if (EventManager::executeEvent(*it))
				it = events.erase(it);
			continue;
		}

		if ((*it).activate_type == EVENT_ON_CLEAR) {
			if (enemies_cleared && EventManager::executeEvent(*it))
				it = events.erase(it);
			continue;
		}

		bool inside = maploc.x >= (*it).location.x &&
					  maploc.y >= (*it).location.y &&
					  maploc.x <= (*it).location.x + (*it).location.w-1 &&
					  maploc.y <= (*it).location.y + (*it).location.h-1;

		if ((*it).activate_type == EVENT_ON_LEAVE) {
			if (inside) {
				if (!(*it).getComponent(EC_WAS_INSIDE_EVENT_AREA)) {
					(*it).components.push_back(Event_Component());
					(*it).components.back().type = EC_WAS_INSIDE_EVENT_AREA;
				}
			}
			else {
				if ((*it).getComponent(EC_WAS_INSIDE_EVENT_AREA)) {
					(*it).deleteAllComponents(EC_WAS_INSIDE_EVENT_AREA);
					if (EventManager::executeEvent(*it))
						it = events.erase(it);
				}
			}
		}
		else if ((*it).activate_type == -1 || (*it).activate_type == EVENT_ON_TRIGGER) {
			if (inside)
				if (EventManager::executeEvent(*it))
					it = events.erase(it);
		}
	}
}

/**
 * Some events have a hotspot (rectangle screen area) where the user can click
 * to trigger the event.
 *
 * The hero must be within range (INTERACT_RANGE) to activate an event.
 *
 * This function checks valid mouse clicks against all clickable events, and
 * executes
 */
void MapRenderer::checkHotspots() {
	if (!inpt->usingMouse()) return;

	show_tooltip = false;

	std::vector<Event>::iterator it;

	// work backwards through events because events can be erased in the loop.
	// this prevents the iterator from becoming invalid.
	for (it = events.end(); it != events.begin(); ) {
		--it;

		for (int x=it->hotspot.x; x < it->hotspot.x + it->hotspot.w; ++x) {
			for (int y=it->hotspot.y; y < it->hotspot.y + it->hotspot.h; ++y) {
				bool matched = false;
				bool is_npc = false;

				Event_Component* npc = (*it).getComponent(EC_NPC_HOTSPOT);
				if (npc) {
					is_npc = true;

					Point p = map_to_screen(float(npc->x), float(npc->y), shakycam.x, shakycam.y);
					p = centerTile(p);

					Rect dest;
					dest.x = p.x - npc->z;
					dest.y = p.y - npc->a;
					dest.w = npc->b;
					dest.h = npc->c;

					if (isWithinRect(dest, inpt->mouse)) {
						matched = true;
						tip_pos.x = dest.x + dest.w/2;
						tip_pos.y = p.y - TOOLTIP_MARGIN_NPC;
					}
				}
				else {
					for (unsigned index = 0; index <= index_objectlayer; ++index) {
						Point p = map_to_screen(float(x), float(y), shakycam.x, shakycam.y);
						p = centerTile(p);

						if (const short current_tile = layers[index][x][y]) {
							// first check if mouse pointer is in rectangle of that tile:
							const Tile_Def &tile = tset.tiles[current_tile];
							Rect dest;
							dest.x = p.x - tile.offset.x;
							dest.y = p.y - tile.offset.y;
							dest.w = tile.tile->getClip().w;
							dest.h = tile.tile->getClip().h;

							if (isWithinRect(dest, inpt->mouse)) {
								matched = true;
								tip_pos = map_to_screen(it->center.x, it->center.y, shakycam.x, shakycam.y);
								tip_pos.y -= TILE_H;
							}
						}
					}
				}

				if (matched) {
					// skip inactive events
					if (!EventManager::isActive(*it)) continue;

					// skip events without hotspots
					if ((*it).hotspot.h == 0) continue;

					// skip events on cooldown
					if ((*it).cooldown_ticks != 0) continue;

					// new tooltip?
					createTooltip((*it).getComponent(EC_TOOLTIP));

					if ((((*it).reachable_from.w == 0 && (*it).reachable_from.h == 0) || isWithinRect((*it).reachable_from, FPointToPoint(cam)))
							&& calcDist(cam, (*it).center) < INTERACT_RANGE) {

						// only check events if the player is clicking
						// and allowed to click
						if (is_npc) {
							curs->setCursor(CURSOR_TALK);
						}
						else {
							curs->setCursor(CURSOR_INTERACT);
						}
						if (!inpt->pressing[MAIN1]) return;
						else if (inpt->lock[MAIN1]) return;

						inpt->lock[MAIN1] = true;
						if (EventManager::executeEvent(*it))
							it = events.erase(it);
					}
					return;
				}
				else show_tooltip = false;
			}
		}
	}
}

void MapRenderer::checkNearestEvent() {
	if (!inpt->usingMouse()) show_tooltip = false;

	std::vector<Event>::iterator it;
	std::vector<Event>::iterator nearest = events.end();
	float best_distance = std::numeric_limits<float>::max();

	// loop in reverse because we may erase elements
	for (it = events.end(); it != events.begin(); ) {
		--it;

		// skip inactive events
		if (!EventManager::isActive(*it)) continue;

		// skip events without hotspots
		if ((*it).hotspot.h == 0) continue;

		// skip events on cooldown
		if ((*it).cooldown_ticks != 0) continue;

		float distance = calcDist(cam, (*it).center);
		if ((((*it).reachable_from.w == 0 && (*it).reachable_from.h == 0) || isWithinRect((*it).reachable_from, FPointToPoint(cam)))
				&& distance < INTERACT_RANGE && distance < best_distance) {
			best_distance = distance;
			nearest = it;
		}

	}

	if (nearest != events.end()) {
		if (!inpt->usingMouse() || TOUCHSCREEN) {
			// new tooltip?
			createTooltip((*nearest).getComponent(EC_TOOLTIP));
			tip_pos = map_to_screen((*nearest).center.x, (*nearest).center.y, shakycam.x, shakycam.y);
			if ((*nearest).getComponent(EC_NPC_HOTSPOT)) {
				tip_pos.y -= TOOLTIP_MARGIN_NPC;
			}
			else {
				tip_pos.y -= TILE_H;
			}
		}

		if (inpt->pressing[ACCEPT] && !inpt->lock[ACCEPT]) {
			if (inpt->pressing[ACCEPT]) inpt->lock[ACCEPT] = true;

			if(EventManager::executeEvent(*nearest))
				events.erase(nearest);
		}
	}
}

void MapRenderer::checkTooltip() {
	if (show_tooltip && SHOW_HUD && !(DEV_MODE && menu->devconsole->visible))
		tip->render(tip_buf, tip_pos, STYLE_TOPLABEL);
}

void MapRenderer::createTooltip(Event_Component *ec) {
	if (ec && !ec->s.empty() && TOOLTIP_CONTEXT != TOOLTIP_MENU) {
		show_tooltip = true;
		if (!tip_buf.compareFirstLine(ec->s)) {
			tip_buf.clear();
			tip_buf.addText(ec->s);
		}
		TOOLTIP_CONTEXT = TOOLTIP_MAP;
	}
	else if (TOOLTIP_CONTEXT != TOOLTIP_MENU) {
		TOOLTIP_CONTEXT = TOOLTIP_NONE;
	}
}

/**
 * Activate a power that is attached to an event
 */
void MapRenderer::activatePower(int power_index, unsigned statblock_index, FPoint &target) {
	if (power_index < 0 || static_cast<unsigned>(power_index) >= powers->powers.size()) {
		logError("MapRenderer: Power index is out of bounds.");
		return;
	}

	if (statblock_index < statblocks.size()) {
		// check power cooldown before activating
		if (statblocks[statblock_index].powers_ai[0].ticks == 0) {
			statblocks[statblock_index].powers_ai[0].ticks = powers->powers[power_index].cooldown;
			powers->activate(power_index, &statblocks[statblock_index], target);
		}
	}
	else {
		logError("MapRenderer: StatBlock index is out of bounds.");
	}
}

bool MapRenderer::isValidTile(const unsigned &tile) {
	if (tile == 0)
		return true;

	if (tile >= tset.tiles.size())
		return false;

	return tset.tiles[tile].tile != NULL;
}

Point MapRenderer::centerTile(const Point& p) {
	Point r = p;

	if (TILESET_ORIENTATION == TILESET_ORTHOGONAL) {
		r.x += TILE_W_HALF;
		r.y += TILE_H_HALF;
	}
	else //TILESET_ISOMETRIC
		r.y += TILE_H_HALF;
	return r;
}

void MapRenderer::getTileBounds(const int_fast16_t x, const int_fast16_t y, const Map_Layer& layerdata, Rect& bounds, Point& center) {
	if (x >= 0 && x < w && y >= 0 && y < h) {
		if (const uint_fast16_t tile_index = layerdata[x][y]) {
			const Tile_Def &tile = tset.tiles[tile_index];
			if (!tile.tile)
				return;
			center = centerTile(map_to_screen(float(x), float(y), shakycam.x, shakycam.y));
			bounds.x = center.x - tile.offset.x;
			bounds.y = center.y - tile.offset.y;
			bounds.w = tile.tile->getClip().w;
			bounds.h = tile.tile->getClip().h;
		}
	}
}

void MapRenderer::drawDevCursor() {
	// Developer mode only: draw colored cursor around tile under mouse pointer
	if (!(DEV_MODE && menu->devconsole->visible))
		return;

	Color dev_cursor_color = Color(255,255,0,255);
	FPoint target = screen_to_map(inpt->mouse.x,  inpt->mouse.y, shakycam.x, shakycam.y);

	if (!collider.is_outside_map(floorf(target.x), floorf(target.y))) {
		if (TILESET_ORIENTATION == TILESET_ORTHOGONAL) {
			Point p_topleft = map_to_screen(floorf(target.x), floorf(target.y), shakycam.x, shakycam.y);
			Point p_bottomright(p_topleft.x + TILE_W, p_topleft.y + TILE_H);

			render_device->drawRectangle(p_topleft, p_bottomright, dev_cursor_color);
		}
		else {
			Point p_left = map_to_screen(floorf(target.x), floorf(target.y+1), shakycam.x, shakycam.y);
			Point p_top(p_left.x + TILE_W_HALF, p_left.y - TILE_H_HALF);
			Point p_right(p_left.x + TILE_W, p_left.y);
			Point p_bottom(p_left.x + TILE_W_HALF, p_left.y + TILE_H_HALF);

			render_device->drawLine(p_left.x, p_left.y, p_top.x, p_top.y, dev_cursor_color);
			render_device->drawLine(p_top.x, p_top.y, p_right.x, p_right.y, dev_cursor_color);
			render_device->drawLine(p_right.x, p_right.y, p_bottom.x, p_bottom.y, dev_cursor_color);
			render_device->drawLine(p_bottom.x, p_bottom.y, p_left.x, p_left.y, dev_cursor_color);
		}

		// draw distance line
		if (menu->devconsole->distance_ticks >= MAX_FRAMES_PER_SEC) {
			Point p0 = map_to_screen(menu->devconsole->target.x, menu->devconsole->target.y, shakycam.x, shakycam.y);
			Point p1 = map_to_screen(pc->stats.pos.x, pc->stats.pos.y, shakycam.x, shakycam.y);
			render_device->drawLine(p0.x, p0.y, p1.x, p1.y, dev_cursor_color);
		}
	}
}

void MapRenderer::drawDevHUD() {
	if (!(DEV_MODE && DEV_HUD))
		return;

	Color color_hazard(255,0,0,255);
	Color color_entity(0,255,0,255);
	int cross_size = TILE_H_HALF / 4;

	// ellipses are distorted for isometric tilesets
	int distort = TILESET_ORIENTATION == TILESET_ORTHOGONAL ? 0 : 2;

	// player
	{
		Point p0 = map_to_screen(pc->stats.pos.x, pc->stats.pos.y, shakycam.x, shakycam.y);
		render_device->drawLine(p0.x - cross_size, p0.y, p0.x + cross_size, p0.y, color_entity);
		render_device->drawLine(p0.x, p0.y - cross_size, p0.x, p0.y + cross_size, color_entity);
	}

	// enemies
	for (size_t i = 0; i < enemym->enemies.size(); ++i) {
		Point p0 = map_to_screen(enemym->enemies[i]->stats.pos.x, enemym->enemies[i]->stats.pos.y, shakycam.x, shakycam.y);
		render_device->drawLine(p0.x - cross_size, p0.y, p0.x + cross_size, p0.y, color_entity);
		render_device->drawLine(p0.x, p0.y - cross_size, p0.x, p0.y + cross_size, color_entity);
	}

	// hazards
	for (size_t i = 0; i < hazards->h.size(); ++i) {
		if (hazards->h[i]->delay_frames != 0)
			continue;

		float radius_c = sqrtf(powf(hazards->h[i]->radius, 2) + powf(hazards->h[i]->radius, 2));
		Point p0 = map_to_screen(hazards->h[i]->pos.x, hazards->h[i]->pos.y, shakycam.x, shakycam.y);
		Point p1 = map_to_screen(hazards->h[i]->pos.x + radius_c, hazards->h[i]->pos.y, shakycam.x, shakycam.y);
		int radius = p1.x - p0.x;
		render_device->drawLine(p0.x - cross_size, p0.y, p0.x + cross_size, p0.y, color_hazard);
		render_device->drawLine(p0.x, p0.y - cross_size, p0.x, p0.y + cross_size, color_hazard);

		render_device->drawEllipse(p0.x - radius, p0.y - radius/distort, p0.x + radius, p0.y + radius/distort, color_hazard, 15);
	}
}

MapRenderer::~MapRenderer() {
	tip_buf.clear();
	clearLayers();
	clearEvents();
	clearQueues();
	delete tip;

	/* unload sounds */
	snd->reset();
	while (!sids.empty()) {
		snd->unload(sids.back());
		sids.pop_back();
	}
}

