/**************************************************************************/
/*  utilities.cpp                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "utilities.h"

#ifdef GLES3_ENABLED

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/os/os.h"
#include "drivers/gles3/storage/config.h"
#include "drivers/gles3/storage/light_storage.h"
#include "drivers/gles3/storage/material_storage.h"
#include "drivers/gles3/storage/mesh_storage.h"
#include "drivers/gles3/storage/particles_storage.h"
#include "drivers/gles3/storage/texture_storage.h"

#ifdef GL_API_ENABLED
#include "drivers/gles3/rasterizer_util_gles3.h"
#endif

using namespace GLES3;

Utilities *Utilities::singleton = nullptr;

Utilities::Utilities() {
	singleton = this;
	frame = 0;
	max_timestamp_query_elements = CLAMP(int(GLOBAL_GET("debug/settings/profiler/max_timestamp_query_elements")), 256, 65535);
#ifdef GL_API_ENABLED
	if (RasterizerUtilGLES3::is_gles_over_gl()) {
		GLint bits = 0;
		glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
		if (bits > 0) {
			gpu_timer = GL_TIMESTAMP;
		} else {
			glGetQueryiv(GL_TIME_ELAPSED, GL_QUERY_COUNTER_BITS, &bits);
			if (bits > 0) {
				gpu_timer = GL_TIME_ELAPSED;
			}
		}
	}
#endif
	for (int i = 0; i < FRAME_COUNT; i++) {
		frames[i].index = 0;
		frames[i].queries.resize(max_timestamp_query_elements);
		glGenQueries(max_timestamp_query_elements, frames[i].queries.ptr());

		frames[i].timestamp_names.resize(max_timestamp_query_elements);
		frames[i].timestamp_cpu_values.resize(max_timestamp_query_elements);
		frames[i].timestamp_count = 0;

		frames[i].timestamp_result_names.resize(max_timestamp_query_elements);
		frames[i].timestamp_cpu_result_values.resize(max_timestamp_query_elements);
		frames[i].timestamp_result_values.resize(max_timestamp_query_elements);
		frames[i].timestamp_result_count = 0;
	}
}

Utilities::~Utilities() {
#ifdef GL_API_ENABLED
	if (elapsed_query_open) {
		glEndQuery(GL_TIME_ELAPSED);
	}
#endif
	singleton = nullptr;
	for (int i = 0; i < FRAME_COUNT; i++) {
		glDeleteQueries(max_timestamp_query_elements, frames[i].queries.ptr());
	}

	if (texture_mem_cache) {
		uint32_t leaked_data_size = 0;
		for (const KeyValue<GLuint, ResourceAllocation> &E : texture_allocs_cache) {
#ifdef DEV_ENABLED
			ERR_PRINT(E.value.name + ": leaked " + itos(E.value.size) + " bytes.");
#else
			ERR_PRINT("Texture with GL ID of " + itos(E.key) + ": leaked " + itos(E.value.size) + " bytes.");
#endif
			leaked_data_size += E.value.size;
		}
		if (leaked_data_size < texture_mem_cache) {
			ERR_PRINT("Texture cache is not empty. There may be an additional texture leak of " + itos(texture_mem_cache - leaked_data_size) + " bytes.");
		}
	}

	if (render_buffer_mem_cache) {
		uint32_t leaked_data_size = 0;
		for (const KeyValue<GLuint, ResourceAllocation> &E : render_buffer_allocs_cache) {
#ifdef DEV_ENABLED
			ERR_PRINT(E.value.name + ": leaked " + itos(E.value.size) + " bytes.");
#else
			ERR_PRINT("Render buffer with GL ID of " + itos(E.key) + ": leaked " + itos(E.value.size) + " bytes.");
#endif
			leaked_data_size += E.value.size;
		}
		if (leaked_data_size < render_buffer_mem_cache) {
			ERR_PRINT("Render buffer cache is not empty. There may be an additional render buffer leak of " + itos(render_buffer_mem_cache - leaked_data_size) + " bytes.");
		}
	}

	if (buffer_mem_cache) {
		uint32_t leaked_data_size = 0;

		for (const KeyValue<GLuint, ResourceAllocation> &E : buffer_allocs_cache) {
#ifdef DEV_ENABLED
			ERR_PRINT(E.value.name + ": leaked " + itos(E.value.size) + " bytes.");
#else
			ERR_PRINT("Buffer with GL ID of " + itos(E.key) + ": leaked " + itos(E.value.size) + " bytes.");
#endif
			leaked_data_size += E.value.size;
		}
		if (leaked_data_size < buffer_mem_cache) {
			ERR_PRINT("Buffer cache is not empty. There may be an additional buffer leak of " + itos(buffer_mem_cache - leaked_data_size) + " bytes.");
		}
	}
}

Vector<uint8_t> Utilities::buffer_get_data(GLenum p_target, GLuint p_buffer, uint32_t p_buffer_size) {
	Vector<uint8_t> ret;

	if (p_buffer_size == 0) {
		return ret;
	}

	ret.resize(p_buffer_size);
	glBindBuffer(p_target, p_buffer);

#if defined(__EMSCRIPTEN__)
	{
		uint8_t *w = ret.ptrw();
		godot_webgl2_glGetBufferSubData(p_target, 0, p_buffer_size, w);
	}
#else
	void *data = glMapBufferRange(p_target, 0, p_buffer_size, GL_MAP_READ_BIT);
	ERR_FAIL_NULL_V(data, Vector<uint8_t>());
	{
		uint8_t *w = ret.ptrw();
		memcpy(w, data, p_buffer_size);
	}
	glUnmapBuffer(p_target);
#endif
	glBindBuffer(p_target, 0);
	return ret;
}

/* INSTANCES */

