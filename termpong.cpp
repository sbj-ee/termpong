// termpong - classic pong in a terminal.
//
// The simulation lives in pong_core.h; this file is the terminal front end -
// character-cell rendering at quarter-cell resolution, and the guesswork
// needed to turn a terminal's auto-repeat back into a held key.
//
// Build: make      Run: ./termpong

#define _XOPEN_SOURCE_EXTENDED 1

#include <curses.h>

#include "pong_core.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace pong;

// ---------------------------------------------------------------- constants

constexpr double TARGET_FPS = 60.0;

// One field unit is the height of a row.  A terminal cell is roughly twice as
// tall as it is wide, so a column is half a unit - which is what keeps a
// 45-degree shot looking like 45 degrees on screen.
constexpr double CELL_W = 0.5;
constexpr double BALL_RX = 0.25;   // the ball glyph is one column wide
constexpr int PADDLE_MARGIN = 2;   // columns in from the field edge

// Terminals report key presses but never releases, so a held key reaches us
// as the terminal's own auto-repeat: one event, a long initial delay, then a
// fast stream.  Both numbers vary by terminal and by the user's settings, so
// a fixed hold window stutters once per press on some machines and drifts on
// others.  These are only the starting guesses; the real values get learned
// from the repeats as they arrive.
constexpr double TAP_HOLD = 0.13;        // full speed bought by a single press
constexpr double COAST_SPEED = 0.25;     // ...then a glide while it is in doubt
constexpr double DEFAULT_DELAY = 0.45;   // assumed press -> first repeat
constexpr double DEFAULT_RATE = 0.05;    // assumed repeat -> repeat
constexpr double REPEAT_SPLIT = 1.0;     // further apart than this: a new press
constexpr double DELAY_FLOOR = 0.12;     // shorter gaps are not an initial delay
constexpr double RATE_CEILING = 0.35;    // longer gaps are not a repeat stream

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

// -------------------------------------------------------------------- colour

// Every playfield cell is a 2x2 grid of quadrants, each holding one of these
// palette slots.  A cell is then drawn as the block glyph matching the pattern
// of quadrants, with its two commonest inks as foreground and background.
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

// Two layers: a quadrant "ink" bitmap for the game objects, and a text layer
// for the scoreboard and banners, which wins wherever it is set.
class Canvas {
public:
    void resize(int w, int h) {
        w_ = w; h_ = h;
        sub_.assign(static_cast<size_t>(w) * h * 4, INK_NONE);
        text_.assign(static_cast<size_t>(w) * h, TextCell{});
    }

    void clear() {
        std::fill(sub_.begin(), sub_.end(), INK_NONE);
        std::fill(text_.begin(), text_.end(), TextCell{});
    }

    int width() const { return w_; }
    int height() const { return h_; }

    // Quadrant coordinates: two per cell in each direction.
    void plot(int subrow, int subcol, uint8_t ink) {
        if (subcol < 0 || subcol >= w_ * 2 || subrow < 0 || subrow >= h_ * 2)
            return;
        sub_[static_cast<size_t>(subrow) * w_ * 2 + subcol] = ink;
    }

    // Both quadrants of one whole column, for things a column wide.
    void plot_col(int subrow, int col, uint8_t ink) {
        plot(subrow, col * 2, ink);
        plot(subrow, col * 2 + 1, ink);
    }

    // Fill a half-open span of half-rows in one column.
    void plot_span(int sub0, int sub1, int col, uint8_t ink) {
        for (int s = sub0; s < sub1; ++s) plot_col(s, col, ink);
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
                uint8_t q[4] = {quad(r * 2, c * 2),     quad(r * 2, c * 2 + 1),
                                quad(r * 2 + 1, c * 2), quad(r * 2 + 1, c * 2 + 1)};

                // A cell carries one foreground and one background, so pick
                // the two inks covering the most of it and fold the rest in.
                uint8_t fg = INK_NONE, bg = INK_NONE;
                int nfg = 0, nbg = 0;
                for (int i = 0; i < 4; ++i) {
                    if (q[i] == INK_NONE || q[i] == fg || q[i] == bg) continue;
                    int n = 0;
                    for (int j = 0; j < 4; ++j) if (q[j] == q[i]) ++n;
                    if (n > nfg) { bg = fg; nbg = nfg; fg = q[i]; nfg = n; }
                    else if (n > nbg) { bg = q[i]; nbg = n; }
                }
                if (fg == INK_NONE) { put(r, c, L' ', INK_NONE, INK_NONE, 0); continue; }
                // The ball is what the eye follows, so it takes the
                // foreground whenever it shares a cell with anything else.
                if (fg != INK_BALL)
                    for (int i = 0; i < 4; ++i)
                        if (q[i] == INK_BALL) { bg = fg; fg = INK_BALL; break; }

                int mask = 0;
                for (int i = 0; i < 4; ++i) if (q[i] == fg) mask |= 1 << i;
                attr_t attr = (fg == INK_BALL || bg == INK_BALL) ? A_BOLD : 0;
                put(r, c, QUAD[mask], fg, bg, attr);
            }
    }

