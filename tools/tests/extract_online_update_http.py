#!/usr/bin/env python3
"""Extract exact installed-IDF HTTP loops for a real-socket regression test.

This does NOT emulate deadlines or substitute a successful transport. The C
harness supplies POSIX socket I/O and the SDK's actual http_parser; the copied
fetch/read loops are the unmodified source of the slow-drip vulnerability.
"""
import hashlib
from pathlib import Path
import sys

sdk, output = map(Path, sys.argv[1:])
source = sdk / 'components/esp_http_client/esp_http_client.c'
text = source.read_text()
functions = []
for signature in (
    'bool esp_http_client_is_complete_data_received(',
    'int64_t esp_http_client_fetch_headers(',
    'int esp_http_client_read(',
):
    start = text.index(signature)
    begin = text.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        if text[end] == '{': depth += 1
        elif text[end] == '}': depth -= 1
        end += 1
    functions.append(text[start:end])
assert 'while (client->state < HTTP_STATE_RES_COMPLETE_HEADER)' in functions[1]
assert 'while (need_read > 0 && is_data_remain)' in functions[2]
output.write_text('// Verbatim functions from installed ESP-IDF; generated for tests only.\n' + '\n\n'.join(functions) + '\n')
print('Tested SDK HTTP source SHA256:', hashlib.sha256(source.read_bytes()).hexdigest())
