#!/usr/bin/env python3

import ctypes
import errno
import json
import os
import platform
import re
import sys
import tempfile
import unicodedata

IGNORABLE_CHARACTERS = (
    '\u200B', '\u200C', '\u200D', '\u200E', '\u200F',
    '\u202A', '\u202B', '\u202C', '\u202D', '\u202E',
    '\u206A', '\u206B', '\u206C', '\u206D', '\u206E', '\u206F',
    '\uFEFF',
)
IGNORED_CHARACTER_CLASS = '[' + ''.join(IGNORABLE_CHARACTERS) + ']*'
IGNORED_CHARACTER_ALTERNATION = (
    '(' + '|'.join(re.escape(value) for value in IGNORABLE_CHARACTERS) + ')*'
)
BASE_PROFILE = '(version 1)\n(allow default)\n'
PROTECTED_RELATIVE = (
    'Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/'
    'synthetic-canary'
)
ALLOWED_RELATIVE = (
    'Library/Application Support/Teagram/tdata/synthetic-allowed-file'
)


def generated_without_ignored_class(generated):
    if IGNORED_CHARACTER_CLASS not in generated:
        raise ValueError('generated profile has no ignored-character class')
    result = generated.replace(IGNORED_CHARACTER_CLASS, '')
    if result == generated or IGNORED_CHARACTER_CLASS in result:
        raise AssertionError('ignored-character class was not removed')
    return result


def regex_literal(value):
    return re.sub(r'([\\.^$|()\[\]{}*+?])', r'\\\1', value)


def ascii_case_pattern(value):
    result = []
    for character in value:
        if character.isascii() and character.isalpha():
            result.append('[' + character.lower() + character.upper() + ']')
        else:
            result.append(regex_literal(character))
    return ''.join(result)


def sbpl_regex(expression):
    encoded = expression.encode('utf-8')
    if len(encoded) + 2 < 900:
        return '(regex\n' + json.dumps(expression, ensure_ascii=False) + '\n)\n'
    chunks = []
    start = 0
    while start < len(encoded):
        end = min(start + 512, len(encoded))
        while end < len(encoded) and (encoded[end] & 0xC0) == 0x80:
            end -= 1
        chunks.append(encoded[start:end].decode('utf-8'))
        start = end
    strings = ''.join(
        json.dumps(chunk, ensure_ascii=False) + '\n' for chunk in chunks)
    return '(regex\n(string-append\n' + strings + ')\n)\n'


def diagnostic_profile(expression):
    return BASE_PROFILE + '(deny file*\n' + sbpl_regex(expression) + ')\n'


