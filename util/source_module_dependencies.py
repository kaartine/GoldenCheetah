#!/usr/bin/env python3
"""Inventory source-module dependencies without running a preprocessor.

The resulting baseline is a description of existing dependency debt.  It is
not an approved dependency graph or a statement that the observed edges are
desirable.
"""

from __future__ import annotations

import argparse
import difflib
import json
import os
import posixpath
import re
import stat
import sys
import tempfile
from collections import defaultdict
from pathlib import Path, PurePosixPath
from typing import Iterable, Iterator


MODULES = (
    "ANT",
    "Charts",
    "Cloud",
    "Core",
    "FileIO",
    "Gui",
    "Metrics",
    "Planning",
    "Python",
    "R",
    "Train",
)
SOURCE_SUFFIXES = frozenset(
    (
        ".h",
        ".hh",
        ".hpp",
        ".hxx",
        ".c",
        ".cc",
        ".cpp",
        ".cxx",
        ".m",
        ".mm",
        ".inc",
        ".inl",
        ".ipp",
        ".tpp",
        ".y",
        ".l",
    )
)
BUILD_DIRECTORY_NAMES = frozenset(
    (".git", ".hg", ".svn", ".qtc_clangd", "CMakeFiles", "build", "debug", "release")
)
BUILD_DIRECTORY_PREFIXES = ("build-", "cmake-build-")
INCLUDE_RE = re.compile(r"^\s*#\s*(?:include|import)\s*(.*?)\s*$")
QUOTED_INCLUDE_RE = re.compile(r'^"([^"\r\n]*)"$')
ANGLE_INCLUDE_RE = re.compile(r"^<([^>\r\n]*)>$")
PARSER_HEADER_RE = re.compile(
    r"(?:_yacc\.h|_lex\.h|\.tab\.h|\.yy\.h)$", re.IGNORECASE
)


class DependencyError(Exception):
    """A deterministic inventory cannot safely be produced."""


def _is_build_directory(name: str) -> bool:
    return name in BUILD_DIRECTORY_NAMES or name.startswith(BUILD_DIRECTORY_PREFIXES)


def _is_excluded_generated_sip(relative: PurePosixPath) -> bool:
    parts = relative.parts
    return (
        len(parts) >= 4
        and parts[0] == "src"
        and parts[1:3] == ("Python", "SIP")
        and relative.name.startswith("sip")
        and relative.suffix in {".h", ".c", ".cpp"}
    )


def _scope_for(relative: PurePosixPath) -> str:
    if len(relative.parts) < 2 or relative.parts[0] != "src":
        raise DependencyError(f"source is outside src: {relative.as_posix()}")
    if relative.parts[1] in MODULES:
        return relative.parts[1]
    raise DependencyError(f"source is outside the inventoried modules: {relative.as_posix()}")


def _iter_tree_files(repository: Path, directory: Path) -> Iterator[Path]:
    try:
        entries = sorted(os.scandir(directory), key=lambda entry: entry.name)
    except OSError as error:
        raise DependencyError(f"cannot enumerate {directory}: {error}") from error

    for entry in entries:
        path = Path(entry.path)
        try:
            if entry.is_symlink():
                raise DependencyError(
                    f"symlink is not permitted in inventoried source: "
                    f"{path.relative_to(repository).as_posix()}"
                )
            if entry.is_dir(follow_symlinks=False):
                if not _is_build_directory(entry.name):
                    yield from _iter_tree_files(repository, path)
                continue
            if path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            if not entry.is_file(follow_symlinks=False):
                raise DependencyError(
                    f"non-regular source is not permitted: "
                    f"{path.relative_to(repository).as_posix()}"
                )
            relative = PurePosixPath(path.relative_to(repository).as_posix())
            if not _is_excluded_generated_sip(relative):
                yield path
        except OSError as error:
            raise DependencyError(f"cannot inspect {path}: {error}") from error


