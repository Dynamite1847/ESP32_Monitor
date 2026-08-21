#!/usr/bin/env python3
"""Extract Han characters from UI-facing C string literals and glyph text files."""

from pathlib import Path


FIRMWARE_DIR = Path(__file__).resolve().parent.parent
SOURCE_ROOTS = (
    FIRMWARE_DIR / "components" / "ui",
    FIRMWARE_DIR / "components" / "app_model",
)
EXCLUDED_NAMES = {
    "desk_ui_font_16.c",
    "desk_ui_cjk_font_16.c",
}


def c_string_contents(source: str):
    normal, line_comment, block_comment, string, character = range(5)
    state = normal
    escaped = False
    content: list[str] = []
    index = 0

    while index < len(source):
        current = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""

        if state == normal:
            if current == "/" and following == "/":
                state = line_comment
                index += 2
                continue
            if current == "/" and following == "*":
                state = block_comment
                index += 2
                continue
            if current == '"':
                state = string
                escaped = False
                content = []
            elif current == "'":
                state = character
                escaped = False
        elif state == line_comment:
            if current == "\n":
                state = normal
        elif state == block_comment:
            if current == "*" and following == "/":
                state = normal
                index += 2
                continue
        elif state == character:
            if escaped:
                escaped = False
            elif current == "\\":
                escaped = True
            elif current == "'":
                state = normal
        elif state == string:
            if escaped:
                escaped = False
                content.append(current)
            elif current == "\\":
                escaped = True
            elif current == '"':
                yield "".join(content)
                state = normal
            else:
                content.append(current)

        index += 1


def is_han(character: str) -> bool:
    codepoint = ord(character)
    return 0x3400 <= codepoint <= 0x9FFF


def main() -> None:
    glyphs: set[str] = set()
    for root in SOURCE_ROOTS:
        for path in root.rglob("*"):
            if not path.is_file() or path.name in EXCLUDED_NAMES:
                continue
            if path.suffix in {".c", ".h"}:
                text = path.read_text(encoding="utf-8")
                for literal in c_string_contents(text):
                    glyphs.update(character for character in literal if is_han(character))
            elif path.suffix == ".txt":
                text = path.read_text(encoding="utf-8")
                glyphs.update(character for character in text if is_han(character))

    for glyph in sorted(glyphs):
        print(glyph)


if __name__ == "__main__":
    main()