private:
    // Indexed by a bitmask of filled quadrants: 1 top-left, 2 top-right,
    // 4 bottom-left, 8 bottom-right.
    static constexpr wchar_t QUAD[16] = {
        L' ',      L'\u2598', L'\u259d', L'\u2580', L'\u2596', L'\u258c', L'\u259e', L'\u259b',
        L'\u2597', L'\u259a', L'\u2590', L'\u259c', L'\u2584', L'\u2599', L'\u259f', L'\u2588'};

    uint8_t quad(int subrow, int subcol) const {
        return sub_[static_cast<size_t>(subrow) * w_ * 2 + subcol];
    }

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
    // Half-column resolution, so a moving ball advances in half-cell steps
    // horizontally as well as vertically.
    int subcol_of(double x) const {
        return clampv(static_cast<int>(std::floor(x / (CELL_W * 0.5))), 0,
                      cols * 2 - 1);
    }
};

// --------------------------------------------------------------------- input

// What the terminal's auto-repeat looks like here.  Learned once and shared by
// both players, since it is a property of the terminal, not of the key.
struct Repeat {
    double delay = 0.0;   // press -> first repeat
    double rate = 0.0;    // repeat -> repeat
    static double blend(double have, double seen) {
        return have > 0.0 ? have * 0.7 + seen * 0.3 : seen;
    }
};
Repeat g_repeat;

// One player's key state.  A press buys a fixed slice of full speed; after
// that the paddle coasts rather than stopping, because the next auto-repeat -
// if the key really is still down - is still on its way.  By the time it
// arrives the repeat cadence is known, and the windows stretch to cover it.
struct Hold {
    int dir = 0;
    double last = -1e9;
    double full = 0.0;    // seconds of full speed from `last`
    double coast = 0.0;   // ...then this many more at COAST_SPEED

    void press(int d, double t) {
        double gap = t - last;
        if (d != dir || gap >= REPEAT_SPLIT) {
            repeats_ = 0;
        } else {
            if (repeats_ == 0) {
                if (gap > DELAY_FLOOR) g_repeat.delay = Repeat::blend(g_repeat.delay, gap);
            } else if (gap < RATE_CEILING) {
                g_repeat.rate = Repeat::blend(g_repeat.rate, gap);
            }
            ++repeats_;
        }
        dir = d;
        last = t;

        if (repeats_ == 0) {   // could be a tap, could be the start of a hold
            double delay = g_repeat.delay > 0.0 ? g_repeat.delay : DEFAULT_DELAY;
            full = TAP_HOLD;
            coast = clampv(delay * 1.25 + 0.05, TAP_HOLD + 0.1, 0.9) - full;
        } else {               // definitely held: just outlast the repeats
            double rate = g_repeat.rate > 0.0 ? g_repeat.rate : DEFAULT_RATE;
            full = clampv(rate * 2.0 + 0.04, 0.09, 0.28);
            coast = std::max(rate * 2.0, 0.08);
        }
    }

    // Fraction of top speed the paddle should be running at, signed.
    double want(double t) {
        if (dir == 0) return 0.0;
        if (t <= last + full) return dir;
        if (t <= last + full + coast) return dir * COAST_SPEED;
        dir = 0;
        return 0.0;
    }

private:
    int repeats_ = 0;
};

// ---------------------------------------------------------------- front end

class Game {
public:
    Game(int rows, int cols) {
        geo_.fit(rows, cols);
        canvas_.resize(cols, rows);
        m_.configure(layout());
    }

