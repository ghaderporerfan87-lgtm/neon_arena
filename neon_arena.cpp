// =====================================================================
//  NEON ARENA  -  top-down arena shooter for 1 or 2 players
//  C++17 + raylib (https://www.raylib.com)
//
//  Build (Linux):   g++ -std=c++17 neon_arena.cpp -o neon_arena -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
//  Build (Windows): g++ -std=c++17 neon_arena.cpp -o neon_arena.exe -lraylib -lopengl32 -lgdi32 -lwinmm
//  Build (macOS):   g++ -std=c++17 neon_arena.cpp -o neon_arena -lraylib -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
//
//  Controls
//    Player 1: WASD move | Mouse aim | LMB / Space shoot | Left Shift dash
//    Player 2: Arrows move (aim = last move direction) | Enter / Right Ctrl shoot | Right Shift dash
//    P = pause, ESC = quit
// =====================================================================
#include "raylib.h"
#include "raymath.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

static const int SCREEN_W = 1280;
static const int SCREEN_H = 720;
static const Rectangle ARENA = {20.0f, 70.0f, SCREEN_W - 40.0f, SCREEN_H - 90.0f};
static const float PLAYER_R = 14.0f;

static float frand(float a, float b) {
    return a + (b - a) * (GetRandomValue(0, 10000) / 10000.0f);
}

static void centerText(const std::string& t, int y, int size, Color c) {
    int w = MeasureText(t.c_str(), size);
    DrawText(t.c_str(), SCREEN_W / 2 - w / 2, y, size, c);
}

// ----------------------------------------------------------- data types
enum class State { Menu, Playing, GameOver };
enum EnemyType { CHASER, RUNNER, BRUTE, SHOOTER };
enum PickupKind { HEAL, RAPID };

struct Player {
    Vector2 pos{0, 0}, vel{0, 0}, aim{1, 0}, dashDir{1, 0};
    float hp = 100.0f, cooldown = 0, invuln = 0, rapid = 0, dashTime = 0, dashCd = 0;
    bool alive = true;
    int score = 0;
    Color color = WHITE;
};

struct Enemy {
    EnemyType type = CHASER;
    Vector2 pos{0, 0};
    float hp = 1, maxHp = 1, speed = 100, radius = 14, shootTimer = 1, flash = 0, spin = 0;
};

struct Bullet { Vector2 pos, vel; float life; int owner; };          // owner -1 = enemy bullet
struct Particle { Vector2 pos, vel; float life, maxLife, size; Color color; };
struct Pickup { Vector2 pos; PickupKind kind; float life; };

static Color enemyColor(EnemyType t) {
    switch (t) {
        case CHASER:  return {255, 80, 120, 255};
        case RUNNER:  return {255, 220, 60, 255};
        case BRUTE:   return {180, 100, 255, 255};
        default:      return {80, 255, 160, 255};
    }
}

// ----------------------------------------------------------------- game
struct Game {
    State state = State::Menu;
    bool paused = false;
    int numPlayers = 1;
    Player players[2];
    std::vector<Enemy> enemies;
    std::vector<Bullet> bullets;
    std::vector<Particle> particles;
    std::vector<Pickup> pickups;
    int wave = 0, toSpawn = 0;
    float spawnTimer = 0, banner = 0, shake = 0, endTimer = 0;

    // ---------------------------------------------------------- setup
    void start(int n) {
        numPlayers = n;
        paused = false;
        enemies.clear(); bullets.clear(); particles.clear(); pickups.clear();
        wave = 0; shake = 0; endTimer = 0;
        players[0] = Player();
        players[1] = Player();
        players[0].color = {80, 200, 255, 255};
        players[1].color = {255, 170, 60, 255};
        players[0].pos = {SCREEN_W / 2.0f - (n == 2 ? 80.0f : 0.0f), SCREEN_H / 2.0f};
        players[1].pos = {SCREEN_W / 2.0f + 80.0f, SCREEN_H / 2.0f};
        players[1].alive = (n == 2);
        state = State::Playing;
        nextWave();
    }

    void nextWave() {
        wave++;
        toSpawn = 6 + wave * 4;
        spawnTimer = 1.5f;
        banner = 2.0f;
        for (int i = 0; i < numPlayers; i++)
            if (players[i].alive) players[i].hp = std::min(100.0f, players[i].hp + 20.0f);
    }