def diagnostic_home_controls(home):
    root = os.path.join(home, 'diagnostic-home-patterns')
    cases = []

    def add(name, path_home, home_pattern):
        denied = os.path.join(path_home, PROTECTED_RELATIVE)
        allowed = os.path.join(path_home, ALLOWED_RELATIVE)
        expression = (
            '^' + home_pattern + '/' + regex_literal(PROTECTED_RELATIVE) + '$')
        cases.append((name, diagnostic_profile(expression), denied, allowed,
                      'deny', expression))

    ascii_profile_home = os.path.join(root, 'ascii-case-home')
    ascii_path_home = os.path.join(root, 'ASCII-CASE-HOME')
    add('home_ascii_case_classes', ascii_path_home,
        regex_literal(os.path.dirname(ascii_profile_home)) + '/'
        + ascii_case_pattern(os.path.basename(ascii_profile_home)))

    kelvin_profile_home = os.path.join(root, 'kelvin-K-home')
    kelvin_path_home = os.path.join(root, 'kelvin-K-home')
    add('home_kelvin_class', kelvin_path_home,
        regex_literal(os.path.dirname(kelvin_profile_home))
        + '/kelvin-[KkK]-home')

    long_s_profile_home = os.path.join(root, 'long-S-home')
    long_s_path_home = os.path.join(root, 'long-ſ-home')
    add('home_long_s_class', long_s_path_home,
        regex_literal(os.path.dirname(long_s_profile_home))
        + '/long-[Ssſ]-home')

    sigma_profile_home = os.path.join(root, 'sigma-Σ-home')
    sigma_path_home = os.path.join(root, 'sigma-ς-home')
    add('home_unicode_case_class', sigma_path_home,
        regex_literal(os.path.dirname(sigma_profile_home))
        + '/sigma-[σςΣ]-home')

    sharp_s_profile_home = os.path.join(root, 'sharp-Maß-home')
    sharp_s_path_home = os.path.join(root, 'sharp-Mass-home')
    add('home_sharp_s_full_fold', sharp_s_path_home,
        regex_literal(os.path.dirname(sharp_s_profile_home))
        + '/sharp-Ma([Ss][Ss]|[ßẞ])-home')

    ignored_profile_home = os.path.join(root, 'ignored-alice-home')
    ignored_path_home = os.path.join(root, 'ignored-al\u200Bice-home')
    add('home_ignored_character_class', ignored_path_home,
        regex_literal(os.path.dirname(ignored_profile_home))
        + '/ignored-al' + IGNORED_CHARACTER_CLASS
        + 'ice-home')
    add('home_ignored_character_alternation', ignored_path_home,
        regex_literal(os.path.dirname(ignored_profile_home))
        + '/ignored-al' + IGNORED_CHARACTER_ALTERNATION
        + 'ice-home')

    nfc_leaf = unicodedata.normalize('NFC', 'nfd-café-home')
    nfd_leaf = unicodedata.normalize('NFD', nfc_leaf)
    nfd_profile_home = os.path.join(root, nfc_leaf)
    nfd_path_home = os.path.join(root, nfd_leaf)
    add('home_nfd_normalization', nfd_path_home,
        regex_literal(unicodedata.normalize('NFD', nfd_profile_home)))

    trie_root = os.path.join(root, 'home-root-alternation')
    trie_nested_home = os.path.join(trie_root, 'nested')
    add('home_root_alternation', trie_nested_home,
        '(' + regex_literal(trie_root) + '|' + regex_literal(trie_nested_home)
        + ')')

    firmlink_path_home = '/System/Volumes/Data' + home
    add('home_optional_firmlink_prefix', firmlink_path_home,
        '(/System/Volumes/Data/|/)' + regex_literal(home.lstrip('/')))

    append_home = os.path.join(root, 'string-append-home')
    append_denied = os.path.join(append_home, PROTECTED_RELATIVE)
    append_allowed = os.path.join(append_home, ALLOWED_RELATIVE)
    alternatives = [regex_literal(append_denied)]
    alternatives.extend('unmatched-diagnostic-branch-' + str(i)
                        for i in range(160))
    expression = '^(' + '|'.join(alternatives) + ')$'
    cases.append(('home_string_append_chunks', diagnostic_profile(expression),
                  append_denied, append_allowed, 'deny', expression))
    return cases


def prepare_control_fixtures(cases):
    for name, _, denied, allowed, _, _ in cases:
        os.makedirs(os.path.dirname(denied), exist_ok=True)
        os.makedirs(os.path.dirname(allowed), exist_ok=True)
        with open(denied, 'wb') as fixture:
            fixture.write(('protected ' + name).encode())
        with open(allowed, 'wb') as fixture:
            fixture.write(('allowed ' + name).encode())


def self_test():
    generated = '(regex "^/home/Library/' + IGNORED_CHARACTER_CLASS + 'Telegram$")'
    without_ignored = generated_without_ignored_class(generated)
    if without_ignored == generated or IGNORED_CHARACTER_CLASS in without_ignored:
        raise AssertionError('diagnostic profile variant did not change')
    try:
        generated_without_ignored_class('(allow default)')
    except ValueError:
        pass
    else:
        raise AssertionError('missing ignored-character class was silently accepted')
    with tempfile.TemporaryDirectory() as home:
        controls = diagnostic_home_controls(home)
    expected_controls = {
        'home_ascii_case_classes',
        'home_kelvin_class',
        'home_long_s_class',
        'home_unicode_case_class',
        'home_sharp_s_full_fold',
        'home_ignored_character_class',
        'home_ignored_character_alternation',
        'home_nfd_normalization',
        'home_root_alternation',
        'home_optional_firmlink_prefix',
        'home_string_append_chunks',
    }
    if {case[0] for case in controls} != expected_controls:
        raise AssertionError('home-pattern diagnostic controls are incomplete')
    if not any('(string-append\n' in case[1] for case in controls):
        raise AssertionError('long regex diagnostic did not exercise string-append')
    for name, _, denied, allowed, _, expression in controls:
        if re.fullmatch(expression, denied) is None:
            raise AssertionError(name + ' does not match its protected spelling')
        if re.fullmatch(expression, allowed) is not None:
            raise AssertionError(name + ' also matches the allowed Teagram path')
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