    void resize(int rows, int cols) {
        geo_.fit(rows, cols);
        canvas_.resize(cols, rows);
        m_.resize(layout());
    }

    bool handle_key(int key) {   // false means quit
        switch (key) {
        case 'q': case 'Q': return false;
        case KEY_RESIZE: return true;
        default: break;
        }

        if (m_.state() == State::Menu) {
            switch (key) {
            case '1': m_.setup(Mode::OnePlayer); break;
            case '2': m_.setup(Mode::TwoPlayer); break;
            case KEY_LEFT: case 'a': case 'h': m_.step_difficulty(-1); break;
            case KEY_RIGHT: case 'd': case 'l': m_.step_difficulty(+1); break;
            case '\n': case '\r': case KEY_ENTER: case ' ':
                m_.setup(Mode::OnePlayer); break;
            default: break;
            }
            return true;
        }

        if (m_.state() == State::Over) {
            if (key == 'r' || key == 'R') m_.rematch();
            else if (key == 'm' || key == 'M') m_.setup(Mode::Demo);
            return true;
        }

        switch (key) {
        case 'p': case 'P': case ' ': m_.toggle_pause(); break;
        case 'm': case 'M': m_.setup(Mode::Demo); break;
        case KEY_UP:   press(m_.mode() == Mode::TwoPlayer ? p2_ : p1_, -1); break;
        case KEY_DOWN: press(m_.mode() == Mode::TwoPlayer ? p2_ : p1_, +1); break;
        case 'w': case 'W': case 'k': press(p1_, -1); break;
        case 's': case 'S': case 'j': press(p1_, +1); break;
        default: break;
        }
        return true;
    }

    void update(double dt) {
        double t = now_seconds();
        m_.set_intent(0, {Intent::Kind::Speed, p1_.want(t)});
        m_.set_intent(1, {Intent::Kind::Speed, p2_.want(t)});
        m_.update(dt);
    }

    Match::Snapshot snapshot() const { return m_.snapshot(); }

    void draw() {
        canvas_.clear();
        draw_field();
        draw_scores();
        draw_ball();
        draw_paddles();
        draw_hud();
        if (m_.state() == State::Menu) draw_menu();
        else if (m_.state() == State::Paused)
            banner({L"PAUSED", L"", L"p resume    m menu    q quit"});
        else if (m_.state() == State::Over) {
            std::wstring who = m_.winner() == 1
                ? (m_.mode() == Mode::TwoPlayer ? L"LEFT PLAYER WINS" : L"YOU WIN!")
                : (m_.mode() == Mode::TwoPlayer ? L"RIGHT PLAYER WINS" : L"COMPUTER WINS");
            wchar_t line[64];
            std::swprintf(line, 64, L"%d  -  %d", m_.score_left(), m_.score_right());
            banner({who, line, L"", L"r rematch    m menu    q quit"});
        }
        canvas_.flush();
    }

private:
    Layout layout() const {
        return {geo_.W, geo_.H, (PADDLE_MARGIN + 0.5) * CELL_W,
                CELL_W / 2.0, BALL_RX};
    }

    void press(Hold& h, int dir) { h.press(dir, now_seconds()); }

    // ------------------------------------------------------------- rendering

    void draw_field() {
        std::wstring wall(geo_.cols, L'─');
        canvas_.text(geo_.top_wall, 0, wall, INK_WALL);
        canvas_.text(geo_.bot_wall, 0, wall, INK_WALL);

        int mid = geo_.cols / 2;
        for (int s = geo_.play_top * 2; s <= geo_.play_bot * 2 + 1; ++s)
            if (((s - geo_.play_top * 2) / 2) % 2 == 0)
                canvas_.plot_col(s, mid, INK_NET);
    }

    void draw_paddles() {
        const Paddle* side[2] = {&m_.left(), &m_.right()};
        const int col[2] = {geo_.col_left, geo_.col_right};
        const uint8_t ink[2] = {INK_P1, INK_P2};
        for (int i = 0; i < 2; ++i) {
            int s0 = geo_.subrow_of(side[i]->y - side[i]->half);
            int s1 = geo_.subrow_of(side[i]->y + side[i]->half);
            canvas_.plot_span(s0, s1, col[i], ink[i]);
        }
    }

