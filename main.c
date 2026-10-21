/*
 * AoQ Free Flip  (libaoqfreeflip.so)
 * ---------------------------------------------------------------------------
 * While you are in the air, the RIGHT thumbstick rotates your whole body freely:
 *      stick forward / back  = front flip / back flip   (pitch, around your head)
 *      stick left / right    = barrel roll left / right (roll,  around your head)
 * Combine them for any direction. When you land you are smoothly levelled back
 * upright. While you flip, the game's own right-stick turning is blocked so you
 * don't also snap-turn.
 *
 * How it works (AoQ 0.5.0, found with aoq_tools.py):
 *   - The player is an OVRPlayerController (CharacterController based). Its
 *     Update (0x80F860) is hooked as the per-frame tick; the game's own fixed
 *     "Rolling" coroutine already rotates this same transform, so rotating it is
 *     supported by the game.
 *   - "In the air" = CharacterController.get_isGrounded == false (0x10D21E4).
 *   - The rotation is applied around the HEAD position (centerEyeAnchor) so the
 *     view spins in place instead of swinging around your feet.
 *   - OVRInput.Get(Axis2D) (0xAF9A58) is hooked: we read the raw right stick and
 *     return zero to the game while flipping (blocks its snap/smooth turning).
 *   - OVRPlayerController.HmdRotatesY is switched off while tilted (it would
 *     re-write the rotation from your head yaw every frame) and restored on
 *     landing.
 *
 * Config (Mods -> Configure Mods -> Free Flip):
 *   Enabled, FlipSpeed (deg/s), Deadzone, InvertPitch, InvertRoll, RollEnabled,
 *   RequireAir, LevelOnLand, LevelSpeed (deg/s)
 *
 * NOT TESTED IN-GAME. Watch:  adb logcat -s AoQFreeFlip
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <android/log.h>

#include "../shared/inline-hook/inlineHook.h"
#include "../shared/utils/utils.h"
#include "../shared/modapi/modapi.h"
#include "../modmanager/modconfig.h"

#define SO_NAME "libaoqfreeflip.so"
#define TAG     "AoQFreeFlip"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* ---- RVAs (AoQ 0.5.0) ------------------------------------------------------- */
#define RVA_PC_Update        0x80F860   /* OVRPlayerController.Update            */
#define RVA_OVR_Axis2D       0xAF9A58   /* OVRInput.Get(Axis2D, Controller)       */
#define RVA_Comp_transform   0xC6A29C   /* Component.get_transform                */
#define RVA_Tr_get_position  0xFD4F80
#define RVA_Tr_set_position  0xFD5058
#define RVA_Tr_get_rotation  0xFD535C
#define RVA_Tr_set_rotation  0xFD5488
#define RVA_Tr_get_up        0xFD592C
#define RVA_Tr_get_forward   0xFD5C0C
#define RVA_CC_isGrounded    0x10D21E4  /* CharacterController.get_isGrounded     */
#define RVA_Rig_centerEye    0x5AADFC   /* OVRCameraRig.get_centerEyeAnchor       */
#define RVA_Time_deltaTime   0xFD3F70

#define AXIS_SECONDARY   2                    /* OVRInput.Axis2D.SecondaryThumbstick (right stick) */
#define CTRL_ACTIVE      ((int)0x80000000)
#define CFG_POLL_FRAMES  120
#define PI_F             3.14159265f
#define DEG2RAD          (PI_F / 180.0f)

typedef struct { float x, y; }       V2;
typedef struct { float x, y, z; }    V3;
typedef struct { float x, y, z, w; } Q4;

/* Struct returns/args are declared as real structs so the compiler uses the same
 * AAPCS convention as the game's IL2CPP code (sret pointer for >4-byte structs). */
typedef void *(*pf_obj)(void *self, void *mi);
typedef V3    (*pf_v3get)(void *self, void *mi);
typedef Q4    (*pf_qget)(void *self, void *mi);
typedef void  (*pf_v3set)(void *self, V3 v, void *mi);
typedef void  (*pf_qset)(void *self, Q4 q, void *mi);
typedef int   (*pf_bool)(void *self, void *mi);
typedef float (*pf_dt)(void *mi);

/* ---- state ------------------------------------------------------------------- */
static unsigned g_frame = 0;
static int   g_enabled = 1, g_roll = 1, g_invP = 0, g_invR = 0, g_reqAir = 1, g_level = 1;
static float g_speed = 300.0f, g_dead = 0.25f, g_levelSpeed = 540.0f;

