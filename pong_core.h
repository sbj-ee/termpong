// pong_core.h - the simulation, with no idea how it is being drawn.
//
// Shared by the terminal front end (termpong.cpp) and the GTK one
// (gtkpong.cpp), so both play exactly the same game, and by the tests, which
// run it with no front end at all.
//
// Everything is in field units: one unit is one unit, in both axes, and a
// front end decides how many pixels or character cells that is worth.  The
// simulation is driven purely by dt - it never reads a clock - so a test can
// run an hour of play in a millisecond.

#ifndef PONG_CORE_H
#define PONG_CORE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace pong {

// ---------------------------------------------------------------- constants

constexpr int WIN_SCORE = 11;       // first to this, and by two
constexpr double PHYS_HZ = 480.0;   // physics substeps per second

constexpr double BALL_R = 0.5;      // ball radius, in field units

// Pace is expressed as time to cross the field rather than absolute speed, so
// the game plays the same in a small window as it does in a large one.
constexpr double SERVE_CROSS = 2.4;    // seconds for the serve to cross
constexpr double MAX_CROSS = 1.05;     // ...at full rally speed
constexpr double PLAYER_CROSS = 0.85;  // seconds for a paddle to cross the field
// A rally may outrun a paddle by this much, and no more: past it the steepest
// returns would be unreachable however well you read them.
constexpr double MAX_HEADROOM = 1.25;
constexpr double ACCEL_TIME = 0.09;    // seconds for a paddle to reach top speed
constexpr double BRAKE_TIME = 0.13;

constexpr double SERVE_ANGLE = 0.55;   // radians off horizontal, at most
constexpr double MIN_VX_RATIO = 0.40;  // keep the ball from stalling vertically

// Collision response.  Restitution is the fraction of approach speed returned
// along the contact normal; friction is the fraction of the contact patch's
// sideways slip that gets cancelled, which is what puts spin on the ball.
constexpr double PADDLE_RESTITUTION = 0.97;
constexpr double WALL_RESTITUTION = 0.985;
constexpr double PADDLE_FRICTION = 0.55;
constexpr double WALL_FRICTION = 0.15;
// Paddle faces are slightly convex, so the contact normal tilts away from
// horizontal near the edges.  This is what lets a player aim a return.
const double PADDLE_CURVE = 32.0 * M_PI / 180.0;
// The paddles are driven, not free: the stroke sets the pace of the return, so
// every rally winds up a little faster than the last shot until it caps out.
constexpr double RALLY_GAIN = 1.06;

// Flight forces.
constexpr double MAGNUS = 0.012;     // sideways accel per unit of spin & speed
constexpr double SPIN_DECAY = 0.7;   // 1/s, spin bleeds away into the air
constexpr double DRAG = 0.0002;      // quadratic drag coefficient
constexpr double MAX_SPIN = 20.0;    // radians per second

constexpr double PADDLE_RATIO = 6.5; // field height / paddle height
constexpr double SETTLE_TIME = 0.10; // AI closing time; longer = gentler
// How hard a paddle chases an absolute position (a mouse, a knob).  Short
// enough to feel direct, long enough that the paddle still has an honest
// velocity when it meets the ball.
constexpr double KNOB_SETTLE = 0.045;

constexpr double SERVE_DELAY = 0.9;
// The trail is sampled on simulated time, not on frames, so it is the same
// streak whatever the front end's frame rate turns out to be.
constexpr double TRAIL_SAMPLE = 1.0 / 120.0;
constexpr int TRAIL_LEN = 18;

// ------------------------------------------------------------------- helpers

template <typename T>
T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline double sgn(double v) { return v < 0 ? -1.0 : (v > 0 ? 1.0 : 0.0); }

inline uint32_t rng_state = 0x2545F491u;
inline double frand() {   // xorshift, uniform in [0, 1)
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (rng_state >> 8) * (1.0 / 16777216.0);
}
inline double frand(double lo, double hi) { return lo + frand() * (hi - lo); }

