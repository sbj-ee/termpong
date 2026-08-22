// The game is a single translation unit, so the tests just include it with
// main() renamed and drive the simulation directly - no terminal involved.
#define main game_main
#include "../termpong.cpp"
#undef main

#include <cassert>
#include <cstdio>

namespace {

struct Stats {
    int hits = 0, points = 0;
    double max_speed = 0, min_speed = 1e9;
    double max_y = -1e9, min_y = 1e9, max_x = -1e9, min_x = 1e9;
    bool nan_seen = false;
};

// Drives one side like a competent human: follow the ball, press toward it.
void bot(Game& g, bool left_side, int up_key, int down_key) {
    auto s = g.snapshot();
    double me = left_side ? s.left_y : s.right_y;
    if (s.y < me - 0.4) g.handle_key(up_key);
    else if (s.y > me + 0.4) g.handle_key(down_key);
}

Stats run(Game& g, double seconds, bool drive_left, bool drive_right) {
    Stats st;
    const double dt = 1.0 / 60.0;
    auto prev = g.snapshot();
    for (int i = 0; i < static_cast<int>(seconds / dt); ++i) {
        g_now += dt;
        if (drive_left) bot(g, true, KEY_UP, KEY_DOWN);
        if (drive_right) bot(g, false, KEY_UP, KEY_DOWN);
        g.update(dt);
        auto s = g.snapshot();
        if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.vx) ||
            !std::isfinite(s.vy) || !std::isfinite(s.spin))
            st.nan_seen = true;
        if (!s.serving) {
            double v = std::hypot(s.vx, s.vy);
            st.max_speed = std::max(st.max_speed, v);
            st.min_speed = std::min(st.min_speed, v);
            st.max_x = std::max(st.max_x, s.x); st.min_x = std::min(st.min_x, s.x);
            st.max_y = std::max(st.max_y, s.y); st.min_y = std::min(st.min_y, s.y);
        }
        if (s.vx * prev.vx < 0 && std::fabs(s.x - s.W / 2) > 1.0) ++st.hits;
        if (s.left + s.right != prev.left + prev.right) ++st.points;
        prev = s;
    }
    return st;
}

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    g_now = 0.0;

    // --- physics sanity: a ball with no spin conserves direction, drag slows it
    {
        Ball b{5.0, 5.0, 20.0, 0.0, 0.0};
        for (int i = 0; i < 480; ++i) integrate(b, 1.0 / 480.0, 20.0, 1.0);
        check(b.vx > 0 && b.vx <= 20.0 + 1e-9, "drag never speeds a ball up");
        check(std::fabs(b.vy) < 1e-9, "no spin means no sideways drift");
    }
    // --- spin curves the flight (Magnus)
    {
        Ball a{5.0, 10.0, 20.0, 0.0, 0.0}, c{5.0, 10.0, 20.0, 0.0, MAX_SPIN};
        for (int i = 0; i < 240; ++i) {
            integrate(a, 1.0 / 480.0, 30.0, 1.0);
            integrate(c, 1.0 / 480.0, 30.0, 1.0);
        }
        check(std::fabs(c.y - a.y) > 0.2, "spin bends the flight path");
    }
    // --- wall bounce keeps the ball inside and reverses it
    {
        Ball b{5.0, 1.0, 0.0, -20.0, 0.0};
        for (int i = 0; i < 240; ++i) integrate(b, 1.0 / 480.0, 20.0, 1.0);
        check(b.y >= BALL_R - 1e-6 && b.y <= 20.0 - BALL_R + 1e-6,
              "wall bounce keeps the ball in the field");
        check(b.vy > 0, "wall bounce reverses the ball");
    }
    // --- a friction contact converts slip into spin
    {
        Ball b{0, 0, -20.0, 6.0, 0.0};
        contact(b, 1.0, 0.0, PADDLE_RESTITUTION, PADDLE_FRICTION, 0.0, 0.0);
        check(b.vx > 0, "paddle contact reverses the ball");
        check(std::fabs(b.spin) > 1.0, "a glancing hit puts spin on the ball");
    }
    // --- a moving paddle drags the ball with it
    {
        Ball still{0, 0, -20.0, 0.0, 0.0}, swung{0, 0, -20.0, 0.0, 0.0};
        contact(still, 1.0, 0.0, PADDLE_RESTITUTION, PADDLE_FRICTION, 0.0, 0.0);
        contact(swung, 1.0, 0.0, PADDLE_RESTITUTION, PADDLE_FRICTION, 0.0, 25.0);
        check(swung.vy > still.vy + 1.0, "a swinging paddle carries the ball");
    }

    // --- long demo match: two computers rallying for five simulated minutes
    {
        Game g(30, 100);
        g.setup(Mode::Demo);          // attract mode never ends
        Stats st = run(g, 300.0, false, false);
        auto s = g.snapshot();
        check(!st.nan_seen, "no NaN in a five-minute match");
        double player_v = s.H / PLAYER_CROSS;
        double max_v = std::min(s.W / MAX_CROSS, player_v * MAX_HEADROOM);
        double min_v = std::min(s.W / SERVE_CROSS, max_v * 0.55) * 0.8;
        check(st.max_speed <= max_v + 1e-6, "ball never exceeds max speed");
        check(st.min_speed >= min_v - 1e-6, "ball never falls below min speed");
        check(st.min_x > -2.0 && st.max_x < s.W + 2.0, "ball stays in the field (x)");
        check(st.min_y > -0.5 && st.max_y < s.H + 0.5, "ball stays in the field (y)");
        check(st.hits > 50 && st.hits > st.points * 3, "rallies actually happen");
        check(st.points > 0, "points get scored");
        std::printf("     ... %d hits, %d points, speed %.1f-%.1f\n",
                    st.hits, st.points, st.min_speed, st.max_speed);
    }

    // --- difficulty ramp: the same bot should win less as the CPU improves
    {
        double margin[4];
        for (int d = 0; d < 4; ++d) {
            Game g(30, 100);
            for (int i = 0; i < (d + 3) % 4; ++i) g.handle_key(KEY_RIGHT);
            g.handle_key('1');
            int mine = 0, theirs = 0;
            for (int match = 0; match < 3; ++match) {
                Game m(30, 100);
                for (int i = 0; i < (d + 3) % 4; ++i) m.handle_key(KEY_RIGHT);
                m.handle_key('1');
                run(m, 240.0, true, false);
                auto s = m.snapshot();
                mine += s.left; theirs += s.right;
            }
            margin[d] = static_cast<double>(mine - theirs);
            std::printf("     %-6s bot %2d - %2d cpu\n",
                        DIFFICULTY_NAME[d], mine, theirs);
        }
        check(margin[0] > margin[3], "insane beats the bot harder than easy");
        check(margin[0] > 0, "easy is beatable by a ball-tracking player");
    }

    // --- two-player mode: both paddles obey their own keys
    {
        Game g(30, 100);
        g.handle_key('2');
        auto before = g.snapshot();
        for (int i = 0; i < 30; ++i) { g_now += 1.0 / 60.0; g.handle_key('s'); g.update(1.0 / 60.0); }
        auto mid = g.snapshot();
        for (int i = 0; i < 30; ++i) { g_now += 1.0 / 60.0; g.handle_key(KEY_UP); g.update(1.0 / 60.0); }
        auto after = g.snapshot();
        check(mid.left_y > before.left_y, "P1 's' moves the left paddle down");
        check(after.right_y < mid.right_y, "P2 arrow moves the right paddle up");
    }

    // --- a held key must not stall: the terminal sends one event, then waits
    //     out its own auto-repeat delay before the stream starts
    {
        g_repeat = Repeat{};
        g_now = 0.0;
        Game g(30, 100);
        g.handle_key('2');            // two players: nothing else drives P1
        const double dt = 1.0 / 60.0;
        double next = 0.0, slowest = 1e9;
        // 0.55 s of key-down, delivered as a terminal would: one press, a
        // 0.5 s gap, then 30 repeats a second.  Stop before the paddle can
        // reach the floor, where it is meant to stand still.
        for (int i = 0; i * dt < 0.55; ++i) {
            g_now = i * dt;
            if (g_now >= next) {
                g.handle_key('s');
                next = (next == 0.0) ? 0.5 : next + 1.0 / 30.0;
            }
            double before = g.snapshot().left_y;
            g.update(dt);
            double v = (g.snapshot().left_y - before) / dt;
            if (g_now > 0.05) slowest = std::min(slowest, v);
        }
        double player_v = g.snapshot().H / PLAYER_CROSS;
        check(slowest > 0.15 * player_v, "a held key never lets the paddle stall");
        std::printf("     ... slowest %.0f%% of top speed while held\n",
                    100.0 * slowest / player_v);
    }

    // --- the computer settles onto its mark rather than hunting around it
    {
        g_now = 0.0;
        Game g(30, 100);
        g.setup(Mode::Demo);
        const double dt = 1.0 / 60.0;
        double prev_y = g.snapshot().right_y, prev_v = 0.0, jerk = 0.0;
        int frames = 60 * 60;
        for (int i = 0; i < frames; ++i) {
            g_now += dt;
            g.update(dt);
            double y = g.snapshot().right_y;
            double v = (y - prev_y) / dt;
            jerk += std::fabs(v - prev_v);
            prev_y = y; prev_v = v;
        }
        double player_v = g.snapshot().H / PLAYER_CROSS;
        double per_frame = jerk / frames / player_v;
        check(per_frame < 0.012, "the computer's paddle changes speed smoothly");
        std::printf("     ... %.1f%% of top speed per frame\n", 100.0 * per_frame);
    }

    // --- resize keeps everything on the field
    {
        Game g(30, 100);
        g.handle_key('1');
        run(g, 5.0, true, false);
        g.resize(16, 48);
        auto s = g.snapshot();
        check(s.x >= 0 && s.x <= s.W && s.y >= 0 && s.y <= s.H,
              "resize keeps the ball in bounds");
        Stats st = run(g, 20.0, true, false);
        check(!st.nan_seen && st.max_x < s.W + 2.0, "play continues after resize");
    }

    std::printf(failures ? "\n%d FAILURES\n" : "\nall good\n", failures);
    return failures ? 1 : 0;
}
