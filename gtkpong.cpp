// gtkpong - the same pong, in a window.
//
// The simulation is pong_core.h, shared with the terminal front end.  What a
// window buys over a terminal is worth being explicit about:
//
//   - Real key presses AND releases, so holding a key holds the paddle.  A
//     terminal only ever reports presses, so it has to guess at auto-repeat.
//   - The mouse as a paddle knob: absolute position, the way the original
//     1972 cabinet worked.  Move the mouse and it takes over; touch a key and
//     the keys take back.
//   - Pixels instead of character cells, and a frame clock tied to the
//     display's refresh rather than a sleep loop.
//
// Build: make gtkpong      Run: ./gtkpong

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "pong_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using namespace pong;

// ---------------------------------------------------------------- constants

constexpr double FIELD_H = 30.0;        // field height, in units
constexpr double MARGIN = 1.6;          // paddle centre line, in from the wall
constexpr double PADDLE_HALF_W = 0.32;  // half thickness of a paddle
constexpr double BALL_RX = 0.5;         // a round ball, unlike the terminal's

constexpr double NET_DASHES = 15.0;
constexpr double DIGIT_UNIT = 0.85;     // one cell of the score font, in units

struct Rgb { double r, g, b; };
constexpr Rgb BG      = {0.031, 0.039, 0.043};
constexpr Rgb FG      = {0.898, 0.949, 0.929};   // paddles, ball, score
constexpr Rgb NET     = {0.180, 0.220, 0.212};
constexpr Rgb ACCENT  = {0.404, 0.855, 0.678};
constexpr Rgb DIM     = {0.478, 0.545, 0.529};

void set_rgb(cairo_t* cr, Rgb c, double a = 1.0) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
}

// ---------------------------------------------------------------------- app

struct App {
    Match m;
    GtkWidget* area = nullptr;
    GtkWindow* win = nullptr;

    gint64 last_frame = 0;
    int px_w = 0, px_h = 0;
    double t = 0.0;              // wall clock for the front end's own effects

    // A key is down until it comes back up - the thing a terminal cannot know.
    bool up_held[2] = {false, false};
    bool down_held[2] = {false, false};

    bool knob = false;           // the mouse is driving the left paddle
    double knob_y = FIELD_H / 2.0;
    double knob_hint_until = 0.0;
};

double field_w(const App& a) {
    if (a.px_h <= 0) return FIELD_H * 16.0 / 9.0;
    return FIELD_H * static_cast<double>(a.px_w) / a.px_h;
}

Layout layout_for(const App& a) {
    return {field_w(a), FIELD_H, MARGIN, PADDLE_HALF_W, BALL_RX};
}

// --------------------------------------------------------------- score font

const char* digit_row(int d, int r) {
    static const char* font[10][5] = {
        {"###","# #","# #","# #","###"}, {" # ","## "," # "," # ","###"},
        {"###","  #","###","#  ","###"}, {"###","  #","###","  #","###"},
        {"# #","# #","###","  #","  #"}, {"###","#  ","###","  #","###"},
        {"###","#  ","###","# #","###"}, {"###","  #","  #","  #","  #"},
        {"###","# #","###","# #","###"}, {"###","# #","###","  #","###"},
    };
    return font[d][r];
}

void draw_number(cairo_t* cr, int value, double centre_x, double top_y,
                 double unit, double alpha) {
    std::string s = std::to_string(value);
    double digit_w = 3 * unit, gap = unit;
    double total = s.size() * digit_w + (s.size() - 1) * gap;
    double x = centre_x - total / 2.0;
    set_rgb(cr, FG, alpha);
    for (char c : s) {
        for (int r = 0; r < 5; ++r) {
            const char* bits = digit_row(c - '0', r);
            for (int b = 0; b < 3; ++b)
                if (bits[b] == '#')
                    cairo_rectangle(cr, x + b * unit, top_y + r * unit,
                                    unit * 0.92, unit * 0.92);
        }
        x += digit_w + gap;
    }
    cairo_fill(cr);
}

// ------------------------------------------------------------------- text

void text_at(cairo_t* cr, double cx, double y, const char* s, double size,
             Rgb c, double alpha = 1.0, bool bold = false) {
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, s, &ext);
    set_rgb(cr, c, alpha);
    cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing, y);
    cairo_show_text(cr, s);
}

// ------------------------------------------------------------------ render