// --------------------------------------------------------------------- ball

struct Ball {
    double x = 0, y = 0, vx = 0, vy = 0, spin = 0;
    double angle = 0;   // accumulated rotation, so a front end can show spin

    double speed() const { return std::hypot(vx, vy); }
};

struct Pt { double x, y; };

// Resolve a contact against a surface whose outward normal (pointing at the
// ball) is (nx, ny) and which is itself moving at (pvx, pvy).
inline void contact(Ball& b, double nx, double ny, double e, double f,
                    double pvx, double pvy) {
    double rvx = b.vx - pvx, rvy = b.vy - pvy;
    double vn = rvx * nx + rvy * ny;
    if (vn > 0) return;   // already separating

    double tx = -ny, ty = nx;
    double vt = rvx * tx + rvy * ty;

    rvx -= (1.0 + e) * vn * nx;
    rvy -= (1.0 + e) * vn * ny;

    // Slip of the contact patch, then a friction impulse that cancels a
    // fraction of it.  For a disc the impulse splits three ways between
    // linear and angular motion, which is where the spin comes from.
    double slip = vt - b.spin * BALL_R;
    double dvt = -f * slip / 3.0;
    double dspin = 2.0 * f * slip / (3.0 * BALL_R);
    rvx += dvt * tx;
    rvy += dvt * ty;
    b.spin = clampv(b.spin + dspin, -MAX_SPIN, MAX_SPIN);

    b.vx = rvx + pvx;
    b.vy = rvy + pvy;
}

// Flight forces plus the top and bottom walls.  Shared by the live ball and by
// the computer's look-ahead, so its predictions obey the same physics.
inline void integrate(Ball& b, double dt, double H, double min_speed) {
    double v = b.speed();
    if (v > 1e-6) {
        // Magnus: sideways push proportional to spin and speed.
        double ax = MAGNUS * b.spin * -b.vy;
        double ay = MAGNUS * b.spin * b.vx;
        ax -= DRAG * v * b.vx;
        ay -= DRAG * v * b.vy;
        b.vx += ax * dt;
        b.vy += ay * dt;
    }
    b.spin *= std::exp(-SPIN_DECAY * dt);
    b.angle += b.spin * dt;

    b.x += b.vx * dt;
    b.y += b.vy * dt;

    if (b.y < BALL_R) {
        b.y = BALL_R + (BALL_R - b.y);
        contact(b, 0.0, 1.0, WALL_RESTITUTION, WALL_FRICTION, 0.0, 0.0);
    } else if (b.y > H - BALL_R) {
        double limit = H - BALL_R;
        b.y = limit - (b.y - limit);
        contact(b, 0.0, -1.0, WALL_RESTITUTION, WALL_FRICTION, 0.0, 0.0);
    }

    // Keep rallies alive: drag must not let the ball crawl, and Magnus must
    // not bend it into a purely vertical loop.
    v = b.speed();
    if (v < min_speed && v > 1e-6) {
        b.vx *= min_speed / v;
        b.vy *= min_speed / v;
        v = min_speed;
    }
    if (std::fabs(b.vx) < MIN_VX_RATIO * v) {
        double want = MIN_VX_RATIO * v;
        double dir = b.vx == 0 ? 1.0 : sgn(b.vx);
        double vy = std::sqrt(std::max(0.0, v * v - want * want));
        double vdir = b.vy == 0 ? 1.0 : sgn(b.vy);
        b.vx = dir * want;
        b.vy = vdir * vy;
    }
}

// ------------------------------------------------------------------- paddles

struct AiParams {
    double speed;     // top speed, as a fraction of a human paddle's
    double zone;      // fraction of the field the ball must cross first
    double aim;       // aim error, in half paddle heights
    double predict;   // 0 = chase the ball, 1 = solve the intercept
    double lag;       // seconds before reacting to a new shot
};

