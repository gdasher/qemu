#!/usr/bin/env python3
"""A reference model for the PIC16 simulation bridge.

Speaks the protocol in target/pic16/SIM-BRIDGE.md and does the least a model
can usefully do: counts edges on the lines it watches, and holds the lines it
drives at whatever levels it was told to. Products supply their own model; this
one exists so the bridge itself can be tested without one.

Usage:
  bridge_model.py --listen <socket> [--watch LINE[=MODE] ...]
                                    [--drive LINE=LEVEL ...]
                                    [--status FILE]

--status writes the edge counts as JSON when the peer goes away, which is how a
test finds out what the guest actually did.
"""

import argparse
import json
import os
import socket
import sys


class Model:
    def __init__(self, conn, watch, drive, status_path):
        self.rx = conn.makefile('r')
        self.tx = conn.makefile('w')
        self.watch = watch
        self.drive = drive
        self.status_path = status_path
        self.edges = {name: 0 for name in watch}
        self.lines = []

    def send(self, text):
        self.tx.write(text + '\n')
        self.tx.flush()

    def handshake(self):
        for line in self.rx:
            words = line.split()
            if not words:
                continue
            if words[0] == 'HELLO':
                continue
            if words[0] == 'LINES':
                self.lines = words[1:]
                break
            raise RuntimeError(f'unexpected {words[0]!r} before LINES')
        else:
            raise RuntimeError('peer closed before announcing its lines')

        for name in list(self.watch) + list(self.drive):
            if name not in self.lines:
                raise RuntimeError(f'{name!r} is not a line on this board; '
                                   f'available: {" ".join(self.lines)}')

        self.send('HELLO bridge_model 1')
        if self.watch:
            self.send('WATCH ' + ' '.join(
                f'{n}={m}' if m else n for n, m in self.watch.items()))
        if self.drive:
            self.send('DRIVE ' + ' '.join(self.drive))
        self.send('READY')

    def reply(self, first):
        # The levels never change once set, so they only need sending once.
        if first and self.drive:
            self.send('SET ' + ' '.join(
                f'{n}={v}' for n, v in self.drive.items()))
        self.send('ACK')

    def run(self):
        self.handshake()
        first = True
        for line in self.rx:
            words = line.split()
            if not words:
                continue
            verb = words[0]
            if verb == 'BYE':
                break
            if verb in ('EDGE', 'STATE', 'PULSE'):
                for token in words[2:]:
                    name = token.split('=')[0]
                    if verb != 'STATE' and name in self.edges:
                        self.edges[name] += 1
                self.reply(first)
                first = False
            elif verb in ('RESET', 'TICK', 'DIR'):
                self.reply(first)
                first = False
            else:
                raise RuntimeError(f'unknown event {verb!r}')
        self.write_status()

    def write_status(self):
        if not self.status_path:
            return
        with open(self.status_path, 'w') as f:
            json.dump({'edges': self.edges}, f)


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('--listen', required=True)
    parser.add_argument('--watch', action='append', default=[])
    parser.add_argument('--drive', action='append', default=[])
    parser.add_argument('--status')
    args = parser.parse_args(argv[1:])

    watch = {}
    for spec in args.watch:
        name, _, mode = spec.partition('=')
        watch[name] = mode
    drive = {}
    for spec in args.drive:
        name, _, level = spec.partition('=')
        drive[name] = level or '0'

    if os.path.exists(args.listen):
        os.unlink(args.listen)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(args.listen)
    server.listen(1)
    conn, _ = server.accept()
    server.close()

    try:
        Model(conn, watch, drive, args.status).run()
    except Exception as exc:            # noqa: BLE001 - report and exit
        print(f'bridge_model: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