static void *g_klass = NULL;
static int   g_offController = -1, g_offRig = -1, g_offHmdY = -1;
static int   g_fieldsTried = 0;
static volatile int g_block = 0;        /* tell the Axis2D hook to hide the right stick */
static int   g_tilted = 0;
static int   g_savedHmdY = -1;          /* original HmdRotatesY while we have it forced off */
static int   g_flipLogged = 0;

/* ---- math --------------------------------------------------------------------- */
static V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 vmul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 vcross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float vlen(V3 a) { return sqrtf(vdot(a, a)); }
static V3 vnorm(V3 a) { float l = vlen(a); return l > 1e-6f ? vmul(a, 1.0f / l) : v3(0, 0, 0); }

static Q4 qmul(Q4 a, Q4 b)
{
    Q4 r;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    return r;
}
static Q4 qnorm(Q4 q)
{
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-6f) { Q4 i = {0, 0, 0, 1}; return i; }
    float s = 1.0f / l; Q4 r = {q.x * s, q.y * s, q.z * s, q.w * s}; return r;
}
/* same maths as Unity's Quaternion.AngleAxis (angle in radians) */
static Q4 qaxis(V3 axis, float rad)
{
    V3 a = vnorm(axis); float h = rad * 0.5f, s = sinf(h);
    Q4 q = {a.x * s, a.y * s, a.z * s, cosf(h)}; return q;
}
static V3 qrot(Q4 q, V3 v)           /* q * v */
{
    V3 u = v3(q.x, q.y, q.z);
    V3 t = vmul(vcross(u, v), 2.0f);
    return vadd(vadd(v, vmul(t, q.w)), vcross(u, t));
}

/* ---- helpers ------------------------------------------------------------------- */
static int obj_alive(void *o)        /* Unity object with a live native side (m_CachedPtr) */
{ return o && *(void **)((char *)o + 8) != NULL; }

static void resolve_fields(void *inst)
{
    if (g_fieldsTried) return;
    g_fieldsTried = 1;
    typedef void *(*pf_gf)(void *klass, const char *name);
    typedef int   (*pf_off)(void *field);
    void *h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) { LOGE("libil2cpp not mapped"); return; }
    pf_gf gf  = (pf_gf)dlsym(h, "il2cpp_class_get_field_from_name");
    pf_off go = (pf_off)dlsym(h, "il2cpp_field_get_offset");
    dlclose(h);
    if (!gf || !go) { LOGE("il2cpp field API missing"); return; }
    g_klass = *(void **)inst;
    void *f;
    if ((f = gf(g_klass, "Controller")))  g_offController = go(f);
    if ((f = gf(g_klass, "CameraRig")))   g_offRig        = go(f);
    if ((f = gf(g_klass, "HmdRotatesY"))) g_offHmdY       = go(f);
    LOGI("field offsets: Controller=%d CameraRig=%d HmdRotatesY=%d", g_offController, g_offRig, g_offHmdY);
}

static void set_hmd_rotates_y(void *self, int tilted)
{
    if (g_offHmdY < 0) return;
    unsigned char *p = (unsigned char *)self + g_offHmdY;
    if (tilted) { if (g_savedHmdY < 0) { g_savedHmdY = *p; } *p = 0; }
    else if (g_savedHmdY >= 0) { *p = (unsigned char)g_savedHmdY; g_savedHmdY = -1; }
}

static void poll_config(void)
{
    static ModConfig mc;
    if (load_config(SO_NAME, &mc) != 0) return;
    ModCfgEntry *e;
    e = get_entry(&mc, "Enabled");     g_enabled    = e ? (int)e->value_num : 1;
    e = get_entry(&mc, "FlipSpeed");   g_speed      = e ? (float)e->value_num : 300.0f;
    e = get_entry(&mc, "Deadzone");    g_dead       = e ? (float)e->value_num : 0.25f;
    e = get_entry(&mc, "InvertPitch"); g_invP       = e ? (int)e->value_num : 0;
    e = get_entry(&mc, "InvertRoll");  g_invR       = e ? (int)e->value_num : 0;
    e = get_entry(&mc, "RollEnabled"); g_roll       = e ? (int)e->value_num : 1;
    e = get_entry(&mc, "RequireAir");  g_reqAir     = e ? (int)e->value_num : 1;
    e = get_entry(&mc, "LevelOnLand"); g_level      = e ? (int)e->value_num : 1;
    e = get_entry(&mc, "LevelSpeed");  g_levelSpeed = e ? (float)e->value_num : 540.0f;
    if (g_dead < 0.05f) g_dead = 0.05f;
    if (g_dead > 0.9f)  g_dead = 0.9f;
}