    bool anyAlive() const {
        for (int i = 0; i < numPlayers; i++) if (players[i].alive) return true;
        return false;
    }

    Player* nearestPlayer(Vector2 p) {
        Player* best = nullptr;
        float bd = 1e9f;
        for (int i = 0; i < numPlayers; i++) {
            if (!players[i].alive) continue;
            float d = Vector2Distance(p, players[i].pos);
            if (d < bd) { bd = d; best = &players[i]; }
        }
        return best;
    }

    // -------------------------------------------------------- helpers
    void burst(Vector2 p, Color c, int n, float speed) {
        for (int i = 0; i < n; i++) {
            float a = frand(0, 2 * PI);
            float s = frand(0.2f, 1.0f) * speed;
            float life = frand(0.25f, 0.6f);
            particles.push_back({p, {cosf(a) * s, sinf(a) * s}, life, life, frand(2, 5), c});
        }
    }

    void spawnEnemy() {
        int roll = GetRandomValue(0, 99);
        EnemyType t = CHASER;
        if (wave >= 2 && roll < 25) t = RUNNER;
        else if (wave >= 3 && roll < 40) t = SHOOTER;
        else if (wave >= 4 && roll < 50) t = BRUTE;

        Enemy e;
        e.type = t;
        switch (t) {
            case CHASER:  e.hp = 3;  e.speed = 110; e.radius = 15; break;
            case RUNNER:  e.hp = 1;  e.speed = 220; e.radius = 11; break;
            case BRUTE:   e.hp = 14; e.speed = 70;  e.radius = 28; break;
            case SHOOTER: e.hp = 4;  e.speed = 95;  e.radius = 16; e.shootTimer = frand(0.5f, 1.5f); break;
        }
        e.speed *= 1.0f + 0.025f * wave;
        e.maxHp = e.hp;

        // random point on the arena edge, preferably far from the players
        Vector2 p{ARENA.x, ARENA.y};
        for (int tries = 0; tries < 12; tries++) {
            switch (GetRandomValue(0, 3)) {
                case 0: p = {frand(ARENA.x, ARENA.x + ARENA.width), ARENA.y + 20}; break;
                case 1: p = {frand(ARENA.x, ARENA.x + ARENA.width), ARENA.y + ARENA.height - 20}; break;
                case 2: p = {ARENA.x + 20, frand(ARENA.y, ARENA.y + ARENA.height)}; break;
                default: p = {ARENA.x + ARENA.width - 20, frand(ARENA.y, ARENA.y + ARENA.height)}; break;
            }
            bool ok = true;
            for (int i = 0; i < numPlayers; i++)
                if (players[i].alive && Vector2Distance(p, players[i].pos) < 350.0f) ok = false;
            if (ok) break;
        }
        e.pos = p;
        burst(p, enemyColor(t), 10, 150);
        enemies.push_back(e);
    }

    void killEnemy(const Enemy& e, int owner) {
        static const int pts[] = {10, 15, 50, 30};
        if (owner >= 0) players[owner].score += pts[e.type];
        burst(e.pos, enemyColor(e.type), e.type == BRUTE ? 40 : 22, 320);
        shake = std::max(shake, e.type == BRUTE ? 10.0f : 4.0f);
        if (GetRandomValue(0, 99) < 12)
            pickups.push_back({e.pos, GetRandomValue(0, 1) ? HEAL : RAPID, 8.0f});
    }

    void hurt(Player& p, float dmg) {
        if (p.invuln > 0 || !p.alive) return;
        p.hp -= dmg;
        p.invuln = 0.7f;
        shake = std::max(shake, 12.0f);
        burst(p.pos, p.color, 16, 260);
        if (p.hp <= 0) {
            p.hp = 0; p.alive = false;
            burst(p.pos, p.color, 60, 450);
            shake = 20.0f;
        }
    }

    void fire(Player& p, int idx) {
        float spread = p.rapid > 0 ? 0.10f : 0.03f;
        float ang = atan2f(p.aim.y, p.aim.x) + frand(-spread, spread);
        Vector2 dir{cosf(ang), sinf(ang)};
        bullets.push_back({Vector2Add(p.pos, Vector2Scale(dir, 24)), Vector2Scale(dir, 950), 1.0f, idx});
        p.cooldown = p.rapid > 0 ? 0.07f : 0.15f;
        p.vel = Vector2Subtract(p.vel, Vector2Scale(dir, 25));   // tiny recoil
        shake = std::max(shake, 1.5f);
    }

