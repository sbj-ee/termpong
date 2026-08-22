# termpong

Classic pong in the terminal, in C++. You play the left paddle; the computer
plays the right, or a friend does. First to 11, win by two.

## Building

Needs a C++20 compiler and ncurses with wide-character support
(`libncursesw5-dev` on Debian/Ubuntu, `ncurses` on macOS via Homebrew).

```sh
make          # builds ./termpong
make test     # headless physics and gameplay tests
make install  # optional, PREFIX=/usr/local by default
```

Run it with `./termpong`. Your terminal needs to be at least 48x16; it can be
resized while playing.

## Controls

| Key | Action |
| --- | --- |
| `1` / `2` | One player, or two players at the same keyboard |
| `←` / `→` | Difficulty, in the menu |
| `↑` / `↓` | Move your paddle (`w`/`s` and `k`/`j` also work) |
| `w` / `s` | Left paddle in two-player mode (`↑`/`↓` drives the right) |
| `p` or space | Pause / resume |
| `m` | Back to the menu |
| `r` | Rematch, once the game is over |
| `q` | Quit |

## How it plays

The ball is simulated in real units rather than character cells. A terminal
cell is about twice as tall as it is wide, so a column counts as half a unit —
which means a 45-degree shot actually looks like 45 degrees, and speeds are
honest in both axes.

Contacts are resolved as impulses. The paddle face is slightly convex, so where
you meet the ball tilts the contact normal and aims the return: flat off the
middle, steep off an edge. Friction at the contact cancels part of the sideways
slip, which spins the ball; spin then curves the flight through a Magnus force
and survives bounces off the walls. A paddle that is *moving* when it connects
drags the ball with it, so timing a return adds pace and spin that a stationary
paddle cannot. Every return leaves 6% faster than it arrived, up to a cap, so
rallies wind up until someone cannot reach the ball.

Speeds are expressed as time to cross the field rather than absolute numbers,
so the game plays at the same pace in an 80x24 window and a 220x60 one.

The ball and paddles are drawn at half-cell vertical resolution using block
glyphs, giving twice the vertical precision the character grid has, and the
ball is simulated at 480 Hz in fixed substeps under a 60 fps render — so motion
stays smooth and the physics does not change with the frame rate.

## The opponent

Difficulty is mostly misjudgement and hesitation, not raw speed. Every
opponent reads the intercept by running the same integrator the ball uses, so
it accounts for spin and wall bounces; the weak ones read it badly, late, and
partially, and they cap out well below your top paddle speed.

| | Top speed | Reads intercept | Aim error | Reaction |
| --- | --- | --- | --- | --- |
| Easy | 55% of yours | 45% | ±3.0 half-paddles | 300 ms |
| Normal | 70% | 80% | ±2.2 | 180 ms |
| Hard | 85% | 100% | ±1.6 | 80 ms |
| Insane | 100% | 100% | ±1.15 | none |

The aim error is triangular, so the opponent is usually close and occasionally
badly wrong — a uniform error just reads as flailing. It is resampled once per
shot, so a misjudged return is committed to rather than corrected mid-flight.

Tuning lives in the constants at the top of `termpong.cpp`; the `DIFFICULTY`
table is the dial that matters most.

## Tests

`make test` runs the simulation headlessly — no terminal, no timing — by
including the game with `main()` renamed and stepping it with a synthetic
clock. It covers the flight forces, wall and paddle contacts, spin transfer,
scoring, the difficulty ramp, two-player input and mid-game resizes.

## License

MIT — see [LICENSE](LICENSE).