RSE::InstanceType Utilities::get_base_type(RID p_rid) const {
	if (GLES3::MeshStorage::get_singleton()->owns_mesh(p_rid)) {
		return RSE::INSTANCE_MESH;
	} else if (GLES3::MeshStorage::get_singleton()->owns_multimesh(p_rid)) {
		return RSE::INSTANCE_MULTIMESH;
	} else if (GLES3::LightStorage::get_singleton()->owns_light(p_rid)) {
		return RSE::INSTANCE_LIGHT;
	} else if (GLES3::LightStorage::get_singleton()->owns_lightmap(p_rid)) {
		return RSE::INSTANCE_LIGHTMAP;
	} else if (GLES3::ParticlesStorage::get_singleton()->owns_particles(p_rid)) {
		return RSE::INSTANCE_PARTICLES;
	} else if (GLES3::LightStorage::get_singleton()->owns_reflection_probe(p_rid)) {
		return RSE::INSTANCE_REFLECTION_PROBE;
	} else if (GLES3::ParticlesStorage::get_singleton()->owns_particles_collision(p_rid)) {
		return RSE::INSTANCE_PARTICLES_COLLISION;
	} else if (GLES3::TextureStorage::get_singleton()->owns_decal(p_rid)) {
		return RSE::INSTANCE_DECAL;
	} else if (owns_visibility_notifier(p_rid)) {
		return RSE::INSTANCE_VISIBLITY_NOTIFIER;
	}
	return RSE::INSTANCE_NONE;
}

bool Utilities::free(RID p_rid) {
	if (GLES3::TextureStorage::get_singleton()->owns_render_target(p_rid)) {
		GLES3::TextureStorage::get_singleton()->render_target_free(p_rid);
		return true;
	} else if (GLES3::TextureStorage::get_singleton()->owns_texture(p_rid)) {
		GLES3::TextureStorage::get_singleton()->texture_free(p_rid);
		return true;
	} else if (GLES3::TextureStorage::get_singleton()->owns_canvas_texture(p_rid)) {
		GLES3::TextureStorage::get_singleton()->canvas_texture_free(p_rid);
		return true;
	} else if (GLES3::MaterialStorage::get_singleton()->owns_shader(p_rid)) {
		GLES3::MaterialStorage::get_singleton()->shader_free(p_rid);
		return true;
	} else if (GLES3::MaterialStorage::get_singleton()->owns_material(p_rid)) {
		GLES3::MaterialStorage::get_singleton()->material_free(p_rid);
		return true;
	} else if (GLES3::MeshStorage::get_singleton()->owns_mesh(p_rid)) {
		GLES3::MeshStorage::get_singleton()->mesh_free(p_rid);
		return true;
	} else if (GLES3::MeshStorage::get_singleton()->owns_multimesh(p_rid)) {
		GLES3::MeshStorage::get_singleton()->multimesh_free(p_rid);
		return true;
	} else if (GLES3::MeshStorage::get_singleton()->owns_mesh_instance(p_rid)) {
		GLES3::MeshStorage::get_singleton()->mesh_instance_free(p_rid);
		return true;
	} else if (GLES3::LightStorage::get_singleton()->owns_light(p_rid)) {
		GLES3::LightStorage::get_singleton()->light_free(p_rid);
		return true;
	} else if (GLES3::LightStorage::get_singleton()->owns_lightmap(p_rid)) {
		GLES3::LightStorage::get_singleton()->lightmap_free(p_rid);
		return true;
	} else if (GLES3::LightStorage::get_singleton()->owns_reflection_probe(p_rid)) {
		GLES3::LightStorage::get_singleton()->reflection_probe_free(p_rid);
		return true;
	} else if (GLES3::LightStorage::get_singleton()->owns_reflection_atlas(p_rid)) {
		GLES3::LightStorage::get_singleton()->reflection_atlas_free(p_rid);
		return true;
	} else if (GLES3::LightStorage::get_singleton()->owns_reflection_probe_instance(p_rid)) {
		GLES3::LightStorage::get_singleton()->reflection_probe_instance_free(p_rid);
		return true;
	} else if (GLES3::ParticlesStorage::get_singleton()->owns_particles(p_rid)) {
		GLES3::ParticlesStorage::get_singleton()->particles_free(p_rid);
		return true;
	} else if (GLES3::ParticlesStorage::get_singleton()->owns_particles_collision(p_rid)) {
		GLES3::ParticlesStorage::get_singleton()->particles_collision_free(p_rid);
		return true;
	} else if (GLES3::ParticlesStorage::get_singleton()->owns_particles_collision_instance(p_rid)) {
		GLES3::ParticlesStorage::get_singleton()->particles_collision_instance_free(p_rid);
		return true;
	} else if (GLES3::MeshStorage::get_singleton()->owns_skeleton(p_rid)) {
		GLES3::MeshStorage::get_singleton()->skeleton_free(p_rid);
		return true;
	} else if (GLES3::TextureStorage::get_singleton()->owns_decal(p_rid)) {
		GLES3::TextureStorage::get_singleton()->decal_free(p_rid);
		return true;
	} else if (owns_visibility_notifier(p_rid)) {
		visibility_notifier_free(p_rid);
		return true;
	} else {
		return false;
	}
}

