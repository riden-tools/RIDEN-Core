#!/usr/bin/env python3
"""No third-party dependency. Connects to an already running daemon."""
import json,socket
with socket.create_connection(('127.0.0.1',19606)) as connection:
    connection.sendall(b'{"cmd":"subscribe"}\n')
    previous=0
    for line in connection.makefile('rb'):
        sample=json.loads(line);raw=sample['registers'];seq=sample['sequence']
        if previous and seq!=previous+1:print('Skipped sequences:',seq-previous-1)
        previous=seq
        print(seq,sample['completed_ns'],raw[10],raw[11],raw[12:14])
