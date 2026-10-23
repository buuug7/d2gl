/*
	D2GL: Diablo 2 LoD Glide/DDraw to OpenGL Wrapper.
	Copyright (C) 2023  Bayaraa

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <deque>
#include <set>

#include "graphic/texture.h"

namespace d2gl {

struct SubTextureInfo {
	uint8_t shift;
	uint16_t tex_num;
	glm::vec<2, uint16_t> offset;
};

struct TextureCache {
	uint32_t last_used_frame = 0;
	std::unordered_map<uint32_t, uint16_t> items;
};

struct TextureManagerData {
	uint16_t tex_count = 0;
	std::vector<SubTextureInfo> sub_texure_info;
	std::unordered_map<uint16_t, bool> available;
	std::unordered_map<uint32_t, TextureCache> cache;
};

typedef std::vector<std::pair<uint16_t, uint16_t>> SubTextureCounts;

struct GlideBackup {
	uint16_t size = 0;
	uint16_t width = 0;
	uint16_t height = 0;
	uint32_t hash = 0;
	std::vector<uint8_t> data;
};

struct GlideTexture {
	uint8_t* memory = nullptr;
	std::map<uint32_t, uint32_t> hash;
	// [TU_FIX] Size class of the last upload that landed at each address, plus
	// the last hash that was actually sourced from a given (address, size).
	// D2Glide reuses texture memory addresses without always invalidating the
	// cells that still point at them; those cells then call grTexSource with
	// their own size while the address holds another cel's data. The size
	// mismatch is the only signal available here that the reference is stale.
	std::map<uint32_t, uint16_t> size;
	std::map<uint64_t, uint32_t> last_good;
	// (address << 32 | size) entries that a stale caller still depends on; the
	// atlas slot behind them must survive pool pressure.
	std::set<uint64_t> pinned;
	// [TU_FIX] CPU-side copy of recently uploaded small textures, keyed the
	// same way. Lets a stale caller be served even after the atlas slot was
	// reclaimed and the address was handed to another cel. Bounded to
	// 64 MB with FIFO eviction (oldest uploads are dropped first).
	std::map<uint64_t, GlideBackup> backup;
	std::deque<uint64_t> backup_order;
	size_t backup_bytes = 0;
};

extern GlideTexture g_glide_texture;

class TextureManager {
	std::map<uint16_t, TextureManagerData> m_data;
	SubTextureCounts m_size_counts;
	uint32_t m_reclaim_count = 0;
	uint32_t m_last_reclaim_frame = UINT32_MAX;

public:
	TextureManager(const SubTextureCounts& size_counts);
	~TextureManager() = default;

	inline size_t getUsage(uint16_t size) { return m_data[size].tex_count - m_data[size].available.size(); }
	inline uint32_t getReclaimCount() const { return m_reclaim_count; }

	const SubTextureInfo* getSubTextureInfo(uint32_t address, uint16_t size, uint16_t width, uint16_t height, uint32_t frame_count);
	void reclaim(uint16_t size, uint32_t frame_count);
	void clearCache();
};

}