// Everything below works in field units; this scales them to pixels.
void render(cairo_t* cr, App& app, int W, int H) {
    const Match& m = app.m;
    double scale = H / FIELD_H;

    set_rgb(cr, BG);
    cairo_paint(cr);

    cairo_save(cr);
    cairo_scale(cr, scale, scale);
    double fw = W / scale, fh = FIELD_H;

    // Net.
    double dash_h = fh / (NET_DASHES * 2.0 - 1.0);
    set_rgb(cr, NET);
    for (int i = 0; i < static_cast<int>(NET_DASHES); ++i)
        cairo_rectangle(cr, fw / 2.0 - 0.09, i * dash_h * 2.0, 0.18, dash_h);
    cairo_fill(cr);

    // Score, sitting behind the play like the numbers on a cabinet.
    double top = fh * 0.06;
    draw_number(cr, m.score_left(), fw / 2.0 - fw / 6.0, top, DIGIT_UNIT, 0.22);
    draw_number(cr, m.score_right(), fw / 2.0 + fw / 6.0, top, DIGIT_UNIT, 0.22);

    // Ball trail: older samples fade out, so a fast ball smears the way it
    // does on a phosphor screen.
    const auto& trail = m.trail();
    for (size_t i = trail.size(); i-- > 0;) {
        double age = static_cast<double>(i) / std::max<size_t>(trail.size(), 1);
        double a = 0.30 * (1.0 - age) * (1.0 - age);
        double r = BALL_RX * (1.0 - 0.45 * age);
        set_rgb(cr, FG, a);
        cairo_arc(cr, trail[i].x, trail[i].y, r, 0, 2 * M_PI);
        cairo_fill(cr);
    }

    // Ball, with a mark that turns at the spin rate: topspin and backspin are
    // things you can watch happen, not just numbers in the physics.
    const Ball& b = m.ball();
    set_rgb(cr, FG);
    cairo_arc(cr, b.x, b.y, BALL_RX, 0, 2 * M_PI);
    cairo_fill(cr);
    if (std::fabs(b.spin) > 0.4) {
        cairo_save(cr);
        cairo_translate(cr, b.x, b.y);
        cairo_rotate(cr, b.angle);
        set_rgb(cr, BG, 0.55);
        cairo_rectangle(cr, -BALL_RX * 0.75, -BALL_RX * 0.16,
                        BALL_RX * 1.5, BALL_RX * 0.32);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    // Paddles.
    const Paddle* side[2] = {&m.left(), &m.right()};
    for (int i = 0; i < 2; ++i) {
        set_rgb(cr, FG);
        cairo_rectangle(cr, side[i]->x - side[i]->half_w,
                        side[i]->y - side[i]->half,
                        side[i]->half_w * 2.0, side[i]->half * 2.0);
        cairo_fill(cr);
    }
    cairo_restore(cr);

    // ---- overlays, in pixels ----
    double unit = H / 34.0;   // a text size that tracks the window
    State st = m.state();
    Mode md = m.mode();

    if (st == State::Menu) {
        double bw = std::min(W * 0.72, unit * 26.0), bh = unit * 12.0;
        double bx = (W - bw) / 2.0, by = (H - bh) / 2.0;
        set_rgb(cr, BG, 0.86);
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_fill(cr);
        set_rgb(cr, ACCENT, 0.8);
        cairo_set_line_width(cr, 2.0);
        cairo_rectangle(cr, bx + 1, by + 1, bw - 2, bh - 2);
        cairo_stroke(cr);

        text_at(cr, W / 2.0, by + unit * 3.0, "P O N G", unit * 2.2, FG, 1.0, true);
        text_at(cr, W / 2.0, by + unit * 5.6, "1  one player      2  two players",
                unit * 0.95, DIM);
        char diff[64];
        std::snprintf(diff, sizeof diff, "\xe2\x97\x80   %s   \xe2\x96\xb6",
                      DIFFICULTY_NAME[m.difficulty()]);
        text_at(cr, W / 2.0, by + unit * 7.6, diff, unit * 1.2, ACCENT);
        text_at(cr, W / 2.0, by + unit * 9.6,
                "arrow keys or the mouse to play", unit * 0.85, DIM);
        text_at(cr, W / 2.0, by + unit * 11.0, "q quits", unit * 0.85, DIM);
    } else if (st == State::Paused) {
        set_rgb(cr, BG, 0.7);
        cairo_paint(cr);
        text_at(cr, W / 2.0, H / 2.0, "PAUSED", unit * 2.0, FG, 1.0, true);
        text_at(cr, W / 2.0, H / 2.0 + unit * 2.2, "p resume    m menu    q quit",
                unit * 0.9, DIM);
    } else if (st == State::Over) {
        set_rgb(cr, BG, 0.78);
        cairo_paint(cr);
        const char* who = m.winner() == 1
            ? (md == Mode::TwoPlayer ? "LEFT PLAYER WINS" : "YOU WIN")
            : (md == Mode::TwoPlayer ? "RIGHT PLAYER WINS" : "COMPUTER WINS");
        char line[64];
        std::snprintf(line, sizeof line, "%d  -  %d", m.score_left(), m.score_right());
        text_at(cr, W / 2.0, H / 2.0 - unit * 1.2, who, unit * 1.8, FG, 1.0, true);
        text_at(cr, W / 2.0, H / 2.0 + unit * 1.4, line, unit * 1.4, ACCENT);
        text_at(cr, W / 2.0, H / 2.0 + unit * 3.4, "r rematch    m menu    q quit",
                unit * 0.9, DIM);
    }

    if (st == State::Play || st == State::Paused) {
        const char* l = md == Mode::TwoPlayer ? "P1  w/s" : "YOU";
        const char* r = md == Mode::TwoPlayer ? "P2  \xe2\x86\x91/\xe2\x86\x93" : "CPU";
        text_at(cr, W * 0.25, H - unit * 0.9, l, unit * 0.8, DIM);
        text_at(cr, W * 0.75, H - unit * 0.9, r, unit * 0.8, DIM);
        if (md != Mode::TwoPlayer)
            text_at(cr, W / 2.0, H - unit * 0.9, DIFFICULTY_NAME[m.difficulty()],
                    unit * 0.8, DIM, 0.7);
        if (app.t < app.knob_hint_until)
            text_at(cr, W / 2.0, unit * 2.0,
                    app.knob ? "mouse control" : "keyboard control",
                    unit * 0.85, ACCENT, 0.8);
    }
}

// ------------------------------------------------------------------- input

void apply_input(App& app) {
    bool two = app.m.mode() == Mode::TwoPlayer;
    for (int side = 0; side < 2; ++side) {
        int dir = (app.down_held[side] ? 1 : 0) - (app.up_held[side] ? 1 : 0);
        if (side == 0 && app.knob && dir == 0)
            app.m.set_intent(0, {Intent::Kind::Position, app.knob_y});
        else
            app.m.set_intent(side, {Intent::Kind::Speed, static_cast<double>(dir)});
    }
    if (!two) app.m.set_intent(1, {});   // the computer owns the right paddle
}

gboolean on_key(GtkEventControllerKey*, guint keyval, guint, GdkModifierType,
                gpointer data) {
    App& app = *static_cast<App*>(data);
    Match& m = app.m;

    switch (keyval) {
    case GDK_KEY_q: case GDK_KEY_Q: case GDK_KEY_Escape:
        gtk_window_close(app.win);
        return TRUE;
    case GDK_KEY_f: case GDK_KEY_F:
        if (gtk_window_is_fullscreen(app.win)) gtk_window_unfullscreen(app.win);
        else gtk_window_fullscreen(app.win);
        return TRUE;
    default: break;
    }

    if (m.state() == State::Menu) {
        switch (keyval) {
        case GDK_KEY_1: case GDK_KEY_KP_1: m.setup(Mode::OnePlayer); break;
        case GDK_KEY_2: case GDK_KEY_KP_2: m.setup(Mode::TwoPlayer); break;
        case GDK_KEY_Left:  m.step_difficulty(-1); break;
        case GDK_KEY_Right: m.step_difficulty(+1); break;
        case GDK_KEY_Return: case GDK_KEY_space: m.setup(Mode::OnePlayer); break;
        default: break;
        }
        return TRUE;
    }

    if (m.state() == State::Over) {
        if (keyval == GDK_KEY_r || keyval == GDK_KEY_R) m.rematch();
        else if (keyval == GDK_KEY_m || keyval == GDK_KEY_M) m.setup(Mode::Demo);
        return TRUE;
    }

    bool two = m.mode() == Mode::TwoPlayer;
    switch (keyval) {
    case GDK_KEY_p: case GDK_KEY_P: m.toggle_pause(); return TRUE;
    case GDK_KEY_m: case GDK_KEY_M: m.setup(Mode::Demo); return TRUE;
    // Arrows drive the right paddle only when a second player owns it.
    case GDK_KEY_Up:   app.up_held[two ? 1 : 0] = true; break;
    case GDK_KEY_Down: app.down_held[two ? 1 : 0] = true; break;
    case GDK_KEY_w: case GDK_KEY_W: app.up_held[0] = true; break;
    case GDK_KEY_s: case GDK_KEY_S: app.down_held[0] = true; break;
    default: return FALSE;
    }

    if (app.knob) {   // a key wins the paddle back from the mouse
        app.knob = false;
        app.knob_hint_until = app.t + 1.5;
    }
    return TRUE;
}

void on_key_release(GtkEventControllerKey*, guint keyval, guint,
                    GdkModifierType, gpointer data) {
    App& app = *static_cast<App*>(data);
    bool two = app.m.mode() == Mode::TwoPlayer;
    switch (keyval) {
    case GDK_KEY_Up:   app.up_held[two ? 1 : 0] = false; break;
    case GDK_KEY_Down: app.down_held[two ? 1 : 0] = false; break;
    case GDK_KEY_w: case GDK_KEY_W: app.up_held[0] = false; break;
    case GDK_KEY_s: case GDK_KEY_S: app.down_held[0] = false; break;
    default: break;
    }
}

void on_motion(GtkEventControllerMotion*, double, double y, gpointer data) {
    App& app = *static_cast<App*>(data);
    if (app.px_h <= 0) return;
    double units = y / (static_cast<double>(app.px_h) / FIELD_H);
    if (!app.knob && std::fabs(units - app.knob_y) < 0.4) return;  // ignore drift
    if (!app.knob) {
        app.knob = true;
        app.knob_hint_until = app.t + 1.5;
    }
    app.knob_y = units;
}

// -------------------------------------------------------------------- frame

gboolean on_tick(GtkWidget* widget, GdkFrameClock* clock, gpointer data) {
    App& app = *static_cast<App*>(data);
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double dt = app.last_frame ? (now - app.last_frame) / 1e6 : 1.0 / 60.0;
    app.last_frame = now;
    dt = std::min(dt, 0.05);
    app.t += dt;

    apply_input(app);
    app.m.update(dt);
    gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

void on_draw(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer data) {
    App& app = *static_cast<App*>(data);
    if (w != app.px_w || h != app.px_h) {
        app.px_w = w; app.px_h = h;
        app.m.resize(layout_for(app));
    }
    render(cr, app, w, h);
}

void on_activate(GtkApplication* gtk_app, gpointer data) {
    App& app = *static_cast<App*>(data);

    GtkWidget* win = gtk_application_window_new(gtk_app);
    app.win = GTK_WINDOW(win);
    gtk_window_set_title(app.win, "Pong");
    gtk_window_set_default_size(app.win, 960, 600);

    app.area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app.area), on_draw, &app, nullptr);
    gtk_window_set_child(app.win, app.area);

    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), &app);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_release), &app);
    gtk_widget_add_controller(win, keys);

    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), &app);
    gtk_widget_add_controller(win, motion);

    gtk_widget_add_tick_callback(app.area, on_tick, &app, nullptr);
    gtk_window_present(app.win);
}

