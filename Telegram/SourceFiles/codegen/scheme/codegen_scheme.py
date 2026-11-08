'''
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
'''
import glob, re, binascii, os, sys

sys.dont_write_bytecode = True
scriptPath = os.path.dirname(os.path.realpath(__file__))
sys.path.append(scriptPath + '/../../../lib_tl/tl')
from generate_tl import generate

REDACTED_DUMP_FIELDS = (
  ('auth_signIn', 'phone_code'),
  ('teagram_userInviteLink', 'link'),
)

def redactDumpFields():
  outputPath = ''
  arguments = sys.argv[1:]
  for index, argument in enumerate(arguments):
    if argument == '-o' and index + 1 < len(arguments):
      outputPath = arguments[index + 1]
      break
    elif argument.startswith('-o') and len(argument) > 2:
      outputPath = argument[2:]
      break
  if not outputPath:
    raise ValueError('Output path required.')

  dumpPath = outputPath + '-dump_to_text.cpp'
  with open(dumpPath, encoding='utf-8') as sourceFile:
    source = sourceFile.read()
  for constructor, field in REDACTED_DUMP_FIELDS:
    start = source.find('bool Serialize_' + constructor + '(')
    if start < 0:
      raise ValueError('Missing dump serializer for ' + constructor)
    end = source.find('\nbool Serialize_', start + 1)
    if end < 0:
      end = len(source)
    serializer = source[start:end]
    lines = serializer.splitlines(True)
    matches = [
      index for index, line in enumerate(lines)
      if 'to.add("  ' + field + ': ")' in line
    ]
    if len(matches) != 1:
      raise ValueError('Expected one dump field ' + constructor + '.' + field)
    lineIndex = matches[0]
    originalType = 'types.push_back(mtpc_string+0)'
    if originalType not in lines[lineIndex]:
      raise ValueError('Expected string dump field ' + constructor + '.' + field)
    lines[lineIndex] = lines[lineIndex].replace(
      originalType,
      'types.push_back(mtpc_redacted_string)',
      1)
    updated = ''.join(lines)
    source = source[:start] + updated + source[end:]
  with open(dumpPath, 'w', encoding='utf-8', newline='') as sourceFile:
    sourceFile.write(source)

generate({
  'namespaces': {
    'creator': 'MTP::details',
  },
  'prefixes': {
    'type': 'MTP',
    'data': 'MTPD',
    'id': 'mtpc',
    'construct': 'MTP_',
  },
  'types': {
    'prime': 'mtpPrime',
    'typeId': 'mtpTypeId',
    'buffer': 'mtpBuffer',
  },
  'sections': [
    'read-write',
  ],

  # define some checked flag conversions
  # the key flag type should be a subset of the value flag type
  # with exact the same names, then the key flag can be implicitly
  # casted to the value flag type
  'flagInheritance': {
    'messageService': 'message',
    'updateShortMessage': 'message',
    'updateShortChatMessage': 'message',
    'updateShortSentMessage': 'message',
    'replyKeyboardHide': 'replyKeyboardMarkup',
    'replyKeyboardForceReply': 'replyKeyboardMarkup',
    'inputPeerNotifySettings': 'peerNotifySettings',
    'peerNotifySettings': 'inputPeerNotifySettings',
    'channelForbidden': 'channel',
    'dialogFolder': 'dialog',
    'dialogCommunity': 'dialog',
  },

  'typeIdExceptions': [
    'channel#c88974ac',
    'ipPortSecret#37982646',
    'accessPointRule#4679b65f',
    'help.configSimple#5a592a6c',
    'messageReplies#81834865',
    'teagram.userInvites#b5cfec46',
  ],

  'renamedTypes': {
    'passwordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000SHA256ModPow': 'passwordKdfAlgoModPow',
   },

  'skip': [
    'int ? = Int;',
    'long ? = Long;',
    'double ? = Double;',
    'string ? = String;',

    'vector {t:Type} # [ t ] = Vector t;',

    'int128 4*[ int ] = Int128;',
    'int256 8*[ int ] = Int256;',

    'vector#1cb5c415 {t:Type} # [ t ] = Vector t;',
  ],
  'builtin': [
    'int',
    'long',
    'double',
    'string',
    'bytes',
    'int128',
    'int256',
  ],
  'builtinTemplates': [
    'vector',
    'flags',
  ],
  'synonyms': {
    'bytes': 'string',
  },
  'builtinInclude': 'mtproto/core_types.h',
  'optimizeSingleData': True,

  'dumpToText': {
    'include': 'mtproto/details/mtproto_dump_to_text.h',
  },

})

redactDumpFields()
