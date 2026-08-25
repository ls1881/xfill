"""Lightweight WORD;SCORE dictionary loading + pattern matching.

Used for the interactive "available options for the selected slot" panel,
which needs a fast, simple pattern match (not a full CSP solve) every time
the user selects a slot or types a letter. Deliberately independent of the
C++ engine (dictionary.hpp/dictionary.cpp) -- that one's hot-path-optimized
bitset machinery exists to make full grid *solving* fast; this only ever
needs "top N words of length L matching pattern P," a much simpler job a
plain Python scan handles in well under the latency a keystroke needs.
"""

from __future__ import annotations

from functools import lru_cache


class WordList:
    def __init__(self, path: str, min_score: int = 0):
        self.by_length: dict[int, list[tuple[str, int]]] = {}
        by_length: dict[int, list[tuple[str, int]]] = {}
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                if ";" in line:
                    word, _, score_s = line.partition(";")
                    try:
                        score = int(score_s)
                    except ValueError:
                        score = 0
                else:
                    word, score = line, 0
                word = word.strip().upper()
                if score < min_score or not word or not word.isalpha() or not word.isascii():
                    continue
                by_length.setdefault(len(word), []).append((word, score))
        for words in by_length.values():
            words.sort(key=lambda ws: -ws[1])
        self.by_length = by_length

    def candidates(self, pattern: str, limit: int = 50) -> list[tuple[str, int]]:
        """Top `limit` words (by score) of len(pattern) matching it, where
        '?' in `pattern` matches any letter."""
        length = len(pattern)
        pattern = pattern.upper()
        out: list[tuple[str, int]] = []
        for word, score in self.by_length.get(length, ()):
            if all(p == "?" or p == w for p, w in zip(pattern, word)):
                out.append((word, score))
                if len(out) >= limit:
                    break
        return out

    def has_length(self, length: int) -> bool:
        return length in self.by_length


@lru_cache(maxsize=16)
def _load_cached(path: str, min_score: int) -> WordList:
    return WordList(path, min_score)


def get_word_list(path: str, min_score: int = 0) -> WordList:
    """Cached by (path, min_score) -- these dictionaries can be hundreds of
    thousands of lines, so re-parsing on every request would make the
    options panel noticeably laggy. Invalidate by restarting the server if
    a dictionary file on disk changes underneath an already-cached path."""
    return _load_cached(path, min_score)