def _source_files(repository: Path) -> list[Path]:
    source_root = repository / "src"
    try:
        source_stat = source_root.lstat()
    except OSError as error:
        raise DependencyError(f"cannot inspect source directory {source_root}: {error}") from error
    if stat.S_ISLNK(source_stat.st_mode) or not stat.S_ISDIR(source_stat.st_mode):
        raise DependencyError(f"source directory is not a real directory: {source_root}")

    files: list[Path] = []
    try:
        root_entries = sorted(os.scandir(source_root), key=lambda entry: entry.name)
    except OSError as error:
        raise DependencyError(f"cannot enumerate {source_root}: {error}") from error

    for entry in root_entries:
        path = Path(entry.path)
        if entry.name in MODULES:
            if entry.is_symlink() or not entry.is_dir(follow_symlinks=False):
                raise DependencyError(f"module is not a real directory: src/{entry.name}")
            files.extend(_iter_tree_files(repository, path))
    return sorted(files, key=lambda path: path.relative_to(repository).as_posix())


def _read_utf8(path: Path, repository: Path) -> str:
    relative = path.relative_to(repository).as_posix()
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
        with os.fdopen(descriptor, "rb") as stream:
            opened_metadata = os.fstat(stream.fileno())
            current_metadata = path.lstat()
            if (
                not stat.S_ISREG(opened_metadata.st_mode)
                or stat.S_ISLNK(current_metadata.st_mode)
                or not stat.S_ISREG(current_metadata.st_mode)
                or (opened_metadata.st_dev, opened_metadata.st_ino)
                != (current_metadata.st_dev, current_metadata.st_ino)
            ):
                raise DependencyError(f"source changed or is not a regular file: {relative}")
            data = stream.read()
    except OSError as error:
        raise DependencyError(f"cannot read {relative}: {error}") from error
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise DependencyError(f"source is not valid UTF-8: {relative}: {error}") from error


def _without_comments(text: str) -> str:
    """Remove C/C++ comments while preserving strings, lines, and columns."""
    output: list[str] = []
    state = "normal"
    index = 0
    while index < len(text):
        char = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "normal":
            if char == "/" and following == "/":
                output.extend((" ", " "))
                state = "line-comment"
                index += 2
                continue
            if char == "/" and following == "*":
                output.extend((" ", " "))
                state = "block-comment"
                index += 2
                continue
            output.append(char)
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            index += 1
            continue
        if state == "line-comment":
            if char == "\n":
                output.append(char)
                state = "normal"
            else:
                output.append(" ")
            index += 1
            continue
        if state == "block-comment":
            if char == "*" and following == "/":
                output.extend((" ", " "))
                state = "normal"
                index += 2
            else:
                output.append("\n" if char == "\n" else " ")
                index += 1
            continue

        output.append(char)
        if char == "\\" and following:
            output.append(following)
            index += 2
            continue
        if (state == "string" and char == '"') or (state == "character" and char == "'"):
            state = "normal"
        index += 1
    return "".join(output)


def _includes(text: str) -> Iterator[tuple[str, str]]:
    for line in _without_comments(text).splitlines():
        match = INCLUDE_RE.match(line)
        if not match:
            continue
        operand = match.group(1).strip()
        quoted = QUOTED_INCLUDE_RE.fullmatch(operand)
        if quoted:
            yield "quote", quoted.group(1)
            continue
        angled = ANGLE_INCLUDE_RE.fullmatch(operand)
        if angled:
            yield "angle", angled.group(1)
            continue
        if operand:
            yield "macro", operand
        else:
            raise DependencyError("empty #include directive")


def _safe_include_path(source: str, include: str) -> str:
    if not include or "\x00" in include or "\\" in include:
        raise DependencyError(f"unsafe include path in {source}: {include!r}")
    include_path = PurePosixPath(include)
    if include_path.is_absolute() or (include_path.parts and ":" in include_path.parts[0]):
        raise DependencyError(f"unsafe absolute include path in {source}: {include!r}")
    local = posixpath.normpath(posixpath.join(posixpath.dirname(source), include))
    if local == ".." or local.startswith("../"):
        raise DependencyError(f"include escapes the repository in {source}: {include!r}")
    return local


