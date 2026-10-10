#!/usr/bin/env python3

import ctypes
import errno
import json
import os
import platform
import re
import sys

IGNORABLE_CHARACTERS = (
    '\u200B', '\u200C', '\u200D', '\u200E', '\u200F',
    '\u202A', '\u202B', '\u202C', '\u202D', '\u202E',
    '\u206A', '\u206B', '\u206C', '\u206D', '\u206E', '\u206F',
    '\uFEFF',
)
IGNORED_ALTERNATIVES = '(' + '|'.join(IGNORABLE_CHARACTERS) + ')*'


def generated_without_ignored_alternatives(generated):
    if IGNORED_ALTERNATIVES not in generated:
        raise ValueError('generated profile has no ignored-character alternatives')
    result = generated.replace(IGNORED_ALTERNATIVES, '')
    if result == generated or IGNORED_ALTERNATIVES in result:
        raise AssertionError('ignored-character alternatives were not removed')
    return result


def self_test():
    generated = '(regex "^/home/Library/' + IGNORED_ALTERNATIVES + 'Telegram$")'
    without_ignored = generated_without_ignored_alternatives(generated)
    if without_ignored == generated or IGNORED_ALTERNATIVES in without_ignored:
        raise AssertionError('diagnostic profile variant did not change')
    try:
        generated_without_ignored_alternatives('(allow default)')
    except ValueError:
        pass
    else:
        raise AssertionError('missing alternatives were silently accepted')
    print('diagnose_mac_seatbelt_profile_variants=PASS')


def open_error(path):
    try:
        descriptor = os.open(path, os.O_RDONLY)
    except OSError as error:
        return error.errno
    os.close(descriptor)
    return 0


def cat_result(path, mode):
    reader, writer = os.pipe()
    sink = os.open('/dev/null', os.O_WRONLY)
    arguments = ['/bin/cat', path]
    actions = [
        (os.POSIX_SPAWN_DUP2, writer, 2),
        (os.POSIX_SPAWN_DUP2, sink, 1),
        (os.POSIX_SPAWN_CLOSE, reader),
        (os.POSIX_SPAWN_CLOSE, writer),
        (os.POSIX_SPAWN_CLOSE, sink),
    ]
    if mode == 'posix_spawn':
        child = os.posix_spawn('/bin/cat', arguments, os.environ,
                              file_actions=actions)
    else:
        child = os.fork()
        if child == 0:
            os.dup2(writer, 2)
            os.dup2(sink, 1)
            os.close(reader)
            os.close(writer)
            os.close(sink)
            os.execve('/bin/cat', arguments, os.environ)
    os.close(writer)
    os.close(sink)
    diagnostic = b''
    while True:
        chunk = os.read(reader, 4096)
        if not chunk:
            break
        diagnostic += chunk
    os.close(reader)
    _, status = os.waitpid(child, 0)
    return {'exit': os.waitstatus_to_exitcode(status),
            'eperm': os.strerror(errno.EPERM).encode() in diagnostic}


def fork_errors(denied, allowed):
    reader, writer = os.pipe()
    child = os.fork()
    if child == 0:
        os.close(reader)
        errors = [open_error(denied), open_error(allowed)]
        os.write(writer, json.dumps(errors).encode())
        os.close(writer)
        os._exit(0)
    os.close(writer)
    errors = os.read(reader, 4096)
    os.close(reader)
    _, status = os.waitpid(child, 0)
    if status != 0:
        raise RuntimeError('fork open probe failed')
    return json.loads(errors)


