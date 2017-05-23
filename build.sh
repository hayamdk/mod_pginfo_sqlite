#!/bin/sh

LD_FLAGS=

ld -liconv -o ./temp.o
if [ $? = 0 ]; then
LD_FLAGS="${LD_FLAGS} -liconv"
else
LD_FLAGS=${LD_FLAGS}
fi
rm -f temp.o

ld -lsqlite3 -o ./temp.o
if [ $? = 0 ]; then
LD_FLAGS="${LD_FLAGS} -lsqlite3"
else
LD_FLAGS=${LD_FLAGS}
fi
rm -f temp.o

gcc -Wall -Ofast -march=native -fpic -flto -finput-charset=cp932 -I./ -D IN_SHARED_MODULE -c utils/tsdstr.c
gcc -Wall -Ofast -march=native -fpic -flto -finput-charset=cp932 -I./ -D IN_SHARED_MODULE -c utils/path.c
gcc -Wall -Ofast -march=native -fpic -flto -finput-charset=cp932 -I./ -D IN_SHARED_MODULE -c utils/arib_parser.c
gcc -Wall -Ofast -march=native -fpic -flto -I./ -D IN_SHARED_MODULE -c utils/aribstr.c
gcc -Wall -Ofast -march=native -fpic -flto -finput-charset=cp932 -I./ -D IN_SHARED_MODULE -c mod_pginfo_sqlite.c
gcc -Wall -shared -fpic -flto -o mod_pginfo_sqlite.so tsdstr.o path.o arib_parser.o aribstr.o mod_pginfo_sqlite.o ${LD_FLAGS}
rm -f tsdstr.o path.o arib_parser.o aribstr.o mod_pginfo_sqlite.o
