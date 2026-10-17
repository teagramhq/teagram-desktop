#!/usr/bin/env python3
"""Prints the Python a shell `run:` body feeds to `python3` on stdin.

Reads the body on stdin. For every quoted heredoc that is the stdin of a
`python3` command reading its program from stdin, prints one `ast.unparse`
line per call and per assignment, so commented-out or deleted code is absent.
A heredoc attached to any other command is data and is skipped. Exits non-zero
when such a heredoc is not valid Python.
"""

import ast
import re
import shlex
import sys

HEREDOC = re.compile(r"<<-?'(\w+)'\s*$")
REDIRECTION = re.compile(
    r"\d*(?:<<-?|>>|>&|<&|&>>?|>\||<>|>|<)\s*(?:'[^']*'|\"[^\"]*\"|[^\s;&|()<>]+)"
)
CONTROL = {"|", "||", "&&", ";", "&", "(", ")", ";;", "|&"}
PREFIX_WORDS = {"!", "if", "then", "else", "elif", "while", "until", "do"}
ASSIGNMENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*=")
# Only flags that leave python3 reading and running its program from stdin.
# Anything else (--version, -c, -m, a script path, an unknown flag) means the
# heredoc is not run, so it is not counted.
STDIN_PROGRAM_FLAGS = {"-B", "-E", "-I", "-O", "-OO", "-P", "-q", "-s", "-S", "-u"}


def last_simple_command(command):
    """Words of the simple command that receives the heredoc, redirections removed."""
    lexer = shlex.shlex(REDIRECTION.sub(" ", command), posix=True, punctuation_chars=";&|()")
    lexer.whitespace_split = True
    words = []
    for token in lexer:
        if token in CONTROL:
            words = []
        else:
            words.append(token)
    return words


def runs_stdin_as_python(command):
    words = last_simple_command(command)
    while words and (words[0] in PREFIX_WORDS or ASSIGNMENT.match(words[0])):
        words.pop(0)
    if words and words[0] == "env":
        # Only `env NAME=value ... python3`; an env option may take an operand.
        words.pop(0)
        while words and ASSIGNMENT.match(words[0]):
            words.pop(0)
    if not words or words[0].rsplit("/", 1)[-1] != "python3":
        return False
    arguments = words[1:]
    while arguments and arguments[0] in STDIN_PROGRAM_FLAGS:
        arguments.pop(0)
    return not arguments or arguments[0] == "-"


def main():
    lines = sys.stdin.read().split("\n")
    index = 0
    while index < len(lines):
        match = HEREDOC.search(lines[index])
        if not match:
            index += 1
            continue
        start = index
        while start > 0 and lines[start - 1].endswith("\\"):
            start -= 1
        command = " ".join(
            line[:-1] if line.endswith("\\") else line for line in lines[start:index + 1]
        )
        end = index + 1
        while end < len(lines) and lines[end].strip() != match.group(1):
            end += 1
        if runs_stdin_as_python(command):
            source = "\n".join(lines[index + 1:end])
            for node in ast.walk(ast.parse(source)):
                if isinstance(node, (ast.Call, ast.Assign)):
                    print(ast.unparse(node))
        index = end + 1


if __name__ == "__main__":
    main()
