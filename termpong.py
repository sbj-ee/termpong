#!/usr/bin/env python3
"""Classic terminal pong.

Left paddle: human (up/down arrows).  Right paddle: computer.
First to WIN_SCORE points wins.  'q' quits, 'p' or space pauses.
"""

import curses
import random
import time

WIN_SCORE = 11
TARGET_FPS = 60.0
FRAME = 1.0 / TARGET_FPS

PADDLE_HEIGHT = 4
PADDLE_MARGIN = 2  # columns from the field edge to the paddle

# Terminal cells are roughly twice as tall as they are wide, so the ball needs
# about twice the horizontal speed to look like it is moving diagonally.
BALL_SPEED_X = 34.0  # columns per second
BALL_SPEED_Y = 15.0  # rows per second
BALL_SPEEDUP = 1.04  # applied on every paddle hit
BALL_SPEED_X_MAX = 70.0

PLAYER_SPEED = 34.0  # rows per second
KEY_HOLD = 0.12  # seconds a keypress keeps the paddle moving on its own
# The computer must be slower than the ball's steepest vertical angle
# (BALL_SPEED_Y * 1.4), or a well-aimed shot could never get past it.
AI_SPEED = 16.0  # rows per second
AI_REACTION_ZONE = 0.35  # ball must be past this fraction of the field to react
# Aim error is measured in half-paddle-heights, so a value above 1.0 lets the
# computer misjudge a shot badly enough to actually miss it.
AI_AIM_ERROR = 1.35

MIN_WIDTH = 48
MIN_HEIGHT = 16


class Paddle:
    def __init__(self, x, y, height):
        self.x = x
        self.y = float(y)  # top row of the paddle
        self.height = height

    @property
    def center(self):
        return self.y + self.height / 2.0

    def move(self, dy, top, bottom):
        self.y = max(top, min(bottom - self.height, self.y + dy))

    def contains(self, row):
        return self.y <= row < self.y + self.height


class Ball:
    def __init__(self, x, y):
        self.reset(x, y, direction=random.choice((-1, 1)))

    def reset(self, x, y, direction):
        self.x = float(x)
        self.y = float(y)
        self.vx = BALL_SPEED_X * direction
        self.vy = BALL_SPEED_Y * random.uniform(-0.6, 0.6)


