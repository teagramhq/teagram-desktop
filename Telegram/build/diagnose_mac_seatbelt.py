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
IGNORED_NON_ASCII_CLASS = '[^ -~]*'
BASE_PROFILE = '(version 1)\n(allow default)\n'
PROTECTED_RELATIVE = (
    'Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/'
    'synthetic-canary'
)
ALLOWED_RELATIVE = (
    'Library/Application Support/Teagram/tdata/synthetic-allowed-file'
)


def generated_without_ignored_characters(generated):
    patterns = (IGNORED_CHARACTER_CLASS, IGNORED_CHARACTER_ALTERNATION,
                IGNORED_NON_ASCII_CLASS)
    result = generated
    removed = False
    for pattern in patterns:
        if pattern in result:
            result = result.replace(pattern, '')
            removed = True
    if not removed:
        raise ValueError('generated profile has no ignored-character pattern')
    if any(pattern in result for pattern in patterns):
        raise AssertionError('ignored-character pattern was not removed')
    return result


def generated_profile_variants(generated, denied, allowed):
    return [
        ('generated', generated, denied, allowed, 'deny', None),
        ('generated_without_ignored_characters',
         generated_without_ignored_characters(generated), denied, allowed,
         'deny', None),
    ]


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

    parent = regex_literal(root)
    add('home_kelvin_group', os.path.join(root, 'kelvin-\u212A-home'),
        parent + '/kelvin-([Kk]|\u212A)-home')
    add('home_long_s_group', os.path.join(root, 'long-\u017F-home'),
        parent + '/long-([Ss]|\u017F)-home')
    add('home_unicode_case_group', os.path.join(root, 'sigma-\u03C2-home'),
        parent + '/sigma-(\u03C3|\u03C2|\u03A3)-home')
    single_s = '([Ss]|\u017F)'
    sharp_s = '(\u00DF|\u1E9E)'
    add('home_sharp_s_group', os.path.join(root, 'sharp-Ma\u00DF-home'),
        parent + '/sharp-Ma(' + single_s + IGNORED_NON_ASCII_CLASS + single_s
        + '|' + sharp_s + ')-home')
    s_token = '(' + single_s + '|' + sharp_s + ')'
    add('home_sharp_s_fallback',
        os.path.join(root, 'run-\u017F\u00DFs\u1E9E-home'),
        parent + '/run-(' + s_token + IGNORED_NON_ASCII_CLASS + '('
        + s_token + IGNORED_NON_ASCII_CLASS + ')*)-home')

    ignored_pattern = (
        parent + '/ignored-al' + IGNORED_NON_ASCII_CLASS + 'ice-home')
    for index, character in enumerate(IGNORABLE_CHARACTERS):
        add('home_ignored_non_ascii_class_' + format(ord(character), '04X'),
            os.path.join(root, 'ignored-al' + character * (index % 2 + 1)
                         + 'ice-home'),
            ignored_pattern)
    # The class must not stretch over ASCII or across a separator.
    ascii_home = os.path.join(root, 'ignored-al\u200Bice-home')
    expression = '^' + ignored_pattern + '/' + regex_literal(
        PROTECTED_RELATIVE) + '$'
    cases.append(('home_ignored_non_ascii_class_rejects_ascii',
                  diagnostic_profile(expression),
                  os.path.join(ascii_home, PROTECTED_RELATIVE),
                  os.path.join(root, 'ignored-alxice-home',
                               PROTECTED_RELATIVE),
                  'deny', expression))

    composed_leaf = unicodedata.normalize(
        'NFD', 'composed-\u200B\u212A\u017F\u03C2\u00DF\u00E9\uFEFF-home')
    composed_pattern = (
        '(/System/Volumes/Data/|/)' + regex_literal(root.lstrip('/'))
        + '/composed-' + IGNORED_NON_ASCII_CLASS + '([Kk]|\u212A)'
        + IGNORED_NON_ASCII_CLASS + single_s + IGNORED_NON_ASCII_CLASS
        + '(\u03C3|\u03C2|\u03A3)' + IGNORED_NON_ASCII_CLASS + '('
        + single_s + IGNORED_NON_ASCII_CLASS + single_s + '|' + sharp_s + ')'
        + IGNORED_NON_ASCII_CLASS + '[Ee]' + IGNORED_NON_ASCII_CLASS
        + '\u0301' + IGNORED_NON_ASCII_CLASS + '-home')
    add('home_composed_production_forms',
        '/System/Volumes/Data' + os.path.join(root, composed_leaf),
        composed_pattern)

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
    without_ignored = generated_without_ignored_characters(generated)
    if without_ignored == generated or IGNORED_CHARACTER_CLASS in without_ignored:
        raise AssertionError('diagnostic profile variant did not change')
    current_generated = (
        '(regex "^/home/Library/' + IGNORED_NON_ASCII_CLASS
        + 'Telegram$")')
    try:
        current_profiles = generated_profile_variants(
            current_generated, '/home/Library/Telegram/protected',
            '/home/Library/Application Support/Teagram/tdata/allowed')
    except ValueError as error:
        raise AssertionError(
            'diagnostic profile list rejected the current generated format') from error
    if (current_profiles[1][1] == current_generated
            or IGNORED_NON_ASCII_CLASS in current_profiles[1][1]):
        raise AssertionError(
            'current generated profile variant retained ignored characters')
    try:
        generated_without_ignored_characters('(allow default)')
    except ValueError:
        pass
    else:
        raise AssertionError('missing ignored-character class was silently accepted')
    with tempfile.TemporaryDirectory() as home:
        controls = diagnostic_home_controls(home)
    expected_controls = {
        'home_ascii_case_classes',
        'home_kelvin_group',
        'home_long_s_group',
        'home_unicode_case_group',
        'home_sharp_s_group',
        'home_sharp_s_fallback',
        'home_ignored_non_ascii_class_rejects_ascii',
        'home_composed_production_forms',
        'home_nfd_normalization',
        'home_root_alternation',
        'home_optional_firmlink_prefix',
        'home_string_append_chunks',
    } | {
        'home_ignored_non_ascii_class_' + format(ord(character), '04X')
        for character in IGNORABLE_CHARACTERS
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
        *generated_profile_variants(generated, denied, allowed),
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