def _is_generated_parser_header(include: str) -> bool:
    return bool(PARSER_HEADER_RE.search(PurePosixPath(include).name))


def _is_generated_sip_include(include: str) -> bool:
    path = PurePosixPath(include)
    return path.name.startswith("sip") and path.suffix.lower() in {".h", ".hpp", ".c", ".cpp"}


def _resolve_include(
    source: str,
    include: str,
    kind: str,
    paths: set[str],
    casefolded_paths: dict[str, list[str]],
    basenames: dict[str, list[str]],
    casefolded_basenames: dict[str, list[str]],
    module_relative_paths: dict[str, list[str]],
    casefolded_module_relative_paths: dict[str, list[str]],
) -> str | None:
    def unique(candidates: list[str], reason: str) -> str | None:
        if len(candidates) > 1:
            rendered = ", ".join(candidates)
            raise DependencyError(
                f"ambiguous {reason} in {source}: {include!r}; candidates: {rendered}"
            )
        return candidates[0] if candidates else None

    def reject_wrong_case(candidates: list[str]) -> None:
        if candidates:
            rendered = ", ".join(candidates)
            raise DependencyError(
                f"first-party include has wrong case in {source}: {include!r}; "
                f"candidates: {rendered}"
            )

    local = _safe_include_path(source, include)
    if kind == "quote" and local in paths:
        return local
    if kind == "quote":
        reject_wrong_case(casefolded_paths.get(local.casefold(), []))

    include_parts = PurePosixPath(include).parts
    if include_parts and include_parts[0] in MODULES:
        explicit = posixpath.normpath(posixpath.join("src", include))
        if explicit in paths:
            return explicit
    if include_parts:
        explicit = posixpath.normpath(posixpath.join("src", include))
        reject_wrong_case(casefolded_paths.get(explicit.casefold(), []))

    normalized = posixpath.normpath(include)
    if len(include_parts) > 1:
        candidates = module_relative_paths.get(normalized, [])
        folded_candidates = casefolded_module_relative_paths.get(normalized.casefold(), [])
        if len(folded_candidates) > 1:
            unique(folded_candidates, "case-folded qualified include")
        resolved = unique(candidates, "qualified include")
        if resolved is not None:
            return resolved
        reject_wrong_case(folded_candidates)
        return None

    candidates = basenames.get(PurePosixPath(include).name, [])
    folded_candidates = casefolded_basenames.get(PurePosixPath(include).name.casefold(), [])
    if len(folded_candidates) > 1:
        unique(folded_candidates, "case-folded include")
    resolved = unique(candidates, "include")
    if resolved is not None:
        return resolved
    reject_wrong_case(folded_candidates)
    return None


def _strongly_connected_components(edges: Iterable[tuple[str, str]]) -> list[list[str]]:
    graph: dict[str, set[str]] = {scope: set() for scope in MODULES}
    for source, target in edges:
        graph[source].add(target)

    next_index = 0
    indices: dict[str, int] = {}
    lowlinks: dict[str, int] = {}
    stack: list[str] = []
    on_stack: set[str] = set()
    components: list[list[str]] = []

    def visit(node: str) -> None:
        nonlocal next_index
        indices[node] = next_index
        lowlinks[node] = next_index
        next_index += 1
        stack.append(node)
        on_stack.add(node)
        for adjacent in sorted(graph[node]):
            if adjacent not in indices:
                visit(adjacent)
                lowlinks[node] = min(lowlinks[node], lowlinks[adjacent])
            elif adjacent in on_stack:
                lowlinks[node] = min(lowlinks[node], indices[adjacent])
        if lowlinks[node] != indices[node]:
            return
        component: list[str] = []
        while True:
            member = stack.pop()
            on_stack.remove(member)
            component.append(member)
            if member == node:
                break
        component.sort()
        if len(component) > 1:
            components.append(component)

    for scope in MODULES:
        if scope not in indices:
            visit(scope)
    return sorted(components)


