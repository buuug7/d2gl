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

#include "pch.h"
#include "texture_manager.h"

#include <cstdarg>
#include <cstdio>

namespace {

// [TU_DIAG] Texture-manager side of the diagnostics. Separate file from
// wrapper.cpp so a failed shared handle cannot hide these lines.
FILE* g_tu2_log = nullptr;

void tu2_diag(const char* format, ...)
{
	if (!g_tu2_log) {
		if (fopen_s(&g_tu2_log, "d2gl_tm.log", "a") != 0 || !g_tu2_log) {
			g_tu2_log = nullptr;
			return;
		}
	}

	va_list args;
	va_start(args, format);
	SYSTEMTIME st;
	GetLocalTime(&st);
	fprintf(g_tu2_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	vfprintf(g_tu2_log, format, args);
	va_end(args);
	fputc('\n', g_tu2_log);
	fflush(g_tu2_log);
}

}  // namespace

namespace d2gl {

// [TU_FIX] Only rewrite the cached hash when it actually changed; this runs on
// every successful texture source.
static void tu_set_last_good(uint64_t key, uint32_t hash)
{
	const auto it = g_glide_texture.last_good.find(key);
	if (it == g_glide_texture.last_good.end())
		g_glide_texture.last_good.emplace(key, hash);
	else if (it->second != hash)
		it->second = hash;
}

GlideTexture g_glide_texture;

TextureManager::TextureManager(const SubTextureCounts& size_counts)
	: m_size_counts(size_counts)
{
	tu2_diag("TMGR init, %u size classes", (uint32_t)size_counts.size());
	uint16_t tex_start = 0;

	for (auto& size_count : size_counts) {
		auto size = size_count.first;
		auto count = size_count.second;

		uint16_t div = 512 / size;
		uint16_t tex_count = count * div * div;

		uint8_t shift = 0;
		switch (size) {
			case 8: shift = 5; break;
			case 16: shift = 4; break;
			case 32: shift = 3; break;
			case 64: shift = 2; break;
			case 128: shift = 1; break;
		}

		m_data.insert({ size, {} });
		m_data[size].sub_texure_info.assign(tex_count + 1, { 0 });
		m_data[size].tex_count = tex_count;
		m_data[size].available.reserve(tex_count);
		m_data[size].cache.reserve(tex_count);

		for (uint16_t i = 0; i < count; i++) {
			uint16_t n = i * div * div;
			uint16_t tex_num = tex_start + i;

			for (uint16_t y = 0; y < div; y++) {
				uint16_t ny = y * div;

				for (uint16_t x = 0; x < div; x++) {
					uint16_t ix = n + ny + x + 1;

					m_data[size].sub_texure_info[ix].tex_num = tex_num;
					m_data[size].sub_texure_info[ix].offset = { x * size, y * size };
					m_data[size].sub_texure_info[ix].shift = shift;

					m_data[size].available[ix] = true;
				}
			}
		}

		tex_start += count;
	}
}

const SubTextureInfo* TextureManager::getSubTextureInfo(uint32_t address, uint16_t size, uint16_t width, uint16_t height, uint32_t frame_count)
{
	if (g_glide_texture.hash.find(address) == g_glide_texture.hash.end())
		return nullptr;

	auto hash = g_glide_texture.hash[address];
	auto& data = m_data[size];

	if (data.cache.find(address) == data.cache.end())
		data.cache.insert({ address, { frame_count } });
	auto& cache = data.cache[address];

	// [TU_FIX] D2Glide hands the same texture memory address to another cel
	// without invalidating the cells that still reference it. When that
	// happens the caller shows up here with its own size while the address
	// holds a texture of a different size, and re-uploading from texture
	// memory would draw the other cel's pixels. Keep serving the sub-texture
	// we already hold for this address instead.
	const auto cached_size = g_glide_texture.size.find(address);
	if (cached_size != g_glide_texture.size.end() && cached_size->second != size) {
		cache.last_used_frame = frame_count;

		const uint64_t key = (uint64_t(address) << 32) | size;
		const auto last_good = g_glide_texture.last_good.find(key);
		const SubTextureInfo* result = nullptr;
		const char* how = "none";

		if (last_good != g_glide_texture.last_good.end()) {
			const auto item = cache.items.find(last_good->second);
			if (item != cache.items.end()) {
				// This address is referenced by a stale caller; its atlas slot
				// must not be recycled by reclaim().
				g_glide_texture.pinned.insert(key);
				result = &data.sub_texure_info[item->second];
				how = "last_good";
			}
		}
		if (!result) {
			// Nothing usable in the atlas; restore the last content uploaded at
			// this address for this size from our own copy.
			const auto backup = g_glide_texture.backup.find(key);
			if (backup != g_glide_texture.backup.end() && backup->second.size == size && !backup->second.data.empty()) {
				if (data.available.empty() && m_last_reclaim_frame != frame_count) {
					m_last_reclaim_frame = frame_count;
					reclaim(size, frame_count);
				}
				if (!data.available.empty()) {
					const auto id = data.available.begin()->first;
					const SubTextureInfo* texture_info = &data.sub_texure_info[id];
					App.context->getCommandBuffer()->textureUpdate(backup->second.data.data(), texture_info->tex_num,
																   { backup->second.width, backup->second.height }, texture_info->offset);

					for (const auto& item : cache.items)
						data.available[item.second] = true;
					cache.items.clear();
					cache.items.insert({ backup->second.hash, id });
					data.available.erase(id);

					tu_set_last_good(key, backup->second.hash);
					g_glide_texture.pinned.insert(key);

					result = texture_info;
					how = "backup";
				}
			}
		}
		if (!result && !cache.items.empty()) {
			result = &data.sub_texure_info[cache.items.begin()->second];
			how = "any";
		}

		static uint32_t stale_logs = 0;
		if (++stale_logs <= 20)
			tu2_diag("STALE      addr=%08X size=%u addr_size=%u last_good=%d items=%u available=%u how=%s frame=%u", address, size,
					 cached_size->second, last_good != g_glide_texture.last_good.end() ? 1 : 0, (uint32_t)cache.items.size(),
					 (uint32_t)data.available.size(), how, frame_count);

		return result;
	}

	if (cache.last_used_frame != frame_count) {
		for (auto it = cache.items.begin(); it != cache.items.end();) {
			if ((*it).first != hash) {
				data.available[(*it).second] = true;
				it = cache.items.erase(it);
			} else
				it++;
		}
		cache.last_used_frame = frame_count;
	}

	if (cache.items.find(hash) == cache.items.end()) {
		// Slots of addresses that are no longer referenced are never returned
		// by the per-address eviction, so the pool eventually runs dry. Sweep
		// them once per frame when that happens.
		if (data.available.empty() && m_last_reclaim_frame != frame_count) {
			m_last_reclaim_frame = frame_count;
			reclaim(size, frame_count);
		}
		if (data.available.begin() == data.available.end()) {
			static uint32_t pool_logs = 0;
			if (++pool_logs <= 50)
				tu2_diag("POOLFULL   size=%u addr=%08X entries=%u pinned=%u frame=%u", size, address, (uint32_t)data.cache.size(),
						 (uint32_t)g_glide_texture.pinned.size(), frame_count);
			return nullptr;
		}

		const auto id = data.available.begin()->first;
		const SubTextureInfo* texture_info = &data.sub_texure_info[id];
		const auto command_buffer = App.context->getCommandBuffer();

		command_buffer->textureUpdate(g_glide_texture.memory + address, texture_info->tex_num, { width, height }, texture_info->offset);

		cache.items.insert({ hash, id });
		data.available.erase(id);
		tu_set_last_good((uint64_t(address) << 32) | size, hash);

		return texture_info;
	}

	tu_set_last_good((uint64_t(address) << 32) | size, hash);
	return &data.sub_texure_info[cache.items[hash]];
}

// [TU_FIX] Each texture address keeps its atlas slot until that same address is
// sourced again with a different hash, so addresses that fall out of use pin
// their slots forever. With hundreds of item sprites per frame the 32x32 pool
// (8192 slots) runs dry after a minute or so, after which every new address
// fails to get a slot and leaves the caller with a stale vertex texture state.
// Sweep entries that have not been touched for a while and give their slots
// back. Entries a stale caller may still rely on are kept: either the address
// was later uploaded with a different size (so the old content cannot be
// re-read from texture memory), or the fallback path pinned it.
void TextureManager::reclaim(uint16_t size, uint32_t frame_count)
{
	const auto data_it = m_data.find(size);
	if (data_it == m_data.end())
		return;

	auto& data = data_it->second;
	const uint32_t keep_frames = 8;
	uint32_t freed_entries = 0;
	uint32_t freed_slots = 0;

	for (auto it = data.cache.begin(); it != data.cache.end();) {
		const uint64_t key = (uint64_t(it->first) << 32) | size;

		// An entry whose address was later uploaded with a different size is
		// the last copy of a texture a stale caller may still ask for; keep it.
		const auto address_size = g_glide_texture.size.find(it->first);
		const bool cross_size = address_size != g_glide_texture.size.end() && address_size->second != size;
		const bool pinned = g_glide_texture.pinned.find(key) != g_glide_texture.pinned.end();

		if (!cross_size && !pinned && it->second.last_used_frame + keep_frames < frame_count) {
			for (const auto& item : it->second.items) {
				data.available[item.second] = true;
				++freed_slots;
			}
			it = data.cache.erase(it);
			++freed_entries;
		} else
			++it;
	}

	if (freed_slots) {
		++m_reclaim_count;
		static uint32_t reclaim_logs = 0;
		if (++reclaim_logs <= 50)
			tu2_diag("RECLAIM    size=%u freed_entries=%u freed_slots=%u available=%u entries=%u frame=%u", size, freed_entries, freed_slots,
					 (uint32_t)data.available.size(), (uint32_t)data.cache.size(), frame_count);
	} else {
		static uint32_t empty_sweeps = 0;
		if (++empty_sweeps <= 10)
			tu2_diag("RECLAIM0   size=%u available=%u entries=%u frame=%u", size, (uint32_t)data.available.size(),
					 (uint32_t)data.cache.size(), frame_count);
	}
}

void TextureManager::clearCache()
{
	for (auto& size_count : m_size_counts) {
		auto size = size_count.first;
		auto& data = m_data[size];

		for (auto& cache : data.cache) {
			for (auto& item : cache.second.items) {
				data.available[item.second] = true;
			}
		}
		data.cache.clear();
	}
}

}