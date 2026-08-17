#include <windows.h>

#include "skinning.h"
#include "skinningshader2bones.h"
#include "stl.h"

#include "../hooks.h"
#include "../settings.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

// disable warnings about __asm blocks
#pragma warning(push)
#pragma warning(disable: 4409 4410 4740)

#ifndef TARGET_BF1942_R

// Fixes dynamic lighting on animated meshes (soldiers, vehicles).
//
// AnimatedMeshLodTemplate::buildVertexBuffer (0x005B14C0) fills a 64 byte vertex, laid out by the
// vertex declaration built at 0x006746D0 (v0 FLOAT3, v1 FLOAT3, v3 FLOAT3, v5 D3DCOLOR, v7 FLOAT3,
// v8 FLOAT3):
//
//   +0x00  v0  texture coordinates    +0x24  v5  packed bone slots, one byte per influence
//   +0x0C  v1  weights                +0x28  v7  offset for bone 0, in bone local space
//   +0x18  v3  normal                 +0x34  v8  offset for bone 1, in bone local space
//
// The renderer uploads an identity block to c9..c11 and each bone's matrix to three registers
// from c12, so slot k lands at c[12 + 3*k], and the shader turns the two offsets back into mesh
// space and blends them. The normal is never rotated by a bone, so the lit side of the mesh
// stays welded to the geometry and swings around with the animation. The game also
// rotates the normal by a fixed (x,y,z) -> (-x,z,y), built from the globals at 0x009AB6C0 and
// 0x009AB700, that positions never get, leaving it in a frame the geometry does not live in. On
// top of that about half the normals it writes are not unit length.
//
// A normal cannot simply be rotated by one bone, because a vertex blended between two bones needs
// both and one normal cannot be expressed in two bone spaces at once. So the vertex is moved to
// the usual skinning formulation:
//
//   - v7 and v8 hold mesh space bind positions, offset * M_bind, instead of bone local offsets
//   - v3 holds a plain mesh space normal
//   - c[12 + 3*slot] holds M_bind^-1 * M_cur, a delta from the bind pose, instead of the bone's
//     own matrix
//
// The geometry that comes out is the same, because (offset * M_bind) * (M_bind^-1 * M_cur) is
// offset * M_cur, so skinningShadowGen, which reads the same vertex buffer and constants but only
// uses positions, is unaffected. One mesh space normal is now valid for both bones, so the patched
// SkinningShader2Bones blends it with WEIGHTS.x and WEIGHTS.y exactly like the position. That
// shader is carried in skinningshader2bones.h and substituted into CreateVertexShader, so
// shaders.rfa does not have to be repacked.

namespace {

// Row major, translation in row 3, rigid.
struct Matrix { float m[16]; };

// out = a * b, row vector convention, with the implicit (0,0,0,1) last column.
void multiply(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            out[r * 4 + c] = a[r * 4 + 0] * b[c]
                           + a[r * 4 + 1] * b[4 + c]
                           + a[r * 4 + 2] * b[8 + c]
                           + (r == 3 ? b[12 + c] : 0.0f);
        }
        out[r * 4 + 3] = (r == 3) ? 1.0f : 0.0f;
    }
}

// Bind poses are rigid, so the inverse rotation is the transpose.
void rigidInverse(const float* m, float* out)
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            out[r * 4 + c] = m[c * 4 + r];

    for (int c = 0; c < 3; c++)
        out[12 + c] = -(m[12] * out[c] + m[13] * out[4 + c] + m[14] * out[8 + c]);

    out[3] = out[7] = out[11] = 0.0f;
    out[15] = 1.0f;
}

// Bind poses are found while building the vertex buffer and needed again when the constants are
// uploaded, keyed on mesh template, lod and bone slot, which both sides can name.
std::map<uint64_t, Matrix> g_bindPoses;

uint64_t poseKey(const void* meshTemplate, int lod, unsigned slot)
{
    return ((uint64_t)(uintptr_t)meshTemplate << 16) | (uint64_t)((lod & 0xFF) << 8) | (slot & 0xFF);
}

// Recovering the bind pose
// ------------------------
// The skin file stores each influence as a bone local offset, so the engine can place a vertex
// knowing only the bone's current matrix. Nothing records how a bone was oriented relative to the
// mesh, and the skeleton is no help: at build time its bone array holds an unrelated pose, and
// none of its bones maps the offsets onto the mesh.
//
// It is recoverable from the vertex data. For a vertex with a single influence, offset * M_bind is
// the mesh position by construction, so eight or more such vertices pin M_bind down by least
// squares, needing no engine state at all.
//
// Bones used only by blended vertices collect no equations and stay unfitted. Their offsets keep
// the game's convention and their constant is left as the bone's own matrix, which is consistent
// for position but leaves their lighting as it was. A soldier's arm bones are the main case.