/* DEPENDENCIES */

void Utilities::base_update_dependency(RID p_base, DependencyTracker *p_instance) {
	if (MeshStorage::get_singleton()->owns_mesh(p_base)) {
		Mesh *mesh = MeshStorage::get_singleton()->get_mesh(p_base);
		p_instance->update_dependency(&mesh->dependency);
	} else if (MeshStorage::get_singleton()->owns_multimesh(p_base)) {
		MultiMesh *multimesh = MeshStorage::get_singleton()->get_multimesh(p_base);
		p_instance->update_dependency(&multimesh->dependency);
		if (multimesh->mesh.is_valid()) {
			base_update_dependency(multimesh->mesh, p_instance);
		}
	} else if (LightStorage::get_singleton()->owns_reflection_probe(p_base)) {
		Dependency *dependency = LightStorage::get_singleton()->reflection_probe_get_dependency(p_base);
		p_instance->update_dependency(dependency);
	} else if (LightStorage::get_singleton()->owns_light(p_base)) {
		Light *l = LightStorage::get_singleton()->get_light(p_base);
		p_instance->update_dependency(&l->dependency);
	} else if (ParticlesStorage::get_singleton()->owns_particles(p_base)) {
		Dependency *dependency = ParticlesStorage::get_singleton()->particles_get_dependency(p_base);
		p_instance->update_dependency(dependency);
	} else if (ParticlesStorage::get_singleton()->owns_particles_collision(p_base)) {
		Dependency *dependency = ParticlesStorage::get_singleton()->particles_collision_get_dependency(p_base);
		p_instance->update_dependency(dependency);
	} else if (TextureStorage::get_singleton()->owns_decal(p_base)) {
		Dependency *dependency = TextureStorage::get_singleton()->decal_get_dependency(p_base);
		p_instance->update_dependency(dependency);
	} else if (owns_visibility_notifier(p_base)) {
		VisibilityNotifier *vn = get_visibility_notifier(p_base);
		p_instance->update_dependency(&vn->dependency);
	}
}

/* VISIBILITY NOTIFIER */

RID Utilities::visibility_notifier_allocate() {
	return visibility_notifier_owner.allocate_rid();
}

void Utilities::visibility_notifier_initialize(RID p_notifier) {
	visibility_notifier_owner.initialize_rid(p_notifier, VisibilityNotifier());
}

void Utilities::visibility_notifier_free(RID p_notifier) {
	VisibilityNotifier *vn = visibility_notifier_owner.get_or_null(p_notifier);
	vn->dependency.deleted_notify(p_notifier);
	visibility_notifier_owner.free(p_notifier);
}

void Utilities::visibility_notifier_set_aabb(RID p_notifier, const AABB &p_aabb) {
	VisibilityNotifier *vn = visibility_notifier_owner.get_or_null(p_notifier);
	ERR_FAIL_NULL(vn);
	vn->aabb = p_aabb;
	vn->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_AABB);
}