def build_inventory(repository: Path) -> dict[str, object]:
    try:
        repository = repository.resolve(strict=True)
    except OSError as error:
        raise DependencyError(f"cannot resolve repository {repository}: {error}") from error
    if not repository.is_dir():
        raise DependencyError(f"repository is not a directory: {repository}")

    files = _source_files(repository)
    relative_paths = [path.relative_to(repository).as_posix() for path in files]
    path_set = set(relative_paths)
    casefolded_path_index: dict[str, list[str]] = defaultdict(list)
    basename_index: dict[str, list[str]] = defaultdict(list)
    casefolded_basename_index: dict[str, list[str]] = defaultdict(list)
    module_relative_index: dict[str, list[str]] = defaultdict(list)
    casefolded_module_relative_index: dict[str, list[str]] = defaultdict(list)
    for relative in relative_paths:
        relative_path = PurePosixPath(relative)
        basename = relative_path.name
        below_module = PurePosixPath(*relative_path.parts[2:]).as_posix()
        casefolded_path_index[relative.casefold()].append(relative)
        basename_index[basename].append(relative)
        casefolded_basename_index[basename.casefold()].append(relative)
        module_relative_index[below_module].append(relative)
        casefolded_module_relative_index[below_module.casefold()].append(relative)
    for index in (
        casefolded_path_index,
        basename_index,
        casefolded_basename_index,
        module_relative_index,
        casefolded_module_relative_index,
    ):
        for candidates in index.values():
            candidates.sort()

    cross_records: set[tuple[str, str, str, str]] = set()
    graph_edges: set[tuple[str, str]] = set()
    generated_records: set[tuple[str, str, str]] = set()
    unresolved_records: set[tuple[str, str]] = set()
    macro_records: set[tuple[str, str]] = set()

    for path, source in zip(files, relative_paths):
        source_scope = _scope_for(PurePosixPath(source))
        try:
            parsed_includes = _includes(_read_utf8(path, repository))
            for kind, operand in parsed_includes:
                if kind == "macro":
                    macro_records.add((source, operand))
                    continue
                target = _resolve_include(
                    source,
                    operand,
                    kind,
                    path_set,
                    casefolded_path_index,
                    basename_index,
                    casefolded_basename_index,
                    module_relative_index,
                    casefolded_module_relative_index,
                )
                if target is None:
                    if _is_generated_parser_header(operand):
                        generated_records.add((source, operand, "parser"))
                    elif _is_generated_sip_include(operand):
                        generated_records.add((source, operand, "sip"))
                    elif kind == "quote":
                        unresolved_records.add((source, operand))
                    continue
                target_scope = _scope_for(PurePosixPath(target))
                if source_scope != target_scope:
                    cross_records.add((source, source_scope, target, target_scope))
                    graph_edges.add((source_scope, target_scope))
        except DependencyError as error:
            if str(error) == "empty #include directive":
                raise DependencyError(f"empty #include directive in {source}") from error
            raise

    return {
        "description": (
            "Observed source-module dependency debt inventory. It describes existing "
            "references and does not define or approve a target architecture."
        ),
        "format_version": 1,
        "modules": list(MODULES),
        "cross_module_includes": [
            {
                "source": source,
                "source_module": source_scope,
                "target": target,
                "target_module": target_scope,
            }
            for source, source_scope, target, target_scope in sorted(cross_records)
        ],
        "generated_includes": [
            {"source": source, "include": include, "generator": generator}
            for source, include, generator in sorted(generated_records)
        ],
        "unresolved_quoted_includes": [
            {"source": source, "include": include}
            for source, include in sorted(unresolved_records)
        ],
        "macro_includes": [
            {"source": source, "expression": expression}
            for source, expression in sorted(macro_records)
        ],
        "strongly_connected_components": _strongly_connected_components(graph_edges),
    }