// Difficulty is mostly misjudgement and hesitation, not raw speed: every
// opponent reads the intercept, the weak ones just read it badly and late.
inline const AiParams DIFFICULTY[4] = {
    {0.55, 0.45, 3.00, 0.45, 0.30},   // easy
    {0.70, 0.30, 2.20, 0.80, 0.18},   // normal
    {0.85, 0.10, 1.60, 1.00, 0.08},   // hard
    {1.00, -1.0, 1.15, 1.00, 0.00},   // insane
};
inline const char* const DIFFICULTY_NAME[4] = {"EASY", "NORMAL", "HARD", "INSANE"};

struct Paddle {
    double x = 0, y = 0;          // y is the centre of the paddle
    double vy = 0;
    double half = 2.0;            // half height
    double half_w = 0.25;         // half thickness
    double max_speed = 0, accel = 0, brake = 0;
    double face_dir = 1.0;        // +1 if it faces right (the left paddle)

    bool ai = false;
    AiParams params = DIFFICULTY[1];
    double aim_off = 0, react_at = 0;
    bool committed = false;
    double target = 0;       // where the computer wants its centre
    bool waiting = false;    // ...but has not reacted yet

    // `want` is a target velocity, not a direction.  Asking for a speed lets
    // the computer ease into position instead of slamming between full tilt
    // and a dead stop, and lets a paddle coast while its front end makes up
    // its mind about whether a key is still held.
    void drive(double want, double dt, double H) {
        double rate = (want == 0.0) ? brake : accel;
        double step = rate * dt;
        vy += clampv(want - vy, -step, step);
        y += vy * dt;
        if (y < half) { y = half; vy = 0; }
        if (y > H - half) { y = H - half; vy = 0; }
    }
};

// --------------------------------------------------------------------- match

enum class Mode { Demo, OnePlayer, TwoPlayer };
enum class State { Menu, Play, Paused, Over };

// What a front end asks a paddle to do this frame: nothing, run at a fraction
// of top speed (a key), or get to a position (a mouse, a knob).
struct Intent {
    enum class Kind { Idle, Speed, Position };
    Kind kind = Kind::Idle;
    double value = 0.0;
};

struct Layout {
    double w = 0, h = 0;         // field size in units
    double margin = 1.25;        // paddle centre line, in from the wall
    double paddle_half_w = 0.25; // half thickness of a paddle
    double ball_rx = 0.25;       // ball half width (its half height is BALL_R)
};

class Match {
public:
    void configure(const Layout& l) {
        layout_ = l;
        place_paddles();
        setup(Mode::Demo);
    }

    // Same match, new field size: everything scales with it, including the
    // rally's pace, so a resize is not a reprieve.
    void resize(const Layout& l) {
        double sx = layout_.w > 0 ? l.w / layout_.w : 1.0;
        double sy = layout_.h > 0 ? l.h / layout_.h : 1.0;
        layout_ = l;
        ball_.x *= sx; ball_.y *= sy;
        ball_.vx *= sx; ball_.vy *= sy;
        left_.y *= sy;  right_.y *= sy;
        place_paddles();
        retune();
        left_.y = clampv(left_.y, left_.half, layout_.h - left_.half);
        right_.y = clampv(right_.y, right_.half, layout_.h - right_.half);
        trail_.clear();
    }

    void setup(Mode m) {
        mode_ = m;
        left_ = Paddle{};
        right_ = Paddle{};
        left_.face_dir = 1.0;  right_.face_dir = -1.0;
        left_.y = right_.y = layout_.h / 2.0;
        left_.ai = (m == Mode::Demo);
        right_.ai = (m != Mode::TwoPlayer);
        left_.params = DIFFICULTY[m == Mode::Demo ? 2 : difficulty_];
        right_.params = DIFFICULTY[m == Mode::Demo ? 1 : difficulty_];
        place_paddles();
        retune();
        score_l_ = score_r_ = 0;
        winner_ = 0;
        serve(frand() < 0.5 ? -1.0 : 1.0);
        state_ = (m == Mode::Demo) ? State::Menu : State::Play;
    }