struct Sample { float offset[3]; float position[3]; };

struct SlotFit {
    double ata[4][4] = {};
    double atb[4][3] = {};
    unsigned count = 0;
    bool solved = false;
    // A few kept aside to check the result against.
    Sample check[8];
    unsigned checkCount = 0;
    // Offsets written before this bone was fitted, rebased as soon as it is. Valid only while the
    // vertex buffer is locked, which it is for the whole build loop.
    std::vector<float*> pending;
};

std::map<uint64_t, SlotFit> g_slotFits;

// Enough to notice that one build has ended and another begun. The vertices of a build arrive in
// order, one stride apart, so any discontinuity means the previous buffer is gone and the pending
// pointers into it must not be written to.
const void* g_lastVertex = nullptr;
const void* g_lastMesh = nullptr;
int g_lastLod = -1;

void addEquation(SlotFit& fit, const float* offset, const float* position)
{
    double a[4] { offset[0], offset[1], offset[2], 1.0 };

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) fit.ata[r][c] += a[r] * a[c];
        for (int c = 0; c < 3; c++) fit.atb[r][c] += a[r] * position[c];
    }
    fit.count++;

    if (fit.checkCount < 8) {
        Sample& sample = fit.check[fit.checkCount++];
        for (int c = 0; c < 3; c++) {
            sample.offset[c] = offset[c];
            sample.position[c] = position[c];
        }
    }
}

// Solves the 4x4 normal equations for the affine transform, in doubles for conditioning.
bool fitAffine(const SlotFit& fit, Matrix& out)
{
    if (fit.count < 8) 
        return false;

    // Gauss-Jordan with partial pivoting on [ata | atb].
    double m[4][7];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) m[r][c] = fit.ata[r][c];
        for (int c = 0; c < 3; c++) m[r][4 + c] = fit.atb[r][c];
    }

    for (int col = 0; col < 4; col++) {
        int pivot = col;

        for (int r = col + 1; r < 4; r++)
            if (fabs(m[r][col]) > fabs(m[pivot][col])) pivot = r;

        if (fabs(m[pivot][col]) < 1e-9)
            return false; // degenerate, vertices too collinear

        if (pivot != col)
            for (int c = 0; c < 7; c++)
                std::swap(m[col][c], m[pivot][c]);

        double inv = 1.0 / m[col][col];
        for (int c = 0; c < 7; c++)
            m[col][c] *= inv;

        for (int r = 0; r < 4; r++) {
            if (r == col) 
                continue;
            
            double f = m[r][col];
            for (int c = 0; c < 7; c++)
                m[r][c] -= f * m[col][c];
        }
    }

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++)
            out.m[r * 4 + c] = (float)m[r][4 + c];

        out.m[r * 4 + 3] = (r == 3) ? 1.0f : 0.0f;
    }

    // Rigid by construction, so tidy the rotation up before it is inverted as one.
    float* r0 = out.m;
    float* r1 = out.m + 4;
    float* r2 = out.m + 8;

    float len0 = sqrtf(r0[0]*r0[0] + r0[1]*r0[1] + r0[2]*r0[2]);
    if (len0 < 1e-6f)
        return false;

    for (int c = 0; c < 3; c++)
        r0[c] /= len0;

    float d1 = r1[0]*r0[0] + r1[1]*r0[1] + r1[2]*r0[2];

    for (int c = 0; c < 3; c++)
        r1[c] -= d1 * r0[c];

    float len1 = sqrtf(r1[0]*r1[0] + r1[1]*r1[1] + r1[2]*r1[2]);
    if (len1 < 1e-6f)
        return false;

    for (int c = 0; c < 3; c++)
        r1[c] /= len1;

    r2[0] = r0[1]*r1[2] - r0[2]*r1[1];
    r2[1] = r0[2]*r1[0] - r0[0]*r1[2];
    r2[2] = r0[0]*r1[1] - r0[1]*r1[0];

    // Reject the fit unless it actually reproduces known vertices.
    for (unsigned s = 0; s < fit.checkCount; s++) {
        const float* o = fit.check[s].offset;

        float error = 0.0f;

        for (int c = 0; c < 3; c++) {
            float p = o[0] * out.m[c] + o[1] * out.m[4 + c] + o[2] * out.m[8 + c] + out.m[12 + c];
            error += fabsf(p - fit.check[s].position[c]);
        }

        if (error > 0.01f)
            return false;
    }
    return true;
}

