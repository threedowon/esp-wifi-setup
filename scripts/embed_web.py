"""Embed the offline settings page in flash during every PlatformIO build."""
from pathlib import Path

Import("env")
root = Path(env["PROJECT_DIR"])
page = (root / "web/index.html").read_text(encoding="utf-8")
target = root / "include/web_page.h"
target.parent.mkdir(exist_ok=True)
target.write_text(
    '#pragma once\n#include <Arduino.h>\n'
    'static const char WEB_PAGE[] PROGMEM = R"SETUPPAGE('
    + page + ')SETUPPAGE";\n', encoding="utf-8"
)