    void set_intent(int side, Intent in) { (side == 0 ? in_l_ : in_r_) = in; }
    void set_difficulty(int d) { difficulty_ = clampv(d, 0, 3); }
    void step_difficulty(int by) { difficulty_ = (difficulty_ + by + 4) % 4; }
    int difficulty() const { return difficulty_; }
    void toggle_pause() {
        if (state_ == State::Play) state_ = State::Paused;
        else if (state_ == State::Paused) state_ = State::Play;
    }
    void rematch() { setup(mode_); }

    void update(double dt) {
        t_ += dt;
        if (state_ == State::Paused || state_ == State::Over) return;

        bool live = t_ >= serve_at_;
        if (left_.ai)  aim_ai(left_, live);
        if (right_.ai) aim_ai(right_, live);

        // One fixed timestep for everything that moves, paddles included: a
        // paddle's position and speed at the moment of contact then do not
        // depend on how fast the front end happens to be drawing.
        accum_ += dt;
        double h = 1.0 / PHYS_HZ;
        int guard = 0;
        while (accum_ >= h && guard++ < 600) {
            accum_ -= h;
            if (left_.ai)  step_ai(left_, h);
            else           left_.drive(want_of(left_, in_l_), h, layout_.h);
            if (right_.ai) step_ai(right_, h);
            else           right_.drive(want_of(right_, in_r_), h, layout_.h);
            if (!live) continue;

            step(h);
            trail_clock_ += h;
            if (trail_clock_ >= TRAIL_SAMPLE) {
                trail_clock_ -= TRAIL_SAMPLE;
                trail_.push_front({ball_.x, ball_.y});
                while (static_cast<int>(trail_.size()) > TRAIL_LEN) trail_.pop_back();
            }
            if (state_ != State::Play && state_ != State::Menu) break;
        }
    }

    const Ball& ball() const { return ball_; }
    const Paddle& left() const { return left_; }
    const Paddle& right() const { return right_; }
    const std::deque<Pt>& trail() const { return trail_; }
    Mode mode() const { return mode_; }
    State state() const { return state_; }
    int score_left() const { return score_l_; }
    int score_right() const { return score_r_; }
    int winner() const { return winner_; }
    double W() const { return layout_.w; }
    double H() const { return layout_.h; }
    bool serving() const { return t_ < serve_at_; }

    // Flat read-only view, for tests and diagnostics.
    struct Snapshot {
        double x, y, vx, vy, spin;
        double left_y, right_y, W, H;
        int left, right, winner;
        bool serving;
    };
    Snapshot snapshot() const {
        return {ball_.x, ball_.y, ball_.vx, ball_.vy, ball_.spin,
                left_.y, right_.y, layout_.w, layout_.h,
                score_l_, score_r_, winner_, serving()};
    }

private:
    void place_paddles() {
        left_.x = layout_.margin;
        right_.x = layout_.w - layout_.margin;
        left_.half_w = right_.half_w = layout_.paddle_half_w;
    }

    // Speeds are derived from the field, so they are recomputed whenever it
    // changes size.
    void retune() {
        player_v_ = layout_.h / PLAYER_CROSS;
        max_v_ = std::min(layout_.w / MAX_CROSS, player_v_ * MAX_HEADROOM);
        serve_v_ = std::min(layout_.w / SERVE_CROSS, max_v_ * 0.55);
        min_v_ = serve_v_ * 0.8;
        double ph = clampv(layout_.h / PADDLE_RATIO, 3.0, 6.0);
        for (Paddle* p : {&left_, &right_}) {
            p->half = ph / 2.0;
            p->max_speed = player_v_ * (p->ai ? p->params.speed : 1.0);
            p->accel = p->max_speed / ACCEL_TIME;
            p->brake = p->max_speed / BRAKE_TIME;
        }
    }

