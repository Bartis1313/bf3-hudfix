#include "hudfix.h"

#include <d3d11.h>

#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hudfix
{
    namespace
    {
        using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device* device, const void* bytecode, SIZE_T length, ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader);

        constexpr int DEVICE_CREATE_PIXEL_SHADER = 15; // idx
        constexpr uint32_t IMMEDIATE_VEC4 = 0x00004002; // operand token of an l(x, y, z, w) literal
        constexpr ULONGLONG SCAN_INTERVAL = 2000;
        constexpr ULONGLONG SETTLE_MS = 2000; // the databases are walked once the loaded set has stopped changing
        const void* const CHECKED_WITHOUT_SHADER = reinterpret_cast<const void*>(1);

        // a cockpit HUD shader is one sampling the HUD render target (VehicleEntityRenderer's "vehicleHud" parameter)
        constexpr char HUD_TEXTURE[] = "texture_vehicleHud";
        constexpr float GAIN_MIN = 1.0f;

        // trying to keep engine's tinting, since it's extremely specific for vehicle huds
        // and these don't expose params unlike scopes
        struct CockpitShader
        {
            const unsigned char* database;
            fb::PixelShaderPermutation* permutation;
            void* original; // the engine's, our reference while the copy is in
            ID3D11PixelShader* neutral; // our reference
        };
        std::vector<CockpitShader> g_shaders;
        constexpr unsigned char BLEND_LERP = 0;
        constexpr unsigned char BLEND_ADDITIVE = 1;
        
        struct SolutionBlend
        {
            const unsigned char* database;
            fb::DxShaderSolution* solution;
            unsigned char blendMode; // the engine's
        };
        std::vector<SolutionBlend> g_solutions;
        std::unordered_set<const fb::PixelShaderPermutation*> g_tracked; // the permutations with a copy
        std::unordered_set<const void*> g_neutrals; // the copies, a permutation holding one isn't checked again
        std::unordered_map<const fb::PixelShaderPermutation*, const void*> g_checked;
        std::vector<const unsigned char*> g_databases; // the loaded databases the copies were made in
        std::vector<const unsigned char*> g_seenDatabases;
        ULONGLONG g_scanTime = 0;
        ULONGLONG g_seenTime = 0;

        std::vector<const unsigned char*> loadedDatabases()
        {
            std::vector<const unsigned char*> out;
            const unsigned char* system = *reinterpret_cast<const unsigned char* const*>(OFF_g_shaderSystem);
            if (!system)
                return out;

            const auto begin = *reinterpret_cast<const unsigned char* const* const*>(system + fb::SHADER_SYSTEM_DATABASES);
            const auto end = *reinterpret_cast<const unsigned char* const* const*>(system + fb::SHADER_SYSTEM_DATABASES + 4);

            for (const auto* d = begin; d && d != end; ++d)
                out.push_back(*d);
            return out;
        }

        bool readsHudTexture(const uint8_t* data, size_t size)
        {
            const uint8_t* end = data + size;
            return std::search(data, end, HUD_TEXTURE, HUD_TEXTURE + sizeof(HUD_TEXTURE) - 1) != end;
        }

        // three channels, all positive, the brightest past 1: a gain rather than a color
        bool isGain(float r, float g, float b)
        {
            return r > 0.0f && g > 0.0f && b > 0.0f && (std::max)({ r, g, b }) > GAIN_MIN;
        }

        float peak(float r, float g, float b)
        {
            return (std::max)({ r, g, b });
        }

        struct Literal
        {
            size_t offset;
            float v[4];
        };

        bool isZero(float x)
        {
            return std::fabs(x) < 1e-3f;
        }

        // the program chunk's l(x, y, z, w) literals
        std::vector<Literal> literals(const uint8_t* data, size_t size)
        {
            std::vector<Literal> out;
            if (size < 32 || std::memcmp(data, "DXBC", 4) != 0)
                return out;

            const uint32_t chunks = *reinterpret_cast<const uint32_t*>(data + 28);
            for (uint32_t c = 0; c < chunks && 32 + 4 * c + 4 <= size; ++c)
            {
                const uint32_t chunk = *reinterpret_cast<const uint32_t*>(data + 32 + 4 * c);
                if (chunk + 8 > size || (std::memcmp(data + chunk, "SHEX", 4) != 0 && std::memcmp(data + chunk, "SHDR", 4) != 0))
                    continue;

                const size_t begin = chunk + 8;
                const size_t end = (std::min)(size, begin + *reinterpret_cast<const uint32_t*>(data + chunk + 4));
                for (size_t p = begin; p + 20 <= end; p += 4)
                {
                    if ((*reinterpret_cast<const uint32_t*>(data + p) & 0x7FFFFFFF) != IMMEDIATE_VEC4)
                        continue;

                    Literal literal{ p + 4 };
                    std::memcpy(literal.v, data + p + 4, sizeof(literal.v));
                    out.push_back(literal);
                    p += 16;
                }
            }
            return out;
        }

        // every gain literal of a HUD shader rewritten for an even level
        bool neutralize(std::vector<uint8_t>& copy, float& level)
        {
            if (!readsHudTexture(copy.data(), copy.size()))
                return false;
            std::vector<Literal> all = literals(copy.data(), copy.size());
            const Literal* base = nullptr;
            const Literal* flicker = nullptr; // read as .yzw
            for (const Literal& l : all)
            {
                const float* v = l.v;
                if (isZero(v[3]) && isGain(v[0], v[1], v[2]) && (!base || peak(v[0], v[1], v[2]) > peak(base->v[0], base->v[1], base->v[2])))
                    base = &l;
                else if (v[0] == 0.0f && isGain(v[1], v[2], v[3]) && (!flicker || peak(v[1], v[2], v[3]) > peak(flicker->v[1], flicker->v[2], flicker->v[3])))
                    flicker = &l;
            }

            if (!base)
                return false; // texture only (a ghost pass at 0.4 of the HUD's inverse alpha)

            const float green = peak(base->v[0], base->v[1], base->v[2]); // the base gain's brightest channel
            level = green * settings().vehicleBrightness / 100.0f;
            const auto write = [&copy](const Literal& l, const float (&v)[4]) { std::memcpy(copy.data() + l.offset, v, sizeof(v)); };
            for (const Literal& l : all)
            {
                const float* v = l.v;
                if (&l == base)
                    write(l, { level, level, level, v[3] });
                else if (&l == flicker)
                {
                    const float f = level * peak(v[1], v[2], v[3]) / green;
                    write(l, { 0.0f, f, f, f });
                }
                // base + this = the warning color (green and blue 0): kept on it
                else if (isZero(v[3]) && isZero(base->v[1] + v[1]) && isZero(base->v[2] + v[2]))
                    write(l, { base->v[0] + v[0] - level, -level, -level, v[3] });
                // the same for the flicker literal, .yzw
                else if (flicker && v[0] == 0.0f && isZero(flicker->v[2] + v[2]) && isZero(flicker->v[3] + v[3]))
                {
                    const float f = level * peak(flicker->v[1], flicker->v[2], flicker->v[3]) / green;
                    write(l, { 0.0f, flicker->v[1] + v[1] - f, -f, -f });
                }
                // base - flicker: all negative, made with base's own fraction of its peak
                else if (v[0] < 0.0f && v[1] < 0.0f && v[2] < 0.0f && isZero(v[3]))
                {
                    const float d = -level * peak(-v[0], -v[1], -v[2]) / green;
                    write(l, { d, d, d, v[3] });
                }
            }
            return true;
        }

        // check bf visual editor for more reference
        void md5Block(uint32_t (&state)[4], const uint8_t* p)
        {
            static const uint32_t K[64] = {
                0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
                0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
                0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
                0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
                0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
                0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
                0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
                0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
            static const uint32_t R[64] = {
                7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
            uint32_t w[16];
            std::memcpy(w, p, sizeof(w));
            uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
            for (uint32_t i = 0; i < 64; ++i)
            {
                uint32_t f, g;
                if (i < 16) { f = (b & c) | (~b & d); g = i; }
                else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
                else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
                else { f = c ^ (b | ~d); g = (7 * i) & 15; }
                const uint32_t t = d;
                d = c;
                c = b;
                const uint32_t x = a + f + K[i] + w[g];
                b += (x << R[i]) | (x >> (32 - R[i]));
                a = t;
            }
            state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        }

        void signDxbc(uint8_t* blob, uint32_t size)
        {
            const uint8_t* data = blob + 20;
            const uint32_t length = size - 20;
            uint32_t state[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
            const uint32_t full = length / 64, rest = length % 64;
            for (uint32_t i = 0; i < full; ++i)
                md5Block(state, data + 64 * i);

            const uint32_t bits = length * 8;
            const uint32_t tail = (bits >> 2) | 1;
            uint8_t block[64] = { };
            if (rest >= 56)
            {
                std::memcpy(block, data + 64 * full, rest);
                block[rest] = 0x80;
                md5Block(state, block);
                std::memset(block, 0, sizeof(block));
                std::memcpy(block, &bits, 4);
            }
            else
            {
                std::memcpy(block, &bits, 4);
                std::memcpy(block + 4, data + 64 * full, rest);
                block[4 + rest] = 0x80;
            }
            std::memcpy(block + 60, &tail, 4);
            md5Block(state, block);
            std::memcpy(blob + 4, state, sizeof(state));
        }

        void makeNeutral(ID3D11Device* device, const unsigned char* database, fb::PixelShaderPermutation* permutation)
        {
            std::vector<uint8_t> copy(permutation->m_data, permutation->m_data + permutation->m_dataSize);
            float level = 0.0f;
            if (!neutralize(copy, level))
                return;
            signDxbc(copy.data(), static_cast<uint32_t>(copy.size()));

            const auto create = reinterpret_cast<CreatePixelShaderFn>((*reinterpret_cast<void***>(device))[DEVICE_CREATE_PIXEL_SHADER]);
            // the reference is the permutation's, and an engine-made one later is taken for the original (holdCockpitShaders)
            if (!permutation->m_shader)
            {
                ID3D11PixelShader* own = nullptr;
                if (FAILED(create(device, permutation->m_data, permutation->m_dataSize, nullptr, &own)) || !own)
                    return;
                permutation->m_shader = own;
            }
            ID3D11PixelShader* made = nullptr;
            const HRESULT result = create(device, copy.data(), copy.size(), nullptr, &made);
            if (FAILED(result) || !made)
            {
                log("cockpit shader: copy refused {:#x}", static_cast<unsigned int>(result));
                return;
            }

            g_neutrals.insert(made);
            g_tracked.insert(permutation);
            g_shaders.push_back({ database, permutation, permutation->m_shader, made });
            log("cockpit shader: permutation {} ({} bytes) -> {}", g_shaders.size() - 1, copy.size(), level);
        }

        // the copies dropped: in a database still loaded the engine's shader and blend go back, in one that is gone nothing is
        // written (the engine released whichever shader its permutations held)
        // othwerwise you leak, copies are not correct
        void release(const std::vector<const unsigned char*>& loaded, const char* why)
        {
            const auto isLoaded = [&loaded](const unsigned char* database) { return std::find(loaded.begin(), loaded.end(), database) != loaded.end(); };
            for (CockpitShader& s : g_shaders)
            {
                if (isLoaded(s.database) && s.permutation->m_shader == s.neutral)
                {
                    s.permutation->m_shader = s.original;
                    s.neutral->Release(); // the permutation's
                }
                s.neutral->Release();
            }
            for (SolutionBlend& s : g_solutions)
            {
                if (isLoaded(s.database))
                    s.solution->m_blendMode = s.blendMode;
            }
            log("cockpit shader: {} copies dropped ({})", g_shaders.size(), why);
            g_shaders.clear();
            g_solutions.clear();
            g_tracked.clear();
            g_checked.clear();
            g_neutrals.clear();
            g_databases.clear();
            g_scanTime = 0;
        }

        void trackSolution(const unsigned char* database, fb::DxShaderSolution* solution)
        {
            for (const SolutionBlend& s : g_solutions)
            {
                if (s.solution == solution)
                    return;
            }
            g_solutions.push_back({ database, solution, solution->m_blendMode });
        }
    }

    // every pixel permutation of every loaded shader database, each checked once for the HUD gains (a new shader on it again),
    // while a vehicle color is set and once the loaded databases have settled, so the copies are in before any cockpit draws
    // then the solutions drawing with them, for their blend specifically
    void scanCockpitShaders()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - g_scanTime < SCAN_INTERVAL)
            return;

        g_scanTime = now;

        fb::DxRenderer* renderer = fb::DxRenderer::GetInstance();
        if (!renderer || !renderer->m_device)
            return;

        const std::vector<const unsigned char*> loaded = loadedDatabases();
        if (loaded != g_seenDatabases)
        {
            g_seenDatabases = loaded;
            g_seenTime = now;
            return;
        }
        if (loaded.empty() || now - g_seenTime < SETTLE_MS)
            return;

        ID3D11Device* device = static_cast<ID3D11Device*>(renderer->m_device);
        const size_t before = g_shaders.size();
        size_t checked = 0;
        for (const unsigned char* database : loaded)
        {
            unsigned char* permutations = *reinterpret_cast<unsigned char* const*>(database + fb::DATABASE_PIXEL_PERMUTATIONS);
            const unsigned int permutationCount = *reinterpret_cast<const unsigned int*>(database + fb::DATABASE_PIXEL_COUNT);
            for (unsigned int i = 0; permutations && i < permutationCount; ++i)
            {
                fb::PixelShaderPermutation* permutation = reinterpret_cast<fb::PixelShaderPermutation*>(permutations + i * fb::PIXEL_PERMUTATION_SIZE);
                if (!permutation->m_data || g_neutrals.count(permutation->m_shader))
                    continue;

                const void* shaderNow = permutation->m_shader ? permutation->m_shader : CHECKED_WITHOUT_SHADER;
                const auto [entry, added] = g_checked.try_emplace(permutation, shaderNow);
                if (!added && entry->second == shaderNow)
                    continue;

                ++checked;
                makeNeutral(device, database, permutation);
                entry->second = permutation->m_shader ? permutation->m_shader : CHECKED_WITHOUT_SHADER; // a shader made here
            }

            unsigned char* solutions = *reinterpret_cast<unsigned char* const*>(database + fb::DATABASE_SOLUTIONS);
            const unsigned int solutionCount = *reinterpret_cast<const unsigned int*>(database + fb::DATABASE_SOLUTION_COUNT);
            for (unsigned int i = 0; solutions && !g_tracked.empty() && i < solutionCount; ++i)
            {
                fb::DxShaderSolution* solution = reinterpret_cast<fb::DxShaderSolution*>(solutions + i * fb::SHADER_SOLUTION_SIZE);
                if (solution->m_pixelPermutation && g_tracked.count(solution->m_pixelPermutation))
                    trackSolution(database, solution);
            }
        }
        g_databases = loaded;
        if (checked)
            log("cockpit shader: checked {} permutations in {} databases, {} neutral copies made", checked, loaded.size(), g_shaders.size() - before);
    }

    // VEHICLE BRIGHTNESS changed
    void rebuildCockpitShaders()
    {
        release(loadedDatabases(), "brightness");
    }

    bool cockpitNeutral()
    {
        const Settings& s = settings();
        return !g_shaders.empty() && (s.color[VehicleHudColor] != 0 || s.color[VehicleCrosshairColor] != 0);
    }

    // every UI frame: the copies in while a vehicle color is set, the engine's shaders back otherwise; a database loaded or
    // removed since the scan drops them (release), the next scan makes them again
    void holdCockpitShaders()
    {
        if (g_shaders.empty() && g_solutions.empty())
            return;

        const std::vector<const unsigned char*> loaded = loadedDatabases();
        if (loaded != g_databases)
        {
            release(loaded, "databases changed");
            return;
        }

        const bool neutral = cockpitNeutral();
        for (CockpitShader& s : g_shaders)
        {
            void* current = s.permutation->m_shader;
            if (current != s.original && current != s.neutral)
                s.original = current; // made again by the engine

            void* wanted = neutral ? s.neutral : s.original;
            if (current == wanted)
                continue;

            if (neutral)
                s.neutral->AddRef(); // the permutation's; the engine's reference on the original is ours meanwhile
            s.permutation->m_shader = wanted;
            if (!neutral)
                s.neutral->Release();
        }
        for (SolutionBlend& s : g_solutions)
            s.solution->m_blendMode = neutral && s.blendMode == BLEND_ADDITIVE ? BLEND_LERP : s.blendMode;
    }
}