def apply_and_probe(library, name, profile, denied, allowed, expected):
    error = ctypes.c_char_p()
    status = library.sandbox_init(profile.encode(), 0, ctypes.byref(error))
    print(json.dumps({'profile': name, 'stage': 'application', 'status': status,
                      'error': error.value.decode(errors='replace')
                      if error.value else None}), flush=True)
    if error.value:
        library.sandbox_free_error(error)
    if status != 0:
        return expected == 'invalid'
    if expected == 'invalid':
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
    if expected == 'deny':
        return (errors == [errno.EPERM, 0] and forked == errors
                and all(result == [{'exit': 1, 'eperm': True},
                                   {'exit': 0, 'eperm': False}]
                        for result in results.values()))
    if expected == 'allow':
        return (errors == [0, 0] and forked == errors
                and all(result == [{'exit': 0, 'eperm': False}] * 2
                        for result in results.values()))
    raise ValueError('unknown expected result: ' + expected)


def main():
    if sys.argv[1:] == ['--self-test']:
        self_test()
        return 0
    if platform.system() != 'Darwin' or len(sys.argv) not in [4, 5]:
        raise ValueError('requires macOS and synthetic home, denied, allowed paths')
    home, denied, allowed = map(os.path.realpath, sys.argv[1:4])
    for path in [denied, allowed]:
        if (os.path.commonpath([home, path]) != home
                or os.path.relpath(path, home).split(os.sep)[0] != 'Library'):
            raise ValueError('probe paths must be inside the synthetic home Library')
        if open_error(path) != 0:
            raise ValueError('unsandboxed fixture is not readable')
    output = sys.stdin.read()
    marker = 'Seatbelt profile:\n'
    if marker in output:
        generated = output.split(marker, 1)[1].strip()
    elif len(sys.argv) == 5 and os.path.isfile(sys.argv[4]):
        with open(sys.argv[4], encoding='utf-8') as profile_file:
            generated = profile_file.read()
    else:
        raise ValueError('application did not provide the generated profile')
    base = BASE_PROFILE
    quoted = json.dumps(denied, ensure_ascii=False)
    escaped = re.sub(r'([\\.^$*+?{}\[\]()|])', r'\\\1', denied)
    ascii_regex = json.dumps('^' + escaped + '$', ensure_ascii=False)
    profiles = [
        ('allow_only', base, denied, allowed, 'allow', None),
        ('literal', base + '(deny file* (literal ' + quoted + '))', denied,
         allowed, 'deny', None),
        ('ascii_regex', base + '(deny file* (regex ' + ascii_regex + '))',
         denied, allowed, 'deny', None),
        ('generated', generated, denied, allowed, 'deny', None),
        ('generated_without_ignored_class',
         generated_without_ignored_class(generated), denied, allowed, 'deny',
         None),
        ('invalid', base + '(', denied, allowed, 'invalid', None),
    ]
    profiles.extend(diagnostic_home_controls(home))
    prepare_control_fixtures(profiles[6:])
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
    for name, profile, profile_denied, profile_allowed, expected, _ in profiles:
        print(json.dumps({'profile': name, 'stage': 'begin',
                          'denied': profile_denied,
                          'allowed': profile_allowed,
                          'expected': expected}), flush=True)
        child = os.fork()
        if child == 0:
            try:
                passed = apply_and_probe(
                    library, name, profile, profile_denied, profile_allowed,
                    expected)
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
