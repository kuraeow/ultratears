"""A progress line for the build's console: percent, amount, speed, time so far and time left, rewritten in place.

Bar: work of a known size (bytes, files, shaders). Ticker: work of unknown length, its time so far once a second.
When the output is not a console (a log file, a pipe) a Bar prints a line every 10 % and a Ticker prints nothing.
With ULTRS_JSON=1 (setup.py --json, for the launcher) a Bar prints events instead: {"ev": "progress", "label", "done",
"total", "eta"} at most twice a second, one line each; setup.py says which step they belong to.
"""
import json
import os
import sys
import threading
import time

JSON = os.environ.get('ULTRS_JSON') == '1'


def emit(ev, **fields):
    """One event line for the launcher (setup.py --json)."""
    print(json.dumps({'ev': ev, **fields}, ensure_ascii=False), flush=True)


def clock(seconds):
    s = max(0, int(seconds))
    return f'{s // 3600}:{s // 60 % 60:02}:{s % 60:02}' if s >= 3600 else f'{s // 60}:{s % 60:02}'


def _tty():
    try:
        return sys.stdout.isatty()
    except (AttributeError, ValueError):
        return False


class Bar:
    def __init__(self, label, total, unit='', scale=1, rate=False, live=False):
        """unit / scale: how amounts are shown (bytes as MiB: unit 'MiB', scale 2**20); rate: show the speed;
        live: drawn again every second while no update comes (work that reports rarely, or a stalled download)."""
        self.label, self.total, self.unit, self.scale, self.rate = label, total, unit, scale, rate
        self.t0 = time.monotonic()
        self.last, self.done, self.width, self.tens, self.printed = 0.0, 0, 0, -1, None
        self.samples = [(self.t0, 0)]   # (time, done) of the last seconds: the speed and time left are recent ones
        self.tty = _tty()
        self.lock = threading.RLock()
        self.stop = threading.Event()
        if live and self.tty:
            threading.Thread(target=self.redraw, daemon=True).start()
        self.update(0, force=True)

    def redraw(self):
        while not self.stop.wait(1):
            if time.monotonic() - self.last >= 1:
                self.update(self.done, force=True)

    def line(self):
        now = time.monotonic()
        el = now - self.t0
        while len(self.samples) > 2 and now - self.samples[1][0] >= 5:
            self.samples.pop(0)
        t1, d1 = self.samples[0]
        speed = (self.done - d1) / (now - t1) if now - t1 >= 1 else None
        parts = []
        if self.total:
            parts.append(f'{100 * self.done / self.total:3.0f}%')
            amount = (f'{self.done / self.scale:.1f} / {self.total / self.scale:.1f}' if self.scale != 1
                      else f'{self.done} / {self.total}')
            parts.append(f'{amount} {self.unit}'.rstrip())
        if self.rate and speed is not None:
            parts.append(f'{speed / self.scale:.1f} {self.unit}/s')
        parts.append(clock(el))
        if self.total and 0 < self.done < self.total and el >= 3 and speed:
            parts.append(f'~{clock((self.total - self.done) / speed)} left')
        return f'  {self.label}: ' + ', '.join(parts)

    def note(self, text):
        """A line of its own (a source that does not answer...), the progress line drawn again below it."""
        if JSON:
            print(f'  {text}', flush=True)
            return
        with self.lock:
            if self.tty:
                sys.stdout.write('\r' + ' ' * self.width + '\r')
                self.width = 0
            print(f'  {text}', flush=True)
            self.last = 0

    def eta(self):
        """Seconds left from the recent speed, or None."""
        now = time.monotonic()
        t1, d1 = self.samples[0]
        if not self.total or now - t1 < 1 or self.done <= d1 or now - self.t0 < 3:
            return None
        return (self.total - self.done) / ((self.done - d1) / (now - t1))

    def update(self, done, total=None, force=False):
        with self.lock:
            if total is not None:
                self.total = total
            self.done = done
            now = time.monotonic()
            if now - self.samples[-1][0] >= 0.5:
                self.samples.append((now, done))
            if JSON:
                if force or now - self.last >= 0.5:
                    self.last = now
                    eta = self.eta()
                    emit('progress', label=self.label, done=self.done, total=self.total, eta=round(eta) if eta else None)
                return
            if not force and now - self.last < 0.3:
                return
            self.last = now
            text = self.line()
            if self.tty:
                sys.stdout.write('\r' + text.ljust(self.width))
                sys.stdout.flush()
                self.width = len(text)
            else:
                tens = int(10 * self.done / self.total) if self.total else 0
                if (force or tens > self.tens) and (self.tens, self.done) != self.printed:
                    self.tens = tens
                    self.printed = (tens, self.done)
                    print(text, flush=True)

    def add(self, n):
        with self.lock:
            self.update(self.done + n)

    def finish(self):
        self.stop.set()
        with self.lock:
            self.update(self.done, force=True)
            if self.tty and not JSON:
                sys.stdout.write('\n')
                sys.stdout.flush()


class Ticker:
    """with Ticker('building the shader translator'): ... shows the time it has taken so far."""
    def __init__(self, label):
        self.label, self.tty = label, _tty()

    def __enter__(self):
        self.t0, self.stop = time.monotonic(), threading.Event()
        if self.tty:
            self.thread = threading.Thread(target=self.run, daemon=True)
            self.thread.start()
        return self

    def run(self):
        while not self.stop.wait(1):
            sys.stdout.write(f'\r  {self.label}: {clock(time.monotonic() - self.t0)}')
            sys.stdout.flush()

    def __exit__(self, *exc):
        self.stop.set()
        if self.tty:
            self.thread.join()
            sys.stdout.write(f'\r  {self.label}: done in {clock(time.monotonic() - self.t0)}\n')
            sys.stdout.flush()
        return False
