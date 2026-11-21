// ===========================================================================
//  Roblox 2016 PS4 - runtime da engine (OpenOrbis / GoldHEN)
//
//  O que roda de verdade aqui:
//   - Bullet Physics (App.BulletPhysics do codigo da engine) simulando o mundo
//   - zlib (da engine) comprimindo/descomprimindo um bloco no boot (self-test)
//   - Renderizador 3D por software (z-buffer) -> VideoOut 1920x1080
//   - Controle DualShock 4 via ScePad
//
//  Controles:
//   Analogico esq.  : mover o personagem (relativo a camera)
//   Analogico dir.  : girar camera / inclinar
//   X               : pular
//   R2 ou Quadrado  : criar tijolo (cai do ceu)
//   L1 / R1         : zoom da camera
//   Circulo         : empurrao (explosao) em volta do personagem
//   Triangulo       : mostrar/esconder HUD
//   OPTIONS         : resetar o mundo
// ===========================================================================
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <orbis/libkernel.h>

#include "engine_modules.h"

#if ENGINE_HAS_BULLET
#include "btBulletDynamicsCommon.h"
#endif
#if ENGINE_HAS_ZLIB
extern "C" {
#include "zlib.h"
}
#endif

extern "C" {
int  sceVideoOutOpen(int userId, int busType, int index, const void *param);
void sceVideoOutSetBufferAttribute(void *attr, uint32_t pixelFormat, uint32_t tilingMode,
                                   uint32_t aspectRatio, uint32_t width, uint32_t height, uint32_t pitchInPixel);
int  sceVideoOutRegisterBuffers(int handle, int startIndex, void *const *addresses, int bufferNum, const void *attr);
int  sceVideoOutSubmitFlip(int handle, int bufferIndex, uint32_t flipMode, int64_t flipArg);
int  sceUserServiceInitialize(const void *params);
int  sceUserServiceGetInitialUser(int *userId);
int  scePadInit(void);
int  scePadOpen(int userId, int type, int index, const void *param);
int  scePadReadState(int handle, void *data);
}

// ---------------------------------------------------------------- video ----
#define FW 1920
#define FH 1080
#define RW 960            // resolucao interna (upscale 2x)
#define RH 540
#define NBUF 2
#define BUF_STRIDE 0x800000

static uint32_t *g_fb[NBUF];
static uint32_t *g_color;   // RW*RH
static float    *g_depth;   // RW*RH

// ---------------------------------------------------------------- pad ------
#define BTN_OPTIONS  0x00000008u
#define BTN_UP       0x00000010u
#define BTN_DOWN     0x00000040u
#define BTN_L1       0x00000400u
#define BTN_R1       0x00000800u
#define BTN_TRIANGLE 0x00001000u
#define BTN_CIRCLE   0x00002000u
#define BTN_CROSS    0x00004000u
#define BTN_SQUARE   0x00008000u

struct Pad {
    uint32_t btn, pressed;
    float lx, ly, rx, ry, l2, r2;
};
static float stick(uint8_t v) {
    float f = ((int)v - 128) / 127.0f;
    if (fabsf(f) < 0.15f) return 0.f;
    return f > 1.f ? 1.f : (f < -1.f ? -1.f : f);
}
static void pad_read(int h, Pad &p, uint32_t &prev) {
    uint8_t buf[1024];
    memset(buf, 0, sizeof(buf));
    if (h >= 0 && scePadReadState(h, buf) == 0) {
        // ScePadData: u32 buttons; stick L(x,y); stick R(x,y); analog L2,R2
        uint32_t b; memcpy(&b, buf, 4);
        p.btn = b;
        p.lx = stick(buf[4]);  p.ly = stick(buf[5]);
        p.rx = stick(buf[6]);  p.ry = stick(buf[7]);
        p.l2 = buf[8] / 255.f; p.r2 = buf[9] / 255.f;
    } else {
        memset(&p, 0, sizeof(p));
    }
    p.pressed = p.btn & ~prev;
    prev = p.btn;
}