    // The ball is a column wide and half a row tall - a square, given the
    // shape of a cell - but it is placed on the half-column grid, so it slides
    // rather than hopping from column to column.
    void dot(double x, double y, uint8_t ink) {
        int sub = geo_.subcol_of(x - BALL_RX);
        int row = geo_.subrow_of(y);
        canvas_.plot(row, sub, ink);
        canvas_.plot(row, sub + 1, ink);
    }

    void draw_ball() {
        for (size_t i = m_.trail().size(); i-- > 0;) {
            uint8_t ink = (i < m_.trail().size() / 3) ? INK_TRAIL1 : INK_TRAIL2;
            dot(m_.trail()[i].x, m_.trail()[i].y, ink);
        }
        dot(m_.ball().x, m_.ball().y, INK_BALL);
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
                            canvas_.plot_col(sub_top + r * 2 + dy, col + b * 2 + dx,
                                             INK_SCORE);
                }
            }
            col += digit_w + gap;
        }
    }

    void draw_scores() {
        if (geo_.H < 14) return;   // small windows keep the scoreboard line only
        int sub_top = geo_.play_top * 2 + 2;
        draw_number(m_.score_left(), geo_.cols / 2 - geo_.cols / 5, sub_top);
        draw_number(m_.score_right(), geo_.cols / 2 + geo_.cols / 5, sub_top);
    }

    void draw_hud() {
        const wchar_t* lname = m_.mode() == Mode::TwoPlayer ? L"P1" :
                               m_.mode() == Mode::Demo ? L"CPU" : L"YOU";
        const wchar_t* rname = m_.mode() == Mode::TwoPlayer ? L"P2" : L"CPU";
        wchar_t left[64];
        std::swprintf(left, 64, L" %ls %d   %d %ls", lname, m_.score_left(),
                      m_.score_right(), rname);
        canvas_.text(geo_.hud_row, 0, std::wstring(geo_.cols, L' '), INK_SCORE);
        canvas_.text(geo_.hud_row, 0, left, INK_SCORE, A_BOLD);

        std::wstring hint;
        if (m_.state() == State::Menu) hint = L"1 one player   2 two players   ←/→ difficulty";
        else if (m_.mode() == Mode::TwoPlayer) hint = L"w/s  vs  ↑/↓   p pause   q quit";
        else hint = L"↑/↓ move   p pause   m menu   q quit";

        int point_at = std::max(m_.score_left(), m_.score_right());
        bool live = m_.state() == State::Play || m_.state() == State::Paused;
        if (live && point_at >= WIN_SCORE - 1 && std::abs(m_.score_left() - m_.score_right()) >= 1)
            hint = (m_.score_left() > m_.score_right()) ? L"MATCH POINT ◀" : L"▶ MATCH POINT";

        int start = static_cast<int>((geo_.cols - hint.size()) / 2);
        if (start > 12) canvas_.text(geo_.hud_row, start, hint, INK_ACCENT);

        std::wstring diff = std::wstring(L"[") +
            std::wstring(DIFFICULTY_NAME[m_.difficulty()],
                         DIFFICULTY_NAME[m_.difficulty()] +
                         std::char_traits<char>::length(DIFFICULTY_NAME[m_.difficulty()])) +
            L"] ";
        if (m_.mode() != Mode::TwoPlayer)
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
        const char* d = DIFFICULTY_NAME[m_.difficulty()];
        wchar_t diff[48];
        std::swprintf(diff, 48, L"◀  %hs  ▶", d);
        banner({L"T E R M P O N G", L"",
                L"1   one player      2   two players",
                std::wstring(diff), L"",
                L"↑/↓ or w/s to move    q to quit"});
    }

    Geometry geo_;
    Canvas canvas_;
    Match m_;
    Hold p1_, p2_;
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
    double deadline = g_now;
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

        // Pace to an absolute deadline rather than sleeping a fixed slice:
        // sleep always overshoots a little, and sleeping "one frame" from
        // wherever that left us lets the error pile up into visible judder.
        deadline += frame;
        double slack = deadline - real_seconds();
        if (slack > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(slack));
        else if (slack < -frame)
            deadline = real_seconds();   // fell badly behind; start afresh
    }

    endwin();
    return 0;
}