def _canonical_json(inventory: dict[str, object]) -> str:
    return json.dumps(inventory, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def _read_baseline(path: Path) -> str:
    try:
        metadata = path.lstat()
    except OSError as error:
        raise DependencyError(f"cannot inspect baseline {path}: {error}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise DependencyError(f"baseline is not a regular non-symlink file: {path}")
    try:
        return path.read_bytes().decode("utf-8")
    except UnicodeDecodeError as error:
        raise DependencyError(f"baseline is not valid UTF-8: {path}: {error}") from error
    except OSError as error:
        raise DependencyError(f"cannot read baseline {path}: {error}") from error


def _write_baseline_atomic(path: Path, contents: str) -> None:
    parent = path.parent
    try:
        parent_metadata = parent.lstat()
    except OSError as error:
        raise DependencyError(f"cannot inspect baseline directory {parent}: {error}") from error
    if stat.S_ISLNK(parent_metadata.st_mode) or not stat.S_ISDIR(parent_metadata.st_mode):
        raise DependencyError(f"baseline parent is not a regular directory: {parent}")

    existing_mode: int | None = None
    try:
        metadata = path.lstat()
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            raise DependencyError(f"baseline is not a regular non-symlink file: {path}")
        existing_mode = stat.S_IMODE(metadata.st_mode)
    except FileNotFoundError:
        pass
    except OSError as error:
        raise DependencyError(f"cannot inspect baseline {path}: {error}") from error

    temporary_name: str | None = None
    try:
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{path.name}.", suffix=".tmp", dir=parent
        )
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(contents.encode("utf-8"))
            stream.flush()
            os.fsync(stream.fileno())
        if existing_mode is not None:
            os.chmod(temporary_name, existing_mode)
        os.replace(temporary_name, path)
        temporary_name = None
        if os.name == "posix":
            directory_descriptor = os.open(parent, os.O_RDONLY)
            try:
                os.fsync(directory_descriptor)
            finally:
                os.close(directory_descriptor)
    except OSError as error:
        raise DependencyError(f"cannot atomically write baseline {path}: {error}") from error
    finally:
        if temporary_name is not None:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass


def _baseline_label(path: Path, repository: Path) -> str:
    try:
        return path.resolve(strict=False).relative_to(repository).as_posix()
    except ValueError:
        return path.as_posix()


def _arguments(arguments: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Inventory current source-module dependency debt deterministically."
    )
    default_repository = Path(__file__).resolve().parents[1]
    parser.add_argument("--repository", type=Path, default=default_repository)
    parser.add_argument("--baseline", type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--print-candidate", action="store_true")
    action.add_argument("--check", action="store_true")
    action.add_argument("--write-baseline", action="store_true")
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    options = _arguments(sys.argv[1:] if arguments is None else arguments)
    try:
        repository = options.repository.resolve(strict=True)
        baseline = options.baseline or repository / "doc" / "design" / "source-module-dependencies.baseline"
        if not baseline.is_absolute():
            baseline = repository / baseline
        candidate = _canonical_json(build_inventory(repository))
        if options.print_candidate:
            sys.stdout.write(candidate)
            return 0
        if options.write_baseline:
            _write_baseline_atomic(baseline, candidate)
            return 0

        current = _read_baseline(baseline)
        if current == candidate:
            return 0
        difference = difflib.unified_diff(
            current.splitlines(keepends=True),
            candidate.splitlines(keepends=True),
            fromfile=_baseline_label(baseline, repository),
            tofile="current source-module dependency inventory",
            lineterm="\n",
        )
        sys.stderr.writelines(difference)
        return 1
    except DependencyError as error:
        print(f"source_module_dependencies.py: error: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"source_module_dependencies.py: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