class Pong:
    def __init__(self, stdscr):
        self.stdscr = stdscr
        self.height, self.width = stdscr.getmaxyx()
        # Playfield interior: rows 2..height-2 (row 0 is the scoreboard,
        # row 1 and the last row are the walls), columns 1..width-2.
        self.top = 2.0
        self.bottom = float(self.height - 1)
        self.left = 1.0
        self.right = float(self.width - 1)

        mid_y = (self.top + self.bottom) / 2.0 - PADDLE_HEIGHT / 2.0
        self.player = Paddle(PADDLE_MARGIN, mid_y, PADDLE_HEIGHT)
        self.ai = Paddle(self.width - 1 - PADDLE_MARGIN, mid_y, PADDLE_HEIGHT)
        self.ball = Ball(self.center_x, self.center_y)

        self.player_score = 0
        self.ai_score = 0
        self.paused = False
        self.winner = None
        self.serve_at = time.perf_counter() + 1.0
        self.held = 0
        self.held_until = 0.0
        self.ai_aim = 0.0

    @property
    def center_x(self):
        return (self.left + self.right) / 2.0

    @property
    def center_y(self):
        return (self.top + self.bottom) / 2.0

    # ------------------------------------------------------------------ input

    def handle_input(self, dt, now):
        pressed = 0
        while True:
            key = self.stdscr.getch()
            if key == -1:
                break
            if key in (ord("q"), ord("Q")):
                return False
            if key in (ord("p"), ord("P"), ord(" ")):
                if self.winner is None:
                    self.paused = not self.paused
            if key in (ord("r"), ord("R")) and self.winner is not None:
                self.restart()
            if key == curses.KEY_UP or key in (ord("k"), ord("w")):
                pressed = -1
            elif key == curses.KEY_DOWN or key in (ord("j"), ord("s")):
                pressed = 1

        # The terminal delivers key repeats as discrete events, so a held key
        # keeps the paddle moving until the repeats stop arriving.
        if pressed:
            self.held = pressed
            self.held_until = now + KEY_HOLD
        elif now > self.held_until:
            self.held = 0

        if self.held and not self.paused and self.winner is None:
            self.player.move(self.held * PLAYER_SPEED * dt, self.top, self.bottom)
        return True

    def restart(self):
        self.player_score = 0
        self.ai_score = 0
        self.winner = None
        self.paused = False
        mid_y = self.center_y - PADDLE_HEIGHT / 2.0
        self.player.y = mid_y
        self.ai.y = mid_y
        self.ball.reset(self.center_x, self.center_y, random.choice((-1, 1)))
        self.serve_at = time.perf_counter() + 1.0

    # ------------------------------------------------------------------ logic

    def update_ai(self, dt):
        ball = self.ball
        span = self.right - self.left
        # The computer only chases the ball once it is heading its way and has
        # crossed the reaction zone; otherwise it drifts back toward center.
        if ball.vx > 0 and (ball.x - self.left) / span > AI_REACTION_ZONE:
            target = ball.y + self.ai_aim
        else:
            target = self.center_y

        delta = target - self.ai.center
        step = AI_SPEED * dt
        if abs(delta) < step:
            step = abs(delta)
        if delta:
            self.ai.move(step * (1 if delta > 0 else -1), self.top, self.bottom)

    def update_ball(self, dt):
        ball = self.ball
        ball.x += ball.vx * dt
        ball.y += ball.vy * dt

        # Top and bottom walls.
        if ball.y < self.top:
            ball.y = self.top + (self.top - ball.y)
            ball.vy = -ball.vy
        elif ball.y > self.bottom - 1:
            limit = self.bottom - 1
            ball.y = limit - (ball.y - limit)
            ball.vy = -ball.vy

        # Paddles.
        if ball.vx < 0 and ball.x <= self.player.x + 1:
            if self.player.contains(round(ball.y)):
                ball.x = self.player.x + 1
                self.bounce(self.player, 1)
        elif ball.vx > 0 and ball.x >= self.ai.x - 1:
            if self.ai.contains(round(ball.y)):
                ball.x = self.ai.x - 1
                self.bounce(self.ai, -1)

        # Scoring.
        if ball.x < self.left:
            self.score(ai_scored=True)
        elif ball.x > self.right - 1:
            self.score(ai_scored=False)

    def aim_ai(self):
        """Re-aim the computer slightly off-center so its returns vary."""
        spread = AI_AIM_ERROR * PADDLE_HEIGHT / 2.0
        self.ai_aim = random.uniform(-spread, spread)

    def bounce(self, paddle, direction):
        ball = self.ball
        # Where the ball struck the paddle, from -1 (top edge) to 1 (bottom).
        offset = (ball.y - paddle.center) / (paddle.height / 2.0)
        offset = max(-1.0, min(1.0, offset))
        speed = min(abs(ball.vx) * BALL_SPEEDUP, BALL_SPEED_X_MAX)
        ball.vx = speed * direction
        ball.vy = BALL_SPEED_Y * offset * 1.4
        if direction > 0:
            self.aim_ai()

    def score(self, ai_scored):
        if ai_scored:
            self.ai_score += 1
        else:
            self.player_score += 1

        if self.player_score >= WIN_SCORE:
            self.winner = "player"
        elif self.ai_score >= WIN_SCORE:
            self.winner = "computer"

        # Serve toward whoever just lost the point.
        self.ball.reset(self.center_x, self.center_y, -1 if ai_scored else 1)
        self.aim_ai()
        self.serve_at = time.perf_counter() + (0.0 if self.winner else 0.8)

    def update(self, dt, now):
        if self.paused or self.winner is not None:
            return
        self.update_ai(dt)
        if now >= self.serve_at:
            self.update_ball(dt)

    # ------------------------------------------------------------------- draw

    def draw(self):
        s = self.stdscr
        s.erase()
        self.draw_field()
        self.draw_scoreboard()

        for paddle in (self.player, self.ai):
            top = int(round(paddle.y))
            for i in range(paddle.height):
                row = top + i
                if self.top <= row < self.bottom:
                    self.put(row, paddle.x, "█", curses.color_pair(2))

        self.put(int(round(self.ball.y)), int(round(self.ball.x)), "O",
                 curses.color_pair(3) | curses.A_BOLD)

        if self.winner is not None:
            who = "YOU WIN!" if self.winner == "player" else "COMPUTER WINS"
            self.banner([who, "r - play again    q - quit"])
        elif self.paused:
            self.banner(["PAUSED", "p - resume    q - quit"])

        s.noutrefresh()
        curses.doupdate()

    def draw_field(self):
        wall = "─" * (self.width - 1)
        for row in (1, self.height - 1):
            self.put(row, 0, wall, curses.color_pair(1))
            # addstr cannot write the very last cell of a line without the
            # cursor running off the screen, so insert that character instead.
            try:
                self.stdscr.insstr(row, self.width - 1, "─",
                                   curses.color_pair(1))
            except curses.error:
                pass
        mid = int(self.center_x)
        for row in range(int(self.top), int(self.bottom)):
            if row % 2 == 0:
                self.put(row, mid, "│", curses.color_pair(1))

    def draw_scoreboard(self):
        left = f" YOU {self.player_score}"
        right = f"{self.ai_score} CPU "
        self.put(0, 0, left.ljust(self.width - 1), curses.color_pair(4))
        self.put(0, self.width - 1 - len(right), right, curses.color_pair(4))
        hint = "↑/↓ move   p pause   q quit"
        start = max(len(left) + 2, (self.width - len(hint)) // 2)
        if start + len(hint) < self.width - len(right) - 1:
            self.put(0, start, hint, curses.color_pair(4) | curses.A_DIM)

    def banner(self, lines):
        width = max(len(line) for line in lines) + 4
        top = int(self.center_y) - len(lines) // 2 - 1
        left = int(self.center_x - width / 2)
        for i, line in enumerate([""] + lines + [""]):
            self.put(top + i, left, line.center(width),
                     curses.color_pair(5) | curses.A_BOLD)

    def put(self, y, x, text, attr=0):
        y, x = int(y), int(x)
        if 0 <= y < self.height and 0 <= x < self.width:
            try:
                self.stdscr.addstr(y, x, text[: self.width - x], attr)
            except curses.error:
                pass  # bottom-right cell always raises

    # ------------------------------------------------------------------- loop

    def run(self):
        previous = time.perf_counter()
        while True:
            now = time.perf_counter()
            dt = min(now - previous, 0.05)
            previous = now

            if not self.handle_input(dt, now):
                return
            self.update(dt, now)
            self.draw()

            sleep = FRAME - (time.perf_counter() - now)
            if sleep > 0:
                time.sleep(sleep)


def init_colors():
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_BLUE, -1)     # walls / net
    curses.init_pair(2, curses.COLOR_CYAN, -1)     # paddles
    curses.init_pair(3, curses.COLOR_YELLOW, -1)   # ball
    curses.init_pair(4, curses.COLOR_WHITE, -1)    # scoreboard
    curses.init_pair(5, curses.COLOR_MAGENTA, -1)  # banners


def main(stdscr):
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.keypad(True)
    try:
        init_colors()
    except curses.error:
        pass

    height, width = stdscr.getmaxyx()
    if width < MIN_WIDTH or height < MIN_HEIGHT:
        stdscr.nodelay(False)
        stdscr.addstr(0, 0, f"Terminal too small: need at least "
                            f"{MIN_WIDTH}x{MIN_HEIGHT}, got {width}x{height}.")
        stdscr.addstr(1, 0, "Resize and try again. Press any key.")
        stdscr.getch()
        return

    Pong(stdscr).run()


if __name__ == "__main__":
    try:
        curses.wrapper(main)
    except KeyboardInterrupt:
        pass
