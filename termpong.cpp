// termpong - classic pong for the terminal.
//
// Left paddle is yours, right paddle is the computer (or a second player).
// The ball is simulated with real units, spin and drag rather than integer
// cells, and drawn at half-cell vertical resolution with block glyphs, so
// motion is smooth instead of steppy.
//
// Build: make      Run: ./termpong

#define _XOPEN_SOURCE_EXTENDED 1

#include <curses.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------- constants

constexpr int WIN_SCORE = 11;   // first to this, and by two
constexpr double TARGET_FPS = 60.0;
constexpr double PHYS_HZ = 480.0;   // physics substeps per second

// The ball is simulated in real units rather than character cells.  One unit
// is the height of a row; a terminal cell is roughly twice as tall as it is
// wide, so a column is half a unit.  Working this way keeps speeds and angles
// honest: a 45-degree shot looks like 45 degrees on screen.
constexpr double CELL_W = 0.5;
constexpr double BALL_R = 0.5;   // the ball glyph is half a row across
constexpr double BALL_RX = 0.25; // ...which is a quarter unit in x

// Pace is expressed as time to cross the field rather than absolute speed, so
// the game plays the same in an 80x24 window as it does in a 220x60 one.
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

constexpr double PADDLE_RATIO = 6.5;    // field height / paddle height
constexpr int PADDLE_MARGIN = 2;        // columns in from the field edge

constexpr double KEY_HOLD = 0.11;       // a keypress keeps the paddle moving

constexpr double SERVE_DELAY = 0.9;
constexpr int TRAIL_LEN = 12;

constexpr int MIN_WIDTH = 48;
constexpr int MIN_HEIGHT = 16;

// ------------------------------------------------------------------- helpers

double real_seconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

// The whole frame shares one timestamp: nothing inside a tick should see the
// clock move underneath it, and tests can drive it directly.
double g_now = 0.0;
double now_seconds() { return g_now; }

template <typename T>
T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

double sgn(double v) { return v < 0 ? -1.0 : (v > 0 ? 1.0 : 0.0); }

uint32_t rng_state = 0x2545F491u;
double frand() {   // xorshift, uniform in [0, 1)
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (rng_state >> 8) * (1.0 / 16777216.0);
}
double frand(double lo, double hi) { return lo + frand() * (hi - lo); }

// -------------------------------------------------------------------- colour

// Every playfield cell is two vertically stacked half-cells, each holding one
// of these palette slots.  A cell is then drawn as a space, a full block, or a
// half block whose foreground and background carry the two halves.
enum Ink : uint8_t {
    INK_NONE = 0, INK_WALL, INK_NET, INK_P1, INK_P2,
    INK_BALL, INK_TRAIL1, INK_TRAIL2, INK_SCORE, INK_ACCENT, INK_COUNT
};

bool g_color = false;

short pair_for(uint8_t fg, uint8_t bg) {
    return static_cast<short>(fg * INK_COUNT + bg + 1);
}

void init_palette() {
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    g_color = true;

    std::array<short, INK_COUNT> fg{};
    if (COLORS >= 256) {
        fg = {-1, 33, 238, 51, 207, 226, 220, 130, 245, 213};
    } else {
        fg = {-1, COLOR_BLUE, COLOR_BLUE, COLOR_CYAN, COLOR_MAGENTA,
              COLOR_YELLOW, COLOR_YELLOW, COLOR_RED, COLOR_WHITE,
              COLOR_MAGENTA};
    }
    for (int f = 0; f < INK_COUNT; ++f)
        for (int b = 0; b < INK_COUNT; ++b) {
            short p = pair_for(static_cast<uint8_t>(f), static_cast<uint8_t>(b));
            if (p < COLOR_PAIRS) init_pair(p, fg[f], fg[b]);
        }
}

// -------------------------------------------------------------------- canvas

struct TextCell {
    wchar_t ch = 0;
    uint8_t ink = INK_NONE;
    attr_t attr = 0;
};

