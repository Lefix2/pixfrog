#!/usr/bin/env python3
"""Assemble the web UI demo for GitHub Pages.

    tools/demo/build.py site/demo

Copies the SPA as the firmware embeds it (components/web_config/src/
web_ui.html) and loads, before its own script, the demo box: snapshot.json
as window.PF_SNAPSHOT, then mock.js. The demo therefore follows every UI
change; only the snapshot needs refreshing when the API grows
(tools/demo/snapshot.py, checked by tests/web).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def main(out):
    os.makedirs(out, exist_ok=True)
    html = open(os.path.join(REPO, "components", "web_config", "src", "web_ui.html")).read()
    snap = json.load(open(os.path.join(HERE, "snapshot.json")))
    with open(os.path.join(out, "demo-data.js"), "w") as f:
        f.write("window.PF_SNAPSHOT = " + json.dumps(snap, separators=(",", ":")) + ";\n")
    with open(os.path.join(HERE, "mock.js")) as src, open(os.path.join(out, "mock.js"), "w") as dst:
        dst.write(src.read())
    tags = ('<script src="demo-data.js"></script>\n<script src="mock.js"></script>\n'
            '<title>pixfrog — démo de l\'interface</title>\n')
    head = html.index("<head>") + len("<head>")
    html = html[:head] + "\n" + tags + html[head:]
    with open(os.path.join(out, "index.html"), "w") as f:
        f.write(html)
    print("demo written to", out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "demo"))