void Utilities::visibility_notifier_set_callbacks(RID p_notifier, const Callable &p_enter_callbable, const Callable &p_exit_callable) {
	VisibilityNotifier *vn = visibility_notifier_owner.get_or_null(p_notifier);
	ERR_FAIL_NULL(vn);
	vn->enter_callback = p_enter_callbable;
	vn->exit_callback = p_exit_callable;
}

AABB Utilities::visibility_notifier_get_aabb(RID p_notifier) const {
	const VisibilityNotifier *vn = visibility_notifier_owner.get_or_null(p_notifier);
	ERR_FAIL_NULL_V(vn, AABB());
	return vn->aabb;
}

void Utilities::visibility_notifier_call(RID p_notifier, bool p_enter, bool p_deferred) {
	VisibilityNotifier *vn = visibility_notifier_owner.get_or_null(p_notifier);
	ERR_FAIL_NULL(vn);

	if (p_enter) {
		if (vn->enter_callback.is_valid()) {
			if (p_deferred) {
				vn->enter_callback.call_deferred();
			} else {
				vn->enter_callback.call();
			}
		}
	} else {
		if (vn->exit_callback.is_valid()) {
			if (p_deferred) {
				vn->exit_callback.call_deferred();
			} else {
				vn->exit_callback.call();
			}
		}
	}
}

/* TIMING */

void Utilities::capture_timestamps_begin() {
	capture_timestamp("Frame Begin");
}

void Utilities::capture_timestamp(const String &p_name) {
	ERR_FAIL_COND(frames[frame].timestamp_count >= max_timestamp_query_elements);

#ifdef GL_API_ENABLED
	if (gpu_timer == GL_TIMESTAMP) {
		glQueryCounter(frames[frame].queries[frames[frame].timestamp_count], GL_TIMESTAMP);
	} else if (gpu_timer == GL_TIME_ELAPSED) {
		// Some desktop GL drivers support interval queries but have no absolute GPU clock.
		if (elapsed_query_open) {
			glEndQuery(GL_TIME_ELAPSED);
		}
		glBeginQuery(GL_TIME_ELAPSED, frames[frame].queries[frames[frame].timestamp_count]);
		elapsed_query_open = true;
	}
#endif // GL_API_ENABLED

	frames[frame].timestamp_names[frames[frame].timestamp_count] = p_name;
	frames[frame].timestamp_cpu_values[frames[frame].timestamp_count] = OS::get_singleton()->get_ticks_usec();
	frames[frame].timestamp_count++;
}

void Utilities::_capture_timestamps_begin() {
#ifdef GL_API_ENABLED
	// Starting an elapsed query can split a render pass. Leave ordinary frames
	// alone unless the profiler or a measured viewport requested GPU timing.
	if (gpu_timer == GL_TIME_ELAPSED && !capturing_timestamps) {
		return;
	}
#endif
	capture_timestamp("Internal Begin");
}

void Utilities::capture_timestamps_end() {
	if (frames[frame].timestamp_count) {
		capture_timestamp("Internal End");
	}
#ifdef GL_API_ENABLED
	if (elapsed_query_open) {
		glEndQuery(GL_TIME_ELAPSED);
		elapsed_query_open = false;
	}
#endif
	frames[frame].index = Engine::get_singleton()->get_frames_drawn();
	frame = (frame + 1) % FRAME_COUNT;

	// Recycle the next slot before offscreen draws can submit new markers
	// between engine frames. Its recorded queries are two frames old.
	Frame &next = frames[frame];
	if (next.timestamp_count) {
#ifdef GL_API_ENABLED
		if (gpu_timer != GL_NONE) {
			uint64_t elapsed = 0;
			for (uint32_t i = 0; i < next.timestamp_count; i++) {
				if (gpu_timer == GL_TIMESTAMP) {
					glGetQueryObjectui64v(next.queries[i], GL_QUERY_RESULT, &next.timestamp_result_values[i]);
				} else {
					next.timestamp_result_values[i] = elapsed;
					if (i + 1 < next.timestamp_count) {
						uint64_t interval = 0;
						glGetQueryObjectui64v(next.queries[i], GL_QUERY_RESULT, &interval);
						elapsed += interval;
					}
				}
			}
		}
#endif
		SWAP(next.timestamp_names, next.timestamp_result_names);
		SWAP(next.timestamp_cpu_values, next.timestamp_cpu_result_values);
	}
	next.timestamp_result_count = next.timestamp_count;
	next.timestamp_count = 0;
}

