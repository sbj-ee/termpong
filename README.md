# termpong

Classic pong in the terminal. You play the left paddle; the computer plays the
right. First to 11 wins.

## Running

Requires Python 3 — no third-party packages, just the standard library `curses`.

```sh
python3 termpong.py
```

Your terminal needs to be at least 48x16.

## Controls

| Key | Action |
| --- | --- |
| `↑` / `↓` | Move your paddle (`w`/`s` and `k`/`j` also work) |
| `p` or space | Pause / resume |
| `r` | Play again, once the game is over |
| `q` | Quit |

## How it plays

The ball leaves the paddle at an angle set by where it struck: hit it with the
middle of the paddle for a flat return, or catch it near an edge to send it away
steeply. It also speeds up 4% with every paddle hit, up to a cap, so long
rallies get progressively harder.

The computer is deliberately imperfect. It moves slower than the ball's steepest
vertical angle, waits until the ball has crossed a third of the field before
committing to it, and aims a little off-center on each shot — so a well-angled
shot can get past it, but a lazy one will not. Tuning lives in the constants at
the top of `termpong.py`; `AI_SPEED` and `AI_AIM_ERROR` are the two dials that
matter most for difficulty.

## License

MIT — see [LICENSE](LICENSE).