// Bone local offset -> mesh space bind position.
void rebaseOffset(float* off, const Matrix& bind)
{
    float x = off[0];
    float y = off[1];
    float z = off[2];
    off[0] = x * bind.m[0] + y * bind.m[4] + z * bind.m[8]  + bind.m[12];
    off[1] = x * bind.m[1] + y * bind.m[5] + z * bind.m[9]  + bind.m[13];
    off[2] = x * bind.m[2] + y * bind.m[6] + z * bind.m[10] + bind.m[14];
}

} // namespace

// Called once per skinned vertex, after the game has finished writing it.
void __cdecl onSkinnedVertexBuilt(void* vertex, const void* meshTemplate, int lod,
                                  const float* sourceVertex)
{
    uint8_t* v = (uint8_t*)vertex;

    // Only meshes the game itself treats as skinned have usable bone data here.
    const uint8_t* const* sub = (const uint8_t* const*)((const uint8_t*)meshTemplate + 0x298);

    if (!*sub || *(const int*)(*sub + 0x50) != 1)
        return;

    if (meshTemplate != g_lastMesh || lod != g_lastLod
        || v != (const uint8_t*)g_lastVertex + 0x40) 
    {
        g_slotFits.clear();
        // Bind poses from an earlier build of this mesh would be applied to offsets this one has
        // not rebased, so they go with it.
        uint64_t first = poseKey(meshTemplate, lod, 0);
        g_bindPoses.erase(g_bindPoses.lower_bound(first), g_bindPoses.upper_bound(first | 0xFF));
    }
    g_lastMesh = meshTemplate;
    g_lastLod = lod;
    g_lastVertex = v;

    uint32_t packed = *(const uint32_t*)(v + 0x24);
    const float* weights = (const float*)(v + 0x0C);
    bool singleInfluence = ((packed >> 8) & 0xFF) < 3 || weights[1] <= 0.0f;

    for (int i = 0; i < 2; i++) {
        unsigned code = (packed >> (i * 8)) & 0xFF;
        // A code below 3 means no bone: it indexes the identity block at c9, so there is nothing
        // to rebase and the mesh space normal is already what the shader wants.
        if (code < 3)
            continue;

        if (i > 0 && weights[i] <= 0.0f)
            continue;   // unused second influence

        uint64_t key = poseKey(meshTemplate, lod, (code - 3) / 3);
        SlotFit& fit = g_slotFits[key];
        float* off = (float*)(v + 0x28 + i * 0x0C);

        if (fit.solved) {
            rebaseOffset(off, g_bindPoses[key]);
            continue;
        }
        fit.pending.push_back(off);

        // Only a single influence vertex gives a clean equation: its mesh position is entirely
        // this bone's doing, so offset * M_bind is exactly the source position.
        if (i != 0 || !singleInfluence || !sourceVertex)
            continue;

        addEquation(fit, off, sourceVertex);

        if (fit.count < 8)
            continue;

        Matrix bind;
        if (!fitAffine(fit, bind))
            continue;

        // The vertex buffer is locked for the whole build loop, so the offsets already written for
        // this bone are still ours to catch up.
        g_bindPoses[key] = bind;
        fit.solved = true;
        for (size_t k = 0; k < fit.pending.size(); k++)
            rebaseOffset(fit.pending[k], bind);

        std::vector<float*>().swap(fit.pending);
    }

    // Undo the engine's stray rotation, which is its own inverse, to get a mesh space normal, and
    // renormalize since about half of them do not arrive unit length.
    float* n = (float*)(v + 0x18);
    float nx = -n[0], ny = n[2], nz = n[1];
    float lengthSquared = nx * nx + ny * ny + nz * nz;

    if (lengthSquared < 1e-12f)
        return;

    float scale = 1.0f / sqrtf(lengthSquared);
    n[0] = nx * scale;
    n[1] = ny * scale;
    n[2] = nz * scale;
}