// ---------------------------------------------------------------- math -----
struct V3 { float x, y, z; };
static inline V3 operator+(V3 a, V3 b) { return V3{a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline V3 operator-(V3 a, V3 b) { return V3{a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline V3 operator*(V3 a, float s) { return V3{a.x*s, a.y*s, a.z*s}; }
static inline float dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline V3 cross(V3 a, V3 b) { return V3{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
static inline V3 norm(V3 a) { float l = sqrtf(dot(a,a)); return l > 1e-6f ? a * (1.f/l) : V3{0,0,0}; }

struct Camera {
    V3 pos, right, up, fwd;
    float focal;
};
struct SV { float x, y, z; };   // vertice em tela

static bool project(const Camera &c, V3 w, SV &o) {
    V3 r = w - c.pos;
    float z = dot(r, c.fwd);
    if (z < 0.3f) return false;
    float inv = c.focal / z;
    o.x = RW * 0.5f + dot(r, c.right) * inv;
    o.y = RH * 0.5f - dot(r, c.up) * inv;
    o.z = z;
    return true;
}

static void tri(const SV &a, const SV &b, const SV &c, uint32_t col) {
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area <= 0.f) return;                       // backface culling
    int minx = (int)floorf(fminf(a.x, fminf(b.x, c.x)));
    int maxx = (int)ceilf (fmaxf(a.x, fmaxf(b.x, c.x)));
    int miny = (int)floorf(fminf(a.y, fminf(b.y, c.y)));
    int maxy = (int)ceilf (fmaxf(a.y, fmaxf(b.y, c.y)));
    if (minx < 0) minx = 0; if (miny < 0) miny = 0;
    if (maxx > RW - 1) maxx = RW - 1; if (maxy > RH - 1) maxy = RH - 1;
    if (minx > maxx || miny > maxy) return;
    float inv = 1.f / area;
    float e0dx = (c.y - b.y), e1dx = (a.y - c.y), e2dx = (b.y - a.y);
    float e0dy = (b.x - c.x), e1dy = (c.x - a.x), e2dy = (a.x - b.x);
    for (int y = miny; y <= maxy; y++) {
        float px = minx + 0.5f, py = y + 0.5f;
        float w0 = (px - b.x) * e0dx + (py - b.y) * e0dy;
        float w1 = (px - c.x) * e1dx + (py - c.y) * e1dy;
        float w2 = (px - a.x) * e2dx + (py - a.y) * e2dy;
        uint32_t *cp = g_color + y * RW + minx;
        float *dp = g_depth + y * RW + minx;
        for (int x = minx; x <= maxx; x++, cp++, dp++) {
            if (w0 >= 0.f && w1 >= 0.f && w2 >= 0.f) {
                float z = (w0 * a.z + w1 * b.z + w2 * c.z) * inv;
                if (z < *dp) { *dp = z; *cp = col; }
            }
            w0 += e0dx; w1 += e1dx; w2 += e2dx;
        }
    }
}

static uint32_t shade(uint32_t col, float k) {
    if (k < 0.25f) k = 0.25f; if (k > 1.f) k = 1.f;
    uint32_t r = (uint32_t)(((col >> 16) & 0xFF) * k);
    uint32_t g = (uint32_t)(((col >> 8) & 0xFF) * k);
    uint32_t b = (uint32_t)((col & 0xFF) * k);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// caixa orientada: eixos locais ax,ay,az (unitarios), meio-tamanho h
static void draw_box(const Camera &cam, V3 c, V3 ax, V3 ay, V3 az, V3 h, uint32_t col) {
    static const int F[6][4] = { {1,3,7,5}, {0,4,6,2}, {2,6,7,3}, {0,1,5,4}, {4,5,7,6}, {0,2,3,1} };
    V3 N[6] = { ax, ax * -1.f, ay, ay * -1.f, az, az * -1.f };
    V3 corner[8];
    for (int i = 0; i < 8; i++) {
        float sx = (i & 1) ? 1.f : -1.f, sy = (i & 2) ? 1.f : -1.f, sz = (i & 4) ? 1.f : -1.f;
        corner[i] = c + ax * (sx * h.x) + ay * (sy * h.y) + az * (sz * h.z);
    }
    SV s[8]; bool ok[8];
    for (int i = 0; i < 8; i++) ok[i] = project(cam, corner[i], s[i]);
    V3 L = norm(V3{0.4f, 1.f, 0.3f});
    for (int f = 0; f < 6; f++) {
        const int *q = F[f];
        if (!(ok[q[0]] && ok[q[1]] && ok[q[2]] && ok[q[3]])) continue;
        uint32_t k = shade(col, 0.45f + 0.55f * fmaxf(0.f, dot(N[f], L)));
        tri(s[q[0]], s[q[1]], s[q[2]], k);
        tri(s[q[0]], s[q[2]], s[q[3]], k);
    }
}

// ---------------------------------------------------------------- texto ----
static const uint8_t F_AZ[26][5] = {
 {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},
 {0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
 {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
 {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},
 {0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
 {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
 {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43} };
static const uint8_t F_09[10][5] = {
 {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
 {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
 {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E} };
static const uint8_t F_SY[8][5] = {
 {0x00,0x60,0x60,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x36,0x36,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
 {0x23,0x13,0x08,0x64,0x62},{0x08,0x08,0x3E,0x08,0x08},{0x14,0x14,0x14,0x14,0x14},{0x00,0x00,0x00,0x00,0x00} };
static const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return F_AZ[c - 'A'];
    if (c >= '0' && c <= '9') return F_09[c - '0'];
    switch (c) { case '.': return F_SY[0]; case '-': return F_SY[1]; case ':': return F_SY[2];
                 case '/': return F_SY[3]; case '%': return F_SY[4]; case '+': return F_SY[5];
                 case '=': return F_SY[6]; default: return 0; }
}
static void rect(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > RW) w = RW - x; if (y + h > RH) h = RH - y;
    for (int j = 0; j < h; j++) { uint32_t *p = g_color + (y + j) * RW + x; for (int i = 0; i < w; i++) p[i] = c; }
}
static void text(int x, int y, const char *s, int sc, uint32_t col) {
    for (; *s; s++) {
        const uint8_t *g = glyph(*s);
        if (g) for (int cx = 0; cx < 5; cx++) for (int cy = 0; cy < 7; cy++)
            if (g[cx] & (1 << cy)) rect(x + cx * sc, y + cy * sc, sc, sc, col);
        x += 6 * sc;
    }
}

// ---------------------------------------------------------------- mundo ----
#if ENGINE_HAS_BULLET
#define MAX_BRICKS 120
struct Brick { btRigidBody *body; V3 half; uint32_t col; };

struct World {
    btDefaultCollisionConfiguration *cfg;
    btCollisionDispatcher *disp;
    btDbvtBroadphase *broad;
    btSequentialImpulseConstraintSolver *solver;
    btDiscreteDynamicsWorld *dyn;
    btRigidBody *ground, *player;
    Brick bricks[MAX_BRICKS];
    int nb, head;
    uint32_t rng;
};

static uint32_t rnd(World &w) { w.rng = w.rng * 1664525u + 1013904223u; return w.rng >> 8; }

static btRigidBody *add_box(World &w, V3 half, V3 pos, float mass, float fric) {
    btCollisionShape *sh = new btBoxShape(btVector3(half.x, half.y, half.z));
    btVector3 inertia(0, 0, 0);
    if (mass > 0) sh->calculateLocalInertia(mass, inertia);
    btTransform t; t.setIdentity(); t.setOrigin(btVector3(pos.x, pos.y, pos.z));
    btDefaultMotionState *ms = new btDefaultMotionState(t);
    btRigidBody::btRigidBodyConstructionInfo ci(mass, ms, sh, inertia);
    ci.m_friction = fric; ci.m_restitution = 0.1f;
    btRigidBody *b = new btRigidBody(ci);
    w.dyn->addRigidBody(b);
    return b;
}
static void free_body(World &w, btRigidBody *b) {
    w.dyn->removeRigidBody(b);
    delete b->getMotionState(); delete b->getCollisionShape(); delete b;
}
static void spawn_brick(World &w, V3 at) {
    static const uint32_t pal[] = {0xFFE0382E,0xFF1B7BD1,0xFFF2C300,0xFF29A84A,0xFFF28C28,0xFF8E44AD,0xFF16A5A5};
    V3 half{0.8f + (rnd(w) % 5) * 0.2f, 0.4f + (rnd(w) % 3) * 0.2f, 0.8f + (rnd(w) % 5) * 0.2f};
    V3 pos{at.x + ((int)(rnd(w) % 7) - 3) * 0.6f, at.y + 14.f, at.z + ((int)(rnd(w) % 7) - 3) * 0.6f};
    int slot;
    if (w.nb < MAX_BRICKS) slot = w.nb++;
    else { slot = w.head; w.head = (w.head + 1) % MAX_BRICKS; free_body(w, w.bricks[slot].body); }
    w.bricks[slot].body = add_box(w, half, pos, 1.f + half.x * half.z, 0.7f);
    w.bricks[slot].half = half;
    w.bricks[slot].col = pal[rnd(w) % 7];
}
static void world_init(World &w) {
    memset(&w, 0, sizeof(w));
    w.rng = 12345;
    w.cfg = new btDefaultCollisionConfiguration();
    w.disp = new btCollisionDispatcher(w.cfg);
    w.broad = new btDbvtBroadphase();
    w.solver = new btSequentialImpulseConstraintSolver();
    w.dyn = new btDiscreteDynamicsWorld(w.disp, w.broad, w.solver, w.cfg);
    w.dyn->setGravity(btVector3(0, -38.f, 0));
    w.ground = add_box(w, V3{120, 1, 120}, V3{0, -1, 0}, 0.f, 1.0f);         // Baseplate
    w.player = add_box(w, V3{0.7f, 1.6f, 0.7f}, V3{0, 4, 0}, 3.f, 0.2f);     // personagem
    w.player->setAngularFactor(btVector3(0, 0, 0));
    w.player->setActivationState(DISABLE_DEACTIVATION);
    // pilares/rampa decorativos (referencia visual de movimento)
    for (int i = 0; i < 8; i++) {
        float a = i * 0.785398f;
        add_box(w, V3{2, 3 + (i % 3) * 2.f, 2}, V3{cosf(a) * 30.f, 3.f + (i % 3) * 2.f, sinf(a) * 30.f}, 0.f, 0.8f);
    }
    for (int i = 0; i < 12; i++) spawn_brick(w, V3{(float)((i % 4) - 2) * 3.f, -10.f, (float)((i / 4) - 1) * 3.f});
}
static void world_free(World &w) {
    for (int i = 0; i < w.nb; i++) free_body(w, w.bricks[i].body);
    // corpos estaticos/jogador liberados junto do mundo (reset recria tudo)
    delete w.dyn; delete w.solver; delete w.broad; delete w.disp; delete w.cfg;
}
#endif

// ---------------------------------------------------------------- main -----
static void sleep_forever() { for (;;) sceKernelUsleep(1000000); }

int main(void) {
    printf("[roblox-ps4] engine runtime iniciando\n");
    sceUserServiceInitialize(NULL);
    int uid = 0; sceUserServiceGetInitialUser(&uid);
    scePadInit();
    int pad = scePadOpen(uid, 0, 0, NULL);
    printf("[roblox-ps4] pad=%d user=%d\n", pad, uid);

    int vid = sceVideoOutOpen(0xFF, 0, 0, NULL);
    size_t total = (size_t)BUF_STRIDE * NBUF;
    off_t phys = 0; void *base = NULL;
    int r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, 0x200000, 3, &phys);
    if (r == 0) r = sceKernelMapDirectMemory(&base, total, 0x33, 0, phys, 0x200000);
    if (vid < 0 || r != 0 || !base) { printf("[roblox-ps4] erro video vid=%d r=0x%x\n", vid, r); sleep_forever(); }
    for (int i = 0; i < NBUF; i++) g_fb[i] = (uint32_t *)((uint8_t *)base + (size_t)i * BUF_STRIDE);

    uint8_t attr[64]; memset(attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute(attr, 0x80000000u, 1, 0, FW, FH, FW);
    void *addrs[NBUF] = { g_fb[0], g_fb[1] };
    r = sceVideoOutRegisterBuffers(vid, 0, addrs, NBUF, attr);
    printf("[roblox-ps4] registerBuffers=0x%x\n", r);

    g_color = (uint32_t *)malloc(sizeof(uint32_t) * RW * RH);
    g_depth = (float *)malloc(sizeof(float) * RW * RH);

    // ---- self-test zlib (modulo real da engine) ----
    char zline[96] = "ZLIB: NAO LINKADA";
#if ENGINE_HAS_ZLIB
    {
        static unsigned char src[8192], cmp[16384], back[8192];
        for (int i = 0; i < (int)sizeof(src); i++) src[i] = (unsigned char)((i * 7) ^ (i >> 5));
        uLongf cl = sizeof(cmp), bl = sizeof(back);
        int a = compress(cmp, &cl, src, sizeof(src));
        int b = uncompress(back, &bl, cmp, cl);
        bool ok = a == Z_OK && b == Z_OK && bl == sizeof(src) && memcmp(src, back, bl) == 0;
        snprintf(zline, sizeof(zline), "ZLIB %s %s %d->%d", zlibVersion(), ok ? "OK" : "FALHOU", (int)sizeof(src), (int)cl);
        printf("[roblox-ps4] %s\n", zline);
    }
#endif

#if ENGINE_HAS_BULLET
    World *W = new World; world_init(*W);
    printf("[roblox-ps4] Bullet: mundo criado\n");
#endif

    float yaw = 0.f, pitch = 0.45f, dist = 16.f;
    bool hud = true;
    uint32_t prev = 0; unsigned frame = 0;
    int cur = 0;
    V3 spawnAt{0, 0, 0};
    float fps = 60.f; 

    for (;;) {
        Pad p; pad_read(pad, p, prev);

        // ---- camera ----
        yaw   -= p.rx * 0.045f;
        pitch += p.ry * 0.030f;
        if (pitch < 0.08f) pitch = 0.08f; if (pitch > 1.45f) pitch = 1.45f;
        if (p.btn & BTN_L1) dist += 0.35f;
        if (p.btn & BTN_R1) dist -= 0.35f;
        if (dist < 6.f) dist = 6.f; if (dist > 40.f) dist = 40.f;
        if (p.pressed & BTN_TRIANGLE) hud = !hud;

        V3 target{0, 2, 0};
#if ENGINE_HAS_BULLET
        World &w = *W;
        btVector3 pp = w.player->getWorldTransform().getOrigin();
        target = V3{pp.x(), pp.y() + 1.f, pp.z()};
        spawnAt = V3{pp.x(), pp.y(), pp.z()};

        // movimento relativo a camera
        V3 fwdXZ{ -sinf(yaw), 0, -cosf(yaw) }, rightXZ{ cosf(yaw), 0, -sinf(yaw) };
        V3 mv = fwdXZ * (-p.ly) + rightXZ * p.lx;
        btVector3 v = w.player->getLinearVelocity();
        w.player->setLinearVelocity(btVector3(mv.x * 16.f, v.y(), mv.z * 16.f));
        if ((p.pressed & BTN_CROSS) && fabsf(v.y()) < 0.6f) w.player->setLinearVelocity(btVector3(mv.x * 16.f, 20.f, mv.z * 16.f));

        if ((p.pressed & BTN_SQUARE) || (p.r2 > 0.6f && (frame % 6) == 0)) spawn_brick(w, spawnAt);
        if (p.pressed & BTN_CIRCLE) {
            for (int i = 0; i < w.nb; i++) {
                btVector3 d = w.bricks[i].body->getWorldTransform().getOrigin() - pp;
                float l = d.length();
                if (l < 14.f && l > 0.01f) { w.bricks[i].body->activate(true); w.bricks[i].body->applyCentralImpulse(d / l * (28.f * (1.f - l / 14.f)) + btVector3(0, 14, 0)); }
            }
        }
        if (p.pressed & BTN_OPTIONS) { world_free(w); delete W; W = new World; world_init(*W); continue; }
        if (pp.y() < -60.f) { w.player->setWorldTransform(btTransform(btQuaternion(0,0,0,1), btVector3(0, 8, 0))); w.player->setLinearVelocity(btVector3(0,0,0)); }

        w.dyn->stepSimulation(1.f / 60.f, 4, 1.f / 120.f);
#endif

        // ---- render ----
        Camera cam;
        V3 off{ sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch) };
        cam.pos = target + off * dist;
        cam.fwd = norm(target - cam.pos);
        cam.right = norm(cross(cam.fwd, V3{0, 1, 0}));
        cam.up = cross(cam.right, cam.fwd);
        cam.focal = RW * 0.5f / tanf(0.5f * 1.0f);

        // ceu em degrade + limpar z
        for (int y = 0; y < RH; y++) {
            float t = (float)y / RH;
            uint32_t c = 0xFF000000u | ((uint32_t)(90 + 110 * t) << 16) | ((uint32_t)(150 + 80 * t) << 8) | (uint32_t)(235 - 20 * t);
            uint32_t *row = g_color + y * RW; for (int x = 0; x < RW; x++) row[x] = c;
        }
        for (int i = 0; i < RW * RH; i++) g_depth[i] = 1e30f;

        V3 AX{1,0,0}, AY{0,1,0}, AZ{0,0,1};
#if ENGINE_HAS_BULLET
        {   // baseplate + decoracao (corpos estaticos) + tijolos + jogador
            int n = w.dyn->getNumCollisionObjects();
            for (int i = 0; i < n; i++) {
                btCollisionObject *o = w.dyn->getCollisionObjectArray()[i];
                btRigidBody *b = btRigidBody::upcast(o);
                if (!b) continue;
                const btBoxShape *bs = (const btBoxShape *)b->getCollisionShape();
                btVector3 he = bs->getHalfExtentsWithMargin();
                btTransform t = b->getWorldTransform();
                btMatrix3x3 m = t.getBasis();
                V3 c{t.getOrigin().x(), t.getOrigin().y(), t.getOrigin().z()};
                V3 ax{m[0][0], m[1][0], m[2][0]}, ay{m[0][1], m[1][1], m[2][1]}, az{m[0][2], m[1][2], m[2][2]};
                uint32_t col = 0xFF6B7280u;
                if (b == w.ground) col = 0xFF4A9A3A;
                else if (b == w.player) col = 0xFFFFD84D;
                else for (int k = 0; k < w.nb; k++) if (w.bricks[k].body == b) { col = w.bricks[k].col; break; }
                draw_box(cam, c, ax, ay, az, V3{(float)he.x(), (float)he.y(), (float)he.z()}, col);
            }
        }
#else
        draw_box(cam, V3{0,-1,0}, AX, AY, AZ, V3{120,1,120}, 0xFF4A9A3A);
#endif

        // ---- HUD ----
        if (hud) {
            rect(10, 10, 330, 118, 0xFF16181D);
            rect(10, 10, 330, 3, 0xFF00A2FF);
            text(20, 20, "ROBLOX 2016 - PS4", 3, 0xFFFFFFFF);
            char buf[96];
            snprintf(buf, sizeof(buf), "FRAME %u", frame); text(20, 48, buf, 2, 0xFF9AA3B0);
            text(20, 66, zline, 2, 0xFF9AA3B0);
#if ENGINE_HAS_BULLET
            snprintf(buf, sizeof(buf), "BULLET: %d CORPOS", w.dyn->getNumCollisionObjects()); text(20, 84, buf, 2, 0xFF9AA3B0);
#endif
            snprintf(buf, sizeof(buf), "L:%+.1f %+.1f R:%+.1f %+.1f", p.lx, p.ly, p.rx, p.ry); text(20, 102, buf, 2, 0xFF00A2FF);
            text(10, RH - 22, "STICKS MOVER/CAMERA  X PULAR  R2 TIJOLO  O EMPURRAO  OPTIONS RESET  TRI HUD", 2, 0xFFFFFFFF);
        }

        // ---- blit 2x para o framebuffer ----
        uint32_t *dst = g_fb[cur];
        for (int y = 0; y < RH; y++) {
            uint32_t *d0 = dst + (size_t)(2 * y) * FW;
            const uint32_t *s = g_color + y * RW;
            for (int x = 0; x < RW; x++) { uint32_t c = s[x]; d0[2*x] = c; d0[2*x+1] = c; }
            memcpy(d0 + FW, d0, FW * sizeof(uint32_t));
        }
        sceVideoOutSubmitFlip(vid, cur, 1, (int64_t)frame);
        cur ^= 1; frame++;
        (void)fps; (void)AX; (void)AY; (void)AZ; (void)spawnAt;
    }
    return 0;
}
