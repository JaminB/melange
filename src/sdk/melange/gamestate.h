#pragma once
#include <cstdint>
// Read-only game state: worms, teams, match values, entities and the game's data variables (build #1077).
namespace melange::gamestate {
bool Available();                     // the #1077 prologue checks passed (vtables, addresses)
struct Vec3 { float x, y, z; };       // world units, +Y up

struct Worm {
    uint8_t slot, team, posInTeam;
    bool active;
    bool alive;                       // active and not dead
    uint16_t health;
    uint8_t physicsState;
    int16_t weapon;                   // -1 if none
    Vec3 pos, vel;
    float yaw;                        // facing angle, radians about +Y, unwrapped; facing = (sin, 0, cos)
    char name[32];
};
struct Team {
    uint8_t slot, colour, alliance, roundsWon;
    bool active, ai, local;
    int32_t score;
    char name[32];
};
struct Match {
    bool inMatch, online;
    int32_t currentTeam, activeWorm;  // -1 = none
    int32_t turnMs, turnMsLeft, roundMs, roundMsLeft;
    float windSpeed, windDir, waterLevel;
    uint32_t turnsStarted;            // counted from GameLogic.Turn.Started
    bool suddenDeath;                 // latched from GameLogic.ActivateSuddenDeath
    char theme[32];
};
struct Snapshot {
    uint64_t frame; uint32_t matchSerial;
    Match match;
    Team teams[4]; uint8_t teamCount;
    Worm worms[16]; uint8_t wormCount;  // only slots with a valid container
};
bool Read(Snapshot* out);             // main thread; fault-guarded; false when !Available(); one read per frame
bool Latest(Snapshot* out);           // any thread: a copy of the last snapshot taken (at most StateHz)
void Want(uint32_t hz);               // ask for periodic snapshots (max over callers, 0..10); main thread;
                                      // a request lasts 1-2 s, so renew it at least once a second

enum class EntityKind : uint8_t { Worm, Projectile, Crate, Barrel, Other };
struct Entity {
    uint32_t handle;                  // index | serial << 12
    uintptr_t object, vtable;
    EntityKind kind;
    bool hasPos; Vec3 pos, vel;
    char type[48];                    // RTTI class name or "vtbl:0x..."
    char label[32];                   // weapon name for projectiles, worm name, ...
};
int Entities(Entity* out, int max);   // main thread; in a match only; returns the total

enum class VarType : uint8_t { Int, Uint, Float, Vector, String, Container, StringTable, Color, Undefined };
struct Var { char name[64]; VarType type; char value[96]; };  // value formatted as JSON text
int Vars(Var* out, int max, const char* prefix = nullptr);    // main thread; not re-entrant
bool Var1(const char* name, Var* out);                        // one named value; main thread

// Read-only raw view for the inspector: fault-guarded copy of up to 4096 bytes. False on fault.
bool Peek(uintptr_t addr, void* out, uint32_t n);
}