    // ---------------------------------------------------------- input
    void input() {
        if (state == State::Menu) {
            if (IsKeyPressed(KEY_ONE)) start(1);
            if (IsKeyPressed(KEY_TWO)) start(2);
        } else if (state == State::GameOver) {
            if (IsKeyPressed(KEY_R)) start(numPlayers);
            if (IsKeyPressed(KEY_M)) state = State::Menu;
        } else if (IsKeyPressed(KEY_P)) {
            paused = !paused;
        }
    }

    // --------------------------------------------------------- update
    void updatePlayer(int i, float dt) {
        Player& p = players[i];
        if (!p.alive) return;

        Vector2 dir{0, 0};
        bool shooting, dashPressed;
        if (i == 0) {
            dir.x = (float)(IsKeyDown(KEY_D) - IsKeyDown(KEY_A));
            dir.y = (float)(IsKeyDown(KEY_S) - IsKeyDown(KEY_W));
            Vector2 d = Vector2Subtract(GetMousePosition(), p.pos);
            if (Vector2Length(d) > 1.0f) p.aim = Vector2Normalize(d);
            shooting = IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsKeyDown(KEY_SPACE);
            dashPressed = IsKeyPressed(KEY_LEFT_SHIFT);
        } else {
            dir.x = (float)(IsKeyDown(KEY_RIGHT) - IsKeyDown(KEY_LEFT));
            dir.y = (float)(IsKeyDown(KEY_DOWN) - IsKeyDown(KEY_UP));
            if (Vector2Length(dir) > 0) p.aim = Vector2Normalize(dir);
            shooting = IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_RIGHT_CONTROL);
            dashPressed = IsKeyPressed(KEY_RIGHT_SHIFT);
        }
        if (Vector2Length(dir) > 0) dir = Vector2Normalize(dir);

        // smooth movement
        p.vel = Vector2Lerp(p.vel, Vector2Scale(dir, 330.0f), std::min(1.0f, 12.0f * dt));

        // dash (short burst + brief invulnerability)
        if (dashPressed && p.dashCd <= 0) {
            p.dashDir = Vector2Length(dir) > 0 ? dir : p.aim;
            p.dashTime = 0.14f;
            p.dashCd = 1.0f;
        }
        if (p.dashTime > 0) {
            p.dashTime -= dt;
            p.vel = Vector2Scale(p.dashDir, 900.0f);
            p.invuln = std::max(p.invuln, 0.05f);
            burst(p.pos, p.color, 1, 30);
        }

        p.pos = Vector2Add(p.pos, Vector2Scale(p.vel, dt));
        p.pos.x = Clamp(p.pos.x, ARENA.x + PLAYER_R, ARENA.x + ARENA.width - PLAYER_R);
        p.pos.y = Clamp(p.pos.y, ARENA.y + PLAYER_R, ARENA.y + ARENA.height - PLAYER_R);

