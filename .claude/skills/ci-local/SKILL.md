---
name: ci-local
description: Replay every CI job locally (format, host tests, oled+tft IDF builds) — mandatory green before any push
---

```bash
./tools/ci-local.sh
```

Ends with `ALL CI JOBS GREEN — safe to push`. On failure, fix and rerun; never push red. Details per job: /build, /host-tests, /format skills.