def apply_and_probe(library, name, profile, denied, allowed):
    error = ctypes.c_char_p()
    status = library.sandbox_init(profile.encode(), 0, ctypes.byref(error))
    print(json.dumps({'profile': name, 'stage': 'application', 'status': status,
                      'error': error.value.decode(errors='replace')
                      if error.value else None}), flush=True)
    if error.value:
        library.sandbox_free_error(error)
    if status != 0:
        return name == 'invalid'
    if name == 'invalid':
        return False
    errors = [open_error(denied), open_error(allowed)]
    print(json.dumps({'profile': name, 'stage': 'parent',
                      'open_errno': errors}), flush=True)
    forked = fork_errors(denied, allowed)
    print(json.dumps({'profile': name, 'stage': 'fork',
                      'open_errno': forked}), flush=True)
    results = {}
    for mode in ['fork_exec', 'posix_spawn']:
        results[mode] = [cat_result(denied, mode), cat_result(allowed, mode)]
        print(json.dumps({'profile': name, 'stage': mode,
                          'cat': results[mode]}), flush=True)
    if name in ['literal', 'ascii_regex']:
        return (errors == [errno.EPERM, 0] and forked == errors
                and all(result == [{'exit': 1, 'eperm': True},
                                   {'exit': 0, 'eperm': False}]
                        for result in results.values()))
    if name == 'allow_only':
        return (errors == [0, 0] and forked == errors
                and all(result == [{'exit': 0, 'eperm': False}] * 2
                        for result in results.values()))
    return True


def main():
    if sys.argv[1:] == ['--self-test']:
        self_test()
        return 0
    if platform.system() != 'Darwin' or len(sys.argv) != 4:
        raise ValueError('requires macOS and synthetic home, denied, allowed paths')
    home, denied, allowed = map(os.path.realpath, sys.argv[1:])
    for path in [denied, allowed]:
        if (os.path.commonpath([home, path]) != home
                or os.path.relpath(path, home).split(os.sep)[0] != 'Library'):
            raise ValueError('probe paths must be inside the synthetic home Library')
        if open_error(path) != 0:
            raise ValueError('unsandboxed fixture is not readable')
    output = sys.stdin.read()
    marker = 'Seatbelt profile:\n'
    if marker not in output:
        raise ValueError('application did not provide the generated profile')
    generated = output.split(marker, 1)[1].strip()
    base = '(version 1)\n(allow default)\n'
    quoted = json.dumps(denied, ensure_ascii=False)
    escaped = re.sub(r'([\\.^$*+?{}\[\]()|])', r'\\\1', denied)
    ascii_regex = json.dumps('^' + escaped + '$', ensure_ascii=False)
    profiles = [
        ('allow_only', base),
        ('literal', base + '(deny file* (literal ' + quoted + '))'),
        ('ascii_regex', base + '(deny file* (regex ' + ascii_regex + '))'),
        ('generated', generated),
        ('generated_without_ignored_alternatives',
         generated_without_ignored_alternatives(generated)),
        ('invalid', base + '('),
    ]
    library = ctypes.CDLL('/usr/lib/libsandbox.dylib', use_errno=True)
    library.sandbox_init.argtypes = [ctypes.c_char_p, ctypes.c_uint64,
                                    ctypes.POINTER(ctypes.c_char_p)]
    library.sandbox_init.restype = ctypes.c_int
    library.sandbox_free_error.argtypes = [ctypes.c_char_p]
    library.sandbox_free_error.restype = None
    print(json.dumps({'seatbelt_diagnosis': 'synthetic-only',
                      'macos': platform.mac_ver()[0],
                      'architecture': platform.machine(),
                      'baseline_open_errno': [0, 0]}), flush=True)
    succeeded = True
    for name, profile in profiles:
        print(json.dumps({'profile': name, 'stage': 'begin'}), flush=True)
        child = os.fork()
        if child == 0:
            try:
                passed = apply_and_probe(library, name, profile, denied, allowed)
                os._exit(0 if passed else 1)
            except Exception as error:
                print(json.dumps({'profile': name, 'error': str(error)}), flush=True)
                os._exit(1)
        _, status = os.waitpid(child, 0)
        succeeded = succeeded and status == 0
        print(json.dumps({'profile': name, 'stage': 'complete',
                          'exit': os.waitstatus_to_exitcode(status)}), flush=True)
    return 0 if succeeded else 1


if __name__ == '__main__':
    sys.exit(main())