    double want_of(const Paddle& p, const Intent& in) const {
        switch (in.kind) {
        case Intent::Kind::Speed:
            return clampv(in.value, -1.0, 1.0) * p.max_speed;
        case Intent::Kind::Position: {
            double delta = clampv(in.value, p.half, layout_.h - p.half) - p.y;
            if (std::fabs(delta) < 0.02) return 0.0;
            return clampv(delta / KNOB_SETTLE, -p.max_speed, p.max_speed);
        }
        default:
            return 0.0;
        }
    }

    void serve(double dir) {
        ball_.x = layout_.w / 2.0;
        ball_.y = layout_.h / 2.0;
        double angle = frand(-SERVE_ANGLE, SERVE_ANGLE);
        ball_.vx = serve_v_ * std::cos(angle) * dir;
        ball_.vy = serve_v_ * std::sin(angle);
        ball_.spin = frand(-4.0, 4.0);
        serve_at_ = t_ + (winner_ ? 0.0 : SERVE_DELAY);
        accum_ = 0.0;
        trail_clock_ = 0.0;
        trail_.clear();
        left_.committed = right_.committed = false;
    }

    void step(double h) {
        double px = ball_.x, py = ball_.y;
        integrate(ball_, h, layout_.h, min_v_);

        double face_l = left_.x + left_.half_w + layout_.ball_rx;
        double face_r = right_.x - right_.half_w - layout_.ball_rx;

        // Only the substep that actually crosses a paddle's plane can be a
        // hit.  Testing "is the ball past the face" instead would let a paddle
        // reach out and rescue a ball it had already missed.
        if (ball_.vx < 0 && px > face_l && ball_.x <= face_l) {
            if (try_hit(left_, face_l, px, py)) return;
        } else if (ball_.vx > 0 && px < face_r && ball_.x >= face_r) {
            if (try_hit(right_, face_r, px, py)) return;
        }

        if (ball_.x < -layout_.ball_rx) point(2);
        else if (ball_.x > layout_.w + layout_.ball_rx) point(1);
    }

    bool try_hit(Paddle& p, double face, double px, double py) {
        // Where the ball was when it reached the paddle's plane, not where it
        // ended the substep - at speed those differ by a visible distance.
        double span = px - ball_.x;
        double frac = std::fabs(span) > 1e-9 ? (px - face) / span : 0.0;
        double hit_y = py + (ball_.y - py) * clampv(frac, 0.0, 1.0);

        double reach = p.half + BALL_R * 0.9;
        double off = (hit_y - p.y) / reach;
        if (std::fabs(off) > 1.0) return false;

        ball_.x = face;
        ball_.y = hit_y;
        double v_in = ball_.speed();
        off = clampv(off, -1.0, 1.0);
        double theta = off * PADDLE_CURVE;
        double nx = std::cos(theta) * p.face_dir;
        double ny = std::sin(theta);
        contact(ball_, nx, ny, PADDLE_RESTITUTION, PADDLE_FRICTION, 0.0, p.vy);

        double v = clampv(v_in * RALLY_GAIN, min_v_, max_v_);
        double cur = ball_.speed();
        if (cur > 1e-6) { ball_.vx *= v / cur; ball_.vy *= v / cur; }
        // Never let a return dribble along the paddle face.
        if (std::fabs(ball_.vx) < MIN_VX_RATIO * v) {
            double want = MIN_VX_RATIO * v;
            ball_.vx = p.face_dir * want;
            ball_.vy = sgn(ball_.vy) * std::sqrt(std::max(0.0, v * v - want * want));
        }
        ball_.vx = std::fabs(ball_.vx) * p.face_dir;

        Paddle& other = (&p == &left_) ? right_ : left_;
        other.committed = false;
        ++hits_;
        return true;
    }