        p.cooldown -= dt; p.invuln -= dt; p.rapid -= dt; p.dashCd -= dt;
        if (shooting && p.cooldown <= 0) fire(p, i);
    }

    void update(float dt) {
        if (state != State::Playing || paused) return;
        shake = std::max(0.0f, shake - 40.0f * dt);

        for (int i = 0; i < numPlayers; i++) updatePlayer(i, dt);

        // ---- wave spawning
        banner = std::max(0.0f, banner - dt);
        if (toSpawn > 0) {
            spawnTimer -= dt;
            if (spawnTimer <= 0) {
                spawnEnemy();
                toSpawn--;
                spawnTimer = std::max(0.25f, 1.1f - wave * 0.06f);
            }
        } else if (enemies.empty() && anyAlive()) {
            nextWave();
        }

        // ---- enemy AI
        for (auto& e : enemies) {
            e.spin += 120.0f * dt;
            e.flash = std::max(0.0f, e.flash - dt);
            Player* t = nearestPlayer(e.pos);
            if (!t) continue;
            Vector2 to = Vector2Subtract(t->pos, e.pos);
            float d = Vector2Length(to);
            Vector2 dir = d > 0.001f ? Vector2Scale(to, 1.0f / d) : Vector2{0, 0};

            if (e.type == SHOOTER) {
                float move = d > 360 ? 1.0f : (d < 240 ? -1.0f : 0.0f);   // keeps its distance
                e.pos = Vector2Add(e.pos, Vector2Scale(dir, e.speed * move * dt));
                e.shootTimer -= dt;
                if (e.shootTimer <= 0 && d < 700) {
                    bullets.push_back({e.pos, Vector2Scale(dir, 360.0f), 4.0f, -1});
                    e.shootTimer = 1.6f;
                }
            } else {
                e.pos = Vector2Add(e.pos, Vector2Scale(dir, e.speed * dt));
            }
        }
        // keep enemies from stacking on top of each other
        for (size_t i = 0; i < enemies.size(); i++)
            for (size_t j = i + 1; j < enemies.size(); j++) {
                Vector2 d = Vector2Subtract(enemies[j].pos, enemies[i].pos);
                float dist = Vector2Length(d), minD = enemies[i].radius + enemies[j].radius;
                if (dist < minD && dist > 0.001f) {
                    Vector2 push = Vector2Scale(d, (minD - dist) * 0.5f / dist);
                    enemies[i].pos = Vector2Subtract(enemies[i].pos, push);
                    enemies[j].pos = Vector2Add(enemies[j].pos, push);
                }
            }

        // ---- bullets
        for (auto& b : bullets) {
            b.pos = Vector2Add(b.pos, Vector2Scale(b.vel, dt));
            b.life -= dt;
            if (!CheckCollisionPointRec(b.pos, ARENA)) b.life = 0;
        }
        for (auto& b : bullets) {
            if (b.life <= 0) continue;
            if (b.owner >= 0) {                                   // player bullet -> enemies
                for (auto& e : enemies) {
                    if (e.hp <= 0) continue;
                    if (Vector2Distance(b.pos, e.pos) < e.radius + 4.0f) {
                        e.hp -= 1; e.flash = 0.08f; b.life = 0;
                        burst(b.pos, WHITE, 3, 120);
                        if (e.hp <= 0) killEnemy(e, b.owner);
                        break;
                    }
                }
            } else {                                              // enemy bullet -> players
                for (int i = 0; i < numPlayers; i++) {
                    Player& p = players[i];
                    if (p.alive && Vector2Distance(b.pos, p.pos) < PLAYER_R + 4.0f) {
                        b.life = 0;
                        hurt(p, 10);
                        break;
                    }
                }
            }
        }

        // ---- enemy body contact
        for (auto& e : enemies)
            for (int i = 0; i < numPlayers; i++) {
                Player& p = players[i];
                if (!p.alive || p.invuln > 0) continue;
                if (Vector2Distance(e.pos, p.pos) < e.radius + PLAYER_R) {
                    Vector2 away = Vector2Normalize(Vector2Subtract(p.pos, e.pos));
                    hurt(p, e.type == BRUTE ? 25.0f : 12.0f);
                    p.vel = Vector2Add(p.vel, Vector2Scale(away, 500.0f));
                }
            }

        // ---- pickups
        for (auto& k : pickups) {
            k.life -= dt;
            for (int i = 0; i < numPlayers; i++) {
                Player& p = players[i];
                if (!p.alive || k.life <= 0) continue;
                if (Vector2Distance(k.pos, p.pos) < 26.0f) {
                    if (k.kind == HEAL) p.hp = std::min(100.0f, p.hp + 30.0f);
                    else p.rapid = 7.0f;
                    burst(k.pos, k.kind == HEAL ? GREEN : GOLD, 18, 220);
                    k.life = 0;
                }
            }
        }

        // ---- particles
        for (auto& pt : particles) {
            pt.pos = Vector2Add(pt.pos, Vector2Scale(pt.vel, dt));
            pt.vel = Vector2Scale(pt.vel, std::max(0.0f, 1.0f - 3.0f * dt));
            pt.life -= dt;
        }

        // ---- cleanup
        enemies.erase(std::remove_if(enemies.begin(), enemies.end(), [](const Enemy& e) { return e.hp <= 0; }), enemies.end());
        bullets.erase(std::remove_if(bullets.begin(), bullets.end(), [](const Bullet& b) { return b.life <= 0; }), bullets.end());
        pickups.erase(std::remove_if(pickups.begin(), pickups.end(), [](const Pickup& k) { return k.life <= 0; }), pickups.end());
        particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& p) { return p.life <= 0; }), particles.end());

        // ---- game over (short delay so the explosion is visible)
        if (!anyAlive()) {
            endTimer += dt;
            if (endTimer > 1.4f) state = State::GameOver;
        }
    }

    // ----------------------------------------------------------- draw
    void drawArena() {
        for (float x = ARENA.x; x <= ARENA.x + ARENA.width; x += 40)
            DrawLineV({x, ARENA.y}, {x, ARENA.y + ARENA.height}, Fade(SKYBLUE, 0.06f));
        for (float y = ARENA.y; y <= ARENA.y + ARENA.height; y += 40)
            DrawLineV({ARENA.x, y}, {ARENA.x + ARENA.width, y}, Fade(SKYBLUE, 0.06f));
        DrawRectangleLinesEx(ARENA, 3, Fade(SKYBLUE, 0.7f));
    }

    void drawEnemy(const Enemy& e) {
        Color c = e.flash > 0 ? WHITE : enemyColor(e.type);
        int sides = e.type == CHASER ? 4 : e.type == RUNNER ? 3 : e.type == BRUTE ? 6 : 8;
        DrawCircleV(e.pos, e.radius * 1.9f, Fade(c, 0.10f));
        DrawPoly(e.pos, sides, e.radius, e.spin, Fade(c, 0.35f));
        DrawPolyLinesEx(e.pos, sides, e.radius, e.spin, 3, c);
        if (e.type == BRUTE && e.hp < e.maxHp) {
            float w = 50.0f, f = e.hp / e.maxHp;
            DrawRectangle((int)(e.pos.x - w / 2), (int)(e.pos.y - e.radius - 14), (int)w, 5, Fade(WHITE, 0.2f));
            DrawRectangle((int)(e.pos.x - w / 2), (int)(e.pos.y - e.radius - 14), (int)(w * f), 5, c);
        }
    }

    void drawPlayer(const Player& p) {
        if (!p.alive) return;
        if (p.invuln > 0 && ((int)(GetTime() * 25) % 2)) return;   // blink while invulnerable
        DrawCircleV(p.pos, 34, Fade(p.color, 0.10f));
        Vector2 tip = Vector2Add(p.pos, Vector2Scale(p.aim, 28));
        DrawLineEx(p.pos, tip, 6, p.color);
        DrawCircleV(p.pos, PLAYER_R, Fade(p.color, 0.35f));
        DrawPolyLinesEx(p.pos, 24, PLAYER_R, 0, 3, p.color);
        if (p.rapid > 0) DrawPolyLinesEx(p.pos, 24, PLAYER_R + 8, 0, 2, GOLD);
    }

    void drawPickup(const Pickup& k) {
        if (k.life < 2.0f && ((int)(GetTime() * 10) % 2)) return;
        Color c = k.kind == HEAL ? GREEN : GOLD;
        DrawCircleV(k.pos, 20, Fade(c, 0.15f));
        DrawPolyLinesEx(k.pos, 4, 14, 45, 2, c);
        if (k.kind == HEAL) {
            DrawRectangle((int)k.pos.x - 6, (int)k.pos.y - 2, 12, 4, c);
            DrawRectangle((int)k.pos.x - 2, (int)k.pos.y - 6, 4, 12, c);
        } else {
            DrawText("R", (int)k.pos.x - 5, (int)k.pos.y - 8, 16, c);
        }
    }

    void drawBar(int x, int y, int w, float frac, Color c) {
        DrawRectangle(x, y, w, 14, Fade(WHITE, 0.1f));
        DrawRectangle(x, y, (int)(w * Clamp(frac, 0, 1)), 14, c);
        DrawRectangleLines(x, y, w, 14, Fade(WHITE, 0.4f));
    }

    void drawHUD() {
        DrawRectangle(0, 0, SCREEN_W, 60, Fade(BLACK, 0.5f));
        for (int i = 0; i < numPlayers; i++) {
            const Player& p = players[i];
            int x = (i == 0) ? 20 : SCREEN_W - 260;
            DrawText(TextFormat("P%d   %06d", i + 1, p.score), x, 8, 20, p.color);
            drawBar(x, 34, 240, p.hp / 100.0f, p.alive ? p.color : DARKGRAY);
            drawBar(x, 52, 240, 1.0f - Clamp(p.dashCd, 0, 1), Fade(WHITE, 0.6f));   // dash charge
        }
        centerText(TextFormat("WAVE %d", wave), 8, 24, WHITE);
        centerText(TextFormat("enemies left: %d", (int)enemies.size() + toSpawn), 36, 16, Fade(WHITE, 0.7f));
    }

    void drawCrosshair() {
        Vector2 m = GetMousePosition();
        Color c = Fade(WHITE, 0.9f);
        DrawCircleLines((int)m.x, (int)m.y, 10, c);
        DrawLine((int)m.x - 16, (int)m.y, (int)m.x - 6, (int)m.y, c);
        DrawLine((int)m.x + 6, (int)m.y, (int)m.x + 16, (int)m.y, c);
        DrawLine((int)m.x, (int)m.y - 16, (int)m.x, (int)m.y - 6, c);
        DrawLine((int)m.x, (int)m.y + 6, (int)m.x, (int)m.y + 16, c);
    }

    void drawMenu() {
        centerText("NEON ARENA", 140, 90, {80, 220, 255, 255});
        centerText("Survive the waves. Shoot everything.", 245, 24, Fade(WHITE, 0.7f));
        centerText("[1]  Solo", 340, 36, WHITE);
        centerText("[2]  Co-op (2 players)", 390, 36, WHITE);
        centerText("P1: WASD + Mouse (LMB / Space shoot, Shift dash)", 500, 20, Fade(WHITE, 0.6f));
        centerText("P2: Arrow keys (Enter shoot, Right Shift dash)", 530, 20, Fade(WHITE, 0.6f));
        centerText("P = pause     ESC = quit", 580, 20, Fade(WHITE, 0.45f));
    }

    void draw() {
        BeginDrawing();
        ClearBackground({8, 8, 20, 255});

        if (state == State::Menu) {
            drawMenu();
            EndDrawing();
            return;
        }

        Camera2D cam{};
        cam.zoom = 1.0f;
        cam.offset = {frand(-shake, shake), frand(-shake, shake)};
        BeginMode2D(cam);
            drawArena();
            for (const auto& k : pickups) drawPickup(k);
            for (const auto& e : enemies) drawEnemy(e);
            for (int i = 0; i < numPlayers; i++) drawPlayer(players[i]);

            BeginBlendMode(BLEND_ADDITIVE);
            for (const auto& b : bullets) {
                Color c = b.owner >= 0 ? players[b.owner].color : Color{255, 60, 90, 255};
                DrawCircleV(b.pos, 9, Fade(c, 0.25f));
                DrawCircleV(b.pos, 3.5f, c);
            }
            for (const auto& p : particles) {
                float a = p.life / p.maxLife;
                DrawCircleV(p.pos, p.size * a, Fade(p.color, a));
            }
            EndBlendMode();
        EndMode2D();

        drawHUD();
        if (!paused && players[0].alive) drawCrosshair();

        if (banner > 0 && state == State::Playing)
            centerText(TextFormat("WAVE %d", wave), SCREEN_H / 2 - 50, 80, Fade(WHITE, std::min(1.0f, banner)));

        if (paused) {
            DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade(BLACK, 0.55f));
            centerText("PAUSED", SCREEN_H / 2 - 40, 80, WHITE);
        }
        if (state == State::GameOver) {
            DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade(BLACK, 0.7f));
            centerText("GAME OVER", 190, 90, {255, 70, 100, 255});
            centerText(TextFormat("Reached wave %d", wave), 300, 30, WHITE);
            for (int i = 0; i < numPlayers; i++)
                centerText(TextFormat("Player %d score: %d", i + 1, players[i].score), 350 + i * 36, 28, players[i].color);
            centerText("R = restart     M = menu", 480, 26, Fade(WHITE, 0.7f));
        }
        EndDrawing();
    }
};

// ----------------------------------------------------------------- main
int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(SCREEN_W, SCREEN_H, "Neon Arena");
    SetTargetFPS(60);
    SetRandomSeed((unsigned int)time(nullptr));

    Game game;
    while (!WindowShouldClose()) {
        game.input();
        game.update(std::min(GetFrameTime(), 1.0f / 30.0f));

        bool wantHidden = (game.state == State::Playing && !game.paused);
        if (wantHidden && !IsCursorHidden()) HideCursor();
        if (!wantHidden && IsCursorHidden()) ShowCursor();

        game.draw();
    }
    CloseWindow();
    return 0;
}