uint32_t Utilities::get_captured_timestamps_count() const {
	return frames[frame].timestamp_result_count;
}

uint64_t Utilities::get_captured_timestamps_frame() const {
	return frames[frame].index;
}

uint64_t Utilities::get_captured_timestamp_gpu_time(uint32_t p_index) const {
	ERR_FAIL_UNSIGNED_INDEX_V(p_index, frames[frame].timestamp_result_count, 0);
	return frames[frame].timestamp_result_values[p_index];
}

uint64_t Utilities::get_captured_timestamp_cpu_time(uint32_t p_index) const {
	ERR_FAIL_UNSIGNED_INDEX_V(p_index, frames[frame].timestamp_result_count, 0);
	return frames[frame].timestamp_cpu_result_values[p_index];
}

String Utilities::get_captured_timestamp_name(uint32_t p_index) const {
	ERR_FAIL_UNSIGNED_INDEX_V(p_index, frames[frame].timestamp_result_count, String());
	return frames[frame].timestamp_result_names[p_index];
}

/* MISC */

void Utilities::update_dirty_resources() {
	MaterialStorage::get_singleton()->_update_global_shader_uniforms();
	MaterialStorage::get_singleton()->_update_queued_materials();
	MeshStorage::get_singleton()->_update_dirty_skeletons();
	MeshStorage::get_singleton()->_update_dirty_multimeshes();
	TextureStorage::get_singleton()->texture_atlas_redraw_textures();
	TextureStorage::get_singleton()->update_texture_atlas();
	TextureStorage::get_singleton()->update_decal_atlas();
}

void Utilities::set_debug_generate_wireframes(bool p_generate) {
	Config *config = Config::get_singleton();
	config->generate_wireframes = p_generate;
}

bool Utilities::has_os_feature(const String &p_feature) const {
	Config *config = Config::get_singleton();
	if (!config) {
		return false;
	}

	if (p_feature == "rgtc") {
		return config->rgtc_supported;
	}
	if (p_feature == "s3tc") {
		return config->s3tc_supported;
	}
	if (p_feature == "bptc") {
		return config->bptc_supported;
	}
	if (p_feature == "astc") {
		return config->astc_supported;
	}
	if (p_feature == "etc2") {
		return config->etc2_supported;
	}
	if (p_feature == "astc_hdr") {
		return config->astc_hdr_supported;
	}

	return false;
}

void Utilities::update_memory_info() {
}

uint64_t Utilities::get_rendering_info(RSE::RenderingInfo p_info) {
	if (p_info == RSE::RENDERING_INFO_TEXTURE_MEM_USED) {
		return texture_mem_cache + render_buffer_mem_cache; // Add render buffer memory to our texture mem.
	} else if (p_info == RSE::RENDERING_INFO_BUFFER_MEM_USED) {
		return buffer_mem_cache;
	} else if (p_info == RSE::RENDERING_INFO_VIDEO_MEM_USED) {
		return texture_mem_cache + buffer_mem_cache + render_buffer_mem_cache;
	}
	return 0;
}

String Utilities::get_video_adapter_name() const {
	const String rendering_device_name = String::utf8((const char *)glGetString(GL_RENDERER));
	// NVIDIA suffixes all GPU model names with "/PCIe/SSE2" in OpenGL (but not Vulkan). This isn't necessary to display nowadays, so it can be trimmed.
	return rendering_device_name.trim_suffix("/PCIe/SSE2");
}

String Utilities::get_video_adapter_vendor() const {
	const String rendering_device_vendor = String::utf8((const char *)glGetString(GL_VENDOR));
	// NVIDIA suffixes its vendor name with " Corporation". This is neither necessary to process nor display.
	return rendering_device_vendor.trim_suffix(" Corporation");
}

String Utilities::get_video_adapter_api_version() const {
	return String::utf8((const char *)glGetString(GL_VERSION));
}

Size2i Utilities::get_maximum_viewport_size() const {
	Config *config = Config::get_singleton();
	ERR_FAIL_NULL_V(config, Size2i());
	return Size2i(config->max_viewport_size[0], config->max_viewport_size[1]);
}

uint32_t Utilities::get_maximum_shader_varyings() const {
	Config *config = Config::get_singleton();
	ERR_FAIL_NULL_V(config, 31);
	return config->max_shader_varyings;
}

uint64_t Utilities::get_maximum_uniform_buffer_size() const {
	Config *config = Config::get_singleton();
	ERR_FAIL_NULL_V(config, 65536);
	return uint64_t(config->max_uniform_buffer_size);
}

#endif // GLES3_ENABLED
