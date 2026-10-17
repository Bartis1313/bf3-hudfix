#include "hudfix.h"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hudfix
{
    // most of this comes from my bf-visual-editor
    namespace
    {
        using SetVectorFn = void(__thiscall*)(fb::ShaderParameterBlock* block, unsigned int index, unsigned int handle, const float* value);
        using ReleaseInstancesFn = void(__thiscall*)(fb::InternalDatabasePartition* partition, int unused);
        using PreRegistrationFn = bool(__thiscall*)(fb::MeshSet* meshSet, int compartment);
        ReleaseInstancesFn oReleaseInstances = nullptr;
        PreRegistrationFn oPreRegistration = nullptr;
        
        // mesh stuff
        using ArenaAllocFn = void*(__thiscall*)(void* arena, unsigned int size, unsigned int align);
        SetVectorFn oSetVector = nullptr;

        using fb::MeshVariationNode;
        using fb::MeshVariationManager;
        constexpr unsigned int TEXTURE_BYTES = 4;

        struct Reticle
        {
            unsigned int meshHash;
            unsigned int material;
            bool grow; // its render block may carry no slot for the parameter
            bool hasOriginal;
            float original[4];
            bool checkShader = false; // a material without vectors of its own
            std::string mesh;
        };
        // a render block replaced by one with a color slot, put back for DEFAULT
        struct Grown
        {
            fb::SurfaceShaderInstance* instance;
            fb::ShaderParameterBlock* original;
            unsigned short originalHeader;
            fb::ShaderParameterBlock* grown;
        };

        struct ReticleProxy
        {
            const fb::InternalDatabasePartition* partition;
			float* transform; // LinearTransform
            float original[16];
            std::string mesh;
        };
        std::vector<ReticleProxy> g_proxies;
        // reticle mesh name -> its bounding box; every reticle is a flat quad whose aim point need not be its center
        struct Bounds
        {
            float low[3], high[3];
        };
        std::unordered_map<std::string, Bounds> g_bounds;
        // each reticle also comes as "*_reticule_small_*"
        constexpr const char* RETICLE = "reticule_";
        constexpr const char* SMALL = "small_";
        // spans closer than this tell no fixed point
        constexpr float MIN_SHRINK = 0.02f;
        // weapon sockets whose zoom asset is still an unresolved import when their partition loads
        struct ProxyCandidate
        {
            const fb::InternalDatabasePartition* partition;
            fb::WeaponRegularSocketObjectData* object;
        };
        std::vector<ProxyCandidate> g_candidates;

        // sub_1038E90 copies it
        constexpr int MAX_LIVE_RECORDS = 256;
        struct LiveRecord
        {
            std::string mesh;
            std::string weapon; // the socket's partition
            const fb::InternalDatabasePartition* partition = nullptr;
            float original[16]; // unscaled Transform
            float written[16]; // what was put on the record last
            float uniform = 0.0f; // how much it's scaled to the "good" size, in here ak74
        };
        std::unordered_map<fb::AttachmentRecord*, LiveRecord> g_live;

        using DrawHolderFn = char(__thiscall*)(fb::AttachmentHolder* holder, char firstPerson, char zoomed, void* pose, void* list, char flag);
        DrawHolderFn oDrawHolder = nullptr;
        // every weapon puts its reticle at its own depth past the sight body, so to prevent that use value that is middle of everything, will not be issue anyway
        constexpr float EYE_RELIEF = 0.1f;
        struct SightBaseline
        {
            const char* folder; // weapons/accessories/some_weapon_name...
            float depth;
            float fov;
        };
        constexpr SightBaseline BASELINES[] = { { "eotech", 0.337f, 20.0f }, { "kobra", 0.337f, 18.0f }, { "pka-s", 0.353f, 15.0f }, { "rx01", 0.329f, 17.0f } };
        constexpr float MIN_UNIFORM = 0.25f;
        constexpr float MAX_UNIFORM = 2.5f;
        std::map<std::pair<const fb::InternalDatabasePartition*, std::string>, float> g_bodies;

        std::vector<Reticle> g_reticles;
        std::vector<Grown> g_grown;
        // (render block, parameter)
        std::unordered_map<unsigned long long, std::array<float, 4>> g_held;
        // (render block, parameter)
        std::unordered_map<unsigned long long, std::array<float, 4>> g_blockOriginal;
        // shader -> whether it reads color
        std::unordered_map<const fb::SurfaceShader*, bool> g_colorShaders;
        std::mutex g_lock;
        unsigned int g_appliedElements = 0;
        // the loading thread fills the variation table, so it is only walked once its size settles
        constexpr ULONGLONG SETTLE_MS = 2000;
        unsigned int g_seenElements = 0;
        ULONGLONG g_seenTime = 0;

        std::string lower(const char* text)
        {
            std::string out = text ? text : "";
            for (char& c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        // the 1p and 1p zoom attachment records of every weapon the local soldier carries
        int liveRecords(fb::AttachmentRecord* (&out)[MAX_LIVE_RECORDS])
        {
            int found = 0;
            fb::ClientPlayer* local = fb::ClientPlayer::GetLocal();
            fb::ClientSoldierEntity* soldier = local ? local->getSoldier() : nullptr;
            fb::ClientSoldierWeaponsComponent* weapons = soldier ? soldier->m_weapons : nullptr;
            if (!weapons)
                return 0;
            for (fb::ClientSoldierWeapon** w = weapons->m_weaponsBegin; w && w != weapons->m_weaponsEnd; ++w)
            {
                if (!*w || reinterpret_cast<uintptr_t>((*w)->m_vtable) != OFF_ClientSoldierWeapon_vtable)
                    continue;
                for (fb::AttachmentHolder* h = (*w)->m_holdersBegin; h && h != (*w)->m_holdersEnd; ++h)
                {
                    for (fb::AttachmentRecord* r = h->m_1pBegin; r && r != h->m_1pEnd && found < MAX_LIVE_RECORDS; ++r)
                        out[found++] = r;
                    for (fb::AttachmentRecord* r = h->m_1pZoomBegin; r && r != h->m_1pZoomEnd && found < MAX_LIVE_RECORDS; ++r)
                        out[found++] = r;
                }
            }
            return found;
        }

        const char* recordMesh(const fb::AttachmentRecord* record)
        {
            const fb::MeshSetLayout* layout = record->m_meshSet ? record->m_meshSet->m_layout : nullptr;
            return layout ? layout->m_name : nullptr;
        }

        bool sameTransform(const float* a, const float* b)
        {
            for (int i = 0; i < 16; ++i)
            {
                if (std::abs(a[i] - b[i]) > 1e-5f * (std::max)(1.0f, std::abs(b[i])))
                    return false;
            }
            return true;
        }

        unsigned int parameterHandle(const char* name)
        {
            unsigned int hash = 5381;
            for (; *name; ++name)
                hash = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(*name))) ^ (33 * hash);
            return hash;
        }

        const unsigned int COLOR_HANDLE = parameterHandle("Color");

        unsigned long long heldKey(const fb::ShaderParameterBlock* block, unsigned int handle)
        {
            return (static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(block)) << 32) | handle;
        }

        constexpr unsigned int MAX_SOLUTIONS = 64;

        bool readDxbc(const fb::DxShaderSolution* solution, const char*& data, unsigned int& size)
        {
            const fb::PixelShaderPermutation* permutation = solution->m_pixelPermutation;
            data = permutation ? reinterpret_cast<const char*>(permutation->m_data) : nullptr;
            size = permutation ? permutation->m_dataSize : 0;
            return data && size >= 32 && size <= (1u << 20) && std::memcmp(data, "DXBC", 4) == 0;
        }

        // whether a pixel shader of the instance's shader reads external_<name>
        bool shaderReads(const fb::SurfaceShaderInstance* instance, const char* name)
        {
            const fb::SurfaceShader* shader = instance->m_shader;
            const fb::SurfaceShaderSolution* pairs = shader ? shader->m_solutions : nullptr;
            const unsigned int count = shader ? shader->m_solutionCount : 0;
            const std::string wanted = std::string("external_") + name;
            for (unsigned int p = 0; pairs && p < count && p < MAX_SOLUTIONS; ++p)
            {
                const fb::DxShaderSolution* solution = pairs[p].m_solution;
                const char* data = nullptr;
                unsigned int size = 0;
                if (!solution || !readDxbc(solution, data, size))
                    continue;

                const std::string_view bytes(data, size);
                for (size_t at = bytes.find(wanted); at != std::string_view::npos; at = bytes.find(wanted, at + 1))
                {
                    const size_t end = at + wanted.size();
                    if (end >= bytes.size() || !(std::isalnum(static_cast<unsigned char>(bytes[end])) || bytes[end] == '_'))
                        return true;
                }
            }
            return false;
        }

        std::array<float, 4> colorize(const Reticle& reticle, const Color& color)
        {
            const float alpha = reticle.hasOriginal && reticle.original[3] > 0.0f ? reticle.original[3] : 1.0f;
            return { color.r, color.g, color.b, alpha };
        }

        float* blockVector(fb::ShaderParameterBlock* block, unsigned int handle)
        {
            const fb::ShaderParameterEntry* entries = block->entries();
            for (unsigned int i = 0; i < block->m_vectorCount; ++i)
            {
                if (entries[i].m_handle == handle)
                    return reinterpret_cast<float*>(block->values() + entries[i].m_offset);
            }
            return nullptr;
        }

        // copied bf-visual-editor growBlockWithVector
        fb::ShaderParameterBlock* growBlock(MeshVariationManager* manager, fb::SurfaceShaderInstance* instance, unsigned int handle, const float* value)
        {
            fb::ShaderParameterBlock* old = instance->m_block;
            const unsigned int vec = old ? old->m_vectorCount : 0, tex = old ? old->m_textureCount : 0, bol = old ? old->m_boolCount : 0;
            if (vec + tex + bol > 128 || vec == 255 || !manager->heap)
                return nullptr;

            const unsigned int nvec = vec + 1, total = nvec + tex + bol;
            const unsigned int head = (8 * total + 15) & ~15u;
            const unsigned int size = sizeof(fb::ShaderParameterBlock) + head + 16 * nvec + TEXTURE_BYTES * tex + bol;
            const unsigned short header = static_cast<unsigned short>((size & 0x1FFF) | (1u << 13) | (tex ? 1u << 14 : 0) | (bol ? 1u << 15 : 0));

            auto grown = static_cast<fb::ShaderParameterBlock*>(reinterpret_cast<ArenaAllocFn>(OFF_MemoryArena_alloc)(manager->heap, size, 16));
            if (!grown || (reinterpret_cast<uintptr_t>(grown) & 15))
                return nullptr;

            std::memset(grown, 0, size);
            grown->m_header = header;
            grown->m_size = static_cast<unsigned short>(size);
            grown->m_vectorCount = static_cast<unsigned char>(nvec);
            grown->m_textureCount = static_cast<unsigned char>(tex);
            grown->m_boolCount = static_cast<unsigned char>(bol);

            fb::ShaderParameterEntry* entries = grown->entries();
            unsigned short offset = static_cast<unsigned short>(head);
            for (unsigned int i = 0; i < total; ++i)
            {
                entries[i].m_offset = offset;
                entries[i].m_count = 1;
                offset = static_cast<unsigned short>(offset + (i < nvec ? 16 : i < nvec + tex ? TEXTURE_BYTES : 1));
            }

            unsigned char* values = grown->values();
            const fb::ShaderParameterEntry* oldEntries = old ? old->entries() : nullptr;
            const unsigned char* oldValues = old ? old->values() : nullptr;
            for (unsigned int i = 0; i < vec; ++i)
            {
                entries[i].m_handle = oldEntries[i].m_handle;
                std::memcpy(values + entries[i].m_offset, oldValues + oldEntries[i].m_offset, 16);
            }
            entries[vec].m_handle = handle;
            std::memcpy(values + entries[vec].m_offset, value, 16);
            for (unsigned int i = 0; i < tex + bol; ++i)
            {
                fb::ShaderParameterEntry& to = entries[nvec + i];
                const fb::ShaderParameterEntry& from = oldEntries[vec + i];
                to.m_handle = from.m_handle;
                std::memcpy(values + to.m_offset, oldValues + from.m_offset, i < tex ? TEXTURE_BYTES : 1);
            }

            // grown again: the record keeps the engine's own block
            const auto record = std::find_if(g_grown.begin(), g_grown.end(), [instance](const Grown& g) { return g.instance == instance; });
            if (record != g_grown.end() && record->grown == old)
                record->grown = grown;
            else
            {
                if (record != g_grown.end())
                    g_grown.erase(record);
                g_grown.push_back({ instance, old, instance->m_header, grown });
            }
            instance->m_block = grown;
            instance->m_header = header;
            return grown;
        }

        void restoreGrown(fb::SurfaceShaderInstance* instance)
        {
            for (auto it = g_grown.begin(); it != g_grown.end(); ++it)
            {
                if (it->instance != instance)
                    continue;
                if (instance->m_block == it->grown)
                {
                    instance->m_block = it->original;
                    instance->m_header = it->originalHeader;
                }
                g_grown.erase(it);
                return;
            }
        }

        // the block with a slot for handle, grown when it has none (only dots grow)
        fb::ShaderParameterBlock* withSlot(MeshVariationManager* manager, const Reticle& reticle, fb::SurfaceShaderInstance* instance, unsigned int handle, const float* value)
        {
            fb::ShaderParameterBlock* block = instance->m_block;
            if (float* existing = block ? blockVector(block, handle) : nullptr)
            {
                // the engine's own value, unless the block is one this grew or the value is already held
                const unsigned long long key = heldKey(block, handle);
                if (!g_held.count(key) && !g_blockOriginal.count(key))
                    g_blockOriginal[key] = { existing[0], existing[1], existing[2], existing[3] };
                return block;
            }
            return reticle.grow ? growBlock(manager, instance, handle, value) : nullptr;
        }

        void hold(fb::ShaderParameterBlock* block, unsigned int handle, const std::array<float, 4>& value)
        {
            std::copy(value.begin(), value.end(), blockVector(block, handle));
            g_held[heldKey(block, handle)] = value;
        }

        void restore(const Reticle& reticle, fb::SurfaceShaderInstance* instance)
        {
            if (fb::ShaderParameterBlock* block = instance->m_block)
                g_held.erase(heldKey(block, COLOR_HANDLE));
            restoreGrown(instance);

            fb::ShaderParameterBlock* block = instance->m_block;
            float* value = block ? blockVector(block, COLOR_HANDLE) : nullptr;
            if (!value)
                return;
            if (const auto original = g_blockOriginal.find(heldKey(block, COLOR_HANDLE)); original != g_blockOriginal.end())
                std::copy(original->second.begin(), original->second.end(), value);
            else if (reticle.hasOriginal)
                std::copy(reticle.original, reticle.original + 3, value);
        }

        // the reticle's material of a variation set in the scope color, or back to its own
        bool tint(MeshVariationManager* manager, Reticle& reticle, fb::MeshVariationSet* set)
        {
            if (reticle.material >= set->m_instanceCount)
                return false;

            fb::SurfaceShaderInstance* instance = &set->m_instances[reticle.material];
            if (reticle.checkShader)
            {
                const fb::SurfaceShader* shader = instance->m_shader;
                auto reads = g_colorShaders.find(shader);
                if (reads == g_colorShaders.end())
                    reads = g_colorShaders.emplace(shader, shaderReads(instance, "Color")).first;
                if (!reads->second)
                    return false;
            }

            const Color* color = slotColor(SightColor);
            if (!color)
            {
                restore(reticle, instance);
                return true;
            }

            alignas(16) const std::array<float, 4> hue = colorize(reticle, *color);
            fb::ShaderParameterBlock* block = withSlot(manager, reticle, instance, COLOR_HANDLE, hue.data());
            if (!block)
                return false;
            hold(block, COLOR_HANDLE, hue);
            return true;
        }

        // every variation of every tinted mesh, then the sets the local soldier's weapons draw with
        int applyLocked()
        {
            MeshVariationManager* manager = *reinterpret_cast<MeshVariationManager**>(OFF_g_meshVariationManager);
            if (!manager || !manager->buckets)
                return 0;

            int written = 0;
            std::unordered_set<fb::MeshVariationSet*> sets;
            for (unsigned int b = 0; b < manager->bucketCount; ++b)
            {
                for (MeshVariationNode* node = manager->buckets[b]; node; node = node->next)
                {
                    if (!node->set)
                        continue;

                    const unsigned int meshHash = static_cast<unsigned int>(node->key >> 32);
                    for (Reticle& reticle : g_reticles)
                    {
                        if (reticle.meshHash == meshHash && tint(manager, reticle, node->set))
                        {
                            sets.insert(node->set);
                            ++written;
                        }
                    }
                }
            }

            fb::AttachmentRecord* records[MAX_LIVE_RECORDS];
            const int count = liveRecords(records);
            for (int i = 0; i < count; ++i)
            {
                const char* name = recordMesh(records[i]);
                fb::MeshVariationSet* set = records[i]->m_variation;
                if (!name || !set)
                    continue;
                const std::string mesh = lower(name);
                for (Reticle& reticle : g_reticles)
                {
                    if (reticle.mesh == mesh && !sets.count(set))
                        written += tint(manager, reticle, set);
                }
            }
            g_appliedElements = manager->elementCount;
            return written;
        }

        void __fastcall hkSetVector(fb::ShaderParameterBlock* _this, void*, unsigned int index, unsigned int handle, const float* value)
        {
            if (handle == COLOR_HANDLE)
            {
                alignas(16) float held[4];
                bool found = false;
                {
                    std::lock_guard lock(g_lock);
                    if (const auto it = g_held.find(heldKey(_this, handle)); it != g_held.end())
                    {
                        std::copy(it->second.begin(), it->second.end(), held);
                        found = true;
                    }
                }
                if (found)
                {
                    oSetVector(_this, index, handle, held);
                    return;
                }
            }
            oSetVector(_this, index, handle, value);
        }

        // the other size of a reticle mesh: "x_reticule_small_1p_mesh" <-> "x_reticule_1p_mesh"
        std::string otherSize(const std::string& mesh)
        {
            const size_t at = mesh.rfind(RETICLE);
            if (at == std::string::npos)
                return { };
            std::string other = mesh;
            const size_t rest = at + std::strlen(RETICLE);
            if (other.compare(rest, std::strlen(SMALL), SMALL) == 0)
                other.erase(rest, std::strlen(SMALL));
            else
                other.insert(rest, SMALL);
            return other;
        }

        // the point the two sizes share, per axis; the box center where they don't tell (no small size, a flat axis)
        bool aimPoint(const std::string& mesh, float out[3])
        {
            const auto own = g_bounds.find(mesh);
            if (own == g_bounds.end())
                return false;

            const auto other = g_bounds.find(otherSize(mesh));
            for (int axis = 0; axis < 3; ++axis)
            {
                const float low = own->second.low[axis], high = own->second.high[axis];
                out[axis] = (low + high) * 0.5f;
                if (other == g_bounds.end() || high - low <= 0.0f)
                    continue;

                // other = aim + r * (own - aim) on both edges
                const float r = (other->second.high[axis] - other->second.low[axis]) / (high - low);
                if (std::abs(1.0f - r) < MIN_SHRINK)
                    continue;
                const float aim = (other->second.low[axis] - r * low) / (1.0f - r);
                if (aim >= (std::min)(low, other->second.low[axis]) && aim <= (std::max)(high, other->second.high[axis]))
                    out[axis] = aim;
            }
            return true;
        }

        // around the reticle's aim point, so it stays on the bullet's path
        void scaleTransform(const std::string& mesh, const float* original, float factor, float* out)
        {
            float aim[3];
            const float* center = aimPoint(mesh, aim) ? aim : nullptr;
            for (int c = 0; c < 3; ++c)
            {
                float shift = 0.0f;
                for (int axis = 0; axis < 3; ++axis)
                {
                    out[axis * 4 + c] = original[axis * 4 + c] * factor;
                    if (center)
                        shift += original[axis * 4 + c] * center[axis];
                }
                out[12 + c] = original[12 + c] + shift * (1.0f - factor);
            }
        }

        void scaleProxy(const ReticleProxy& proxy, float factor)
        {
            scaleTransform(proxy.mesh, proxy.original, factor, proxy.transform);
        }

        bool fromProxy(const fb::AttachmentRecord* record, LiveRecord& entry)
        {
            const char* name = recordMesh(record);
            if (!name)
                return false;
            const std::string mesh = lower(name);
            for (const ReticleProxy& proxy : g_proxies)
            {
                // spawned before the socket was scaled, it still has the original
                if (proxy.mesh != mesh || !(sameTransform(record->m_transform, proxy.transform) || sameTransform(record->m_transform, proxy.original)))
                    continue;

                entry.mesh = mesh;
                entry.weapon = lower(proxy.partition->m_name);
                entry.partition = proxy.partition;
                std::copy(proxy.original, proxy.original + 16, entry.original);
                std::copy(record->m_transform, record->m_transform + 16, entry.written);
                return true;
            }
            return false;
        }

        // the live weapons' sight records, before the sockets change
        void trackLiveRecords()
        {
            fb::AttachmentRecord* records[MAX_LIVE_RECORDS];
            const int count = liveRecords(records);
            std::unordered_map<fb::AttachmentRecord*, LiveRecord> live;
            for (int i = 0; i < count; ++i)
            {
                fb::AttachmentRecord* record = records[i];
                if (const auto known = g_live.find(record); known != g_live.end() && sameTransform(record->m_transform, known->second.written))
                {
                    live.insert(*known);
                    continue;
                }
                LiveRecord entry;
                if (fromProxy(record, entry))
                    live[record] = entry;
            }
            g_live.swap(live);
        }

        // the widest side of a reticle quad
        float meshExtent(const std::string& mesh)
        {
            const auto bounds = g_bounds.find(mesh);
            if (bounds == g_bounds.end())
                return 0.0f;
            return (std::max)(bounds->second.high[0] - bounds->second.low[0], bounds->second.high[1] - bounds->second.low[1]);
        }

        // "weapons/accessories/eotech/eotech_reticule_1p_mesh" -> "eotech"
        std::string accessoryFolder(const std::string& mesh)
        {
            constexpr std::string_view ACCESSORIES = "weapons/accessories/";
            if (!mesh.starts_with(ACCESSORIES))
                return { };
            const size_t end = mesh.find('/', ACCESSORIES.size());
            return end == std::string::npos ? std::string() : mesh.substr(ACCESSORIES.size(), end - ACCESSORIES.size());
        }

        // the weapon a sight holder belongs to
        fb::ClientSoldierWeapon* holderWeapon(const fb::AttachmentHolder* holder)
        {
            fb::ClientPlayer* local = fb::ClientPlayer::GetLocal();
            fb::ClientSoldierEntity* soldier = local ? local->getSoldier() : nullptr;
            fb::ClientSoldierWeaponsComponent* weapons = soldier ? soldier->m_weapons : nullptr;
            for (fb::ClientSoldierWeapon** w = weapons ? weapons->m_weaponsBegin : nullptr; w && w != weapons->m_weaponsEnd; ++w)
            {
                if (*w && reinterpret_cast<uintptr_t>((*w)->m_vtable) == OFF_ClientSoldierWeapon_vtable && holder >= (*w)->m_holdersBegin && holder < (*w)->m_holdersEnd)
                    return *w;
            }
            return nullptr;
        }

        // how much the reticle is scaled to look as big as the same sight's on the ak74
        float uniformFactor(const LiveRecord& entry, fb::ClientSoldierWeapon* weapon)
        {
            const std::string folder = accessoryFolder(entry.mesh);
            const auto baseline = std::find_if(std::begin(BASELINES), std::end(BASELINES), [&folder](const SightBaseline& b) { return folder == b.folder; });
            if (baseline == std::end(BASELINES))
                return 1.0f; // not 1x
            const auto body = g_bodies.find({ entry.partition, folder });
            if (body == g_bodies.end())
                return 1.0f;

            // the small twin is measured against the full size one
            const bool twin = entry.mesh.find(std::string(RETICLE) + SMALL) != std::string::npos;
            const float own = meshExtent(entry.mesh), full = twin ? meshExtent(otherSize(entry.mesh)) : own;
            const float fov = weapon ? reinterpret_cast<float(__thiscall*)(fb::ClientSoldierWeapon*, char)>(OFF_ClientSoldierWeapon_renderFov)(weapon, 1) : 0.0f;
            if (own <= 0.0f || full <= 0.0f || !(fov > 0.0f && fov < 180.0f))
                return 0.0f;

            const float depth = (std::max)(entry.original[14] - body->second, 0.0f);
            constexpr float HALF_DEGREE = 3.14159265f / 360.0f;
            const float depthRatio = (depth + EYE_RELIEF) / (baseline->depth + EYE_RELIEF);
            const float twinRatio = full / own;
            const float fovRatio = std::tan(fov * HALF_DEGREE) / std::tan(baseline->fov * HALF_DEGREE);
            const float factor = std::clamp(depthRatio * twinRatio * fovRatio, MIN_UNIFORM, MAX_UNIFORM);
            log("uniform sights: {} {} depth {:.3f} gun fov {:.1f}: {:.0f}% (depth {:.2f} x small twin {:.2f} x fov {:.2f})", entry.weapon, entry.mesh, depth, fov,
                factor * 100.0f, depthRatio, twinRatio, fovRatio);
            return factor;
        }

        // the zoomed reticles of the local soldier's weapon
        void uniformReticles(fb::AttachmentHolder* holder)
        {
            if (holder->m_1pZoomBegin == holder->m_1pZoomEnd)
                return;

            std::lock_guard lock(g_lock);
            const float scale = elementFactor(Scope);
            for (fb::AttachmentRecord* record = holder->m_1pZoomBegin; record && record != holder->m_1pZoomEnd; ++record)
            {
                auto known = g_live.find(record);
                if (known == g_live.end() || !sameTransform(record->m_transform, known->second.written))
                {
                    LiveRecord entry;
                    if (!fromProxy(record, entry))
                        continue;
                    known = g_live.insert_or_assign(record, entry).first;
                }
                LiveRecord& entry = known->second;
                if (entry.uniform <= 0.0f)
                    entry.uniform = uniformFactor(entry, holderWeapon(holder));
                scaleTransform(entry.mesh, entry.original, scale * (entry.uniform > 0.0f ? entry.uniform : 1.0f), entry.written);
                std::copy(entry.written, entry.written + 16, record->m_transform);
            }
        }

        char __fastcall hkDrawHolder(fb::AttachmentHolder* _this, void*, char firstPerson, char zoomed, void* pose, void* list, char flag)
        {
            if (firstPerson && zoomed && settings().uniformSights)
                uniformReticles(_this);
            return oDrawHolder(_this, firstPerson, zoomed, pose, list, flag);
        }

        void scaleLiveRecords(float factor)
        {
            for (auto& [record, entry] : g_live)
            {
                scaleTransform(entry.mesh, entry.original, factor, entry.written);
                std::copy(entry.written, entry.written + 16, record->m_transform);
            }
        }

        // each mesh's render data, loaded once; only the reticles' bounds are kept
        bool __fastcall hkPreRegistration(fb::MeshSet* _this, void*, int compartment)
        {
            const bool result = oPreRegistration(_this, compartment);
            const fb::MeshSetLayout* layout = _this->m_layout;
            const char* name = layout ? layout->m_name : nullptr;
            if (name && lower(name).find("reticule") != std::string::npos)
            {
                const float* low = layout->m_boundingMin;
                const float* high = layout->m_boundingMax;
                const std::string mesh = lower(name);
                const std::string other = otherSize(mesh);

                std::lock_guard lock(g_lock);
                g_bounds[mesh] = { { low[0], low[1], low[2] }, { high[0], high[1], high[2] } };

                // the other size's aim point may have changed with this one
                const float factor = elementFactor(Scope);
                for (const ReticleProxy& proxy : g_proxies)
                {
                    if (proxy.mesh == mesh || proxy.mesh == other)
                        scaleProxy(proxy, factor);
                }
            }
            return result;
        }

        // an import is resolved by now, but stays a data pointer of unknown kind
        const char* meshName(fb::DataContainer* mesh)
        {
            if (!mesh || (reinterpret_cast<uintptr_t>(mesh) & 3))
                return nullptr;

            const char* type = fb::typeName(mesh);
            const size_t length = type ? std::strlen(type) : 0;
            if (length < 9 || std::strcmp(type + length - 9, "MeshAsset") != 0)
                return nullptr;

            return static_cast<const fb::MeshAsset*>(mesh)->m_Name;
        }

        // the candidates whose mesh is a reticle become proxies
        void resolveProxies()
        {
            std::erase_if(g_candidates, [](const ProxyCandidate& candidate)
            {
                // a sight body, its reticle's depth is counted from it
                if (const char* first = meshName(candidate.object->m_asset1p))
                {
                    const std::string name = lower(first);
                    const std::string folder = accessoryFolder(name);
                    if (!folder.empty() && name.find("reticule") == std::string::npos)
                        g_bodies.emplace(std::pair{ candidate.partition, folder }, candidate.object->m_transform[14]);
                }

                const char* mesh = meshName(candidate.object->m_zoomAsset);
                if (!mesh)
                    return false; // not resolved yet

                const std::string name = lower(mesh);
                if (name.find("reticule") == std::string::npos)
                    return true;

                ReticleProxy proxy{ candidate.partition, candidate.object->m_transform };
                std::copy(proxy.transform, proxy.transform + 16, proxy.original);
                proxy.mesh = name;
                g_proxies.push_back(proxy);
                return true;
            });
        }

        void __fastcall hkReleaseInstances(fb::InternalDatabasePartition* _this, void*, int unused)
        {
            {
                std::lock_guard lock(g_lock);
                std::erase_if(g_proxies, [_this](const ReticleProxy& proxy) { return proxy.partition == _this; });
                std::erase_if(g_candidates, [_this](const ProxyCandidate& candidate) { return candidate.partition == _this; });
                std::erase_if(g_bodies, [_this](const auto& body) { return body.first.first == _this; });
            }
            forgetZoomLevels(_this);
            oReleaseInstances(_this, unused);
        }

        void record(const Reticle& reticle)
        {
            std::lock_guard lock(g_lock);
            if (std::none_of(g_reticles.begin(), g_reticles.end(), [&](const Reticle& r) { return r.meshHash == reticle.meshHash && r.material == reticle.material; }))
                g_reticles.push_back(reticle);
            g_appliedElements = 0; // its render material may already exist
        }
    }

    void patchReticles(fb::InternalDatabasePartition* partition)
    {
        std::unordered_set<const fb::DataContainer*> own(partition->m_instances.begin(), partition->m_instances.end());
        const std::string partitionName = lower(partition->m_name);
        if (partitionName.find("weapons/") != std::string::npos)
        {
            for (fb::DataContainer* object : partition->m_instances)
            {
                const char* type = fb::typeName(object);
                if (!type || std::strcmp(type, "WeaponRegularSocketObjectData") != 0)
                    continue;

                std::lock_guard lock(g_lock);
                g_candidates.push_back({ partition, static_cast<fb::WeaponRegularSocketObjectData*>(object) });
            }
        }

        for (fb::DataContainer* object : partition->m_instances)
        {
            // RigidMeshAsset, SkinnedMeshAsset...
            const char* type = fb::typeName(object);
            const size_t length = type ? std::strlen(type) : 0;
            if (length < 9 || std::strcmp(type + length - 9, "MeshAsset") != 0)
                continue;

            const auto asset = static_cast<const fb::MeshAsset*>(object);
            const std::string name = lower(asset->m_Name);
            const unsigned int meshHash = asset->m_nameHash;
            const auto& materials = asset->m_materials;

            if (name.find("weapons/accessories/") == std::string::npos)
                continue;

            const bool dot = name.find("reticule") != std::string::npos;
            const bool scope = name.find("zoom") != std::string::npos;
            if (!dot && !scope)
                continue;

            for (uint32_t i = 0; i < materials.size(); ++i)
            {
                Reticle reticle{ meshHash, i, dot, false, { } };
                reticle.mesh = name;

                // a material from another partition is still an unresolved import here
                fb::DataContainer* material = materials.m_firstElement[i];
                const char* materialType = own.count(material) ? fb::typeName(material) : nullptr;
                bool vectors = false;
                if (materialType && std::strcmp(materialType, "MeshMaterial") == 0)
                {
                    const auto& parameters = static_cast<const fb::MeshMaterial*>(material)->m_vectorParameters;
                    vectors = parameters.size() != 0;
                    for (uint32_t j = 0; j < parameters.size(); ++j)
                    {
                        const fb::VectorShaderParameter& parameter = parameters.m_firstElement[j];
                        if (lower(parameter.m_name) != "color")
                            continue;
                        reticle.hasOriginal = true;
                        std::copy(std::begin(parameter.m_value), std::end(parameter.m_value), reticle.original);
                    }
                }

                // scopes, acogs etc... You can also color them of course...
                if (!dot)
                {
                    if (vectors)
                        continue;
                    reticle.checkShader = true;
                    reticle.grow = true;
                }
                record(reticle);
            }
        }
    }

    void applyScopeColor()
    {
        std::lock_guard lock(g_lock);
        const int written = applyLocked();
        log("material colors: {} render materials of {} meshes, {} blocks grown", written, g_reticles.size(), g_grown.size());
    }

    // render materials are built as a map's mesh variation database loads, the color follows them
    void updateScopeColor()
    {
        MeshVariationManager* manager = *reinterpret_cast<MeshVariationManager**>(OFF_g_meshVariationManager);
        if (!manager || manager->elementCount == g_appliedElements)
            return;

        const ULONGLONG now = GetTickCount64();
        if (manager->elementCount != g_seenElements)
        {
            g_seenElements = manager->elementCount;
            g_seenTime = now;
            return;
        }
        if (now - g_seenTime < SETTLE_MS)
            return;

        std::lock_guard lock(g_lock);
        const int written = !g_reticles.empty() ? applyLocked() : 0;
        g_appliedElements = manager->elementCount;
        if (written)
            log("material colors: {} render materials, {} blocks grown", written, g_grown.size());

        const size_t known = g_proxies.size();
        resolveProxies();
        const float factor = elementFactor(Scope);
        for (size_t i = known; i < g_proxies.size(); ++i)
            scaleProxy(g_proxies[i], factor);
    }

    // the sockets for the next spawn, the records of the weapons carried now
    void applyScopeSize()
    {
        const float factor = elementFactor(Scope);
        std::lock_guard lock(g_lock);
        resolveProxies();
        trackLiveRecords();
        size_t unbounded = 0;
        for (const ReticleProxy& proxy : g_proxies)
        {
            scaleProxy(proxy, factor);
            unbounded += !g_bounds.count(proxy.mesh);
        }
        scaleLiveRecords(factor);
        log("scope size {:.0f}% on {} reticle sockets, {} without bounds, uniform sights {}", factor * 100.0f, g_proxies.size(), unbounded,
            settings().uniformSights);
    }

    void installScopeHooks()
    {
        hook(OFF_ShaderParameterBlock_setVector, hkSetVector, &oSetVector);
        hook(OFF_InternalDatabasePartition_releaseInstances, hkReleaseInstances, &oReleaseInstances);
        hook(OFF_MeshSet_preRegistration, hkPreRegistration, &oPreRegistration);
        hook(OFF_AttachmentHolder_draw, hkDrawHolder, &oDrawHolder);
    }
}