// A window is awkward to check from a script, so the same renderer can put a
// frame of a match straight into a PNG.
int shoot(const char* path, double seconds, int mode, int w, int h) {
    App app;
    app.px_w = w; app.px_h = h;
    app.m.configure(layout_for(app));
    if (mode == 1) app.m.setup(Mode::OnePlayer);
    else if (mode == 2) app.m.setup(Mode::TwoPlayer);

    const double dt = 1.0 / 120.0;
    for (double t = 0; t < seconds; t += dt) {
        app.t += dt;
        // Give the left paddle to a tracker so a screenshot shows a rally.
        if (mode != 0) {
            const Paddle& p = app.m.left();
            double d = app.m.ball().y - p.y;
            app.m.set_intent(0, {Intent::Kind::Speed, clampv(d, -1.0, 1.0)});
        }
        app.m.update(dt);
    }

    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(surf);
    render(cr, app, w, h);
    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surf, path);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        std::fprintf(stderr, "could not write %s: %s\n", path,
                     cairo_status_to_string(st));
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::strcmp(argv[1], "--shot") == 0) {
        double secs = argc > 3 ? std::atof(argv[3]) : 8.0;
        int mode = argc > 4 ? std::atoi(argv[4]) : 1;
        int w = argc > 5 ? std::atoi(argv[5]) : 960;
        int h = argc > 6 ? std::atoi(argv[6]) : 600;
        return shoot(argv[2], secs, mode, w, h);
    }

    App app;
    GtkApplication* gtk_app = gtk_application_new("dev.termpong.gtk",
                                                  G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), &app);
    int status = g_application_run(G_APPLICATION(gtk_app), 1, argv);
    g_object_unref(gtk_app);
    return status;
}
