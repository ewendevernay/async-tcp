#!/bin/bash

CFLAGS="-Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable"
LFLAGS="-lm -lpthread"

if [ $# -eq 0 ] || [ $1 == debug ]; then
    gcc -g -DDEBUG $CFLAGS server.c -o chat_server $LFLAGS
elif [ $1 == release ]; then
    gcc -O2 -DNDEBUG $CFLAGS server.c -o chat_server $LFLAGS
else
	echo Unkown argument!
fi