// Called with the assembled bone constant buffer, just before it is uploaded. Layout is three
// registers of identity followed by three per bone slot, each register holding a column of the
// matrix so the shader's dp4 works.
void __cdecl onBoneConstantsReady(float* buffer, int boneCount, const void* meshTemplate, int lod)
{
    for (int slot = 0; slot < boneCount; slot++) 
    {
        std::map<uint64_t, Matrix>::const_iterator it =
            g_bindPoses.find(poseKey(meshTemplate, lod, slot));

        if (it == g_bindPoses.end())
            continue;   // nothing was rebased against this slot

        float* dst = buffer + 12 + slot * 12;

        float current[16]
        {
            dst[0], dst[4], dst[8],  0.0f,
            dst[1], dst[5], dst[9],  0.0f,
            dst[2], dst[6], dst[10], 0.0f,
            dst[3], dst[7], dst[11], 1.0f,
        };

        float inverseBind[16];
        float delta[16];
        rigidInverse(it->second.m, inverseBind);
        multiply(inverseBind, current, delta);

        for (int c = 0; c < 3; c++) {
            dst[c * 4 + 0] = delta[c];
            dst[c * 4 + 1] = delta[4 + c];
            dst[c * 4 + 2] = delta[8 + c];
            dst[c * 4 + 3] = delta[12 + c];
        }
    }
}

// Returns the shader to create, or null to leave the game's own choice alone. The resource name is
// matched rather than the shader contents, so editing the archived shader cannot make this miss.
// It arrives as the full path without an extension and has to match exactly: shaders.rfa also
// holds newbf2_SkinningShader2Bones, and Shaders/Tree/Sprite ends in the same path component as
// Shaders/Sprite, so neither a substring search nor a leaf comparison would be safe.
const unsigned long* __cdecl pickVertexShader(const bfs::string* name)
{
    if (!name || !(*name == "Shaders/SkinningShader2Bones")) return nullptr;
    return g_skinningShader2BonesTokens;
}

void skinning_hook_init()
{
    if (!g_settings.fixAnimatedMeshLighting) return;

    // The tail of the per vertex loop in buildVertexBuffer, where the branch for skinned vertices
    // and the branch for vertices that matched nothing in the skin come back together.
    //
    // edi is the vertex offset and [esp+0x101f4] the vertex buffer base, both still valid because
    // the increments come after this instruction. esi holds the mesh template, ebp the source
    // vertex offset and [esp+0x10] the source buffer, and [esp+0x10204] the lod. Every stack
    // offset below has 0x24 added for the pushfd + pushad, and 4 more for each argument pushed
    // before it is read:
    //   source  0x00010 + 0x24        = 0x00034
    //   lod     0x10204 + 0x24 + 0x04 = 0x1022C
    //   base    0x101f4 + 0x24 + 0x0C = 0x10224
    BEGIN_ASM_CODE(vertex)
        pushfd
        pushad
        mov eax, [esp+0x34]
        add eax, ebp
        push eax
        mov eax, [esp+0x1022C]
        push eax
        push esi
        mov eax, [esp+0x10224]
        add eax, edi
        push eax
        mov eax, onSkinnedVertexBuilt
        call eax
        add esp, 16
        popad
        popfd
    MOVE_CODE_AND_ADD_CODE(vertex, 0x005B1F0C, 5, HOOK_ADD_ORIGINAL_BEFORE);

    // The bone constant buffer is assembled at esp+0xd0 and uploaded a few instructions later.
    // ebx holds the bone count, ebp the object being drawn. 0xF4 = 0xd0 + 0x24.
    BEGIN_ASM_CODE(constants)
        pushfd
        pushad
        lea eax, [esp+0xF4]
        mov edx, [ebp+0x134]
        mov ecx, [ebp+0x24]
        push edx
        push ecx
        push ebx
        push eax
        mov eax, onBoneConstantsReady
        call eax
        add esp, 16
        popad
        popfd
    MOVE_CODE_AND_ADD_CODE(constants, 0x005B0D2C, 5, HOOK_ADD_ORIGINAL_AFTER);

    // IDirect3DDevice8::CreateVertexShader(pDeclaration, pFunction, pHandle, Usage), inside the
    // function that loads a vertex shader by name. Its first argument is that name and is still on
    // the stack here, at [esp+0x120] once the five call arguments are pushed, so one hook can both
    // recognise the shader and replace it. pFunction is at [esp+8]. Both get 0x24 added for the
    // pushfd + pushad. Only the shader code is swapped, the declaration and the wrapper object the
    // engine builds around it stay its own.
    BEGIN_ASM_CODE(shader)
        pushfd
        pushad
        mov eax, [esp+0x144]
        push eax
        mov eax, pickVertexShader
        call eax
        add esp, 4
        test eax, eax
        je keep_original_shader
        mov [esp+0x2C], eax
        keep_original_shader:
        popad
        popfd
    MOVE_CODE_AND_ADD_CODE(shader, 0x00674BBA, 6, HOOK_ADD_ORIGINAL_AFTER);
}

#else // TARGET_BF1942_R

void skinning_hook_init()
{
    // the addresses above are for the retail BF1942.exe only
}

#endif // TARGET_BF1942_R

#pragma warning(pop)
