#include "hudfix.h"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
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
        using SetVectorFn = void(__thiscall*)(unsigned char* block, unsigned int index, unsigned int handle, const float* value);
        using ReleaseInstancesFn = void(__thiscall*)(fb::InternalDatabasePartition* partition, int unused);
        using PreRegistrationFn = bool(__thiscall*)(unsigned char* meshSet, int compartment);
        ReleaseInstancesFn oReleaseInstances = nullptr;
        PreRegistrationFn oPreRegistration = nullptr;
        
        // mesh stuff
        constexpr int MESH_SET_LAYOUT = 0x10;
        constexpr int LAYOUT_BOUNDING_MIN = 0x10;
        constexpr int LAYOUT_BOUNDING_MAX = 0x20;
        constexpr int LAYOUT_NAME = 0x58;
        using ArenaAllocFn = void*(__thiscall*)(void* arena, unsigned int size, unsigned int align);
        SetVectorFn oSetVector = nullptr;

        constexpr int ASSET_NAME = 0x8;
        constexpr int MESH_NAME_HASH = 0x18;
        constexpr int MESH_MATERIALS = 0x20;
        constexpr int MATERIAL_VECTOR_PARAMETERS = 0xC + 0x8; // Shader struct, its VectorParameters

        using fb::MeshVariationNode;
        using fb::MeshVariationManager;
        using fb::SET_INSTANCES;
        using fb::SET_INSTANCE_COUNT;
        using fb::INSTANCE_STRIDE;
        constexpr int INSTANCE_BLOCK = 0x4;
        constexpr int INSTANCE_HEADER = 0x8;
        // ShaderParameterBlock
        constexpr int BLOCK_HEADER = 0x0;
        constexpr int BLOCK_SIZE = 0x2;
        constexpr int BLOCK_VECTOR_COUNT = 0x4;
        constexpr int BLOCK_TEXTURE_COUNT = 0x5;
        constexpr int BLOCK_BOOL_COUNT = 0x6;
        constexpr int BLOCK_ENTRIES = 0x10;
        constexpr unsigned int TEXTURE_BYTES = 4;

        struct Reticle
        {
            unsigned int meshHash;
            unsigned int material;
            bool grow; // its render block may carry no slot for the parameter
            bool hasOriginal;
            float original[4];
            float strength = 0.5f;
            bool checkShader = false; // a material without vectors of its own
            std::string mesh;
        };
        // a render block replaced by one with a color slot, put back for DEFAULT
        struct Grown
        {
            unsigned char* instance;
            unsigned char* original;
            unsigned short originalHeader;
            unsigned char* grown;
        };

        // WeaponRegularSocketObjectData
        constexpr int SOCKET_TRANSFORM = 0x20;
        constexpr int SOCKET_ZOOM_ASSET = 0x10;
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
            unsigned char* object;
        };
        std::vector<ProxyCandidate> g_candidates;

        // sub_1038E90 copies it
        constexpr int MAX_LIVE_RECORDS = 256;
        struct LiveRecord
        {
            std::string mesh;
            float original[16]; // unscaled Transform
            float written[16]; // what was put on the record last
        };
        std::unordered_map<fb::AttachmentRecord*, LiveRecord> g_live;

        std::vector<Reticle> g_reticles;
        std::vector<Grown> g_grown;
        // (render block, parameter)
        std::unordered_map<unsigned long long, std::array<float, 4>> g_held;
        // (render block, parameter)
        std::unordered_map<unsigned long long, std::array<float, 4>> g_blockOriginal;
        // shader -> whether it reads color
        std::unordered_map<const unsigned char*, bool> g_colorShaders;
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

        // MeshSet
        const char* recordMesh(const fb::AttachmentRecord* record)
        {
            const unsigned char* layout = record->m_meshSet ? *reinterpret_cast<unsigned char* const*>(record->m_meshSet + MESH_SET_LAYOUT) : nullptr;
            return layout ? *reinterpret_cast<const char* const*>(layout + LAYOUT_NAME) : nullptr;

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

        unsigned long long heldKey(const unsigned char* block, unsigned int handle)
        {
            return (static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(block)) << 32) | handle;
        }

        // SurfaceShader solution pairs
        constexpr int SHADER_SOLUTIONS = 0x44;
        constexpr int SHADER_SOLUTION_COUNT = 0x48;
        constexpr int SOLUTION_STRIDE = 0x10;
        constexpr int SOLUTION_POINTER = 0x8;
        constexpr int SOLUTION_PIXEL = 0x20;
        constexpr int PERMUTATION_DATA = 0x24;
        constexpr int PERMUTATION_SIZE = 0x28;
        constexpr unsigned int MAX_SOLUTIONS = 64;

        bool readDxbc(const unsigned char* solution, const char*& data, unsigned int& size)
        {
            const unsigned char* permutation = *reinterpret_cast<unsigned char* const*>(solution + SOLUTION_PIXEL);
            data = permutation ? *reinterpret_cast<const char* const*>(permutation + PERMUTATION_DATA) : nullptr;
            size = permutation ? *reinterpret_cast<const unsigned int*>(permutation + PERMUTATION_SIZE) : 0;
            return data && size >= 32 && size <= (1u << 20) && std::memcmp(data, "DXBC", 4) == 0;
        }

        // whether a pixel shader of the instance's shader reads external_<name>
        bool shaderReads(const unsigned char* instance, const char* name)
        {
            const unsigned char* shader = *reinterpret_cast<unsigned char* const*>(instance);
            const unsigned char* pairs = shader ? *reinterpret_cast<unsigned char* const*>(shader + SHADER_SOLUTIONS) : nullptr;
            const unsigned int count = shader ? *reinterpret_cast<const unsigned int*>(shader + SHADER_SOLUTION_COUNT) : 0;
            const std::string wanted = std::string("external_") + name;
            for (unsigned int p = 0; pairs && p < count && p < MAX_SOLUTIONS; ++p)
            {
                const unsigned char* solution = *reinterpret_cast<unsigned char* const*>(pairs + SOLUTION_STRIDE * p + SOLUTION_POINTER);
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

        // the palette's soft colors fully saturated, any white in them is what the bloom blows up
        std::array<float, 4> colorize(const Reticle& reticle, const Color& color)
        {
            const float high = (std::max)({ color.r, color.g, color.b }), low = (std::min)({ color.r, color.g, color.b });
            const float range = high > low ? high - low : 1.0f;
            const float intensity = reticle.strength;
            const float alpha = reticle.hasOriginal && reticle.original[3] > 0.0f ? reticle.original[3] : 1.0f;
            return { (color.r - low) / range * intensity, (color.g - low) / range * intensity, (color.b - low) / range * intensity, alpha };
        }

        float* blockVector(unsigned char* block, unsigned int handle)
        {
            const unsigned int count = block[BLOCK_VECTOR_COUNT];
            for (unsigned int i = 0; i < count; ++i)
            {
                const unsigned char* entry = block + BLOCK_ENTRIES + 8 * i;
                if (*reinterpret_cast<const unsigned int*>(entry) == handle)
                    return reinterpret_cast<float*>(block + BLOCK_ENTRIES + *reinterpret_cast<const unsigned short*>(entry + 4));
            }
            return nullptr;
        }

        // copied bf-visual-editor growBlockWithVector
        unsigned char* growBlock(MeshVariationManager* manager, unsigned char* instance, unsigned int handle, const float* value)
        {
            unsigned char* old = *reinterpret_cast<unsigned char**>(instance + INSTANCE_BLOCK);
            const unsigned int vec = old ? old[BLOCK_VECTOR_COUNT] : 0, tex = old ? old[BLOCK_TEXTURE_COUNT] : 0, bol = old ? old[BLOCK_BOOL_COUNT] : 0;
            if (vec + tex + bol > 128 || vec == 255 || !manager->heap)
                return nullptr;

            const unsigned int nvec = vec + 1, total = nvec + tex + bol;
            const unsigned int head = (8 * total + 15) & ~15u;
            const unsigned int size = 16 + head + 16 * nvec + TEXTURE_BYTES * tex + bol;
            const unsigned short header = static_cast<unsigned short>((size & 0x1FFF) | (1u << 13) | (tex ? 1u << 14 : 0) | (bol ? 1u << 15 : 0));

            auto grown = static_cast<unsigned char*>(reinterpret_cast<ArenaAllocFn>(OFF_MemoryArena_alloc)(manager->heap, size, 16));
            if (!grown || (reinterpret_cast<uintptr_t>(grown) & 15))
                return nullptr;

            std::memset(grown, 0, size);
            *reinterpret_cast<unsigned short*>(grown + BLOCK_HEADER) = header;
            *reinterpret_cast<unsigned short*>(grown + BLOCK_SIZE) = static_cast<unsigned short>(size);
            grown[BLOCK_VECTOR_COUNT] = static_cast<unsigned char>(nvec);
            grown[BLOCK_TEXTURE_COUNT] = static_cast<unsigned char>(tex);
            grown[BLOCK_BOOL_COUNT] = static_cast<unsigned char>(bol);

            const auto entry = [](unsigned char* block, unsigned int index) { return block + BLOCK_ENTRIES + 8 * index; };
            const auto handleOf = [](unsigned char* e) -> unsigned int& { return *reinterpret_cast<unsigned int*>(e); };
            const auto offsetOf = [](unsigned char* e) -> unsigned short& { return *reinterpret_cast<unsigned short*>(e + 4); };
            unsigned short offset = static_cast<unsigned short>(head);
            for (unsigned int i = 0; i < total; ++i)
            {
                unsigned char* e = entry(grown, i);
                offsetOf(e) = offset;
                *reinterpret_cast<unsigned short*>(e + 6) = 1;
                offset = static_cast<unsigned short>(offset + (i < nvec ? 16 : i < nvec + tex ? TEXTURE_BYTES : 1));
            }

            unsigned char* values = grown + BLOCK_ENTRIES;
            const unsigned char* oldValues = old ? old + BLOCK_ENTRIES : nullptr;
            for (unsigned int i = 0; i < vec; ++i)
            {
                handleOf(entry(grown, i)) = handleOf(entry(old, i));
                std::memcpy(values + offsetOf(entry(grown, i)), oldValues + offsetOf(entry(old, i)), 16);
            }
            handleOf(entry(grown, vec)) = handle;
            std::memcpy(values + offsetOf(entry(grown, vec)), value, 16);
            for (unsigned int i = 0; i < tex + bol; ++i)
            {
                unsigned char* to = entry(grown, nvec + i);
                unsigned char* from = entry(old, vec + i);
                handleOf(to) = handleOf(from);
                std::memcpy(values + offsetOf(to), oldValues + offsetOf(from), i < tex ? TEXTURE_BYTES : 1);
            }

            // grown again: the record keeps the engine's own block
            const auto record = std::find_if(g_grown.begin(), g_grown.end(), [instance](const Grown& g) { return g.instance == instance; });
            if (record != g_grown.end() && record->grown == old)
                record->grown = grown;
            else
            {
                if (record != g_grown.end())
                    g_grown.erase(record);
                g_grown.push_back({ instance, old, *reinterpret_cast<unsigned short*>(instance + INSTANCE_HEADER), grown });
            }
            *reinterpret_cast<unsigned char**>(instance + INSTANCE_BLOCK) = grown;
            *reinterpret_cast<unsigned short*>(instance + INSTANCE_HEADER) = header;
            return grown;
        }

        void restoreGrown(unsigned char* instance)
        {
            for (auto it = g_grown.begin(); it != g_grown.end(); ++it)
            {
                if (it->instance != instance)
                    continue;
                if (*reinterpret_cast<unsigned char**>(instance + INSTANCE_BLOCK) == it->grown)
                {
                    *reinterpret_cast<unsigned char**>(instance + INSTANCE_BLOCK) = it->original;
                    *reinterpret_cast<unsigned short*>(instance + INSTANCE_HEADER) = it->originalHeader;
                }
                g_grown.erase(it);
                return;
            }
        }

        // the block with a slot for handle, grown when it has none (only dots grow)
        unsigned char* withSlot(MeshVariationManager* manager, const Reticle& reticle, unsigned char* instance, unsigned int handle, const float* value)
        {
            unsigned char* block = *reinterpret_cast<unsigned char**>(instance + INSTANCE_BLOCK);
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

        void hold(unsigned char* block, unsigned int handle, const std::array<float, 4>& value)
        {
            std::copy(value.begin(), value.end(), blockVector(block, handle));
            g_held[heldKey(block, handle)] = value;
        }

        void restore(const Reticle& reticle, unsigned char* instance)
        {
            if (unsigned char* block = *reinterpret_cast<unsigned char**>(instance + INSTANCE_BLOCK))
                g_held.erase(heldKey(block, COLOR_HANDLE));
            restoreGrown(instance);

            unsigned char* block = *reinterpret_cast<unsigned char**>(instance + INSTANCE_BLOCK);
            float* value = block ? blockVector(block, COLOR_HANDLE) : nullptr;
            if (!value)
                return;
            if (const auto original = g_blockOriginal.find(heldKey(block, COLOR_HANDLE)); original != g_blockOriginal.end())
                std::copy(original->second.begin(), original->second.end(), value);
            else if (reticle.hasOriginal)
                std::copy(reticle.original, reticle.original + 3, value);
        }

        // the reticle's material of a variation set in the scope color, or back to its own
        bool tint(MeshVariationManager* manager, Reticle& reticle, unsigned char* set)
        {
            if (reticle.material >= *reinterpret_cast<unsigned int*>(set + SET_INSTANCE_COUNT))
                return false;

            unsigned char* instance = *reinterpret_cast<unsigned char**>(set + SET_INSTANCES) + INSTANCE_STRIDE * reticle.material;
            if (reticle.checkShader)
            {
                const unsigned char* shader = *reinterpret_cast<unsigned char* const*>(instance);
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
            unsigned char* block = withSlot(manager, reticle, instance, COLOR_HANDLE, hue.data());
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
            std::unordered_set<unsigned char*> sets;
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
                unsigned char* set = records[i]->m_variation;
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

        void __fastcall hkSetVector(unsigned char* _this, void*, unsigned int index, unsigned int handle, const float* value)
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
                const char* name = recordMesh(record);
                if (!name)
                    continue;
                const std::string mesh = lower(name);
                for (const ReticleProxy& proxy : g_proxies)
                {
                    if (proxy.mesh != mesh || !sameTransform(record->m_transform, proxy.transform))
                        continue;
                    LiveRecord& entry = live[record];
                    entry.mesh = mesh;
                    std::copy(proxy.original, proxy.original + 16, entry.original);
                    std::copy(proxy.transform, proxy.transform + 16, entry.written);
                    break;
                }
            }
            g_live.swap(live);
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
        bool __fastcall hkPreRegistration(unsigned char* _this, void*, int compartment)
        {
            const bool result = oPreRegistration(_this, compartment);
            const unsigned char* layout = *reinterpret_cast<unsigned char* const*>(_this + MESH_SET_LAYOUT);
            const char* name = layout ? *reinterpret_cast<const char* const*>(layout + LAYOUT_NAME) : nullptr;
            if (name && lower(name).find("reticule") != std::string::npos)
            {
                const float* low = reinterpret_cast<const float*>(layout + LAYOUT_BOUNDING_MIN);
                const float* high = reinterpret_cast<const float*>(layout + LAYOUT_BOUNDING_MAX);
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
        const char* meshName(const unsigned char* object)
        {
            fb::DataContainer* mesh = *reinterpret_cast<fb::DataContainer* const*>(object + SOCKET_ZOOM_ASSET);
            if (!mesh || (reinterpret_cast<uintptr_t>(mesh) & 3))
                return nullptr;

            const char* type = fb::typeName(mesh);
            const size_t length = type ? std::strlen(type) : 0;
            if (length < 9 || std::strcmp(type + length - 9, "MeshAsset") != 0)
                return nullptr;

            return *reinterpret_cast<const char* const*>(reinterpret_cast<const unsigned char*>(mesh) + ASSET_NAME);
        }

        // the candidates whose mesh is a reticle become proxies
        void resolveProxies()
        {
            std::erase_if(g_candidates, [](const ProxyCandidate& candidate)
            {
                const char* mesh = meshName(candidate.object);
                if (!mesh)
                    return false; // not resolved yet

                const std::string name = lower(mesh);
                if (name.find("reticule") == std::string::npos)
                    return true;

                ReticleProxy proxy{ candidate.partition, reinterpret_cast<float*>(candidate.object + SOCKET_TRANSFORM) };
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
                g_candidates.push_back({ partition, reinterpret_cast<unsigned char*>(object) });
            }
        }

        for (fb::DataContainer* object : partition->m_instances)
        {
            // RigidMeshAsset, SkinnedMeshAsset...
            const char* type = fb::typeName(object);
            const size_t length = type ? std::strlen(type) : 0;
            if (length < 9 || std::strcmp(type + length - 9, "MeshAsset") != 0)
                continue;

            const auto asset = reinterpret_cast<unsigned char*>(object);
            const std::string name = lower(*reinterpret_cast<const char* const*>(asset + ASSET_NAME));
            const unsigned int meshHash = *reinterpret_cast<const unsigned int*>(asset + MESH_NAME_HASH);
            const auto& materials = *reinterpret_cast<const fb::Array<fb::DataContainer*>*>(asset + MESH_MATERIALS);

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
                    const auto& parameters = *reinterpret_cast<const fb::Array<fb::VectorShaderParameter>*>(reinterpret_cast<const unsigned char*>(material) + MATERIAL_VECTOR_PARAMETERS);
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
        log("scope size {:.0f}% on {} reticle sockets, {} without bounds", factor * 100.0f, g_proxies.size(), unbounded);
    }

    void installScopeHooks()
    {
        hook(OFF_ShaderParameterBlock_setVector, hkSetVector, &oSetVector);
        hook(OFF_InternalDatabasePartition_releaseInstances, hkReleaseInstances, &oReleaseInstances);
        hook(OFF_MeshSet_preRegistration, hkPreRegistration, &oPreRegistration);
    }
}
