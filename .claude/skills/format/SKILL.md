---
name: format
description: clang-format all C/C++ sources (style from repo .clang-format, must match CI's clang-format 18)
---

Format in place:
```bash
git ls-files '*.h' '*.cpp' | xargs clang-format -i
```

Check only (what CI runs):
```bash
git ls-files '*.cpp' '*.h' | xargs clang-format --dry-run -Werror --style=file
```

Generated files (`font_data.cpp`, `stb_truetype.h`, …) are excluded via `.clang-format-ignore`. Never hand-tweak style to silence it.