// Two layers: a half-cell "ink" bitmap for the game objects, and a text layer
// for the scoreboard and banners, which wins wherever it is set.
class Canvas {
public:
    void resize(int w, int h) {
        w_ = w; h_ = h;
        sub_.assign(static_cast<size_t>(w) * h * 2, INK_NONE);
        text_.assign(static_cast<size_t>(w) * h, TextCell{});
    }

    void clear() {
        std::fill(sub_.begin(), sub_.end(), INK_NONE);
        std::fill(text_.begin(), text_.end(), TextCell{});
    }

    int width() const { return w_; }
    int height() const { return h_; }

    void plot(int subrow, int col, uint8_t ink) {
        if (col < 0 || col >= w_ || subrow < 0 || subrow >= h_ * 2) return;
        sub_[static_cast<size_t>(subrow) * w_ + col] = ink;
    }

    // Fill a half-open span of half-rows in one column.
    void plot_span(int sub0, int sub1, int col, uint8_t ink) {
        for (int s = sub0; s < sub1; ++s) plot(s, col, ink);
    }

    void text(int row, int col, const std::wstring& s, uint8_t ink,
              attr_t attr = 0) {
        for (size_t i = 0; i < s.size(); ++i) {
            int c = col + static_cast<int>(i);
            if (c < 0 || c >= w_ || row < 0 || row >= h_) continue;
            text_[static_cast<size_t>(row) * w_ + c] = {s[i], ink, attr};
        }
    }

    void flush() const {
        for (int r = 0; r < h_; ++r)
            for (int c = 0; c < w_; ++c) {
                const TextCell& t = text_[static_cast<size_t>(r) * w_ + c];
                if (t.ch) {
                    put(r, c, t.ch, t.ink, INK_NONE, t.attr);
                    continue;
                }
                uint8_t top = sub_[static_cast<size_t>(r * 2) * w_ + c];
                uint8_t bot = sub_[static_cast<size_t>(r * 2 + 1) * w_ + c];
                attr_t attr = (top == INK_BALL || bot == INK_BALL) ? A_BOLD : 0;
                if (top == INK_NONE && bot == INK_NONE)
                    put(r, c, L' ', INK_NONE, INK_NONE, 0);
                else if (top == bot)
                    put(r, c, L'█', top, INK_NONE, attr);   // full block
                else if (bot == INK_NONE)
                    put(r, c, L'▀', top, INK_NONE, attr);   // upper half
                else if (top == INK_NONE)
                    put(r, c, L'▄', bot, INK_NONE, attr);   // lower half
                else
                    put(r, c, L'▀', top, bot, attr);
            }
    }

private:
    void put(int row, int col, wchar_t ch, uint8_t fg, uint8_t bg,
             attr_t attr) const {
        cchar_t cc;
        wchar_t buf[2] = {ch, 0};
        short pair = g_color ? pair_for(fg, bg) : 0;
        if (pair >= COLOR_PAIRS) pair = 0;
        setcchar(&cc, buf, attr, pair, nullptr);
        // The bottom-right cell cannot be written without the cursor running
        // off the screen, so insert that one instead.
        if (row == h_ - 1 && col == w_ - 1) mvins_wch(row, col, &cc);
        else mvadd_wch(row, col, &cc);
    }

    int w_ = 0, h_ = 0;
    std::vector<uint8_t> sub_;
    std::vector<TextCell> text_;
};

// ------------------------------------------------------------------ geometry

struct Geometry {
    int rows = 0, cols = 0;
    int hud_row = 0, top_wall = 1, bot_wall = 0;
    int play_top = 2, play_bot = 0;     // inclusive rows
    double H = 0.0, W = 0.0;            // playfield size in units
    int col_left = 0, col_right = 0;    // paddle columns
    double x_left = 0.0, x_right = 0.0; // paddle centre lines