    void point(int who) {
        if (who == 1) ++score_l_; else ++score_r_;
        ++points_;
        int hi = std::max(score_l_, score_r_), lo = std::min(score_l_, score_r_);
        if (hi >= WIN_SCORE && hi - lo >= 2) {
            winner_ = (score_l_ > score_r_) ? 1 : 2;
            if (mode_ == Mode::Demo) setup(Mode::Demo);   // attract mode loops
            else state_ = State::Over;
            return;
        }
        serve(who == 1 ? 1.0 : -1.0);   // serve toward whoever just lost
    }

    // Where will the ball reach this paddle's plane?  The computer runs the
    // same integrator the ball does, so a good opponent reads spin correctly.
    double predict_intercept(const Paddle& p) const {
        Ball b = ball_;
        double target_x = p.x + p.face_dir * (p.half_w + layout_.ball_rx);
        double h = 1.0 / 120.0;
        for (int i = 0; i < 1200; ++i) {
            integrate(b, h, layout_.h, min_v_);
            if (p.face_dir > 0 ? (b.x <= target_x) : (b.x >= target_x)) break;
            if (b.x < -1.0 || b.x > layout_.w + 1.0) break;
        }
        return clampv(b.y, 0.0, layout_.h);
    }

    // Deciding where to go is a once-a-frame job; getting there happens on
    // the physics clock, in step_ai below.
    void aim_ai(Paddle& p, bool live) {
        const AiParams& a = p.params;
        bool incoming = live && ((ball_.vx > 0) == (p.face_dir < 0));
        double crossed = p.face_dir < 0 ? ball_.x / layout_.w
                                        : 1.0 - ball_.x / layout_.w;

        if (!incoming) {
            p.committed = false;
            p.react_at = 0.0;
        } else if (!p.committed && crossed > a.zone) {
            // A new shot: pick this return's misjudgement and start reacting.
            p.committed = true;
            p.react_at = t_ + a.lag;
            // Triangular, so the opponent is usually close and occasionally
            // badly wrong - a uniform error reads as random flailing.
            p.aim_off = (frand() + frand() - 1.0) * a.aim * p.half;
        }

        p.waiting = p.committed && t_ < p.react_at;
        if (p.waiting) return;

        double target = layout_.h / 2.0;   // idle: drift back to the middle
        if (p.committed) {
            // Weak opponents just chase the ball's current height; strong ones
            // solve the intercept with the same integrator the ball uses, so
            // they read spin and wall bounces correctly.
            double naive = ball_.y;
            double solved = a.predict > 0.0 ? predict_intercept(p) : naive;
            target = naive + (solved - naive) * a.predict + p.aim_off;
            target = clampv(target, p.half, layout_.h - p.half);
        }
        p.target = target;
    }

    void step_ai(Paddle& p, double h) {
        if (p.waiting) { p.drive(0.0, h, layout_.h); return; }
        // Ask for a speed proportional to the distance left, so the paddle
        // settles onto its mark instead of oscillating around it - and so its
        // velocity is honest at the moment of contact, since a swinging paddle
        // imparts spin.
        double delta = p.target - p.y;
        double want = clampv(delta / SETTLE_TIME, -p.max_speed, p.max_speed);
        if (std::fabs(delta) < 0.05) want = 0.0;
        p.drive(want, h, layout_.h);
    }

    Layout layout_;
    Ball ball_;
    Paddle left_, right_;
    Intent in_l_, in_r_;
    std::deque<Pt> trail_;

    Mode mode_ = Mode::Demo;
    State state_ = State::Menu;
    int score_l_ = 0, score_r_ = 0;
    int winner_ = 0;
    int difficulty_ = 1;
    long hits_ = 0, points_ = 0;
    double t_ = 0.0, serve_at_ = 0.0, accum_ = 0.0, trail_clock_ = 0.0;
    double serve_v_ = 0, max_v_ = 0, min_v_ = 0, player_v_ = 0;
};

}  // namespace pong

#endif  // PONG_CORE_H
