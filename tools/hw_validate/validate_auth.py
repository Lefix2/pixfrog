#!/usr/bin/env python3
"""HTTP Basic auth: open without password, gated with one, UART recovery."""
import sys, time
from pixfrog_uart import Board, Checks, http, main_guard

POST_JSON = ["-X", "POST", "-H", "Content-Type: application/json", "-d", '{"refresh_hz":60}']


def run(board: Board):
    c = Checks("auth")
    board.cmd("global web_enabled 1")
    board.cmd("global web_password -")  # known starting state
    c.check("link up", board.wait_link())

    code, _ = http("/api/global", *POST_JSON)
    c.check("POST open when no password set", code == 200)
    code, body = http("/api/config")
    c.check("auth_enabled=false", '"auth_enabled":false' in body)

    code, _ = http("/api/global", "-X", "POST", "-H", "Content-Type: application/json",
                   "-d", '{"web_password":"test1234"}')
    c.check("set password via API", code == 200)
    c.check("web_auth=1 on console", board.get("global", "web_auth") == "1")

    code, _ = http("/api/global", *POST_JSON)
    c.check("POST without credentials → 401", code == 401)
    t0 = time.time()
    code, _ = http("/api/global", "-u", "x:wrongpass", *POST_JSON)
    c.check("wrong password → 401", code == 401)
    c.check("flat anti-bruteforce delay (>0.4s)", time.time() - t0 > 0.4)
    code, _ = http("/api/global", "-u", "x:test1234", *POST_JSON)
    c.check("correct password → 200", code == 200)
    code, _ = http("/api/config")
    c.check("GET stays open", code == 200)

    out = board.cmd("global web_password -")
    c.check("UART recovery clears password", board.kv(out, "web_auth") == "0")
    code, _ = http("/api/global", *POST_JSON)
    c.check("POST open again", code == 200)

    board.cmd("global web_enabled 0")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