    void fit(int r, int c) {
        rows = r; cols = c;
        hud_row = 0; top_wall = 1; bot_wall = rows - 1;
        play_top = 2; play_bot = rows - 2;
        H = static_cast<double>(play_bot - play_top + 1);
        W = cols * CELL_W;
        col_left = PADDLE_MARGIN;
        col_right = cols - 1 - PADDLE_MARGIN;
        x_left = (col_left + 0.5) * CELL_W;
        x_right = (col_right + 0.5) * CELL_W;
    }

    int row_of(double y) const { return play_top + static_cast<int>(std::floor(y)); }
    int subrow_of(double y) const {
        return play_top * 2 + static_cast<int>(std::floor(y * 2.0));
    }
    int col_of(double x) const {
        return clampv(static_cast<int>(std::floor(x / CELL_W)), 0, cols - 1);
    }
};

// --------------------------------------------------------------------- balls

struct Ball {
    double x = 0, y = 0, vx = 0, vy = 0, spin = 0;

    double speed() const { return std::hypot(vx, vy); }
};

// Resolve a contact against a surface whose outward normal (pointing at the
// ball) is (nx, ny) and which is itself moving at (pvx, pvy).
void contact(Ball& b, double nx, double ny, double e, double f,
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
void integrate(Ball& b, double dt, double H, double min_speed) {
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
const AiParams DIFFICULTY[4] = {
    {0.55, 0.45, 3.00, 0.45, 0.30},   // easy
    {0.70, 0.30, 2.20, 0.80, 0.18},   // normal
    {0.85, 0.10, 1.60, 1.00, 0.08},   // hard
    {1.00, -1.0, 1.15, 1.00, 0.00},   // insane
};
const char* DIFFICULTY_NAME[4] = {"EASY", "NORMAL", "HARD", "INSANE"};

struct Paddle {
    double x = 0, y = 0;          // y is the centre of the paddle
    double vy = 0;
    double half = 2.0;
    double max_speed = 0, accel = 0, brake = 0;
    double face_dir = 1.0;        // +1 if it faces right (the left paddle)
    int col = 0;
    uint8_t ink = INK_P1;

    bool ai = false;
    AiParams params = DIFFICULTY[1];
    double aim_off = 0, react_at = 0;
    bool committed = false;

    void drive(int dir, double dt, double H) {
        if (dir != 0) {
            double want = dir * max_speed;
            double d = want - vy;
            double step = accel * dt;
            vy += clampv(d, -step, step);
        } else {
            double step = brake * dt;
            vy -= clampv(vy, -step, step);
        }
        y += vy * dt;
        if (y < half) { y = half; vy = 0; }
        if (y > H - half) { y = H - half; vy = 0; }
    }
};

// ---------------------------------------------------------------------- game

enum class Mode { Demo, OnePlayer, TwoPlayer };
enum class State { Menu, Play, Paused, Over };

class Game {
public:
    explicit Game(int rows, int cols) {
        geo_.fit(rows, cols);
        canvas_.resize(cols, rows);
        setup(Mode::Demo);
        state_ = State::Menu;
    }

    // Speeds are derived from the field, so they have to be recomputed
    // whenever the window changes size.
    void retune() {
        player_v_ = geo_.H / PLAYER_CROSS;
        max_v_ = std::min(geo_.W / MAX_CROSS, player_v_ * MAX_HEADROOM);
        serve_v_ = std::min(geo_.W / SERVE_CROSS, max_v_ * 0.55);
        min_v_ = serve_v_ * 0.8;
        double h = clampv(geo_.H / PADDLE_RATIO, 3.0, 6.0);
        for (Paddle* p : {&left_, &right_}) {
            p->half = h / 2.0;
            p->max_speed = player_v_ * (p->ai ? p->params.speed : 1.0);
            p->accel = p->max_speed / ACCEL_TIME;
            p->brake = p->max_speed / BRAKE_TIME;
        }
    }

    void resize(int rows, int cols) {
        double old_h = geo_.H, old_w = geo_.W;
        geo_.fit(rows, cols);
        canvas_.resize(cols, rows);
        double sx = 1.0, sy = 1.0;
        if (old_h > 0 && old_w > 0) {
            sx = geo_.W / old_w;
            sy = geo_.H / old_h;
            ball_.x *= sx; ball_.y *= sy;
            left_.y *= sy; right_.y *= sy;
        }
        left_.x = geo_.x_left;   left_.col = geo_.col_left;
        right_.x = geo_.x_right; right_.col = geo_.col_right;
        retune();
        // Carry the rally's pace across the resize rather than snapping it.
        ball_.vx *= sx; ball_.vy *= sy;
        left_.y = clampv(left_.y, left_.half, geo_.H - left_.half);
        right_.y = clampv(right_.y, right_.half, geo_.H - right_.half);
        trail_.clear();
    }

    void setup(Mode m) {
        mode_ = m;
        left_ = Paddle{};
        right_ = Paddle{};
        left_.x = geo_.x_left;   left_.col = geo_.col_left;
        right_.x = geo_.x_right; right_.col = geo_.col_right;
        left_.face_dir = 1.0;    right_.face_dir = -1.0;
        left_.ink = INK_P1;      right_.ink = INK_P2;
        left_.y = right_.y = geo_.H / 2.0;
        left_.ai = (m == Mode::Demo);
        right_.ai = (m != Mode::TwoPlayer);
        left_.params = DIFFICULTY[m == Mode::Demo ? 2 : difficulty_];
        right_.params = DIFFICULTY[m == Mode::Demo ? 1 : difficulty_];
        retune();
        score_l_ = score_r_ = 0;
        winner_ = 0;
        serve(frand() < 0.5 ? -1.0 : 1.0);
        state_ = (m == Mode::Demo) ? State::Menu : State::Play;
    }

    bool handle_key(int key) {   // false means quit
        switch (key) {
        case 'q': case 'Q': return false;
        case KEY_RESIZE: return true;
        default: break;
        }

        if (state_ == State::Menu) {
            switch (key) {
            case '1': setup(Mode::OnePlayer); break;
            case '2': setup(Mode::TwoPlayer); break;
            case KEY_LEFT: case 'a': case 'h':
                difficulty_ = (difficulty_ + 3) % 4; break;
            case KEY_RIGHT: case 'd': case 'l':
                difficulty_ = (difficulty_ + 1) % 4; break;
            case '\n': case '\r': case KEY_ENTER: case ' ':
                setup(Mode::OnePlayer); break;
            default: break;
            }
            return true;
        }

        if (state_ == State::Over) {
            if (key == 'r' || key == 'R') setup(mode_);
            else if (key == 'm' || key == 'M') setup(Mode::Demo);
            return true;
        }

        switch (key) {
        case 'p': case 'P': case ' ':
            state_ = (state_ == State::Paused) ? State::Play : State::Paused;
            break;
        case 'm': case 'M': setup(Mode::Demo); break;
        case KEY_UP:    hold(mode_ == Mode::TwoPlayer ? p2_ : p1_, -1); break;
        case KEY_DOWN:  hold(mode_ == Mode::TwoPlayer ? p2_ : p1_, +1); break;
        case 'w': case 'W': case 'k': hold(p1_, -1); break;
        case 's': case 'S': case 'j': hold(p1_, +1); break;
        default: break;
        }
        return true;
    }

    void update(double dt) {
        double t = now_seconds();
        if (p1_.until < t) p1_.dir = 0;
        if (p2_.until < t) p2_.dir = 0;

        if (state_ == State::Paused || state_ == State::Over) return;

        bool live = t >= serve_at_;
        if (!left_.ai)  left_.drive(p1_.dir, dt, geo_.H);
        if (!right_.ai) right_.drive(p2_.dir, dt, geo_.H);
        if (left_.ai)  drive_ai(left_, dt, live);
        if (right_.ai) drive_ai(right_, dt, live);

        if (!live) return;

        // Fixed substeps keep the physics identical whatever the frame rate.
        accum_ += dt;
        double h = 1.0 / PHYS_HZ;
        int guard = 0;
        while (accum_ >= h && guard++ < 400) {
            accum_ -= h;
            step(h);
            if (state_ != State::Play && state_ != State::Menu) break;
        }

        trail_.push_front({ball_.x, ball_.y});
        while (static_cast<int>(trail_.size()) > TRAIL_LEN) trail_.pop_back();
    }

    // Read-only view of the simulation, for tests and diagnostics.
    struct Snapshot {
        double x, y, vx, vy, spin;
        double left_y, right_y, W, H;
        int left, right, winner;
        bool serving;
    };
    Snapshot snapshot() const {
        return {ball_.x, ball_.y, ball_.vx, ball_.vy, ball_.spin,
                left_.y, right_.y, geo_.W, geo_.H,
                score_l_, score_r_, winner_, now_seconds() < serve_at_};
    }

    void draw() {
        canvas_.clear();
        draw_field();
        draw_scores();
        draw_ball();
        draw_paddles();
        draw_hud();
        if (state_ == State::Menu) draw_menu();
        else if (state_ == State::Paused)
            banner({L"PAUSED", L"", L"p resume    m menu    q quit"});
        else if (state_ == State::Over) {
            std::wstring who = winner_ == 1
                ? (mode_ == Mode::TwoPlayer ? L"LEFT PLAYER WINS" : L"YOU WIN!")
                : (mode_ == Mode::TwoPlayer ? L"RIGHT PLAYER WINS" : L"COMPUTER WINS");
            wchar_t line[64];
            std::swprintf(line, 64, L"%d  -  %d", score_l_, score_r_);
            banner({who, line, L"", L"r rematch    m menu    q quit"});
        }
        canvas_.flush();
    }

private:
    struct Hold { int dir = 0; double until = 0; };
    struct Pt { double x, y; };

    void hold(Hold& h, int dir) {
        h.dir = dir;
        h.until = now_seconds() + KEY_HOLD;
    }

    void serve(double dir) {
        ball_.x = geo_.W / 2.0;
        ball_.y = geo_.H / 2.0;
        double angle = frand(-SERVE_ANGLE, SERVE_ANGLE);
        ball_.vx = serve_v_ * std::cos(angle) * dir;
        ball_.vy = serve_v_ * std::sin(angle);
        ball_.spin = frand(-4.0, 4.0);
        serve_at_ = now_seconds() + (winner_ ? 0.0 : SERVE_DELAY);
        accum_ = 0.0;
        trail_.clear();
        left_.committed = right_.committed = false;
    }

    void step(double h) {
        double px = ball_.x, py = ball_.y;
        integrate(ball_, h, geo_.H, min_v_);

        double face_l = left_.x + CELL_W / 2.0 + BALL_RX;
        double face_r = right_.x - CELL_W / 2.0 - BALL_RX;

        // Only the substep that actually crosses a paddle's plane can be a
        // hit.  Testing "is the ball past the face" instead would let a paddle
        // reach out and rescue a ball it had already missed.
        if (ball_.vx < 0 && px > face_l && ball_.x <= face_l) {
            if (try_hit(left_, face_l, px, py)) return;
        } else if (ball_.vx > 0 && px < face_r && ball_.x >= face_r) {
            if (try_hit(right_, face_r, px, py)) return;
        }

        if (ball_.x < -BALL_RX) point(2);
        else if (ball_.x > geo_.W + BALL_RX) point(1);
    }

    bool try_hit(Paddle& p, double face, double px, double py) {
        // Where the ball was when it reached the paddle's plane, not where it
        // ended the substep - at speed those differ by a visible fraction of
        // a cell.
        double span = px - ball_.x;
        double frac = std::fabs(span) > 1e-9 ? (px - face) / span : 0.0;
        double hit_y = py + (ball_.y - py) * clampv(frac, 0.0, 1.0);

        double reach = p.half + BALL_R * 0.9;
        double off = (hit_y - p.y) / reach;
        if (std::fabs(off) > 1.0) {
            return false;
        }

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
        return true;
    }

    void point(int who) {
        if (who == 1) ++score_l_; else ++score_r_;
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
        double target_x = p.x + p.face_dir * (CELL_W / 2.0 + BALL_RX);
        double h = 1.0 / 120.0;
        for (int i = 0; i < 1200; ++i) {
            integrate(b, h, geo_.H, min_v_);
            if (p.face_dir > 0 ? (b.x <= target_x) : (b.x >= target_x)) break;
            if (b.x < -1.0 || b.x > geo_.W + 1.0) break;
        }
        return clampv(b.y, 0.0, geo_.H);
    }

    void drive_ai(Paddle& p, double dt, bool live) {
        const AiParams& a = p.params;
        bool incoming = live && ((ball_.vx > 0) == (p.face_dir < 0));
        double crossed = p.face_dir < 0 ? ball_.x / geo_.W
                                        : 1.0 - ball_.x / geo_.W;

        if (!incoming) {
            p.committed = false;
            p.react_at = 0.0;
        } else if (!p.committed && crossed > a.zone) {
            // A new shot: pick this return's misjudgement and start reacting.
            p.committed = true;
            p.react_at = now_seconds() + a.lag;
            // Triangular, so the opponent is usually close and occasionally
            // badly wrong - a uniform error reads as random flailing.
            p.aim_off = (frand() + frand() - 1.0) * a.aim * p.half;
        }

        double target = geo_.H / 2.0;   // idle: drift back to the middle
        if (p.committed && now_seconds() >= p.react_at) {
            // Weak opponents just chase the ball's current height; strong ones
            // solve the intercept with the same integrator the ball uses, so
            // they read spin and wall bounces correctly.
            double naive = ball_.y;
            double solved = a.predict > 0.0 ? predict_intercept(p) : naive;
            target = naive + (solved - naive) * a.predict + p.aim_off;
            target = clampv(target, p.half, geo_.H - p.half);
        } else if (p.committed) {
            p.drive(0, dt, geo_.H);   // still reacting
            return;
        }

        double delta = target - p.y;
        int dir = 0;
        if (std::fabs(delta) > 0.25) {
            dir = delta > 0 ? 1 : -1;
            // Ease off near the target so the paddle settles instead of
            // oscillating, and so its velocity is honest at the moment of
            // contact - a swinging paddle imparts spin.
            double brake = p.vy * p.vy / (2.0 * p.accel);
            if (brake > std::fabs(delta) && sgn(p.vy) == dir) dir = 0;
        }
        p.drive(dir, dt, geo_.H);
    }

    // ------------------------------------------------------------- rendering

    void draw_field() {
        std::wstring wall(geo_.cols, L'─');
        canvas_.text(geo_.top_wall, 0, wall, INK_WALL);
        canvas_.text(geo_.bot_wall, 0, wall, INK_WALL);

        int mid = geo_.cols / 2;
        for (int s = geo_.play_top * 2; s <= geo_.play_bot * 2 + 1; ++s)
            if (((s - geo_.play_top * 2) / 2) % 2 == 0) canvas_.plot(s, mid, INK_NET);
    }

    void draw_paddles() {
        for (const Paddle* p : {&left_, &right_}) {
            int s0 = geo_.subrow_of(p->y - p->half);
            int s1 = geo_.subrow_of(p->y + p->half);
            canvas_.plot_span(s0, s1, p->col, p->ink);
        }
    }

    void draw_ball() {
        for (size_t i = trail_.size(); i-- > 0;) {
            uint8_t ink = (i < trail_.size() / 3) ? INK_TRAIL1 : INK_TRAIL2;
            canvas_.plot(geo_.subrow_of(trail_[i].y), geo_.col_of(trail_[i].x), ink);
        }
        canvas_.plot(geo_.subrow_of(ball_.y), geo_.col_of(ball_.x), INK_BALL);
    }

    static const char* digit_row(int d, int r) {
        static const char* font[10][5] = {
            {"###","# #","# #","# #","###"}, {" # ","## "," # "," # ","###"},
            {"###","  #","###","#  ","###"}, {"###","  #","###","  #","###"},
            {"# #","# #","###","  #","  #"}, {"###","#  ","###","  #","###"},
            {"###","#  ","###","# #","###"}, {"###","  #","  #","  #","  #"},
            {"###","# #","###","# #","###"}, {"###","# #","###","  #","###"},
        };
        return font[d][r];
    }

    // Scores are painted into the half-cell layer so they sit behind the ball
    // like the ghost numbers on an arcade cabinet.
    void draw_number(int value, int centre_col, int sub_top) {
        std::string s = std::to_string(value);
        int digit_w = 3 * 2, gap = 2;
        int total = static_cast<int>(s.size()) * digit_w +
                    (static_cast<int>(s.size()) - 1) * gap;
        int col = centre_col - total / 2;
        for (char c : s) {
            int d = c - '0';
            for (int r = 0; r < 5; ++r) {
                const char* bits = digit_row(d, r);
                for (int b = 0; b < 3; ++b) {
                    if (bits[b] != '#') continue;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx)
                            canvas_.plot(sub_top + r * 2 + dy, col + b * 2 + dx,
                                         INK_SCORE);
                }
            }
            col += digit_w + gap;
        }
    }

    void draw_scores() {
        if (geo_.H < 14) return;   // small windows keep the scoreboard line only
        int sub_top = geo_.play_top * 2 + 2;
        draw_number(score_l_, geo_.cols / 2 - geo_.cols / 5, sub_top);
        draw_number(score_r_, geo_.cols / 2 + geo_.cols / 5, sub_top);
    }

    void draw_hud() {
        const wchar_t* lname = mode_ == Mode::TwoPlayer ? L"P1" :
                               mode_ == Mode::Demo ? L"CPU" : L"YOU";
        const wchar_t* rname = mode_ == Mode::TwoPlayer ? L"P2" : L"CPU";
        wchar_t left[64];
        std::swprintf(left, 64, L" %ls %d   %d %ls", lname, score_l_,
                      score_r_, rname);
        canvas_.text(geo_.hud_row, 0, std::wstring(geo_.cols, L' '), INK_SCORE);
        canvas_.text(geo_.hud_row, 0, left, INK_SCORE, A_BOLD);

        std::wstring hint;
        if (state_ == State::Menu) hint = L"1 one player   2 two players   ←/→ difficulty";
        else if (mode_ == Mode::TwoPlayer) hint = L"w/s  vs  ↑/↓   p pause   q quit";
        else hint = L"↑/↓ move   p pause   m menu   q quit";

        int point_at = std::max(score_l_, score_r_);
        bool live = state_ == State::Play || state_ == State::Paused;
        if (live && point_at >= WIN_SCORE - 1 && std::abs(score_l_ - score_r_) >= 1)
            hint = (score_l_ > score_r_) ? L"MATCH POINT ◀" : L"▶ MATCH POINT";

        int start = static_cast<int>((geo_.cols - hint.size()) / 2);
        if (start > 12) canvas_.text(geo_.hud_row, start, hint, INK_ACCENT);

        std::wstring diff = std::wstring(L"[") +
            std::wstring(DIFFICULTY_NAME[difficulty_],
                         DIFFICULTY_NAME[difficulty_] +
                         std::char_traits<char>::length(DIFFICULTY_NAME[difficulty_])) +
            L"] ";
        if (mode_ != Mode::TwoPlayer)
            canvas_.text(geo_.hud_row, geo_.cols - static_cast<int>(diff.size()),
                         diff, INK_ACCENT);
    }

    void banner(const std::vector<std::wstring>& lines) {
        size_t w = 0;
        for (const auto& l : lines) w = std::max(w, l.size());
        int inner = static_cast<int>(w) + 6;
        int box_h = static_cast<int>(lines.size()) + 2;
        int top = (geo_.rows - box_h) / 2;
        int left = (geo_.cols - inner) / 2;
        if (left < 0 || top < 0) return;

        std::wstring bar(inner - 2, L'─');
        canvas_.text(top, left, L"╭" + bar + L"╮", INK_ACCENT, A_BOLD);
        for (size_t i = 0; i < lines.size(); ++i) {
            std::wstring row = lines[i];
            int pad = inner - 2 - static_cast<int>(row.size());
            int lp = pad / 2;
            std::wstring body = std::wstring(lp, L' ') + row +
                                std::wstring(pad - lp, L' ');
            canvas_.text(top + 1 + static_cast<int>(i), left,
                         L"│" + body + L"│", INK_ACCENT,
                         i == 0 ? A_BOLD : 0);
        }
        canvas_.text(top + box_h - 1, left, L"╰" + bar + L"╯",
                     INK_ACCENT, A_BOLD);
    }

    void draw_menu() {
        const char* d = DIFFICULTY_NAME[difficulty_];
        wchar_t diff[48];
        std::swprintf(diff, 48, L"◀  %hs  ▶", d);
        banner({L"T E R M P O N G", L"",
                L"1   one player      2   two players",
                std::wstring(diff), L"",
                L"↑/↓ or w/s to move    q to quit"});
    }

    Geometry geo_;
    Canvas canvas_;
    Ball ball_;
    Paddle left_, right_;
    Hold p1_, p2_;
    std::deque<Pt> trail_;

    Mode mode_ = Mode::Demo;
    State state_ = State::Menu;
    int score_l_ = 0, score_r_ = 0;
    int winner_ = 0;
    int difficulty_ = 1;
    double serve_at_ = 0.0, accum_ = 0.0;
    double serve_v_ = 0, max_v_ = 0, min_v_ = 0, player_v_ = 0;
};

// ---------------------------------------------------------------------- main

volatile std::sig_atomic_t g_resized = 0;
void on_winch(int) { g_resized = 1; }

void too_small(int rows, int cols) {
    erase();
    char msg[64];
    std::snprintf(msg, sizeof msg, "Need %dx%d, have %dx%d.",
                  MIN_WIDTH, MIN_HEIGHT, cols, rows);
    mvaddstr(0, 0, msg);
    mvaddstr(1, 0, "Resize, or q to quit.");
    refresh();
}

}  // namespace