/* ---- hook: OVRInput.Get(Axis2D) — read the raw stick, hide it from the game while flipping --- */
MAKE_HOOK(FF_Axis2D, RVA_OVR_Axis2D, V2, int axis, int ctrl, void *mi)
{
    V2 r = FF_Axis2D(axis, ctrl, mi);
    if (g_block && axis == AXIS_SECONDARY) {
        float m = sqrtf(r.x * r.x + r.y * r.y);
        if (m > g_dead) { r.x = 0.0f; r.y = 0.0f; }
    }
    return r;
}

/* ---- rotate the player around the head ------------------------------------------------- */
static void rotate_about(void *tr, V3 pivot, Q4 delta)
{
    pf_v3get gpos = (pf_v3get)getRealOffset(RVA_Tr_get_position);
    pf_v3set spos = (pf_v3set)getRealOffset(RVA_Tr_set_position);
    pf_qget  grot = (pf_qget) getRealOffset(RVA_Tr_get_rotation);
    pf_qset  srot = (pf_qset) getRealOffset(RVA_Tr_set_rotation);
    V3 pos = gpos(tr, NULL);
    Q4 rot = grot(tr, NULL);
    V3 rel = qrot(delta, vsub(pos, pivot));
    srot(tr, qnorm(qmul(delta, rot)), NULL);
    spos(tr, vadd(pivot, rel), NULL);
}

/* ---- the per-frame tick ------------------------------------------------------------------ */
static void tick(void *self)
{
    g_frame++;
    if (g_frame == 1 || g_frame % CFG_POLL_FRAMES == 0) poll_config();
    if (!obj_alive(self)) return;
    resolve_fields(self);

    if (!g_enabled) {                         /* switched off at runtime: hand everything back */
        g_block = 0;
        if (g_tilted) { set_hmd_rotates_y(self, 0); g_tilted = 0; }
        return;
    }
    if (g_offController < 0 || g_offRig < 0) return;

    void *cc  = *(void **)((char *)self + g_offController);
    void *rig = *(void **)((char *)self + g_offRig);
    if (!obj_alive(cc) || !obj_alive(rig)) { g_block = 0; return; }

    pf_obj  get_tr  = (pf_obj) getRealOffset(RVA_Comp_transform);
    pf_obj  get_eye = (pf_obj) getRealOffset(RVA_Rig_centerEye);
    pf_bool grounded_f = (pf_bool)getRealOffset(RVA_CC_isGrounded);
    pf_dt   get_dt  = (pf_dt)  getRealOffset(RVA_Time_deltaTime);
    pf_v3get gpos = (pf_v3get)getRealOffset(RVA_Tr_get_position);
    pf_v3get gup  = (pf_v3get)getRealOffset(RVA_Tr_get_up);
    pf_v3get gfwd = (pf_v3get)getRealOffset(RVA_Tr_get_forward);

    void *tr  = get_tr(self, NULL);
    void *eye = get_eye(rig, NULL);
    if (!obj_alive(tr) || !obj_alive(eye)) { g_block = 0; return; }

    float dt = get_dt(NULL);
    if (dt <= 0.0f) return;
    if (dt > 0.05f) dt = 0.05f;               /* hitch protection: never a huge single-frame spin */

    int air     = !grounded_f(cc, NULL);
    int allowed = air || !g_reqAir;

    /* raw right stick (through the trampoline = the game's original, unmodified) */
    V2 s = FF_Axis2D(AXIS_SECONDARY, CTRL_ACTIVE, NULL);
    float mag = sqrtf(s.x * s.x + s.y * s.y);
    int deflected = mag > g_dead;

    g_block = allowed && deflected;

    V3 up = gup(tr, NULL);
    V3 pivot = gpos(eye, NULL);

    if (allowed && deflected) {
        /* radial deadzone rescale so the flip speed ramps from 0 at the deadzone edge */
        float k = (mag - g_dead) / (1.0f - g_dead);
        if (k > 1.0f) k = 1.0f;
        float nx = s.x / mag * k, ny = s.y / mag * k;

        /* axes: head direction, flattened onto the player's current "floor" plane */
        V3 f = gfwd(eye, NULL);
        f = vsub(f, vmul(up, vdot(f, up)));
        if (vlen(f) < 0.15f) {                /* looking straight along the up axis: use the body */
            f = gfwd(tr, NULL);
            f = vsub(f, vmul(up, vdot(f, up)));
        }
        f = vnorm(f);
        if (vlen(f) < 0.5f) return;
        V3 right = vnorm(vcross(up, f));

        float pitch = (g_invP ? -ny : ny) * g_speed * DEG2RAD * dt;       /* +y (forward) = front flip */
        float roll  = g_roll ? -(g_invR ? -nx : nx) * g_speed * DEG2RAD * dt : 0.0f; /* +x = roll right  */

        Q4 delta = qmul(qaxis(right, pitch), qaxis(f, roll));
        if (!g_tilted) { g_tilted = 1; set_hmd_rotates_y(self, 1); }
        rotate_about(tr, pivot, delta);
        if (!g_flipLogged) { LOGI("flipping (air=%d)", air); g_flipLogged = 1; }
        return;
    }
    g_flipLogged = 0;

    /* level the player back upright after landing (or any time we're allowed and idle on the ground) */
    if (g_tilted && g_level && !air) {
        float c = up.y; if (c > 1.0f) c = 1.0f; if (c < -1.0f) c = -1.0f;
        float ang = acosf(c);
        if (ang < 0.01f) {                    /* essentially upright: finish */
            g_tilted = 0; set_hmd_rotates_y(self, 0);
            LOGI("levelled");
            return;
        }
        V3 axis = vcross(up, v3(0, 1, 0));
        if (vlen(axis) < 1e-4f) axis = vcross(up, gfwd(tr, NULL));   /* exactly upside-down */
        float step = g_levelSpeed * DEG2RAD * dt;
        if (step > ang) step = ang;
        rotate_about(tr, pivot, qaxis(axis, step));
    } else if (g_tilted && up.y > 0.9995f) {  /* tilted flag stale (e.g. game reset the rotation) */
        g_tilted = 0; set_hmd_rotates_y(self, 0);
    }
}