int main() {
    std::setlocale(LC_ALL, "");
    rng_state ^= static_cast<uint32_t>(
        std::chrono::system_clock::now().time_since_epoch().count());

    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    curs_set(0);
    init_palette();
    std::signal(SIGWINCH, on_winch);

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    Game game(std::max(rows, MIN_HEIGHT), std::max(cols, MIN_WIDTH));

    const double frame = 1.0 / TARGET_FPS;
    g_now = real_seconds();
    double previous = g_now;
    bool running = true;

    while (running) {
        g_now = real_seconds();
        double start = g_now;
        double dt = std::min(start - previous, 0.05);
        previous = start;

        if (g_resized) {
            g_resized = 0;
            endwin();
            refresh();
            erase();
            getmaxyx(stdscr, rows, cols);
            if (rows >= MIN_HEIGHT && cols >= MIN_WIDTH) game.resize(rows, cols);
        }

        int key;
        while ((key = getch()) != ERR)
            if (!game.handle_key(key)) { running = false; break; }
        if (!running) break;

        getmaxyx(stdscr, rows, cols);
        if (rows < MIN_HEIGHT || cols < MIN_WIDTH) {
            too_small(rows, cols);
        } else {
            game.update(dt);
            erase();
            game.draw();
            refresh();
        }

        double sleep = frame - (real_seconds() - start);
        if (sleep > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(sleep));
    }

    endwin();
    return 0;
}