MAKE_HOOK(FF_PC_Update, RVA_PC_Update, void, void *self, void *mi)
{
    FF_PC_Update(self, mi);
    tick(self);
}

/* ---- entry --------------------------------------------------------------------------------- */
__attribute__((constructor)) void lib_main(void)
{
    LOGI("AoQ Free Flip loading...");
    aoqmm_register(SO_NAME, "Free Flip", "1.0.0", "You",
                   "Right stick = free flips while airborne (forward/back = front/back flip, left/right = roll).");
    aoqmm_ensure_config(SO_NAME,
        "{\n"
        "  \"entries\": [\n"
        "    {\"key\":\"Enabled\",\"type\":\"bool\",\"value\":true,"
            "\"description\":\"Right stick flips you while airborne.\"},\n"
        "    {\"key\":\"FlipSpeed\",\"type\":\"float\",\"value\":300.0,"
            "\"description\":\"Full-stick rotation speed in degrees per second.\"},\n"
        "    {\"key\":\"Deadzone\",\"type\":\"float\",\"value\":0.25,"
            "\"description\":\"Stick deadzone (0.05 - 0.9).\"},\n"
        "    {\"key\":\"InvertPitch\",\"type\":\"bool\",\"value\":false,"
            "\"description\":\"Flip the front/back flip direction.\"},\n"
        "    {\"key\":\"InvertRoll\",\"type\":\"bool\",\"value\":false,"
            "\"description\":\"Flip the left/right roll direction.\"},\n"
        "    {\"key\":\"RollEnabled\",\"type\":\"bool\",\"value\":true,"
            "\"description\":\"Stick left/right rolls you. Off = front/back flips only.\"},\n"
        "    {\"key\":\"RequireAir\",\"type\":\"bool\",\"value\":true,"
            "\"description\":\"Only flip while off the ground (off = flip anywhere).\"},\n"
        "    {\"key\":\"LevelOnLand\",\"type\":\"bool\",\"value\":true,"
            "\"description\":\"Smoothly return upright after landing.\"},\n"
        "    {\"key\":\"LevelSpeed\",\"type\":\"float\",\"value\":540.0,"
            "\"description\":\"Upright recovery speed in degrees per second.\"}\n"
        "  ]\n"
        "}\n"
    );
    INSTALL_HOOK(FF_Axis2D);
    INSTALL_HOOK(FF_PC_Update);
    LOGI("hooks installed");
}
